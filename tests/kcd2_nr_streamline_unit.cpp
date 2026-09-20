#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>

// Logical reproduction of DlssNr::StreamlinePicture wrapping logic
enum class FGNvngxReplacement : int
{
    None = 0,
    Nukem = 1,
    Other = 2
};

struct MockState
{
    bool kcd2NrBeforeFg = false;
    FGNvngxReplacement activeFgNvngx = FGNvngxReplacement::None;
};

static MockState g_state;

struct MockConfig
{
    bool dlssNrEnabled = true;
    bool dlssNrFinishedPicture = true;
};

static MockConfig g_cfg;

using GetFunction = void* (*)(const char*);

template <bool Local>
struct MockHooks
{
    inline static void* originalPresent = nullptr;
    inline static void* originalPresent1 = nullptr;
    inline static void* originalCreateHwnd = nullptr;
    inline static void* originalCreate = nullptr;
    inline static void* getIndex = nullptr;
    inline static void* getBuffer = nullptr;

    static void* DummyPresent() { return nullptr; }
    static void* DummyPresent1() { return nullptr; }
    static void* DummyCreateHwnd() { return nullptr; }
    static void* DummyCreate() { return nullptr; }

    static void* Wrap(const char* name, GetFunction getFunction)
    {
        if (!name || !getFunction) return nullptr;
        const bool present = std::strcmp(name, "slHookPresent") == 0;
        const bool present1 = std::strcmp(name, "slHookPresent1") == 0;
        const bool createHwnd = std::strcmp(name, "slHookCreateSwapChainForHwnd") == 0;
        const bool create = Local && std::strcmp(name, "slHookCreateSwapChain") == 0;
        if (!present && !present1 && !createHwnd && !create) return nullptr;

        auto* function = getFunction(name);
        getIndex = getFunction("slHookGetCurrentBackBufferIndex");
        getBuffer = getFunction("slHookGetBuffer");
        if (!function || !getIndex || !getBuffer || !getFunction("slHookPresent") || !getFunction("slHookPresent1"))
            return nullptr;

        if (present) { originalPresent = function; return (void*)&DummyPresent; }
        if (present1) { originalPresent1 = function; return (void*)&DummyPresent1; }
        if (createHwnd) { originalCreateHwnd = function; return (void*)&DummyCreateHwnd; }
        originalCreate = function;
        return (void*)&DummyCreate;
    }
};

void* MockWrap(const char* name, GetFunction getFunction, bool local = false)
{
    if (local && (!g_state.kcd2NrBeforeFg || g_state.activeFgNvngx != FGNvngxReplacement::None))
        return nullptr;
    return local ? MockHooks<true>::Wrap(name, getFunction) : MockHooks<false>::Wrap(name, getFunction);
}

// Mock Streamline plugin export provider
static void* MockPluginProvider(const char* name)
{
    static int s_dummy = 0;
    if (std::strcmp(name, "slHookPresent") == 0 ||
        std::strcmp(name, "slHookPresent1") == 0 ||
        std::strcmp(name, "slHookCreateSwapChainForHwnd") == 0 ||
        std::strcmp(name, "slHookCreateSwapChain") == 0 ||
        std::strcmp(name, "slHookGetCurrentBackBufferIndex") == 0 ||
        std::strcmp(name, "slHookGetBuffer") == 0)
    {
        return (void*)&s_dummy;
    }
    return nullptr;
}

int main()
{
    // Test 1: Local wrapping disabled without Kcd2NrBeforeFg quirk
    {
        g_state.kcd2NrBeforeFg = false;
        g_state.activeFgNvngx = FGNvngxReplacement::None;
        assert(MockWrap("slHookPresent", MockPluginProvider, true) == nullptr);
        assert(MockWrap("slHookCreateSwapChain", MockPluginProvider, true) == nullptr);
    }

    // Test 2: Local wrapping disabled when replacement FG (e.g. Nukem/Other) is active
    {
        g_state.kcd2NrBeforeFg = true;
        g_state.activeFgNvngx = FGNvngxReplacement::Nukem;
        assert(MockWrap("slHookPresent", MockPluginProvider, true) == nullptr);

        g_state.activeFgNvngx = FGNvngxReplacement::Other;
        assert(MockWrap("slHookPresent", MockPluginProvider, true) == nullptr);
    }

    // Test 3: Local wrapping successfully enabled when quirk is active and replacement is None
    {
        g_state.kcd2NrBeforeFg = true;
        g_state.activeFgNvngx = FGNvngxReplacement::None;

        void* hookedPresent = MockWrap("slHookPresent", MockPluginProvider, true);
        assert(hookedPresent != nullptr);
        assert(hookedPresent == (void*)&MockHooks<true>::DummyPresent);

        void* hookedPresent1 = MockWrap("slHookPresent1", MockPluginProvider, true);
        assert(hookedPresent1 != nullptr);
        assert(hookedPresent1 == (void*)&MockHooks<true>::DummyPresent1);

        void* hookedCreateHwnd = MockWrap("slHookCreateSwapChainForHwnd", MockPluginProvider, true);
        assert(hookedCreateHwnd != nullptr);
        assert(hookedCreateHwnd == (void*)&MockHooks<true>::DummyCreateHwnd);

        void* hookedCreate = MockWrap("slHookCreateSwapChain", MockPluginProvider, true);
        assert(hookedCreate != nullptr);
        assert(hookedCreate == (void*)&MockHooks<true>::DummyCreate);
    }

    // Test 4: Native Streamline wrapping (local = false) does NOT wrap legacy slHookCreateSwapChain
    {
        // Even with KCD2 quirk active, native SL uses CreateSwapChainForHwnd
        void* nativeLegacyCreate = MockWrap("slHookCreateSwapChain", MockPluginProvider, false);
        assert(nativeLegacyCreate == nullptr);

        void* nativeHwndCreate = MockWrap("slHookCreateSwapChainForHwnd", MockPluginProvider, false);
        assert(nativeHwndCreate != nullptr);
        assert(nativeHwndCreate == (void*)&MockHooks<false>::DummyCreateHwnd);
    }

    // Test 5: Unrecognized functions are never wrapped
    {
        assert(MockWrap("slUnknownFunction", MockPluginProvider, true) == nullptr);
        assert(MockWrap("slUnknownFunction", MockPluginProvider, false) == nullptr);
        assert(MockWrap(nullptr, MockPluginProvider, true) == nullptr);
    }

    // Test 6: Incomplete plugin provider (missing required hooks) returns nullptr
    {
        auto incompleteProvider = [](const char* name) -> void* {
            if (std::strcmp(name, "slHookPresent") == 0) return (void*)1;
            return nullptr; // Missing getBuffer, getIndex, etc.
        };
        assert(MockWrap("slHookPresent", incompleteProvider, true) == nullptr);
    }

    std::puts("PASS: kcd2_nr_streamline_unit (local Streamline wrapping, KCD2 quirk gating, and legacy/native creation paths)");
    return 0;
}
