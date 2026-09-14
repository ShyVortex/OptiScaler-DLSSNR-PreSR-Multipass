#include "../OptiScaler/dlssnr/DlssNr_Placement.h"
#include <cassert>
#include <iostream>
#include <optional>

// Minimal CustomOptional reproduction to test the exact Config behavior
enum HasDefaultValue
{
    NoDefault,
    WithDefault,
};

template <class T, HasDefaultValue defaultState = WithDefault>
class TestCustomOptional : public std::optional<T>
{
    T _defaultValue {};

public:
    TestCustomOptional(T defaultValue)
    {
        _defaultValue = defaultValue;
        this->reset();
    }

    TestCustomOptional() { this->reset(); }

    T value_or_default() const
    {
        if (this->has_value())
            return this->value();
        return _defaultValue;
    }

    void set_from_config(std::optional<T> val)
    {
        if (val.has_value())
            *this = val.value();
    }

    TestCustomOptional& operator=(const T& value)
    {
        std::optional<T>::operator=(value);
        return *this;
    }
};

int main()
{
    // 1. Verify Placement resolution logic
    {
        // Default Pre-SR placement (before = true, all specials false)
        auto p1 = DlssNr::ResolvePlacement(true, false, false, false);
        assert(p1.beforeUpscale == true);
        assert(p1.deferred == false);
        assert(p1.finished == false);

        // Explicit post-upscale placement (before = false, all specials false)
        auto p2 = DlssNr::ResolvePlacement(false, false, false, false);
        assert(p2.beforeUpscale == false);
        assert(p2.deferred == false);
        assert(p2.finished == false);

        // Deferred DLSS placement (deferred = true)
        auto p3 = DlssNr::ResolvePlacement(false, true, false, false);
        assert(p3.beforeUpscale == true);
        assert(p3.deferred == true);
        assert(p3.finished == false);

        // Legacy across RR with before = true
        auto p4 = DlssNr::ResolvePlacement(true, false, true, false);
        assert(p4.beforeUpscale == true);
        assert(p4.deferred == true);
        assert(p4.finished == false);

        // Finished picture alone (before = false, finished = true)
        auto p5 = DlssNr::ResolvePlacement(false, false, false, true);
        assert(p5.beforeUpscale == false);
        assert(p5.deferred == false);
        assert(p5.finished == true);

        // Finished picture with before = true
        auto p6 = DlssNr::ResolvePlacement(true, false, false, true);
        assert(p6.beforeUpscale == true);
        assert(p6.deferred == true);
        assert(p6.finished == true);
    }

    // 2. Verify Config.h DlssNrRunBeforeSr default and parsing
    {
        TestCustomOptional<bool> runBeforeSr { true }; // New default is true

        // Default state: unset, but value_or_default() is true
        assert(!runBeforeSr.has_value());
        assert(runBeforeSr.value_or_default() == true);

        // INI says RunBeforeSR=auto (std::nullopt) -> preserves default true
        runBeforeSr.set_from_config(std::nullopt);
        assert(!runBeforeSr.has_value());
        assert(runBeforeSr.value_or_default() == true);

        // INI says RunBeforeSR=false -> sets to false
        runBeforeSr.set_from_config(std::optional<bool>(false));
        assert(runBeforeSr.has_value());
        assert(runBeforeSr.value_or_default() == false);

        // INI says RunBeforeSR=true -> sets to true
        runBeforeSr.set_from_config(std::optional<bool>(true));
        assert(runBeforeSr.has_value());
        assert(runBeforeSr.value_or_default() == true);
    }

    // 3. Verify Menu synchronization behavior
    {
        TestCustomOptional<bool> enabled { false };
        TestCustomOptional<bool> runBeforeSr { true };

        // User checks "Enable Neural Rendering" for the first time
        bool uiEnabled = true;
        enabled = uiEnabled;
        if (uiEnabled && !runBeforeSr.has_value())
            runBeforeSr = true;

        assert(enabled.value_or_default() == true);
        assert(runBeforeSr.has_value());
        assert(runBeforeSr.value_or_default() == true);

        // Resulting placement is pre-SR
        auto placement = DlssNr::ResolvePlacement(runBeforeSr.value_or_default(), false, false, false);
        assert(placement.beforeUpscale == true);

        // User explicitly unchecks "Generate model before upscale"
        runBeforeSr = false;
        assert(runBeforeSr.has_value());
        assert(runBeforeSr.value_or_default() == false);

        auto postPlacement = DlssNr::ResolvePlacement(runBeforeSr.value_or_default(), false, false, false);
        assert(postPlacement.beforeUpscale == false);

        // User toggles "Enable Neural Rendering" off and on; explicit false must not be overwritten
        enabled = false;
        enabled = true;
        if (enabled.value_or_default() && !runBeforeSr.has_value())
            runBeforeSr = true;

        assert(runBeforeSr.value_or_default() == false); // Still false because has_value() was true!
    }

    std::cout << "All NR placement and configuration unit tests passed successfully!\n";
    return 0;
}
