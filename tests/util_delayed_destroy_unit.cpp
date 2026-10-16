#include <cassert>
#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// Standalone unit test validating the retire-queue GPU teardown semantics introduced by commit
// b22d503f "[PATCH 1/3] Fix DelayedDestroy cross-thread GPU teardown race"
// (Util::RetireDelayedDestroy / Util::ProcessDelayedDestroys).
//
// The queue functions below MIRROR OptiScaler/Util.cpp:918-971 verbatim (the real Util.cpp is not
// standalone-compilable: pch.h / spdlog / State dependencies). The only adaptations are
// LOG_ERROR -> std::fprintf (Util.cpp:941) and the anonymous-namespace queue being referenced
// from the Util namespace block, which preserves the single-TU linkage of the original.
//
// Semantics under test:
//   - retired objects survive until the 2s drain deadline passes (a drain before the deadline
//     destroys nothing),
//   - matured entries are destroyed exactly, and only, on the DRAINING (calling) thread,
//   - FIFO destruction order,
//   - exactly-once destruction (a second drain is a no-op),
//   - a destructor that itself retires a nested object does not deadlock (destruction runs
//     outside the queue lock) and the nested entry waits for a later drain,
//   - un-drained entries stay owned by the queue (no leak of the closure).

// === MIRROR of Util.cpp:918-927 ===
// Retired GPU objects wait out their drain delay here instead of being destroyed on a
// detached thread. Destruction happens on the thread that calls ProcessDelayedDestroys()
// (the render thread via the per-frame evaluate paths), so teardown no longer races the
// driver work that is concurrently running on that thread.
namespace
{
std::mutex delayedDestroyMutex;
std::vector<std::pair<std::chrono::steady_clock::time_point, std::function<void()>>> delayedDestroys;
constexpr std::chrono::seconds delayedDestroyDelay { 2 };
} // namespace

