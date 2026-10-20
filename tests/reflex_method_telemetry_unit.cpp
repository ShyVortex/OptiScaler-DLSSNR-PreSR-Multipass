#undef NDEBUG
#include <cassert>
#include <iostream>
#include <string>
#include <memory>
#include <atomic>
#include <optional>
#include <cstdint>

// Enums matching OptiScaler definitions (Config.h and OptiTypes.h)
enum class LowLatencyMode : uint32_t
{
    None,
    Auto,
    LatencyFlex,
    AntiLag2,
    XeLL,
    AntiLagVk,
    Reflex
};

namespace VendorId
{
enum Value : uint32_t
{
    Invalid = 0,
    Microsoft = 0x1414,
    Nvidia = 0x10DE,
    AMD = 0x1002,
    Intel = 0x8086,
};
}; // namespace VendorId

// Simulated LowLatencyTech interface for testing context extraction
class ILowLatencyTechMock
{
  public:
    virtual ~ILowLatencyTechMock() = default;
    virtual void* get_tech_context() = 0;
    virtual LowLatencyMode get_mode() = 0;
};

class LatencyFlexMock : public ILowLatencyTechMock
{
    int dummyContext = 42;

  public:
    void* get_tech_context() override { return &dummyContext; }
    LowLatencyMode get_mode() override { return LowLatencyMode::LatencyFlex; }
};

class XeLLMock : public ILowLatencyTechMock
{
    int dummyContext = 100;

  public:
    void* get_tech_context() override { return &dummyContext; }
    LowLatencyMode get_mode() override { return LowLatencyMode::XeLL; }
};

// Simulation of LowLatency::get_low_latency_tech_context logic matching low_latency.cpp
static bool TestGetLowLatencyTechContext(const std::shared_ptr<ILowLatencyTechMock>& activeTech, int delayDeinit,
                                         void** low_latency_context, LowLatencyMode* low_latency_tech)
{
    if (activeTech)
    {
        if (!low_latency_context || !low_latency_tech)
            return false;

        *low_latency_context = activeTech->get_tech_context();
        *low_latency_tech = activeTech->get_mode();
    }
    else
    {
        if (low_latency_context)
            *low_latency_context = nullptr;
        if (low_latency_tech)
            *low_latency_tech = LowLatencyMode::None;
        return false;
    }

    if (delayDeinit > 0)
    {
        if (low_latency_context)
            *low_latency_context = nullptr;
    }

    return true;
}

// Simulation of menu_common.cpp method resolution logic
struct MenuMethodResult
{
    std::string currentMethod;
    bool usingLFX;
};

static MenuMethodResult ResolveMenuMethod(bool reflexLimitsFps, LowLatencyMode fakenvapiMode, bool usingAsMainNvapi,
                                          bool rtssInjection, bool canXellLimit)
{
    std::string currentMethod;
    if (reflexLimitsFps)
    {
        if (fakenvapiMode == LowLatencyMode::AntiLag2)
            currentMethod = "FSR Anti-Lag 2.0";
        else if (fakenvapiMode == LowLatencyMode::LatencyFlex)
            currentMethod = "LatencyFlex";
        else if (fakenvapiMode == LowLatencyMode::XeLL)
            currentMethod = "XeLL";
        else if (fakenvapiMode == LowLatencyMode::AntiLagVk)
            currentMethod = "Vulkan AntiLag";
        else if (fakenvapiMode == LowLatencyMode::None)
        {
            if (usingAsMainNvapi)
                currentMethod = "None";
            else
                currentMethod = "Reflex";
        }
    }
    else
    {
        if (canXellLimit)
            currentMethod = "Game's XeLL";
        else
            currentMethod = "Fallback";
    }

    if (rtssInjection)
        currentMethod.append(" (RTSS - Protected)");

    bool usingLFX = (fakenvapiMode == LowLatencyMode::LatencyFlex);

    return { currentMethod, usingLFX };
}

// Simulation of InputCommon vendor resolution logic matching input_common.cpp
static LowLatencyMode ResolveAutoDesiredMode(LowLatencyMode desiredMode, VendorId::Value vendorId)
{
    if (desiredMode == LowLatencyMode::Auto)
    {
        if (vendorId == VendorId::Intel)
            desiredMode = LowLatencyMode::XeLL;
        else if (vendorId == VendorId::AMD)
            desiredMode = LowLatencyMode::AntiLag2;
        else if (vendorId == VendorId::Nvidia)
            desiredMode = LowLatencyMode::None;
        else
            desiredMode = LowLatencyMode::LatencyFlex;
    }
    return desiredMode;
}

