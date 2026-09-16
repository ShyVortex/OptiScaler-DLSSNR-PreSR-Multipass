#include <cassert>
#include <cstdio>
#include <cstdint>
#include <atomic>
#include <shared_mutex>
#include <thread>
#include <chrono>

// Mock OwnedMutex implementation matching OptiScaler/OwnedMutex.h
class MockOwnedMutex {
private:
    std::shared_mutex mtx;
    std::atomic<uint32_t> owner{0};

public:
    bool try_lock_for(uint32_t _owner, std::chrono::milliseconds timeout) {
        // Test helper: verify if lock can be acquired without blocking
        // For shared_mutex, if current thread already holds exclusive lock, try_lock returns false (deadlock risk)
        auto start = std::chrono::steady_clock::now();
        while (!mtx.try_lock()) {
            if (std::chrono::steady_clock::now() - start > timeout) {
                return false; // Deadlock or contended!
            }
            std::this_thread::yield();
        }
        owner.store(_owner, std::memory_order_release);
        return true;
    }

    void lock(uint32_t _owner) {
        mtx.lock();
        owner.store(_owner, std::memory_order_release);
    }

    void unlock(uint32_t _owner) {
        owner.store(0, std::memory_order_release);
        mtx.unlock();
    }

    uint32_t getOwner() const {
        return owner.load(std::memory_order_seq_cst);
    }
};

struct MockState {
    bool externalFrameGeneration = false;
    int activeFgNvngx = 0;
    int activeFgOutput = 0;
    int activeFgInput = 0;
    int dlssgDetectedInterpolationCount = 0;
};

struct MockConfig {
    bool FGDLSSGAmpereMfgUnlock = false;
    bool FGDLSSGAdaMfgUnlock = false;
};

// Simulate the exact swapchain re-entrancy logic from wrapped_swapchain.cpp
bool SimulateResizeBuffers(MockOwnedMutex& localMutex, const MockState& state, const MockConfig& config, int siteId)
{
    const uint32_t currentOwner = localMutex.getOwner();
    const bool presentOwnsLock = (currentOwner == 4 || currentOwner == 5);
    const bool isDlssgMod = state.externalFrameGeneration ||
                            state.activeFgNvngx != 0 ||
                            state.activeFgOutput == 1 || // DLSSG
                            state.activeFgInput == 1 ||  // DLSSG
                            config.FGDLSSGAmpereMfgUnlock ||
                            config.FGDLSSGAdaMfgUnlock ||
                            state.dlssgDetectedInterpolationCount > 0;

    bool lockAcquired = false;
    if (!presentOwnsLock && !isDlssgMod)
    {
        // Try acquiring lock with a short 20ms timeout to detect deadlock immediately
        if (!localMutex.try_lock_for(siteId, std::chrono::milliseconds(20))) {
            return false; // DEADLOCK DETECTED!
        }
        lockAcquired = true;
    }

    // Perform buffer resize work...
    if (lockAcquired) {
        localMutex.unlock(siteId);
    }

    return true; // Successfully executed without deadlock
}

int main()
{
    printf("[TEST] Running Swapchain Present Re-Entrancy Unit Tests...\n");

    MockOwnedMutex localMutex;
    MockState state;
    MockConfig config;

    // ------------------------------------------------------------------------
    // Test 1: Normal call from outside Present (currentOwner == 0)
    // ------------------------------------------------------------------------
    {
        bool ok = SimulateResizeBuffers(localMutex, state, config, 1);
        assert(ok && "ResizeBuffers outside present must succeed");
        assert(localMutex.getOwner() == 0 && "Mutex must be unlocked after return");
        printf("  [PASS] Test 1: Normal execution outside Present acquires and releases lock\n");
    }

    // ------------------------------------------------------------------------
    // Test 2: Present() holds lock (owner == 4) and DLSSG calls ResizeBuffers
    // Simulating Ada MFG where FGDLSSGAdaMfgUnlock == true
    // ------------------------------------------------------------------------
    {
        config.FGDLSSGAdaMfgUnlock = true;
        localMutex.lock(4); // Present() owns lock

        bool ok = SimulateResizeBuffers(localMutex, state, config, 1);
        assert(ok && "Re-entrant call from Present under Ada MFG must NOT deadlock!");
        assert(localMutex.getOwner() == 4 && "Present must still hold owner 4");

        localMutex.unlock(4); // Present finishes
        printf("  [PASS] Test 2: Re-entrant ResizeBuffers during Present with Ada MFG bypasses lock safely (No Deadlock)\n");
    }

    // ------------------------------------------------------------------------
    // Test 3: Present1() holds lock (owner == 5) and DLSSG calls ResizeBuffers1
    // Simulating native DLSSG with dlssgDetectedInterpolationCount > 0
    // ------------------------------------------------------------------------
    {
        config.FGDLSSGAdaMfgUnlock = false;
        state.dlssgDetectedInterpolationCount = 2; // 3X MFG active
        localMutex.lock(5); // Present1() owns lock

        bool ok = SimulateResizeBuffers(localMutex, state, config, 2);
        assert(ok && "Re-entrant call from Present1 with dlssgDetectedInterpolationCount > 0 must NOT deadlock!");
        assert(localMutex.getOwner() == 5 && "Present1 must still hold owner 5");

        localMutex.unlock(5); // Present1 finishes
        printf("  [PASS] Test 3: Re-entrant ResizeBuffers1 during Present1 with active DLSSG ratio bypasses lock safely\n");
    }

    // ------------------------------------------------------------------------
    // Test 4: Regression test against old buggy logic
    // In old logic: if (!(presentOwnsLock && isDlssgMod))
    // When isDlssgMod was false on Ada, it attempted localMutex.lock() during Present, causing deadlock!
    // ------------------------------------------------------------------------
    {
        state = MockState{}; // reset
        config = MockConfig{}; // reset
        // Even if all mod flags are false, if presentOwnsLock == true, we MUST NOT attempt lock!
        localMutex.lock(4);

        bool ok = SimulateResizeBuffers(localMutex, state, config, 1);
        assert(ok && "When presentOwnsLock is true, lock attempt must be bypassed regardless of flags!");
        assert(localMutex.getOwner() == 4);

        localMutex.unlock(4);
        printf("  [PASS] Test 4: presentOwnsLock unconditionally protects against recursive deadlock\n");
    }

    printf("[TEST] All Swapchain Present Re-Entrancy unit tests passed successfully!\n");
    return 0;
}
