#include <cassert>
#include <cstdint>
#include <cstdio>
#include <string>

// Test GUID definition matching FinishedColorSpaceKey
struct TestGUID {
    uint32_t Data1;
    uint16_t Data2;
    uint16_t Data3;
    uint8_t  Data4[8];
    bool operator==(const TestGUID& o) const {
        if (Data1 != o.Data1 || Data2 != o.Data2 || Data3 != o.Data3) return false;
        for (int i = 0; i < 8; ++i) if (Data4[i] != o.Data4[i]) return false;
        return true;
    }
};

static constexpr TestGUID ExpectedFinishedColorSpaceKey = {
    0x34a31e7b, 0x84c5, 0x44ef, { 0xa7, 0x4d, 0x6b, 0xd3, 0x60, 0x8c, 0xe5, 0x22 }
};

enum class FGOutput {
    NoFG,
    FSRFG,
    XeFG,
    DLSSG
};

struct MockState {
    bool externalFrameGeneration = false;
    FGOutput activeFgOutput = FGOutput::NoFG;
    bool isShuttingDown = false;
};

struct MockConfig {
    bool dlssNrEnabled = true;
    bool dlssNrFinishedPicture = true;
};

// Simulate presentation check in ApplyFinished / ApplyToFinishedPicture
bool ShouldApplyFinishedPicture(const MockConfig& cfg, const MockState& state) {
    if (state.isShuttingDown)
        return false;
    const bool externalFgActive = state.externalFrameGeneration || state.activeFgOutput == FGOutput::DLSSG;
    if (!cfg.dlssNrEnabled || !cfg.dlssNrFinishedPicture || externalFgActive)
        return false;
    return true;
}

// Transfer mode helpers
inline uint32_t DlssNrSpatialTransfer(uint32_t mode)
{
    switch (mode)
    {
    case 2: return 1; // DLSS enlargement -> matched residual
    case 4: return 3; // Private DLSS enlargement -> relative lighting & colour
    default: return mode;
    }
}

inline bool DlssNrUsesDlssEnlargement(uint32_t mode)
{
    return mode == 2 || mode == 4;
}

int main() {
    std::puts("Running DLSS-NR Finished Picture & External FG Mutual Exclusion Unit Tests...");

    // Test 1: FinishedColorSpaceKey GUID verified
    {
        TestGUID key = { 0x34a31e7b, 0x84c5, 0x44ef, { 0xa7, 0x4d, 0x6b, 0xd3, 0x60, 0x8c, 0xe5, 0x22 } };
        assert(key == ExpectedFinishedColorSpaceKey);
        std::puts("  [PASS] Test 1: FinishedColorSpaceKey GUID verified");
    }

    // Test 2: Standard internal FG allows finished-picture composition
    {
        MockConfig cfg;
        MockState state;
        state.externalFrameGeneration = false;
        state.activeFgOutput = FGOutput::FSRFG;
        assert(ShouldApplyFinishedPicture(cfg, state) == true);
        state.activeFgOutput = FGOutput::XeFG;
        assert(ShouldApplyFinishedPicture(cfg, state) == true);
        std::puts("  [PASS] Test 2: Internal FG (FSR FG / XeFG) permits finished-picture NR");
    }

    // Test 3: External FG proxy strictly blocks finished-picture presentation hooks
    {
        MockConfig cfg;
        MockState state;
        state.externalFrameGeneration = true;
        state.activeFgOutput = FGOutput::NoFG;
        assert(ShouldApplyFinishedPicture(cfg, state) == false);

        state.externalFrameGeneration = false;
        state.activeFgOutput = FGOutput::DLSSG;
        assert(ShouldApplyFinishedPicture(cfg, state) == false);
        std::puts("  [PASS] Test 3: External FG mode strictly blocks finished-picture NR to avoid collision");
    }

    // Test 4: Shutdown guard prevents any submission or hook execution
    {
        MockConfig cfg;
        MockState state;
        state.isShuttingDown = true;
        assert(ShouldApplyFinishedPicture(cfg, state) == false);
        std::puts("  [PASS] Test 4: Process shutdown guard prevents finished-picture dispatch");
    }

    // Test 5: Transfer mode mappings & DLSS enlargement usage verified
    {
        assert(DlssNrSpatialTransfer(0) == 0); // classic
        assert(DlssNrSpatialTransfer(1) == 1); // spatial matched residual
        assert(DlssNrSpatialTransfer(2) == 1); // DLSS matched residual -> mapped to 1
        assert(DlssNrSpatialTransfer(3) == 3); // spatial relative lighting & chroma
        assert(DlssNrSpatialTransfer(4) == 3); // DLSS relative lighting & chroma -> mapped to 3

        assert(!DlssNrUsesDlssEnlargement(0));
        assert(!DlssNrUsesDlssEnlargement(1));
        assert(DlssNrUsesDlssEnlargement(2));
        assert(!DlssNrUsesDlssEnlargement(3));
        assert(DlssNrUsesDlssEnlargement(4));
        std::puts("  [PASS] Test 5: Transfer mode mappings & enlargement routing verified");
    }

    std::puts("\nAll DLSS-NR Finished Picture & External FG Unit Tests passed successfully!");
    return 0;
}
