// Compile the real owner aggregation function and real GpuSubmission token.
// Owner callbacks replace GPU work and report the surrounding lock/scope state.
#include <dlssnr/DlssNr_GpuSubmission.h>
#include <cstdio>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

static unsigned checks = 0, failures = 0;
static const char* scenario = "";
static void Expect(bool condition, const char* message)
{
    ++checks;
    if (!condition)
    {
        ++failures;
        std::printf("FAIL [%s]: %s\n", scenario, message);
    }
}

std::recursive_mutex nrOwnersMutex;
unsigned nrNotificationDepth = 0;
static unsigned collections = 0;
static bool throwCollection = false;
void CollectRetiredNrOwners()
{
    ++collections;
    if (throwCollection)
        throw std::bad_alloc();
}

// Probe from another thread because try_lock on this recursive mutex would
// succeed on the submitting thread even while it owns the lock.
static bool RegistryLocked()
{
    bool acquired = false;
    std::thread probe(
        [&]
        {
            acquired = nrOwnersMutex.try_lock();
            if (acquired)
                nrOwnersMutex.unlock();
        });
    probe.join();
    return !acquired;
}

#include "notification-scope.inc"

struct Failure
{
    unsigned identity;
};
struct Owner
{
    unsigned id;
    bool throws = false;
    bool throwsOnBegin = false;
    unsigned begins = 0, completions = 0;
    ID3D12CommandQueue* seenQueue = nullptr;
    std::vector<unsigned>& order;
    unsigned quarantines = 0;
    void QuarantineFinishedCommands(UINT, ID3D12CommandList* const*)
    {
        ++quarantines;
        Expect(RegistryLocked(), "preparation quarantine must precede registry unlock");
        Expect(nrNotificationDepth > 0, "quarantine must precede notification cleanup");
    }
    DlssNr::GpuSubmission BeginFinishedCommands(UINT, ID3D12CommandList* const*)
    {
        ++begins;
        if (throwsOnBegin)
            throw Failure { id };
        return DlssNr::GpuSubmission(
            [this](ID3D12CommandQueue* queue)
            {
                ++completions;
                seenQueue = queue;
                order.push_back(id);
                Expect(RegistryLocked(), "every owner completion must hold the registry lock");
                Expect(nrNotificationDepth > 0, "every owner completion must retain the notification scope");
                if (throws)
                    throw Failure { id };
            });
    }
};
std::vector<Owner*> nrOwners;

struct State
{
    bool isShuttingDown = false;
    static State& Instance()
    {
        static State state;
        return state;
    }
};
#define LOG_ERROR(...) ((void) 0)

namespace DlssNr
{
#include "owner-aggregation.inc"
}

static void Run(bool firstThrows, bool secondThrows, bool abandon)
{
    scenario = abandon        ? "outer abandonment with failing children"
               : !firstThrows ? "normal completion"
               : secondThrows ? "primary and cleanup failures"
                              : "primary failure";
    std::vector<unsigned> order;
    Owner first { 1, firstThrows, false, 0, 0, nullptr, order };
    Owner second { 2, secondThrows, false, 0, 0, nullptr, order };
    Owner third { 3, false, false, 0, 0, nullptr, order };
    nrOwners = { &first, &second, &third };
    ID3D12CommandQueue queue;
    ID3D12CommandList commands;
    ID3D12CommandList* lists[] = { &commands };
    unsigned caught = 0;
    {
        auto submission = DlssNr::BeginFinishedPictureSubmission(1, lists);
        Expect(bool(submission), "tracked owners must produce a token");
        Expect(!RegistryLocked() && nrNotificationDepth == 0,
               "preparation must release the registry lock and notification scope");
        if (!abandon)
        {
            try
            {
                submission.Complete(&queue);
            }
            catch (const Failure& failure)
            {
                caught = failure.identity;
            }
            catch (...)
            {
                caught = 99;
            }
            Expect(caught == (firstThrows ? 1u : 0u), "cleanup must preserve the original exception");
            submission.Complete(&queue); // Completion/throw consumes the outer token.
        }
    }
    Expect(order == std::vector<unsigned>({ 1, 2, 3 }), "a failing child must not skip or repeat later children");
    Expect(first.completions == 1 && second.completions == 1 && third.completions == 1,
           "each captured owner must complete exactly once");
    Expect(first.seenQueue == (abandon ? nullptr : &queue), "first owner must receive the supplied queue");
    const auto* followingQueue = abandon || firstThrows ? nullptr : &queue;
    Expect(second.seenQueue == followingQueue && third.seenQueue == followingQueue,
           "uncompleted children must abandon with nullptr after a failure");
    Expect(!RegistryLocked() && nrNotificationDepth == 0,
           "normal and exceptional cleanup must release the registry lock and notification scope");
    nrOwners.clear();
}

int main()
{
    Run(false, false, false);
    Run(true, false, false);
    Run(true, true, false);
    Run(true, true, true);
    scenario = "empty registry";
    auto empty = DlssNr::BeginFinishedPictureSubmission(0, nullptr);
    Expect(!empty, "an empty owner registry must produce an empty token");
    scenario = "pre-submit preparation failure";
    std::vector<unsigned> order;
    Owner first { 1, false, false, 0, 0, nullptr, order };
    Owner second { 2, false, true, 0, 0, nullptr, order };
    Owner third { 3, false, false, 0, 0, nullptr, order };
    nrOwners = { &first, &second, &third };
    bool escaped = false;
    try
    {
        (void) DlssNr::BeginFinishedPictureSubmission(0, nullptr);
    }
    catch (...)
    {
        escaped = true;
    }
    Expect(!escaped, "pre-submit bookkeeping failures must not cross the COM hook boundary");
    Expect(first.completions == 1 && first.seenQueue == nullptr,
           "already captured owners must be abandoned when later preparation fails");
    Expect(first.quarantines == 1 && second.quarantines == 1 && third.quarantines == 1,
           "failed preparation must quarantine captured, throwing and unvisited owners");
    Expect(third.begins == 0, "the unvisited owner's protection must not depend on creating its token");
    Expect(!RegistryLocked() && nrNotificationDepth == 0,
           "preparation failure must release the registry lock and notification scope");
    Expect(nrNotificationDepth == 0 && collections == 10, "preparation and callback scopes must unwind exactly once");
    nrOwners.clear();
    scenario = "notification cleanup allocation failure";
    throwCollection = true;
    {
        NrNotificationScope scope;
    }
    Expect(nrNotificationDepth == 0, "failed collection must not terminate or strand notification depth");
    throwCollection = false;
    {
        NrNotificationScope scope;
    }
    Expect(collections == 12, "later notification cleanup must retry collection");
    std::printf("%u checks, %u failures (CPU-only production owner aggregation)\n", checks, failures);
    return failures ? 1 : 0;
}
