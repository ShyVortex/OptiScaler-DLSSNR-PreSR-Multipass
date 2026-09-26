#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <string>
#include <vector>

// Standalone unit test for XeMfgLoader memory patching, transaction safety,
// signature fallbacks, and pacing verification.

namespace XeMfgTest
{
struct PatchDefinition
{
    const char* name;
    uint32_t rva;
    const uint8_t* pattern;
    size_t patternLen;
    const uint8_t* mask;
    const uint8_t* originalBytes;
    size_t patchLen;
    std::vector<uint8_t> replacementBytes;
};

// Original bytes
static const uint8_t U1_ORIG[] = { 0x0f, 0x85, 0xcc, 0x00, 0x00, 0x00 };
static const uint8_t U1_PATCH[] = { 0xe9, 0xcd, 0x00, 0x00, 0x00, 0x90 };

static const uint8_t U2_ORIG[] = { 0x74, 0x09 };
static const uint8_t U2_PATCH[] = { 0xeb, 0x06 };

static const uint8_t U3_ORIG[] = { 0xbb, 0x03, 0x00, 0x00, 0x00 };
static const uint8_t U4_ORIG[] = { 0xc7, 0x87, 0x6c, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00 };
static const uint8_t U5_ORIG[] = { 0xb8, 0x01, 0x00, 0x00, 0x00 };

static const uint8_t U1_SIG[] = { 0x0F, 0x85, 0xCC, 0x00, 0x00, 0x00, 0x83, 0xFB,
                                  0x01, 0x0F, 0x86, 0xC3, 0x00, 0x00, 0x00 };
static const uint8_t U2_SIG[] = { 0x74, 0x09, 0x80, 0x79, 0x64, 0x00, 0x74, 0x03, 0xB0, 0x01, 0xC3, 0x32, 0xC0, 0xC3 };
static const uint8_t U3_SIG[] = { 0xBB, 0x03, 0x00, 0x00, 0x00, 0xE8, 0x00, 0x00,
                                  0x00, 0x00, 0x84, 0xC0, 0x74, 0x0D, 0x48, 0x8B };
static const uint8_t U3_MASK[] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00,
                                   0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
static const uint8_t U4_SIG[] = { 0xC7, 0x87, 0x6C, 0x01, 0x00, 0x00, 0x01, 0x00,
                                  0x00, 0x00, 0xC6, 0x87, 0x68, 0x01, 0x00, 0x00 };
static const uint8_t U5_SIG[] = { 0xB8, 0x01, 0x00, 0x00, 0x00, 0x89, 0x47, 0x20, 0x33, 0xC0, 0x48, 0x8B, 0x9C, 0x24 };

static const uint8_t PACING_GATE_SIG[] = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74,
                                           0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x20 };
static const uint8_t PACING_SCHED_SIG[] = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0xE8 };
static const uint8_t PACING_BURST_SIG[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74,
                                            0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x30 };

struct PatchRecord
{
    uint8_t* address = nullptr;
    std::vector<uint8_t> originalBytes;
    std::vector<uint8_t> patchedBytes;
};

class SimulatedXeMfgEngine
{
  public:
    bool applied = false;
    bool lastFailure = false;
    uint32_t patchesApplied = 0;
    bool verifiedPacing = false;
    uint32_t maxGeneratedFrames = 3;
    std::vector<PatchRecord> appliedRecords;

    uint8_t* FindSignature(uint8_t* base, size_t size, const uint8_t* pattern, size_t patternLen,
                           const uint8_t* mask = nullptr)
    {
        if (!base || size < patternLen)
            return nullptr;

        for (size_t i = 0; i <= size - patternLen; ++i)
        {
            bool match = true;
            for (size_t j = 0; j < patternLen; ++j)
            {
                if (mask && mask[j] == 0x00)
                    continue;
                if (base[i + j] != pattern[j])
                {
                    match = false;
                    break;
                }
            }
            if (match)
                return base + i;
        }
        return nullptr;
    }

