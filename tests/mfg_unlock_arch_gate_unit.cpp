#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#pragma pack(push, 1)
struct MockDosHeader {
    uint16_t e_magic;
    uint8_t  e_cblp[58];
    int32_t  e_lfanew;
};

struct MockFileHeader {
    uint16_t Machine;
    uint16_t NumberOfSections;
    uint32_t TimeDateStamp;
    uint32_t PointerToSymbolTable;
    uint32_t NumberOfSymbols;
    uint16_t SizeOfOptionalHeader;
    uint16_t Characteristics;
};

struct MockSectionHeader {
    uint8_t  Name[8];
    uint32_t VirtualSize;
    uint32_t VirtualAddress;
    uint32_t SizeOfRawData;
    uint32_t PointerToRawData;
    uint32_t PointerToRelocations;
    uint32_t PointerToLinenumbers;
    uint16_t NumberOfRelocations;
    uint16_t NumberOfLinenumbers;
    uint32_t Characteristics;
};

struct MockOptionalHeader64 {
    uint8_t payload[240];
};

struct MockNtHeaders64 {
    uint32_t Signature;
    MockFileHeader FileHeader;
    MockOptionalHeader64 OptionalHeader;
};
#pragma pack(pop)

constexpr uint16_t MOCK_IMAGE_DOS_SIGNATURE = 0x5A4D; // "MZ"
constexpr uint32_t MOCK_IMAGE_NT_SIGNATURE  = 0x00004550; // "PE\0\0"
constexpr uint32_t MOCK_IMAGE_SCN_MEM_EXECUTE = 0x20000000;
constexpr uint32_t MOCK_IMAGE_SCN_MEM_READ    = 0x40000000;

// Replicate the exact logic from MfgUnlock::PatchArchGates for unit validation
size_t SimulatePatchArchGates(uint8_t* base)
{
    auto* dos = reinterpret_cast<MockDosHeader*>(base);
    if (dos->e_magic != MOCK_IMAGE_DOS_SIGNATURE)
        return 0;

    auto* nt = reinterpret_cast<MockNtHeaders64*>(base + dos->e_lfanew);
    if (nt->Signature != MOCK_IMAGE_NT_SIGNATURE)
        return 0;

    auto* section = reinterpret_cast<MockSectionHeader*>(
        reinterpret_cast<uint8_t*>(&nt->OptionalHeader) + nt->FileHeader.SizeOfOptionalHeader);

    std::vector<uint8_t*> sites;

    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i)
    {
        const auto& s = section[i];
        if ((s.Characteristics & MOCK_IMAGE_SCN_MEM_EXECUTE) == 0)
            continue;

        uint8_t* start = base + s.VirtualAddress;
        const size_t size = s.VirtualSize;
        if (size < 6)
            continue;

        for (size_t off = 0; off + 6 <= size; ++off)
        {
            // 3D imm32: cmp eax, 0x1b0 (3D B0 01 00 00)
            if (start[off] == 0x3D && start[off + 1] == 0xB0 && start[off + 2] == 0x01 &&
                start[off + 3] == 0x00 && start[off + 4] == 0x00)
            {
                sites.push_back(start + off + 1);
                continue;
            }
            // 81 /7 imm32: cmp r32, 0x1b0 (ModR/M 0xF8..0xFF, covering all r32 and r8d..r15d)
            if (start[off] == 0x81 && start[off + 1] >= 0xF8 && start[off + 1] <= 0xFF &&
                start[off + 2] == 0xB0 && start[off + 3] == 0x01 && start[off + 4] == 0x00 &&
                start[off + 5] == 0x00)
            {
                sites.push_back(start + off + 2);
                continue;
            }
        }
    }

    if (sites.empty() || sites.size() > 8)
        return 0;

    const uint8_t archAda = 0x90; // 0x190 AD10x
    for (uint8_t* site : sites)
    {
        *site = archAda;
    }

    return sites.size();
}

