#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>

// Mock NVSDK return codes
constexpr unsigned int NVSDK_NGX_Result_Success = 0x1;
constexpr unsigned int NVSDK_NGX_Result_Fail = 0xBAD00001;

struct MockConfig
{
    bool dlssNrRunBeforeSr = true;
    bool dlssNrEnabled = true;
};

struct MockModel
{
    bool hasFeature = false;
    bool prepareShouldFail = false;

    void RetryAfterFailure() { prepareShouldFail = false; }

    unsigned int Prepare(bool beforeUpscale)
    {
        if (prepareShouldFail)
            return NVSDK_NGX_Result_Fail;
        hasFeature = true;
        return NVSDK_NGX_Result_Success;
    }
};

struct MockNrState
{
    bool failed = false;
    const char* reason = "";
    bool passCreateFailed[2] { false, false };
    MockModel models[2];

    void RetryAfterFailure()
    {
        failed = false;
        reason = "";
        for (auto& m : models)
            m.RetryAfterFailure();
        passCreateFailed[0] = false;
        passCreateFailed[1] = false;
    }

    bool PrepareRunModels(bool beforeUpscale, uint32_t workWidth, uint32_t workHeight)
    {
        for (unsigned int pass = 0; pass < 1; ++pass)
        {
            if (passCreateFailed[pass])
                break;

            const auto prepared = models[pass].Prepare(beforeUpscale);
            if (prepared != NVSDK_NGX_Result_Success)
            {
                passCreateFailed[pass] = true;
                if (pass == 0)
                {
                    failed = true;
                    if (!beforeUpscale)
                    {
                        reason = "the NVIDIA NGX driver could not create Neural Rendering at display resolution (try "
                                 "enabling 'Generate model before upscale' or reducing Working Scale)";
                    }
                    else
                    {
                        reason = "the NVIDIA NGX driver could not create Neural Rendering";
                    }
                }
                return false;
            }
        }
        return true;
    }
};

int main()
{
    std::cout << "Running DLSS-NR Status Reporting & Guidance Unit Tests...\n";

    // Test 1: Pre-SR failure produces standard error
    {
        MockNrState state;
        state.models[0].prepareShouldFail = true;

        bool ok = state.PrepareRunModels(true, 1505, 847);
        assert(!ok);
        assert(state.failed);
        assert(std::string(state.reason) == "the NVIDIA NGX driver could not create Neural Rendering");
        assert(strstr(state.reason, "display resolution") == nullptr);
        std::cout << "  [PASS] Test 1: Pre-SR failure produces concise driver error\n";
    }

    // Test 2: Post-SR failure at display resolution provides actionable guidance
    {
        MockNrState state;
        state.models[0].prepareShouldFail = true;

        bool ok = state.PrepareRunModels(false, 2560, 1440);
        assert(!ok);
        assert(state.failed);
        assert(strstr(state.reason, "display resolution") != nullptr);
        assert(strstr(state.reason, "Generate model before upscale") != nullptr);
        std::cout << "  [PASS] Test 2: Post-SR failure contains display resolution guidance\n";
    }

    // Test 3: Quick action button workflow switches to Pre-SR and retries successfully
    {
        MockConfig config;
        config.dlssNrRunBeforeSr = false; // User had post-SR selected

        MockNrState state;
        state.models[0].prepareShouldFail = true;

        // Run post-SR: fails
        state.PrepareRunModels(config.dlssNrRunBeforeSr, 2560, 1440);
        assert(state.failed);
        const char* reason = state.reason;

        // UI detects failure with display resolution suggestion
        bool showQuickFix = (!config.dlssNrRunBeforeSr && strstr(reason, "display resolution") != nullptr);
        assert(showQuickFix);

        // Click quick fix button:
        config.dlssNrRunBeforeSr = true;
        state.RetryAfterFailure();
        assert(!state.failed);
        assert(state.reason == nullptr || state.reason[0] == '\0');

        // In Pre-SR at render resolution, driver succeeds
        state.models[0].prepareShouldFail = false;
        bool retryOk = state.PrepareRunModels(config.dlssNrRunBeforeSr, 1505, 847);
        assert(retryOk);
        assert(!state.failed);
        assert(state.models[0].hasFeature);
        std::cout << "  [PASS] Test 3: Quick action button workflow successfully switches to Pre-SR\n";
    }

    std::cout << "\nAll DLSS-NR Status Reporting & Guidance Unit Tests passed successfully!\n";
    return 0;
}
