#include <cassert>
#include <cstdio>
#include <string>

struct Version {
    int major = 0;
    int minor = 0;
    int patch = 0;

    bool operator>=(const Version& other) const {
        if (major != other.major) return major > other.major;
        if (minor != other.minor) return minor > other.minor;
        return patch >= other.patch;
    }
};

struct MenuDmfgState {
    bool isExternalAmpere = false;
    bool hasDynamicMfgSupport = false;
    bool dlssgGameDMFGSupported = false;
    Version streamlineVersion{ 2, 4, 0 };
    bool dynamicMfgChecked = false;
};

struct MenuDmfgResult {
    bool checkboxEnabled = false;
    std::string helpMessage;
    bool sliderVisible = false;
};

MenuDmfgResult EvaluateSm86MenuDmfg(const MenuDmfgState& state) {
    MenuDmfgResult res;
    if (!state.hasDynamicMfgSupport) {
        res.checkboxEnabled = false;
        res.helpMessage = "Disabled: requires SilyNoMeta's fork of dlssg_sm86 (or a build supporting DynamicMFG).\n"
                          "Upstream sdli1995 does not support dynamic mode yet.";
        res.sliderVisible = false;
    } else {
        res.checkboxEnabled = true;
        res.helpMessage = "Requests dynamic multi-frame generation pacing in SilyNoMeta's dlssg_sm86.";
        res.sliderVisible = state.dynamicMfgChecked;
    }
    return res;
}

MenuDmfgResult EvaluateNativeStreamlineMenuDmfg(const MenuDmfgState& state) {
    MenuDmfgResult res;
    const bool canEnable = state.dlssgGameDMFGSupported || (state.streamlineVersion >= Version{ 2, 11, 0 });
    if (!canEnable) {
        res.checkboxEnabled = false;
        res.helpMessage = "Disabled: requires Streamline 2.11+ (sl.dlss_g.dll) and supporting driver runtime.";
        res.sliderVisible = false;
    } else {
        res.checkboxEnabled = true;
        res.helpMessage = "Forces Dynamic MFG in Streamline DLSSG";
        res.sliderVisible = state.dynamicMfgChecked;
    }
    return res;
}

int main() {
    std::printf("=== Running Dynamic MFG Menu Logic & UI State Unit Tests ===\n");

    // Case 1: External Ampere mod with upstream sdli1995 (no DynamicMFG support)
    {
        MenuDmfgState s;
        s.isExternalAmpere = true;
        s.hasDynamicMfgSupport = false;

        auto res = EvaluateSm86MenuDmfg(s);
        assert(!res.checkboxEnabled && "Checkbox must be disabled on sdli1995");
        assert(res.helpMessage.find("requires SilyNoMeta's fork") != std::string::npos);
        assert(!res.sliderVisible && "Slider must not be visible when disabled");
        std::printf("  [PASS] Case 1: External mod with sdli1995 renders disabled checkbox with SilyNoMeta requirement\n");
    }

    // Case 2: External Ampere mod with SilyNoMeta fork (DynamicMFG supported), unchecked
    {
        MenuDmfgState s;
        s.isExternalAmpere = true;
        s.hasDynamicMfgSupport = true;
        s.dynamicMfgChecked = false;

        auto res = EvaluateSm86MenuDmfg(s);
        assert(res.checkboxEnabled && "Checkbox must be enabled on SilyNoMeta");
        assert(!res.sliderVisible && "Slider must remain hidden when checkbox is unchecked");
        std::printf("  [PASS] Case 2: SilyNoMeta fork allows enabling, slider hidden when unchecked\n");
    }

    // Case 3: External Ampere mod with SilyNoMeta fork, checked
    {
        MenuDmfgState s;
        s.isExternalAmpere = true;
        s.hasDynamicMfgSupport = true;
        s.dynamicMfgChecked = true;

        auto res = EvaluateSm86MenuDmfg(s);
        assert(res.checkboxEnabled && "Checkbox must be enabled on SilyNoMeta");
        assert(res.sliderVisible && "Slider must be revealed when checkbox is checked");
        std::printf("  [PASS] Case 3: SilyNoMeta fork reveals target FPS slider when checked\n");
    }

    // Case 4: Native Streamline with older Streamline (Cyberpunk 2077 default: 2.4.0)
    {
        MenuDmfgState s;
        s.streamlineVersion = { 2, 4, 0 };
        s.dlssgGameDMFGSupported = false;

        auto res = EvaluateNativeStreamlineMenuDmfg(s);
        assert(!res.checkboxEnabled && "Checkbox must be disabled on older Streamline");
        assert(res.helpMessage.find("requires Streamline 2.11+") != std::string::npos);
        assert(!res.sliderVisible && "Slider must not be visible");
        std::printf("  [PASS] Case 4: Cyberpunk 2077 (Streamline 2.4) renders disabled checkbox with Streamline 2.11+ requirement\n");
    }

    // Case 5: Native Streamline upgraded to 2.11+ or Blackwell/patched Ada
    {
        MenuDmfgState s;
        s.streamlineVersion = { 2, 11, 0 };
        s.dlssgGameDMFGSupported = true;
        s.dynamicMfgChecked = false;

        auto res1 = EvaluateNativeStreamlineMenuDmfg(s);
        assert(res1.checkboxEnabled && "Checkbox must be enabled on Streamline 2.11+");
        assert(!res1.sliderVisible && "Slider hidden when unchecked");

        s.dynamicMfgChecked = true;
        auto res2 = EvaluateNativeStreamlineMenuDmfg(s);
        assert(res2.checkboxEnabled);
        assert(res2.sliderVisible && "Slider visible when checked");
        std::printf("  [PASS] Case 5: Streamline 2.11+ allows enabling and reveals slider conditionally\n");
    }

    std::printf("=== All Dynamic MFG Menu Logic Unit Tests PASSED! ===\n");
    return 0;
}