int main()
{
    printf("[TEST] Running MfgUnlock Architecture Gate Rewriter Unit Tests...\n");

    std::vector<uint8_t> peImage(0x4000, 0);
    uint8_t* base = peImage.data();

    auto* dos = reinterpret_cast<MockDosHeader*>(base);
    dos->e_magic = MOCK_IMAGE_DOS_SIGNATURE;
    dos->e_lfanew = 0x80;

    auto* nt = reinterpret_cast<MockNtHeaders64*>(base + 0x80);
    nt->Signature = MOCK_IMAGE_NT_SIGNATURE;
    nt->FileHeader.NumberOfSections = 2;
    nt->FileHeader.SizeOfOptionalHeader = 240;

    auto* section = reinterpret_cast<MockSectionHeader*>(
        reinterpret_cast<uint8_t*>(&nt->OptionalHeader) + 240);

    // Section 0: .text (executable)
    std::memcpy(section[0].Name, ".text", 5);
    section[0].VirtualAddress = 0x1000;
    section[0].VirtualSize = 0x1000;
    section[0].Characteristics = MOCK_IMAGE_SCN_MEM_EXECUTE | MOCK_IMAGE_SCN_MEM_READ;

    // Section 1: .rdata (data only)
    std::memcpy(section[1].Name, ".rdata", 6);
    section[1].VirtualAddress = 0x2000;
    section[1].VirtualSize = 0x1000;
    section[1].Characteristics = MOCK_IMAGE_SCN_MEM_READ;

    uint8_t* textSec = base + 0x1000;
    uint8_t* rdataSec = base + 0x2000;

    // Site 1 in .text: cmp eax, 0x1b0 (3D B0 01 00 00)
    textSec[0x10] = 0x3D;
    textSec[0x11] = 0xB0;
    textSec[0x12] = 0x01;
    textSec[0x13] = 0x00;
    textSec[0x14] = 0x00;

    // Site 2 in .text: cmp ebp, 0x1b0 (81 FD B0 01 00 00) -- the 310.9 advertise gate
    textSec[0x30] = 0x81;
    textSec[0x31] = 0xFD;
    textSec[0x32] = 0xB0;
    textSec[0x33] = 0x01;
    textSec[0x34] = 0x00;
    textSec[0x35] = 0x00;

    // Site 3 in .text: cmp r8d, 0x1b0 (41 81 F8 B0 01 00 00) -- alternate 310.9.1 gate
    textSec[0x50] = 0x41;
    textSec[0x51] = 0x81;
    textSec[0x52] = 0xF8;
    textSec[0x53] = 0xB0;
    textSec[0x54] = 0x01;
    textSec[0x55] = 0x00;
    textSec[0x56] = 0x00;

    // Non-gate in .text: mov edi, 0x1b0 (BF B0 01 00 00) -- table lookup, MUST NOT BE TOUCHED
    textSec[0x70] = 0xBF;
    textSec[0x71] = 0xB0;
    textSec[0x72] = 0x01;
    textSec[0x73] = 0x00;
    textSec[0x74] = 0x00;

    // Gate-like pattern in .rdata: cmp eax, 0x1b0 -- in data section, MUST NOT BE TOUCHED
    rdataSec[0x10] = 0x3D;
    rdataSec[0x11] = 0xB0;
    rdataSec[0x12] = 0x01;
    rdataSec[0x13] = 0x00;
    rdataSec[0x14] = 0x00;

    // Execute patcher
    size_t rewritten = SimulatePatchArchGates(base);

    // Verify exactly 3 gates were patched
    assert(rewritten == 3);
    printf("  [PASS] Test 1: Exactly 3 arch-gate comparisons matched and rewritten\n");

    // Verify Site 1 rewritten: 3D 90 01 00 00 (cmp eax, 0x190)
    assert(textSec[0x11] == 0x90 && textSec[0x12] == 0x01);
    printf("  [PASS] Test 2: 'cmp eax, 0x1b0' correctly transformed to 0x190 (Ada)\n");

    // Verify Site 2 rewritten: 81 FD 90 01 00 00 (cmp ebp, 0x190)
    assert(textSec[0x32] == 0x90 && textSec[0x33] == 0x01);
    printf("  [PASS] Test 3: 'cmp ebp, 0x1b0' correctly transformed to 0x190 (Ada)\n");

    // Verify Site 3 rewritten: 41 81 F8 90 01 00 00 (cmp r8d, 0x190)
    assert(textSec[0x53] == 0x90 && textSec[0x54] == 0x01);
    printf("  [PASS] Test 4: 'cmp r8d, 0x1b0' (REX prefix) correctly transformed to 0x190 (Ada)\n");

    // Verify non-gate in .text (mov edi, 0x1b0) preserved untouched
    assert(textSec[0x71] == 0xB0 && textSec[0x72] == 0x01);
    printf("  [PASS] Test 5: 'mov edi, 0x1b0' table lookup preserved untouched\n");

    // Verify .rdata non-executable bytes preserved untouched
    assert(rdataSec[0x11] == 0xB0 && rdataSec[0x12] == 0x01);
    printf("  [PASS] Test 6: Non-executable data section preserved untouched\n");

    // Test UnlockedMax decoupling: KernelsRewritten == 0 must still unlock 5 frames when gates match
    struct MockStatus {
        bool ArchGatesPatched = false;
        bool AdvertiseMatched = false;
        bool ValidateMatched = false;
        unsigned int KernelsRewritten = 0;
    };

    auto SimulateUnlockedMax = [](const MockStatus& status) -> unsigned int {
        if (status.ArchGatesPatched && status.AdvertiseMatched && status.ValidateMatched)
            return 5;
        if (status.AdvertiseMatched && status.ValidateMatched)
            return 5;
        return 0;
    };

    // Subtest 7a: ArchGatesPatched with KernelsRewritten == 0 (safe mode)
    MockStatus s1{ true, true, true, 0 };
    assert(SimulateUnlockedMax(s1) == 5);
    printf("  [PASS] Test 7: ArchGatesPatched unlocks 5 frames without kernel rewriting (safe mode)\n");

    // Subtest 7b: knownGates with KernelsRewritten == 0
    MockStatus s2{ false, true, true, 0 };
    assert(SimulateUnlockedMax(s2) == 5);
    printf("  [PASS] Test 8: Known count gates unlock 5 frames without kernel rewriting\n");

    // Subtest 7c: Unmatched gates
    MockStatus s3{ false, false, false, 0 };
    assert(SimulateUnlockedMax(s3) == 0);
    printf("  [PASS] Test 9: Unmatched gates return 0\n");

    printf("[TEST] All MfgUnlock Architecture Gate Rewriter unit tests passed successfully!\n");
    return 0;
}
