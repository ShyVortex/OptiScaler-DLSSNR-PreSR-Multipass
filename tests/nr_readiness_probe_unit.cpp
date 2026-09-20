#include <cassert>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <vector>
#include <map>

// Mock GPU Timeline & Fence
struct MockTimeline
{
    uint64_t fenceValue = 0;
    uint64_t completedValue = 0;
};

// Simulation of GpuLifetime Recording & Probe
struct MockRecording
{
    void* commands = nullptr;
    bool open = true;
    bool signalFailed = false;
    std::map<std::shared_ptr<MockTimeline>, uint64_t> completions;

    bool Finished() const
    {
        if (signalFailed || completions.empty())
            return false;
        for (const auto& [timeline, val] : completions)
        {
            if (timeline->completedValue < val)
                return false;
        }
        return true;
    }
};

class MockGpuLifetime
{
public:
    std::vector<std::shared_ptr<MockRecording>> recordings;

    void Record(void* cmdList)
    {
        for (const auto& rec : recordings)
        {
            if (rec->open && rec->commands == cmdList)
                return;
        }
        auto rec = std::make_shared<MockRecording>();
        rec->commands = cmdList;
        recordings.push_back(rec);
    }

    std::function<bool()> CompletionProbe(void* cmdList)
    {
        for (const auto& rec : recordings)
        {
            if (rec->open && rec->commands == cmdList)
            {
                return [rec]() -> bool {
                    return rec->Finished();
                };
            }
        }
        return []() { return false; };
    }

    void Submit(void* cmdList, std::shared_ptr<MockTimeline> timeline)
    {
        timeline->fenceValue++;
        for (auto& rec : recordings)
        {
            if (rec->open && rec->commands == cmdList)
            {
                rec->completions[timeline] = timeline->fenceValue;
            }
        }
    }
};

// Simulation of DlssNr_Proxy CreationReady logic
struct SimulatedProxyState
{
    uint64_t creationEpoch = 0;
    bool creationReady = false;
    std::function<bool()> creationComplete;

    bool CreationReady(uint64_t epoch)
    {
        return creationReady = creationReady || (epoch != creationEpoch) ||
                               (creationComplete && creationComplete());
    }

    void Reset()
    {
        creationEpoch = 0;
        creationReady = false;
        creationComplete = nullptr;
    }
};

int main()
{
    std::cout << "Running DLSS-NR Readiness Probe Unit Tests...\n";

    // Test 1: Stalled presentation counter (epoch stays constant at 0)
    // Before GPU fence signal, feature is not ready. Once fence signals, probe satisfies readiness.
    {
        SimulatedProxyState state;
        MockGpuLifetime lifetime;
        auto timeline = std::make_shared<MockTimeline>();

        void* createCmdList = (void*) 0x1000;
        lifetime.Record(createCmdList);

        state.creationEpoch = 0;
        state.creationReady = false;
        state.creationComplete = lifetime.CompletionProbe(createCmdList);

        // Before submit
        assert(!state.CreationReady(0));

        // Submit to queue, fence value = 1, completed = 0
        lifetime.Submit(createCmdList, timeline);
        assert(!state.CreationReady(0));

        // GPU completes work: completedValue = 1
        timeline->completedValue = 1;
        assert(state.CreationReady(0));
        assert(state.creationReady == true);

        // Subsequent calls stay ready
        assert(state.CreationReady(0));
        std::cout << "  [PASS] Test 1: Zero-epoch presentation counter resolves readiness via GPU completion probe\n";
    }

    // Test 2: Standard epoch advance (presentation counter increments)
    // Satisfies readiness even before probe or if probe is absent
    {
        SimulatedProxyState state;
        state.creationEpoch = 42;
        state.creationReady = false;
        state.creationComplete = nullptr;

        assert(!state.CreationReady(42));
        assert(state.CreationReady(43));
        assert(state.creationReady == true);
        std::cout << "  [PASS] Test 2: Monotonic epoch advancement satisfies readiness\n";
    }

    // Test 3: Multiple command lists and queue timelines
    {
        SimulatedProxyState state;
        MockGpuLifetime lifetime;
        auto queueA = std::make_shared<MockTimeline>();
        auto queueB = std::make_shared<MockTimeline>();

        void* cmdList1 = (void*) 0x2001;
        void* cmdList2 = (void*) 0x2002;

        lifetime.Record(cmdList1);
        lifetime.Record(cmdList2);

        auto probe1 = lifetime.CompletionProbe(cmdList1);
        auto probe2 = lifetime.CompletionProbe(cmdList2);

        lifetime.Submit(cmdList1, queueA);
        lifetime.Submit(cmdList2, queueB);

        assert(!probe1());
        assert(!probe2());

        // Signal queueA only
        queueA->completedValue = queueA->fenceValue;
        assert(probe1());
        assert(!probe2());

        // Signal queueB
        queueB->completedValue = queueB->fenceValue;
        assert(probe2());
        std::cout << "  [PASS] Test 3: Multiple command lists and timelines tracked independently\n";
    }

    // Test 4: Reset cleans latch and unbinds probe
    {
        SimulatedProxyState state;
        state.creationEpoch = 100;
        state.creationReady = true;

        state.Reset();
        assert(state.creationReady == false);
        assert(state.creationEpoch == 0);
        assert(!state.creationComplete);
        std::cout << "  [PASS] Test 4: State reset correctly clears readiness latches\n";
    }

    std::cout << "\nAll DLSS-NR Readiness Probe Unit Tests passed successfully!\n";
    return 0;
}
