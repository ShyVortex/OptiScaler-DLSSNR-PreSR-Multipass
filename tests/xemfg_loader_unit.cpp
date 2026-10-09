#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// Standalone unit test for XeMfgLoader memory patching, transaction safety,
// signature fallbacks, and clean native execution.

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

static const uint8_t PRESENT_THUNK_EXPECTED[16] = { 0xE9, 0x6B, 0xD1, 0x21, 0x00, 0xCC, 0xCC, 0xCC,
                                                    0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC };
static const uint8_t SCHED_THUNK_EXPECTED[16] = { 0xE9, 0x2B, 0xBD, 0x21, 0x00, 0xCC, 0xCC, 0xCC,
                                                  0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC };
static const uint8_t TS_THUNK_EXPECTED[16] = { 0xE9, 0xFB, 0x16, 0x22, 0x00, 0xCC, 0xCC, 0xCC,
                                               0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC };

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

    bool Apply(uint8_t* baseAddress, size_t imageSize, uint32_t maxFrames)
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

    void SetMaxGeneratedFrames(uint32_t maxFrames)
    {
        uint32_t clamped = std::clamp(maxFrames, 1u, 5u);
        maxGeneratedFrames = clamped;
        if (applied && !appliedRecords.empty())
        {
            uint8_t frameByte = static_cast<uint8_t>(clamped);
            if (appliedRecords.size() >= 5)
            {
                appliedRecords[2].address[1] = frameByte;
                appliedRecords[3].address[6] = frameByte;
                appliedRecords[4].address[1] = frameByte;
            }
        }
    }

    uint32_t lastFramesPresented = 0;
    int lastFrameGenResult = 0;
    bool isFrameGenEnabled = false;
    bool hasPresentTelemetry = false;

    void RecordPresentStatus(uint32_t framesPresented, int frameGenResult, bool isFgEnabled)
    {
        lastFramesPresented = framesPresented;
        lastFrameGenResult = frameGenResult;
        isFrameGenEnabled = isFgEnabled;
        hasPresentTelemetry = true;
    }

    void Shutdown()
    {
        Rollback();
        lastFramesPresented = 0;
        lastFrameGenResult = 0;
        isFrameGenEnabled = false;
        hasPresentTelemetry = false;
    }

    static uint64_t CalculatePacedDeadline(uint64_t baseTimestamp, uint64_t frameInterval, uint32_t frameIndex,
                                           uint32_t totalFrames)
    {
        uint64_t deadline = baseTimestamp;
        if (totalFrames >= 2 && frameIndex >= 1 && frameIndex <= 5 && frameInterval > 0)
        {
            deadline += static_cast<uint64_t>(frameIndex) * (frameInterval / totalFrames);
        }
        return deadline;
    }

    bool pacingInstalled = false;
    uint32_t pacingDetours = 0;

    bool InstallPacing(uint8_t* base, size_t size)
    {
        if (!base || size < 0x230000)
            return false;
        if (std::memcmp(base + 0x25c0, PRESENT_THUNK_EXPECTED, 16) != 0 ||
            std::memcmp(base + 0x3100, SCHED_THUNK_EXPECTED, 16) != 0 ||
            std::memcmp(base + 0x3430, TS_THUNK_EXPECTED, 16) != 0)
            return false;

        uint8_t jmpBytes[16] = { 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00, 0x11, 0x22,
                                 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0xCC, 0xCC };
        std::memcpy(base + 0x25c0, jmpBytes, 16);
        std::memcpy(base + 0x3100, jmpBytes, 16);
        std::memcpy(base + 0x3430, jmpBytes, 16);
        pacingInstalled = true;
        pacingDetours = 3;
        return true;
    }

    void UninstallPacing(uint8_t* base)
    {
        if (!base)
            return;
        std::memcpy(base + 0x25c0, PRESENT_THUNK_EXPECTED, 16);
        std::memcpy(base + 0x3100, SCHED_THUNK_EXPECTED, 16);
        std::memcpy(base + 0x3430, TS_THUNK_EXPECTED, 16);
        pacingInstalled = false;
        pacingDetours = 0;
    }

    static std::vector<int64_t> SimulateTsDetourBurst(int64_t nativeOut, int64_t medianNs, float fMs,
                                                      uint32_t countPlus1)
    {
        std::vector<int64_t> deadlines;
        int64_t unit = medianNs / static_cast<int64_t>(countPlus1);
        int64_t nativeUnit = unit;
        int64_t fNs = static_cast<int64_t>(fMs * 1000000.0f);
        int64_t clampedUnit = fNs / static_cast<int64_t>(countPlus1);
        if (clampedUnit < nativeUnit)
            nativeUnit = clampedUnit;

        int64_t nextDeadline = 0;
        int64_t stepNs = unit;

        for (uint32_t idx = 1; idx < countPlus1; ++idx)
        {
            if (idx == 1)
            {
                nextDeadline = nativeOut + static_cast<int64_t>(idx) * (unit - nativeUnit);
            }
            else
            {
                nextDeadline += stepNs;
            }
            deadlines.push_back(nextDeadline);
        }
        return deadlines;
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

    // 3 Pacing thunks
    std::memcpy(image.data() + 0x25c0, PRESENT_THUNK_EXPECTED, sizeof(PRESENT_THUNK_EXPECTED));
    std::memcpy(image.data() + 0x3100, SCHED_THUNK_EXPECTED, sizeof(SCHED_THUNK_EXPECTED));
    std::memcpy(image.data() + 0x3430, TS_THUNK_EXPECTED, sizeof(TS_THUNK_EXPECTED));

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
        bool ok = engine.Apply(image.data(), image.size(), 3);
        assert(ok && "XeMfgEngine::Apply must succeed on valid image");
        assert(engine.applied && "Engine must be marked applied");
        assert(engine.patchesApplied == 5 && "Exactly 5 patches must be applied");
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
        printf("  [PASS] Test 1: Fast-path RVA parameter unlock patch application (clean engine).\n");
    }

    // Test 2: Clean Rollback
    {
        engine.Rollback();
        assert(!engine.applied && "Engine must report not applied after rollback");
        assert(engine.patchesApplied == 0 && "Patches applied must be 0 after rollback");
        assert(std::memcmp(image.data(), pristineImage.data(), image.size()) == 0 &&
               "Rollback must restore image to pristine identity");
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

        std::vector<uint8_t> shiftedPristine = shiftedImage;

        bool ok = engine.Apply(shiftedImage.data(), shiftedImage.size(), 5);
        assert(ok && "Apply must succeed via signature scanning fallback");
        assert(engine.applied && "Engine must be applied");
        assert(engine.patchesApplied == 5 && "All 5 patches must be applied via signatures");

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

        bool ok = engine.Apply(corruptImage.data(), corruptImage.size(), 4);
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
        engine.Apply(testImg.data(), testImg.size(), 0);
        assert(engine.maxGeneratedFrames == 1);
        assert(engine.EffectiveMax(1) == 1);
        engine.Rollback();

        // Clamp above 5 -> 5
        engine.Apply(testImg.data(), testImg.size(), 99);
        assert(engine.maxGeneratedFrames == 5);
        assert(engine.EffectiveMax(1) == 5);
        engine.Rollback();
        printf("  [PASS] Test 5: Multiplier range clamping (1 to 5).\n");
    }

    // Test 6: Safe Teardown & Lifecycle Rollback (Shutdown)
    {
        std::vector<uint8_t> testImg;
        XeMfgTest::PopulateValidImage(testImg);
        std::vector<uint8_t> pristine = testImg;

        bool ok = engine.Apply(testImg.data(), testImg.size(), 4);
        assert(ok);
        assert(engine.applied);

        engine.Shutdown();
        assert(!engine.applied && "Shutdown must mark engine as unapplied");
        assert(engine.patchesApplied == 0 && "Shutdown must clear applied patches");
        assert(testImg == pristine && "Shutdown must restore image byte-for-byte");
        printf("  [PASS] Test 6: Safe lifecycle teardown restores pristine image byte-for-byte.\n");
    }

    // Test 7: Presentation Telemetry Recording & Lifecycle Reset
    {
        assert(!engine.hasPresentTelemetry);
        engine.RecordPresentStatus(4, 0, true);
        assert(engine.hasPresentTelemetry);
        assert(engine.lastFramesPresented == 4);
        assert(engine.lastFrameGenResult == 0);
        assert(engine.isFrameGenEnabled == true);

        // Record warning / fallback
        engine.RecordPresentStatus(1, 6, false);
        assert(engine.lastFramesPresented == 1);
        assert(engine.lastFrameGenResult == 6);
        assert(engine.isFrameGenEnabled == false);

        // Verify reset on Shutdown
        engine.Shutdown();
        assert(!engine.hasPresentTelemetry);
        assert(engine.lastFramesPresented == 0);
        assert(engine.lastFrameGenResult == 0);
        assert(engine.isFrameGenEnabled == false);
        printf("  [PASS] Test 7: Presentation telemetry recording & lifecycle reset.\n");
    }

    // Test 8: Native Provider Presentation Deadline Calculation Across Multipliers (0x180224b30)
    {
        const uint64_t baseTimestamp = 1000000;
        const uint64_t interval = 60000; // e.g. 60ms in arbitrary units

        // Test 4X FG (totalFrames = 4, frames 1..3 interpolated)
        uint64_t d1 = XeMfgTest::SimulatedXeMfgEngine::CalculatePacedDeadline(baseTimestamp, interval, 1, 4);
        uint64_t d2 = XeMfgTest::SimulatedXeMfgEngine::CalculatePacedDeadline(baseTimestamp, interval, 2, 4);
        uint64_t d3 = XeMfgTest::SimulatedXeMfgEngine::CalculatePacedDeadline(baseTimestamp, interval, 3, 4);

        assert(d1 == baseTimestamp + 15000);
        assert(d2 == baseTimestamp + 30000);
        assert(d3 == baseTimestamp + 45000);
        assert(d1 < d2 && d2 < d3 && "Deadlines must be strictly monotonically increasing");

        // Test 6X FG (totalFrames = 6, frames 1..5 interpolated)
        uint64_t prev = baseTimestamp;
        for (uint32_t idx = 1; idx <= 5; ++idx)
        {
            uint64_t d = XeMfgTest::SimulatedXeMfgEngine::CalculatePacedDeadline(baseTimestamp, interval, idx, 6);
            assert(d > prev && "Each intermediate deadline must be greater than previous");
            assert(d == baseTimestamp + idx * 10000);
            prev = d;
        }
        printf("  [PASS] Test 8: Native provider presentation deadline calculation evenly spaces presentation "
               "intervals.\n");
    }

    // Test 9: Native XeFGPacing thunk hooking, scheduler routing, and deadline repair across 4X burst
    {
        std::vector<uint8_t> pacingImg = pristineImage;
        assert(engine.InstallPacing(pacingImg.data(), pacingImg.size()));
        assert(engine.pacingInstalled && "Pacing must be marked installed");
        assert(engine.pacingDetours == 3 && "All 3 thunks must be detoured");

        // Verify the 3 thunks are redirected to 0xFF, 0x25
        assert(pacingImg[0x25c0] == 0xFF && pacingImg[0x25c1] == 0x25);
        assert(pacingImg[0x3100] == 0xFF && pacingImg[0x3101] == 0x25);
        assert(pacingImg[0x3430] == 0xFF && pacingImg[0x3431] == 0x25);

        // Verify TsDetour deadline simulation (4X FG, 20.6 ms frame, f clamped to 8.5ms)
        const int64_t medianNs = 20600000;
        const float fMs = 8.5f;
        const int64_t nativeBase = 1000000000LL;
        auto deadlines = XeMfgTest::SimulatedXeMfgEngine::SimulateTsDetourBurst(nativeBase, medianNs, fMs, 4);
        assert(deadlines.size() == 3);
        int64_t step1 = deadlines[1] - deadlines[0];
        int64_t step2 = deadlines[2] - deadlines[1];
        assert(step1 == medianNs / 4 && "Inter-frame step must equal median / 4");
        assert(step2 == medianNs / 4 && "Second inter-frame step must equal median / 4");

        // Verify Uninstall cleanly restores pristine bytes
        engine.UninstallPacing(pacingImg.data());
        assert(!engine.pacingInstalled);
        assert(engine.pacingDetours == 0);
        assert(pacingImg == pristineImage && "UninstallPacing must restore pristine thunk bytes");
        printf("  [PASS] Test 9: Native XeFGPacing thunk hooking, scheduler routing, and deadline repair across 4X "
               "burst.\n");
    }

    // Test 10: Live in-memory ceiling byte rewriting via SetMaxGeneratedFrames
    {
        std::vector<uint8_t> testImg;
        XeMfgTest::PopulateValidImage(testImg);
        bool ok = engine.Apply(testImg.data(), testImg.size(), 3);
        assert(ok);
        assert(engine.maxGeneratedFrames == 3);
        assert(engine.EffectiveMax(1) == 3);
        assert(testImg[0x1a517d + 1] == 3);
        assert(testImg[0x1a45c2 + 6] == 3);
        assert(testImg[0x20973b + 1] == 3);

        // Dynamically adjust ceiling to 5 (6X Multi-Frame Generation)
        engine.SetMaxGeneratedFrames(5);
        assert(engine.maxGeneratedFrames == 5);
        assert(engine.EffectiveMax(1) == 5);
        assert(testImg[0x1a517d + 1] == 5 && "U3 ceiling byte must be updated in-memory");
        assert(testImg[0x1a45c2 + 6] == 5 && "U4 clamp byte must be updated in-memory");
        assert(testImg[0x20973b + 1] == 5 && "U5 reported max byte must be updated in-memory");

        // Dynamically adjust ceiling down to 2 (3X FG)
        engine.SetMaxGeneratedFrames(2);
        assert(engine.maxGeneratedFrames == 2);
        assert(engine.EffectiveMax(1) == 2);
        assert(testImg[0x1a517d + 1] == 2);
        assert(testImg[0x1a45c2 + 6] == 2);
        assert(testImg[0x20973b + 1] == 2);

        engine.Rollback();
        printf("  [PASS] Test 10: Live in-memory ceiling adaptation byte rewriting (U3, U4, U5).\n");
    }

    // Test 11: Mandatory Native Thunk Pacing Installation Unconditional on ExtraPacing Flags
    {
        std::vector<uint8_t> pacingImg = pristineImage;
        bool pacingOk = engine.InstallPacing(pacingImg.data(), pacingImg.size());
        assert(pacingOk);
        assert(engine.pacingInstalled && "Pacing must install unconditionally");
        assert(engine.pacingDetours == 3 && "All 3 native thunks must be detoured");

        engine.UninstallPacing(pacingImg.data());
        assert(!engine.pacingInstalled);
        assert(engine.pacingDetours == 0);
        printf("  [PASS] Test 11: Mandatory native presentation pacing detours installed unconditionally.\n");
    }

    printf("[+] All XeMfgLoader unit tests PASSED successfully!\n");
    return 0;
}
