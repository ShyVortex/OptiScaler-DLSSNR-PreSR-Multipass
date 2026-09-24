#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

// Logical reproduction of KCD2 HDR10 quirk structures & patching logic
constexpr uint32_t kTimestamp = 0x6a350e20;
constexpr uint32_t kImageSize = 0x5b2d000;
constexpr uint32_t kCallbackRva = 0x1dec4c0;
constexpr size_t kSelectionOffset = 0x80;

constexpr unsigned char kCallback[] = {
    0x48, 0x89, 0x5c, 0x24, 0x18, 0x57, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0xf9, 0x48, 0x8d, 0x15, 0x34, 0x87,
    0xfe, 0x01, 0x48, 0x8b, 0x0d, 0xcd, 0x13, 0xb4, 0x02, 0x48, 0x8b, 0x01, 0xff, 0x90, 0xb8, 0x00, 0x00, 0x00,
    0x48, 0x8b, 0xd8, 0x48, 0x85, 0xc0, 0x74, 0x78, 0x48, 0x8b, 0x17, 0x48, 0x8b, 0xcf, 0x48, 0x89, 0x6c, 0x24,
    0x30, 0x48, 0x89, 0x74, 0x24, 0x38, 0xff, 0x52, 0x10, 0x48, 0x8d, 0x0d, 0x7a, 0xe3, 0x4c, 0x03, 0x40, 0x32,
    0xf6, 0x8b, 0xe8, 0xe8, 0xa0, 0xc6, 0x6f, 0xfe, 0x48, 0x85, 0xc0, 0x74, 0x1c, 0x48, 0x8b, 0xc8, 0xe8, 0xbf,
    0x4d, 0xa7, 0xfe, 0x48, 0x85, 0xc0, 0x74, 0x0f, 0x48, 0x8b, 0x08, 0x48, 0x8b, 0x51, 0x40, 0x48, 0x8b, 0xc8,
    0xff, 0xd2, 0x0f, 0xb6, 0xf0, 0x85, 0xed, 0x48, 0x8b, 0x6c, 0x24, 0x30, 0x74, 0x0c, 0x40, 0x84, 0xf6, 0x74,
    0x07, 0xba, 0x01, 0x00, 0x00, 0x00, 0xeb, 0x10, 0x48, 0x8b, 0x07, 0x33, 0xd2, 0x48, 0x8b, 0xcf, 0xff, 0x50,
    0x38, 0xba, 0xff, 0xff, 0xff, 0xff, 0x48, 0x8b, 0x03, 0x48, 0x8b, 0xcb, 0xff, 0x50, 0x38, 0x48, 0x8b, 0x74,
    0x24, 0x38, 0x48, 0x8b, 0x5c, 0x24, 0x40, 0x48, 0x83, 0xc4, 0x20, 0x5f, 0xc3,
};

static_assert(kCallback[kSelectionOffset - 1] == 0xba && kCallback[kSelectionOffset] == 1);

#pragma pack(push, 2)
struct MockDosHeader
{
    uint16_t e_magic = 0x5A4D; // "MZ"
    uint16_t pad[29] = {};
    uint32_t e_lfanew = 0x80;
};
#pragma pack(pop)

struct MockFileHeader
{
    uint16_t Machine = 0x8664; // AMD64
    uint16_t NumberOfSections = 1;
    uint32_t TimeDateStamp = kTimestamp;
    uint32_t PointerToSymbolTable = 0;
    uint32_t NumberOfSymbols = 0;
    uint16_t SizeOfOptionalHeader = 0xF0;
    uint16_t Characteristics = 0x0022;
};

