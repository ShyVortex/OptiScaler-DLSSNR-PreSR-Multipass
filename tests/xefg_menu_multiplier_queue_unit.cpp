#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

// DLSS Unlocked #57 (Silent Hill Townfall): changing the XeMFG multiplier from the overlay froze the game.
// The menu is drawn inside the real swapchain's Present, which Intel's runtime issues from its own Present
// while FGHooks::FGPresent holds fg->Mutex. The menu must therefore queue the count, and XeFG must apply it
// on the game's present thread, without taking fg->Mutex inside SetInterpolatedFrameCount.
//
// Validates:
// 1. The menu never calls SetInterpolatedFrameCount on the current FG directly; all three XeMFG sites queue.
// 2. XeFG_Dx12::SetInterpolatedFrameCount takes no OwnedLockGuard (f904ab52 self-deadlock reverted).
// 3. XeFG_Dx12::RequestInterpolatedFrameCount only stores the request; it calls nothing in Intel's runtime.
// 4. XeFG_Dx12::Present applies the pending request first, before passthrough or dispatch.
// 5. IFGFeature's default RequestInterpolatedFrameCount forwards, so other backends are unchanged.
// 6. Queue semantics: latest request wins, it is applied once, and -1 means none.
// 7. The Streamline options path still calls SetInterpolatedFrameCount synchronously (game thread).

static std::string ReadFile(const char* path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        std::fprintf(stderr, "cannot open %s\n", path);
        std::exit(1);
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

static std::string FunctionBody(const std::string& source, const std::string& signature)
{
    const auto start = source.find(signature);
    assert(start != std::string::npos && "function not found");
    const auto open = source.find('{', start);
    int depth = 1;
    size_t end = open + 1;
    while (depth > 0 && end < source.size())
    {
        if (source[end] == '{')
            ++depth;
        else if (source[end] == '}')
            --depth;
        ++end;
    }
    assert(depth == 0 && "unbalanced function");
    return source.substr(open, end - open);
}

static size_t Count(const std::string& text, const std::string& needle)
{
    size_t count = 0;
    for (auto at = text.find(needle); at != std::string::npos; at = text.find(needle, at + needle.size()))
        ++count;
    return count;
}

// Mirrors the production pending slot (XeFG_Dx12::_pendingInterpolationCount).
struct PendingSlot
{
    std::atomic<int> pending { -1 };
    std::vector<unsigned> applied;

    void Request(unsigned count) { pending.store(static_cast<int>(count), std::memory_order_release); }

    void Present()
    {
        const int count = pending.exchange(-1, std::memory_order_acq_rel);
        if (count >= 0)
            applied.push_back(static_cast<unsigned>(count));
    }
};

int main()
{
    const std::string menu = ReadFile("OptiScaler/menu/menu_common.cpp");
    const std::string xefg = ReadFile("OptiScaler/framegen/xefg/XeFG_Dx12.cpp");
    const std::string xefgHeader = ReadFile("OptiScaler/framegen/xefg/XeFG_Dx12.h");
    const std::string feature = ReadFile("OptiScaler/framegen/IFGFeature.h");
    const std::string streamline = ReadFile("OptiScaler/hooks/Streamline_Hooks.cpp");

    // Test 1
    assert(Count(menu, "currentFG->SetInterpolatedFrameCount(") == 0 &&
           "the menu must not call into the FG runtime from inside Present");
    assert(Count(menu, "currentFG->RequestInterpolatedFrameCount(") == 3 && "all three XeMFG menu sites must queue");
    std::puts("Test 1 PASS: menu queues every XeMFG multiplier change");

    // Test 2
    const auto setBody = FunctionBody(xefg, "bool XeFG_Dx12::SetInterpolatedFrameCount(");
    assert(setBody.find("OwnedLockGuard") == std::string::npos && setBody.find("Mutex") == std::string::npos &&
           "SetInterpolatedFrameCount must not take fg->Mutex: FGPresent already holds it on this call chain");
    std::puts("Test 2 PASS: SetInterpolatedFrameCount takes no fg->Mutex");

    // Test 3
    const auto requestBody = FunctionBody(xefg, "void XeFG_Dx12::RequestInterpolatedFrameCount(");
    assert(requestBody.find("_pendingInterpolationCount.store(") != std::string::npos);
    assert(requestBody.find("XeFGProxy::") == std::string::npos &&
           requestBody.find("SetInterpolatedFrameCount(") == std::string::npos &&
           "the request must not touch Intel's runtime");
    assert(xefgHeader.find("std::atomic<int> _pendingInterpolationCount { -1 };") != std::string::npos);
    assert(xefgHeader.find("void RequestInterpolatedFrameCount(UINT interpolatedFrameCount) override;") !=
           std::string::npos);
    std::puts("Test 3 PASS: RequestInterpolatedFrameCount only stores the request");

    // Test 4
    const auto presentBody = FunctionBody(xefg, "bool XeFG_Dx12::Present()");
    const auto consume = presentBody.find("_pendingInterpolationCount.exchange(-1");
    const auto apply = presentBody.find("SetInterpolatedFrameCount(static_cast<UINT>(pendingCount))");
    const auto passthrough = presentBody.find("if (_passthrough)");
    assert(consume != std::string::npos && apply != std::string::npos && passthrough != std::string::npos);
    assert(consume < apply && apply < passthrough && "the request is applied before passthrough and dispatch");
    std::puts("Test 4 PASS: Present applies the pending request first");

    // Test 5
    const auto defaultBody = FunctionBody(feature, "virtual void RequestInterpolatedFrameCount(");
    assert(defaultBody.find("SetInterpolatedFrameCount(interpolatedFrameCount);") != std::string::npos);
    std::puts("Test 5 PASS: other backends forward the request unchanged");

    // Test 6
    PendingSlot slot;
    slot.Present();
    assert(slot.applied.empty() && "no request, nothing applied");
    slot.Request(2);
    slot.Request(4); // dragging the slider: only the final value matters
    slot.Present();
    slot.Present();
    assert(slot.applied.size() == 1 && slot.applied[0] == 4 && "latest request wins and is applied once");
    slot.Request(0); // passthrough is a valid request, distinct from "none"
    slot.Present();
    assert(slot.applied.size() == 2 && slot.applied[1] == 0);
    std::puts("Test 6 PASS: latest request wins, applied once, 0 distinct from none");

    // Test 7
    assert(streamline.find("state.currentFG->SetInterpolatedFrameCount(targetCount)") != std::string::npos &&
           "Streamline options run on the game thread and need the synchronous result");
    std::puts("Test 7 PASS: Streamline path stays synchronous");

    std::puts("All XeFG menu multiplier queue tests passed (7/7).");
    return 0;
}