    bool Apply(uint8_t* baseAddress, size_t imageSize, uint32_t maxFrames, bool extraPacing)
    {
        applied = false;
        lastFailure = false;
        patchesApplied = 0;
        appliedRecords.clear();
        maxGeneratedFrames = std::clamp(maxFrames, 1u, 5u);

        if (!baseAddress || imageSize < 0x230000)
        {
            lastFailure = true;
            return false;
        }

        std::vector<uint8_t> u3Patch(U3_ORIG, U3_ORIG + sizeof(U3_ORIG));
        u3Patch[1] = static_cast<uint8_t>(maxGeneratedFrames);

        std::vector<uint8_t> u4Patch(U4_ORIG, U4_ORIG + sizeof(U4_ORIG));
        u4Patch[6] = static_cast<uint8_t>(maxGeneratedFrames);

        std::vector<uint8_t> u5Patch(U5_ORIG, U5_ORIG + sizeof(U5_ORIG));
        u5Patch[1] = static_cast<uint8_t>(maxGeneratedFrames);

        PatchDefinition patches[] = {
            { "U1/frame-count-fallback", 0x20da4f, U1_SIG, sizeof(U1_SIG), nullptr, U1_ORIG, sizeof(U1_ORIG),
              std::vector<uint8_t>(U1_PATCH, U1_PATCH + sizeof(U1_PATCH)) },
            { "U2/model-downgrade", 0x1a5de4, U2_SIG, sizeof(U2_SIG), nullptr, U2_ORIG, sizeof(U2_ORIG),
              std::vector<uint8_t>(U2_PATCH, U2_PATCH + sizeof(U2_PATCH)) },
            { "U3/default-ceiling", 0x1a517d, U3_SIG, sizeof(U3_SIG), U3_MASK, U3_ORIG, sizeof(U3_ORIG), u3Patch },
            { "U4/override-clamp", 0x1a45c2, U4_SIG, sizeof(U4_SIG), nullptr, U4_ORIG, sizeof(U4_ORIG), u4Patch },
            { "U5/reported-maximum", 0x20973b, U5_SIG, sizeof(U5_SIG), nullptr, U5_ORIG, sizeof(U5_ORIG), u5Patch }
        };

        for (const auto& p : patches)
        {
            uint8_t* target = nullptr;
            if (p.rva + p.patchLen <= imageSize && std::memcmp(baseAddress + p.rva, p.originalBytes, p.patchLen) == 0)
            {
                target = baseAddress + p.rva;
            }
            else
            {
                target = FindSignature(baseAddress, imageSize, p.pattern, p.patternLen, p.mask);
            }

            if (!target)
            {
                Rollback();
                lastFailure = true;
                return false;
            }

            PatchRecord rec;
            rec.address = target;
            rec.originalBytes.assign(target, target + p.patchLen);
            rec.patchedBytes = p.replacementBytes;

            std::memcpy(target, p.replacementBytes.data(), p.patchLen);
            appliedRecords.push_back(rec);
            patchesApplied++;
        }

        if (extraPacing)
        {
            bool g1 = (FindSignature(baseAddress, imageSize, PACING_GATE_SIG, sizeof(PACING_GATE_SIG)) != nullptr);
            bool g2 = (FindSignature(baseAddress, imageSize, PACING_SCHED_SIG, sizeof(PACING_SCHED_SIG)) != nullptr);
            bool g3 = (FindSignature(baseAddress, imageSize, PACING_BURST_SIG, sizeof(PACING_BURST_SIG)) != nullptr);
            verifiedPacing = (g1 && g2 && g3);
        }

        applied = true;
        return true;
    }

    void Rollback()
    {
        for (auto it = appliedRecords.rbegin(); it != appliedRecords.rend(); ++it)
        {
            if (it->address && !it->originalBytes.empty())
            {
                std::memcpy(it->address, it->originalBytes.data(), it->originalBytes.size());
            }
        }
        appliedRecords.clear();
        patchesApplied = 0;
        applied = false;
    }

    uint32_t EffectiveMax(uint32_t nativeReported) const
    {
        if (lastFailure)
            return 1;
        if (!applied)
            return nativeReported;
        return std::max(maxGeneratedFrames, nativeReported);
    }
};

