#include <cassert>
#include <cstdint>
#include <iostream>
#include <memory>

// Mock NVSDK return codes
constexpr unsigned int NVSDK_NGX_Result_Success = 0x1;
constexpr unsigned int NVSDK_NGX_Result_Fail = 0xBAD00001;

// Mock State simulating OptiScaler State singleton
struct MockState
{
    uint64_t frameCount = 0;
    uint64_t swapchainFrameCount = 0;
    bool externalFrameGeneration = false;

    static MockState& Instance()
    {
        static MockState s;
        return s;
    }

    void Reset()
    {
        frameCount = 0;
        swapchainFrameCount = 0;
        externalFrameGeneration = false;
    }
};

// Mock IFeature_Dx12 evaluation epoch resolution logic
struct MockFeatureEvaluator
{
    uint64_t lastSeenSwapchain = 0;
    uint64_t evalFrameCounter = 0;

    uint64_t ResolveSubmissionEpoch(uint64_t inputEpoch, bool interop)
    {
        uint64_t submissionEpoch = inputEpoch;
        if (!interop)
        {
            if (submissionEpoch == 0)
            {
                const uint64_t scFrames = MockState::Instance().swapchainFrameCount;
                if (scFrames > 0 && scFrames != lastSeenSwapchain)
                {
                    lastSeenSwapchain = scFrames;
                    submissionEpoch = scFrames;
                    MockState::Instance().frameCount = scFrames;
                }
                else
                {
                    // Wrapped swapchain is either bypassed or not advancing frame count (e.g. external FG).
                    submissionEpoch = ++evalFrameCounter;
                    MockState::Instance().frameCount = submissionEpoch;
                }
            }
        }
        return submissionEpoch;
    }
};

// Mock DlssNr Proxy Context
struct MockProxyContext
{
    struct ProxyState
    {
        bool hasFeature = false;
        bool failed = false;
        uint64_t creationEpoch = 0;
        uint64_t creationFrameCount = 0;
        uint64_t prepareCallCount = 0;
    } state;

    void Reset() { state = {}; }

    unsigned int Prepare(uint64_t submissionEpoch, bool* ready)
    {
        *ready = false;
        if (state.failed)
            return NVSDK_NGX_Result_Fail;

        if (!state.hasFeature)
        {
            // Simulate Feature 18 creation
            state.hasFeature = true;
            state.creationEpoch = submissionEpoch;
            state.creationFrameCount = ++state.prepareCallCount;
            // Creation must reach GPU before evaluation
            return NVSDK_NGX_Result_Success;
        }

        ++state.prepareCallCount;
        *ready = (submissionEpoch != state.creationEpoch) || (state.prepareCallCount > state.creationFrameCount);
        return NVSDK_NGX_Result_Success;
    }

    bool Ready(uint64_t epoch) const
    {
        return state.hasFeature && !state.failed &&
               (epoch != state.creationEpoch || state.prepareCallCount > state.creationFrameCount);
    }
};

int main()
{
    std::cout << "Running DLSS-NR External FG & Epoch Readiness Unit Tests...\n";

    // Test 1: Standard Swapchain active - epoch follows swapchain cadence
    {
        MockState::Instance().Reset();
        MockFeatureEvaluator evaluator;

        MockState::Instance().swapchainFrameCount = 100;
        uint64_t epoch1 = evaluator.ResolveSubmissionEpoch(0, false);
        assert(epoch1 == 100);
        assert(MockState::Instance().frameCount == 100);

        MockState::Instance().swapchainFrameCount = 101;
        uint64_t epoch2 = evaluator.ResolveSubmissionEpoch(0, false);
        assert(epoch2 == 101);
        assert(MockState::Instance().frameCount == 101);

        std::cout << "  [PASS] Test 1: Swapchain active synchronizes frameCount and submissionEpoch\n";
    }

    // Test 2: External FG / Bypassed Swapchain - monotonic evaluation advancement
    {
        MockState::Instance().Reset();
        MockState::Instance().externalFrameGeneration = true;
        MockState::Instance().swapchainFrameCount = 0; // swapchain Present bypassed!
        MockFeatureEvaluator evaluator;

        // Frame 1
        uint64_t epoch1 = evaluator.ResolveSubmissionEpoch(0, false);
        assert(epoch1 == 1);
        assert(MockState::Instance().frameCount == 1);

        // Frame 2
        uint64_t epoch2 = evaluator.ResolveSubmissionEpoch(0, false);
        assert(epoch2 == 2);
        assert(MockState::Instance().frameCount == 2);

        // Frame 3
        uint64_t epoch3 = evaluator.ResolveSubmissionEpoch(0, false);
        assert(epoch3 == 3);
        assert(MockState::Instance().frameCount == 3);

        std::cout << "  [PASS] Test 2: Bypassed swapchain monotonically advances submissionEpoch and frameCount\n";
    }

    // Test 3: Proxy Context readiness under normal monotonic epoch progression
    {
        MockProxyContext ctx;
        bool ready = false;

        // Frame 1: Creation
        unsigned int res1 = ctx.Prepare(1, &ready);
        assert(res1 == NVSDK_NGX_Result_Success);
        assert(!ready);
        assert(!ctx.Ready(1));

        // Frame 2: Subsequent frame with advancing epoch
        unsigned int res2 = ctx.Prepare(2, &ready);
        assert(res2 == NVSDK_NGX_Result_Success);
        assert(ready);
        assert(ctx.Ready(2));

        std::cout << "  [PASS] Test 3: Proxy Context unlocks readiness on next frame with advancing epoch\n";
    }

    // Test 4: Proxy Context double-lock protection under static/zero epoch (anomalous mod conditions)
    {
        MockProxyContext ctx;
        bool ready = false;

        // Frame 1: Creation at epoch 0
        unsigned int res1 = ctx.Prepare(0, &ready);
        assert(res1 == NVSDK_NGX_Result_Success);
        assert(!ready);

        // Frame 2: Even if epoch is STILL 0, invocation count > creationFrameCount guarantees unlock!
        unsigned int res2 = ctx.Prepare(0, &ready);
        assert(res2 == NVSDK_NGX_Result_Success);
        assert(ready);
        assert(ctx.Ready(0));

        // Frame 3: Remains unlocked
        unsigned int res3 = ctx.Prepare(0, &ready);
        assert(res3 == NVSDK_NGX_Result_Success);
        assert(ready);
        assert(ctx.Ready(0));

        std::cout << "  [PASS] Test 4: Double-lock protection unlocks readiness even if epoch is static zero\n";
    }

    // Test 5: End-to-end integration: Evaluator + ProxyContext in External FG scenario
    {
        MockState::Instance().Reset();
        MockState::Instance().externalFrameGeneration = true;
        MockFeatureEvaluator evaluator;
        MockProxyContext ctx;
        bool ready = false;

        // Frame 1 (Game evaluates upscaler)
        uint64_t epoch1 = evaluator.ResolveSubmissionEpoch(0, false);
        assert(epoch1 == 1);
        ctx.Prepare(epoch1, &ready);
        assert(!ready); // Creation frame, waiting for GPU

        // Frame 2 (Next game frame)
        uint64_t epoch2 = evaluator.ResolveSubmissionEpoch(0, false);
        assert(epoch2 == 2);
        ctx.Prepare(epoch2, &ready);
        assert(ready); // Ready to run DLSS-NR model!

        std::cout << "  [PASS] Test 5: End-to-end pipeline in external FG mode transitions to ready\n";
    }

    std::cout << "All DLSS-NR External FG & Epoch Readiness Unit Tests passed successfully!\n";
    return 0;
}