namespace Util
{
// === MIRROR of Util.cpp:929-944 ===
void RetireDelayedDestroy(std::function<void()> destroy)
{
    if (!destroy)
        return;

    try
    {
        std::scoped_lock lock(delayedDestroyMutex);
        delayedDestroys.emplace_back(std::chrono::steady_clock::now() + delayedDestroyDelay, std::move(destroy));
    }
    catch (...)
    {
        // LOG_ERROR("DelayedDestroy: retire queue allocation failed; destroying immediately on this thread")
        // in Util.cpp:941
        std::fprintf(stderr,
                     "DelayedDestroy: retire queue allocation failed; destroying immediately on this thread\n");
        destroy();
    }
}

// === MIRROR of Util.cpp:946-971 ===
void ProcessDelayedDestroys()
{
    std::vector<std::function<void()>> matured;

    {
        std::scoped_lock lock(delayedDestroyMutex);
        const auto now = std::chrono::steady_clock::now();

        for (auto it = delayedDestroys.begin(); it != delayedDestroys.end();)
        {
            if (it->first <= now)
            {
                matured.push_back(std::move(it->second));
                it = delayedDestroys.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    // Destroy outside the queue lock: feature destructors release GPU objects and may
    // re-enter retirement (nested features, NR lifetimes) from this same thread.
    for (auto& destroy : matured)
        destroy();
}
} // namespace Util

namespace
{
// Sleep past the 2s drain deadline with margin for scheduler jitter (the delay is
// delayedDestroyDelay == 2s from the mirror of Util.cpp:926).
void SleepPastDeadline()
{
    std::this_thread::sleep_for(delayedDestroyDelay + std::chrono::milliseconds(250));
}

struct DestructionLog
{
    std::vector<std::string> order;
    std::vector<std::thread::id> threads;

    void Record(const std::string& name) { order.push_back(name); }
    void Record(const std::string& name, std::thread::id thread) { Record(name); threads.push_back(thread); }
};

// An object whose ownership the queue must retain until a drain destroys its closure.
struct Tracker
{
    unsigned* destroyed = nullptr;
    ~Tracker() { ++*destroyed; }
};
} // namespace

int main()
{
    std::printf("=== Running Util Retire-Queue Delayed Destroy Unit Tests ===\n");

    DestructionLog log;

    // Case 1: a drain before the deadline destroys nothing (objects survive the 2s grace window)
    {
        Util::RetireDelayedDestroy([&log] { log.Record("first", std::this_thread::get_id()); });
        Util::ProcessDelayedDestroys();
        assert(log.order.empty()); // nothing matured yet
        std::printf("  [PASS] Case 1: drain before the 2s deadline destroys nothing\n");
    }

    // Retire the FIFO batch, the nesting parent, the ownership probe and a null closure.
    unsigned trackerDestroyed = 0;
    std::weak_ptr<Tracker> ownedWeak;
    {
        auto tracker = std::make_shared<Tracker>();
        tracker->destroyed = &trackerDestroyed;
        ownedWeak = tracker;
        Util::RetireDelayedDestroy([tracker] {}); // closure captures the shared owner
        tracker.reset();                           // the queue's closure must now be the sole owner
        assert(!ownedWeak.expired());              // un-drained: the entry (and its closure) stays owned
        assert(trackerDestroyed == 0);
    }
    Util::RetireDelayedDestroy(nullptr); // mirror of the null no-op guard (Util.cpp:931-932)
    for (const char* name : { "A", "B", "C" })
        Util::RetireDelayedDestroy(
            [&log, name] { log.Record(name, std::this_thread::get_id()); });
    // The parent's destructor retires a nested child (must not deadlock; child waits for a later drain)
    unsigned childDestroyed = 0;
    std::thread::id childThreadId {};
    Util::RetireDelayedDestroy([&] {
        log.Record("parent", std::this_thread::get_id());
        Util::RetireDelayedDestroy([&] {
            ++childDestroyed;
            childThreadId = std::this_thread::get_id();
        });
    });

    SleepPastDeadline();

    // Case 2 + Case 3: after the deadline, a drain destroys exactly the matured entries on the
    // DRAINING (calling) thread, in FIFO order
    {
        std::thread::id drainThreadId {};
        std::thread drainer([&] {
            drainThreadId = std::this_thread::get_id();
            Util::ProcessDelayedDestroys();
        });
        drainer.join();

        // FIFO order over all matured entries; the null closure contributed nothing
        const std::string expected[] = { "first", "A", "B", "C", "parent" };
        assert(log.order.size() == 5);
        for (size_t i = 0; i < 5; ++i)
            assert(log.order[i] == expected[i]);

        // Every destruction happened on the draining thread, not on any other thread
        assert(log.threads.size() == 5);
        for (std::thread::id id : log.threads)
            assert(id == drainThreadId);

        // The parent's nested retire did NOT deadlock (we reached this assert) and the child is
        // still queued for a later drain
        assert(childDestroyed == 0);

        // The ownership probe: destroying the matured closure released the queue's shared owner
        assert(ownedWeak.expired());
        assert(trackerDestroyed == 1);

        std::printf("  [PASS] Case 2: matured entries are destroyed on the draining thread in FIFO order\n");
        std::printf("  [PASS] Case 3: nested retire from a destructor does not deadlock and waits for a later "
                    "drain\n");
    }

    // Case 4: exactly-once destruction — a second drain is a no-op (and the immature nested child
    // still survives)
    {
        Util::ProcessDelayedDestroys();
        assert(log.order.size() == 5); // unchanged
        assert(childDestroyed == 0);   // nested child not matured yet
        std::printf("  [PASS] Case 4: a second drain is a no-op (exactly-once destruction)\n");
    }

    // Case 5: the nested child matures on its own 2s schedule and is destroyed on the thread that
    // drains it (main this time)
    {
        SleepPastDeadline();
        Util::ProcessDelayedDestroys();
        assert(childDestroyed == 1);
        assert(childThreadId == std::this_thread::get_id());
        assert(log.order.size() == 5); // the child closure does not log a name

        // The queue is fully drained now
        Util::ProcessDelayedDestroys();
        assert(log.order.size() == 5);
        assert(childDestroyed == 1);
        std::printf("  [PASS] Case 5: the nested child is destroyed exactly once on a later draining thread\n");
    }

    std::printf("=== All Util Retire-Queue Delayed Destroy Unit Tests PASSED! ===\n");
    return 0;
}