static void PopulateValidImage(std::vector<uint8_t>& image)
{
    image.assign(0x235000, 0x90); // NOP sled

    // U1 at 0x20da4f
    std::memcpy(image.data() + 0x20da4f, U1_SIG, sizeof(U1_SIG));
    // U2 at 0x1a5de4
    std::memcpy(image.data() + 0x1a5de4, U2_SIG, sizeof(U2_SIG));
    // U3 at 0x1a517d
    std::memcpy(image.data() + 0x1a517d, U3_SIG, sizeof(U3_SIG));
    // U4 at 0x1a45c2
    std::memcpy(image.data() + 0x1a45c2, U4_SIG, sizeof(U4_SIG));
    // U5 at 0x20973b
    std::memcpy(image.data() + 0x20973b, U5_SIG, sizeof(U5_SIG));

    // Pacing anchors at 0x224cf0, 0x21ee30, 0x224b30
    std::memcpy(image.data() + 0x224cf0, PACING_GATE_SIG, sizeof(PACING_GATE_SIG));
    std::memcpy(image.data() + 0x21ee30, PACING_SCHED_SIG, sizeof(PACING_SCHED_SIG));
    std::memcpy(image.data() + 0x224b30, PACING_BURST_SIG, sizeof(PACING_BURST_SIG));
}
} // namespace XeMfgTest