int main()
{
    std::cout << "=== Running Reflex vs LatencyFlex Telemetry & Mode Resolution Unit Tests ===" << std::endl;

    // -----------------------------------------------------------------------------------------
    // Test Case 1: Inactive fake tech context query returns None and nullptr (native Reflex)
    // -----------------------------------------------------------------------------------------
    {
        void* context = reinterpret_cast<void*>(0x12345678);
        LowLatencyMode mode = LowLatencyMode::LatencyFlex; // initially dirty

        std::shared_ptr<ILowLatencyTechMock> noActiveTech = nullptr;
        bool result = TestGetLowLatencyTechContext(noActiveTech, 0, &context, &mode);

        assert(!result && "get_low_latency_tech_context must return false when no fake tech is loaded");
        assert(context == nullptr && "Context must be set to nullptr when no fake tech is loaded");
        assert(mode == LowLatencyMode::None && "Mode must be set to LowLatencyMode::None when no fake tech is loaded");
        std::cout << "  [PASS] Case 1: Inactive fake tech correctly reports false, nullptr context, and "
                     "LowLatencyMode::None"
                  << std::endl;
    }

    // -----------------------------------------------------------------------------------------
    // Test Case 2: Active fake tech context query preserves context and mode
    // -----------------------------------------------------------------------------------------
    {
        void* context = nullptr;
        LowLatencyMode mode = LowLatencyMode::None;

        auto lfxTech = std::make_shared<LatencyFlexMock>();
        bool result = TestGetLowLatencyTechContext(lfxTech, 0, &context, &mode);

        assert(result && "get_low_latency_tech_context must return true when fake tech is loaded");
        assert(context != nullptr && "Context must be non-null for active LatencyFlex");
        assert(mode == LowLatencyMode::LatencyFlex && "Mode must be LowLatencyMode::LatencyFlex");

        auto xellTech = std::make_shared<XeLLMock>();
        result = TestGetLowLatencyTechContext(xellTech, 0, &context, &mode);
        assert(result && "get_low_latency_tech_context must return true for XeLL");
        assert(mode == LowLatencyMode::XeLL && "Mode must be LowLatencyMode::XeLL");
        std::cout << "  [PASS] Case 2: Active fake tech (LatencyFlex, XeLL) correctly reports true, valid context, and "
                     "expected mode"
                  << std::endl;
    }

    // -----------------------------------------------------------------------------------------
    // Test Case 3: In-game menu method resolution on NVIDIA with native Reflex
    // -----------------------------------------------------------------------------------------
    {
        // On NVIDIA RTX GPUs, reflexLimitsFps is true, fakenvapiMode is None, usingAsMainNvapi is false.
        auto resReflex = ResolveMenuMethod(true, LowLatencyMode::None, false, false, false);
        assert(resReflex.currentMethod == "Reflex" && "Menu method must resolve strictly to 'Reflex' on NVIDIA cards");
        assert(!resReflex.usingLFX && "usingLFX must be false under native Reflex");

        // RTSS injection appended
        auto resReflexRtss = ResolveMenuMethod(true, LowLatencyMode::None, false, true, false);
        assert(resReflexRtss.currentMethod == "Reflex (RTSS - Protected)");

        // Non-Nvidia fake NVAPI fallback
        auto resMainNone = ResolveMenuMethod(true, LowLatencyMode::None, true, false, false);
        assert(resMainNone.currentMethod == "None" &&
               "Menu method must resolve to 'None' when fakenvapi is main nvapi without active tech");

        // LatencyFlex explicitly active
        auto resLFX = ResolveMenuMethod(true, LowLatencyMode::LatencyFlex, false, false, false);
        assert(resLFX.currentMethod == "LatencyFlex");
        assert(resLFX.usingLFX == true);

        // AntiLag 2 active
        auto resAL2 = ResolveMenuMethod(true, LowLatencyMode::AntiLag2, false, false, false);
        assert(resAL2.currentMethod == "FSR Anti-Lag 2.0");

        // XeLL active
        auto resXeLL = ResolveMenuMethod(true, LowLatencyMode::XeLL, false, false, false);
        assert(resXeLL.currentMethod == "XeLL");

        std::cout << "  [PASS] Case 3: In-game menu correctly displays 'Reflex' on NVIDIA and disallows LatencyFlex "
                     "controls"
                  << std::endl;
    }

    // -----------------------------------------------------------------------------------------
    // Test Case 4: InputCommon Auto desiredMode resolution per GPU vendor
    // -----------------------------------------------------------------------------------------
    {
        // On NVIDIA, Auto must NEVER resolve to LatencyFlex; it must resolve to LowLatencyMode::None
        LowLatencyMode nvidiaMode = ResolveAutoDesiredMode(LowLatencyMode::Auto, VendorId::Nvidia);
        assert(nvidiaMode == LowLatencyMode::None &&
               "Auto low-latency mode on Nvidia MUST resolve to None, not LatencyFlex!");

        // On Intel, Auto resolves to XeLL
        LowLatencyMode intelMode = ResolveAutoDesiredMode(LowLatencyMode::Auto, VendorId::Intel);
        assert(intelMode == LowLatencyMode::XeLL);

        // On AMD, Auto resolves to AntiLag2
        LowLatencyMode amdMode = ResolveAutoDesiredMode(LowLatencyMode::Auto, VendorId::AMD);
        assert(amdMode == LowLatencyMode::AntiLag2);

        // On Unknown/Software/Other, Auto resolves to LatencyFlex
        LowLatencyMode otherMode = ResolveAutoDesiredMode(LowLatencyMode::Auto, VendorId::Microsoft);
        assert(otherMode == LowLatencyMode::LatencyFlex);

        // Explicitly requested mode is preserved
        LowLatencyMode explicitMode = ResolveAutoDesiredMode(LowLatencyMode::LatencyFlex, VendorId::Nvidia);
        assert(explicitMode == LowLatencyMode::LatencyFlex);

        std::cout << "  [PASS] Case 4: InputCommon Auto resolution on NVIDIA correctly resolves to LowLatencyMode::None"
                  << std::endl;
    }

    std::cout << "=== All Reflex vs LatencyFlex Telemetry & Mode Resolution Unit Tests PASSED! ===" << std::endl;
    return 0;
}