struct MockOptionalHeader64
{
    uint16_t Magic = 0x020B; // PE32+
    uint8_t MajorLinkerVersion = 0;
    uint8_t MinorLinkerVersion = 0;
    uint32_t SizeOfCode = 0;
    uint32_t SizeOfInitializedData = 0;
    uint32_t SizeOfUninitializedData = 0;
    uint32_t AddressOfEntryPoint = 0;
    uint32_t BaseOfCode = 0;
    uint64_t ImageBase = 0x140000000;
    uint32_t SectionAlignment = 0x1000;
    uint32_t FileAlignment = 0x200;
    uint16_t MajorOperatingSystemVersion = 6;
    uint16_t MinorOperatingSystemVersion = 0;
    uint16_t MajorImageVersion = 0;
    uint16_t MinorImageVersion = 0;
    uint16_t MajorSubsystemVersion = 6;
    uint16_t MinorSubsystemVersion = 0;
    uint32_t Win32VersionValue = 0;
    uint32_t SizeOfImage = kImageSize;
    uint32_t SizeOfHeaders = 0x400;
};

struct MockNtHeaders64
{
    uint32_t Signature = 0x00004550; // "PE\0\0"
    MockFileHeader FileHeader;
    MockOptionalHeader64 OptionalHeader;
};

enum class FGOutput : int
{
    NoFG = 0,
    FSRFG = 1,
    XeFG = 2,
    DLSSG = 3
};

enum class FGNvngxReplacement : int
{
    None = 0,
    Nukem = 1,
    Other = 2
};

bool MockPatchCallback(unsigned char* image, size_t imageSize)
{
    if (imageSize < sizeof(MockDosHeader) + sizeof(MockNtHeaders64) + kCallbackRva + sizeof(kCallback))
        return false;

    auto* dos = reinterpret_cast<const MockDosHeader*>(image);
    if (dos->e_magic != 0x5A4D || dos->e_lfanew < sizeof(MockDosHeader) || dos->e_lfanew > 0x1000)
        return false;

    auto* nt = reinterpret_cast<const MockNtHeaders64*>(image + dos->e_lfanew);
    if (nt->Signature != 0x00004550 || nt->FileHeader.Machine != 0x8664 || nt->OptionalHeader.Magic != 0x020B ||
        nt->FileHeader.TimeDateStamp != kTimestamp || nt->OptionalHeader.SizeOfImage != kImageSize)
        return false;

    auto* callback = image + kCallbackRva;
    if (std::memcmp(callback, kCallback, sizeof(kCallback)) != 0)
        return false; // Unknown build or another mod modified this callback

    auto* selection = callback + kSelectionOffset;
    *selection = 2; // Patch from 1 (scRGB) to 2 (HDR10)
    return true;
}

bool ShouldApplyKcd2Quirk(bool kcd2QuirkEnabled, FGOutput activeFgOutput, FGNvngxReplacement activeFgNvngx)
{
    if (!kcd2QuirkEnabled || activeFgOutput != FGOutput::DLSSG || activeFgNvngx != FGNvngxReplacement::None)
        return false;
    return true;
}