int main()
{
    printf("[+] Starting XeMfgLoader Unit Tests...\n");

    std::vector<uint8_t> image;
    XeMfgTest::PopulateValidImage(image);
    std::vector<uint8_t> pristineImage = image;

    XeMfgTest::SimulatedXeMfgEngine engine;

    // Test 1: Standard Application on Valid Image
    {
        bool ok = engine.Apply(image.data(), image.size(), 3, true);
        assert(ok && "XeMfgEngine::Apply must succeed on valid image");
        assert(engine.applied && "Engine must be marked applied");
        assert(engine.patchesApplied == 5 && "Exactly 5 patches must be applied");
        assert(engine.verifiedPacing && "Extra pacing anchors must be verified");
        assert(!engine.lastFailure && "Last failure must be false");

        // Verify U1
        assert(std::memcmp(image.data() + 0x20da4f, XeMfgTest::U1_PATCH, sizeof(XeMfgTest::U1_PATCH)) == 0);
        // Verify U2
        assert(std::memcmp(image.data() + 0x1a5de4, XeMfgTest::U2_PATCH, sizeof(XeMfgTest::U2_PATCH)) == 0);
        // Verify U3 MaxFrames = 3
        assert(image[0x1a517d + 1] == 3);
        // Verify U4 MaxFrames = 3
        assert(image[0x1a45c2 + 6] == 3);
        // Verify U5 MaxFrames = 3
        assert(image[0x20973b + 1] == 3);

        // Check EffectiveMax
        assert(engine.EffectiveMax(1) == 3 && "EffectiveMax must report 3 (4X MFG)");
        printf("  [PASS] Test 1: Fast-path RVA patch application and pacing verification.\n");
    }

    // Test 2: Clean Rollback
    {
        engine.Rollback();
        assert(!engine.applied && "Engine must report not applied after rollback");
        assert(engine.patchesApplied == 0 && "Patches applied must be 0 after rollback");
        assert(image == pristineImage && "Image after rollback must match pristine binary byte-for-byte");
        assert(engine.EffectiveMax(1) == 1 && "EffectiveMax must revert to native reported count");
        printf("  [PASS] Test 2: Full rollback restores byte-for-byte image identity.\n");
    }

    // Test 3: Relocated / Shifted RVAs with Signature Scanning Fallback
    {
        std::vector<uint8_t> shiftedImage(0x235000, 0x90);
        // Shift every patch forward by 0x100
        std::memcpy(shiftedImage.data() + 0x20da4f + 0x100, XeMfgTest::U1_SIG, sizeof(XeMfgTest::U1_SIG));
        std::memcpy(shiftedImage.data() + 0x1a5de4 + 0x100, XeMfgTest::U2_SIG, sizeof(XeMfgTest::U2_SIG));
        std::memcpy(shiftedImage.data() + 0x1a517d + 0x100, XeMfgTest::U3_SIG, sizeof(XeMfgTest::U3_SIG));
        std::memcpy(shiftedImage.data() + 0x1a45c2 + 0x100, XeMfgTest::U4_SIG, sizeof(XeMfgTest::U4_SIG));
        std::memcpy(shiftedImage.data() + 0x20973b + 0x100, XeMfgTest::U5_SIG, sizeof(XeMfgTest::U5_SIG));
        std::memcpy(shiftedImage.data() + 0x224cf0 + 0x100, XeMfgTest::PACING_GATE_SIG,
                    sizeof(XeMfgTest::PACING_GATE_SIG));
        std::memcpy(shiftedImage.data() + 0x21ee30 + 0x100, XeMfgTest::PACING_SCHED_SIG,
                    sizeof(XeMfgTest::PACING_SCHED_SIG));
        std::memcpy(shiftedImage.data() + 0x224b30 + 0x100, XeMfgTest::PACING_BURST_SIG,
                    sizeof(XeMfgTest::PACING_BURST_SIG));

        std::vector<uint8_t> shiftedPristine = shiftedImage;

        bool ok = engine.Apply(shiftedImage.data(), shiftedImage.size(), 5, true);
        assert(ok && "Apply must succeed via signature scanning fallback");
        assert(engine.applied && "Engine must be applied");
        assert(engine.patchesApplied == 5 && "All 5 patches must be applied via signatures");
        assert(engine.verifiedPacing && "Pacing anchors must be located via signatures");

        // Verify U3, U4, U5 with MaxFrames = 5 (6X FG)
        assert(shiftedImage[0x1a517d + 0x100 + 1] == 5);
        assert(shiftedImage[0x1a45c2 + 0x100 + 6] == 5);
        assert(shiftedImage[0x20973b + 0x100 + 1] == 5);
        assert(engine.EffectiveMax(1) == 5 && "EffectiveMax must report 5 (6X MFG)");

        engine.Rollback();
        assert(shiftedImage == shiftedPristine && "Rollback on shifted image must match pristine state");
        printf("  [PASS] Test 3: Relocated signature scanning fallback and 6X multiplier.\n");
    }

    // Test 4: Transactional Failure & Atomic Rollback
    {
        std::vector<uint8_t> corruptImage;
        XeMfgTest::PopulateValidImage(corruptImage);
        // Corrupt U4 so that signature and RVA both fail
        std::memset(corruptImage.data() + 0x1a45c2, 0xCC, 64);
        std::vector<uint8_t> corruptPristine = corruptImage;

        bool ok = engine.Apply(corruptImage.data(), corruptImage.size(), 4, true);
        assert(!ok && "Apply must fail when a patch target cannot be resolved");
        assert(!engine.applied && "Engine must not be marked applied");
        assert(engine.lastFailure && "lastFailure must be set to true");
        assert(engine.patchesApplied == 0 && "Zero patches must remain applied after atomic rollback");
        assert(corruptImage == corruptPristine && "Image must be atomically rolled back to pre-attempt state");
        assert(engine.EffectiveMax(1) == 1 && "EffectiveMax must be safely clamped to 1 on failure");
        printf("  [PASS] Test 4: Atomic rollback on patch failure prevents half-patched state.\n");
    }

    // Test 5: Multiplier Range Clamping (1 to 5)
    {
        std::vector<uint8_t> testImg;
        XeMfgTest::PopulateValidImage(testImg);

        // Clamp below 1 -> 1
        engine.Apply(testImg.data(), testImg.size(), 0, false);
        assert(engine.maxGeneratedFrames == 1);
        assert(engine.EffectiveMax(1) == 1);
        engine.Rollback();

        // Clamp above 5 -> 5
        engine.Apply(testImg.data(), testImg.size(), 99, false);
        assert(engine.maxGeneratedFrames == 5);
        assert(engine.EffectiveMax(1) == 5);
        engine.Rollback();
        printf("  [PASS] Test 5: Multiplier range clamping (1 to 5).\n");
    }

    printf("[+] All XeMfgLoader unit tests PASSED successfully!\n");
    return 0;
}