int main()
{
    // Test 1: Callback signature integrity
    {
        assert(kCallback[kSelectionOffset - 1] == 0xba); // mov edx, immediate
        assert(kCallback[kSelectionOffset] == 1);        // immediate = 1 (scRGB)
    }

    // Test 2: Successful patch of matching WHGame.dll image
    {
        std::vector<unsigned char> mockImage(kCallbackRva + sizeof(kCallback) + 0x1000, 0);

        MockDosHeader* dos = reinterpret_cast<MockDosHeader*>(mockImage.data());
        dos->e_magic = 0x5A4D;
        dos->e_lfanew = 0x80;

        MockNtHeaders64* nt = reinterpret_cast<MockNtHeaders64*>(mockImage.data() + dos->e_lfanew);
        nt->Signature = 0x00004550;
        nt->FileHeader.Machine = 0x8664;
        nt->FileHeader.TimeDateStamp = kTimestamp;
        nt->OptionalHeader.Magic = 0x020B;
        nt->OptionalHeader.SizeOfImage = kImageSize;

        std::memcpy(mockImage.data() + kCallbackRva, kCallback, sizeof(kCallback));

        bool patched = MockPatchCallback(mockImage.data(), mockImage.size());
        assert(patched == true);

        // Verify only selection byte was modified from 1 to 2
        unsigned char* patchedCallback = mockImage.data() + kCallbackRva;
        for (size_t i = 0; i < sizeof(kCallback); ++i)
        {
            if (i == kSelectionOffset)
                assert(patchedCallback[i] == 2);
            else
                assert(patchedCallback[i] == kCallback[i]);
        }
    }

    // Test 3: Rejection of mismatching timestamp or image size
    {
        std::vector<unsigned char> mockImage(kCallbackRva + sizeof(kCallback) + 0x1000, 0);

        MockDosHeader* dos = reinterpret_cast<MockDosHeader*>(mockImage.data());
        dos->e_magic = 0x5A4D;
        dos->e_lfanew = 0x80;

        MockNtHeaders64* nt = reinterpret_cast<MockNtHeaders64*>(mockImage.data() + dos->e_lfanew);
        nt->Signature = 0x00004550;
        nt->FileHeader.Machine = 0x8664;
        nt->FileHeader.TimeDateStamp = 0x12345678; // Incorrect timestamp
        nt->OptionalHeader.Magic = 0x020B;
        nt->OptionalHeader.SizeOfImage = kImageSize;

        std::memcpy(mockImage.data() + kCallbackRva, kCallback, sizeof(kCallback));
        assert(MockPatchCallback(mockImage.data(), mockImage.size()) == false);

        // Restore timestamp, corrupt image size
        nt->FileHeader.TimeDateStamp = kTimestamp;
        nt->OptionalHeader.SizeOfImage = 0x1000;
        assert(MockPatchCallback(mockImage.data(), mockImage.size()) == false);
    }

    // Test 4: Rejection of modified or unknown callback signature
    {
        std::vector<unsigned char> mockImage(kCallbackRva + sizeof(kCallback) + 0x1000, 0);

        MockDosHeader* dos = reinterpret_cast<MockDosHeader*>(mockImage.data());
        dos->e_magic = 0x5A4D;
        dos->e_lfanew = 0x80;

        MockNtHeaders64* nt = reinterpret_cast<MockNtHeaders64*>(mockImage.data() + dos->e_lfanew);
        nt->Signature = 0x00004550;
        nt->FileHeader.Machine = 0x8664;
        nt->FileHeader.TimeDateStamp = kTimestamp;
        nt->OptionalHeader.Magic = 0x020B;
        nt->OptionalHeader.SizeOfImage = kImageSize;

        std::memcpy(mockImage.data() + kCallbackRva, kCallback, sizeof(kCallback));
        // Corrupt one byte of callback
        mockImage[kCallbackRva + 5] ^= 0xFF;

        assert(MockPatchCallback(mockImage.data(), mockImage.size()) == false);
    }

    // Test 5: Quirk gating verification
    {
        // Quirk disabled -> do not apply
        assert(ShouldApplyKcd2Quirk(false, FGOutput::DLSSG, FGNvngxReplacement::None) == false);

        // Non-DLSSG output -> do not apply
        assert(ShouldApplyKcd2Quirk(true, FGOutput::NoFG, FGNvngxReplacement::None) == false);
        assert(ShouldApplyKcd2Quirk(true, FGOutput::FSRFG, FGNvngxReplacement::None) == false);
        assert(ShouldApplyKcd2Quirk(true, FGOutput::XeFG, FGNvngxReplacement::None) == false);

        // Replacement FG active (e.g. Nukem) -> do not apply
        assert(ShouldApplyKcd2Quirk(true, FGOutput::DLSSG, FGNvngxReplacement::Nukem) == false);
        assert(ShouldApplyKcd2Quirk(true, FGOutput::DLSSG, FGNvngxReplacement::Other) == false);

        // Native DLSSG with KCD2 quirk enabled -> apply!
        assert(ShouldApplyKcd2Quirk(true, FGOutput::DLSSG, FGNvngxReplacement::None) == true);
    }

    std::puts("PASS: kcd2_hdr_unit (signature check, 1-byte patch logic, and quirk activation guards)");
    return 0;
}
