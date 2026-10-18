#include <cassert>
#include <cstdint>
#include <cstdio>

// Mock of shortcut debouncing logic from menu_common.cpp
struct ShortcutDebounceSystem
{
    uint64_t lastInputTick = 0;
    static constexpr uint64_t debounceThreshold = 500;
    bool capturingKey = false;

    bool inputMenu = false;
    bool inputDlssNr = false;

    // Simulated key release state
    int releasedKey = 0;

    void ProcessFrame(uint64_t currentTick, int pressedShortcutVk, bool isKeyReleased)
    {
        const bool canAcceptInputs = lastInputTick + debounceThreshold < currentTick;

        auto CheckShortcut = [&](int vk, bool& inputFlag)
        {
            if (inputFlag)
                return;
            if (vk <= 0 || vk >= 256)
                return;

            if (isKeyReleased && pressedShortcutVk == vk)
            {
                inputFlag = true;
                lastInputTick = currentTick;
            }
        };

        if (!capturingKey && canAcceptInputs)
        {
            CheckShortcut(0x24 /* VK_HOME / Menu */, inputMenu);
            CheckShortcut(0x76 /* VK_F7 / DlssNr */, inputDlssNr);
        }
        else if (capturingKey)
        {
            lastInputTick = currentTick;
        }
    }
};

int main()
{
    std::printf("=== Running Shortcut Debounce Unit Tests ===\n");

    // Case 1: Initial key trigger accepts input and updates lastInputTick
    {
        ShortcutDebounceSystem sys;
        assert(sys.lastInputTick == 0);

        // Frame at tick 1000: User presses and releases VK_F7 (DLSS-NR toggle)
        sys.ProcessFrame(1000, 0x76, true);
        assert(sys.inputDlssNr && "DLSS-NR toggle should be accepted on initial press");
        assert(sys.lastInputTick == 1000 && "lastInputTick must be updated to currentTick (1000)");
        std::printf("  [PASS] Case 1: Initial trigger accepted and sets lastInputTick\n");

        // Reset input flag as menu handling does
        sys.inputDlssNr = false;

        // Case 2: Immediate subsequent key press at tick 1050 (+50ms) is rejected by debounce
        sys.ProcessFrame(1050, 0x76, true);
        assert(!sys.inputDlssNr && "Rapid key press within 50ms must be rejected by debounce");
        assert(sys.lastInputTick == 1000 && "lastInputTick must not change on rejected press");
        std::printf("  [PASS] Case 2: Rapid key press within 50ms rejected by debounce\n");

        // Case 3: Key press at tick 1499 (+499ms) is still within debounce window and rejected
        sys.ProcessFrame(1499, 0x76, true);
        assert(!sys.inputDlssNr && "Key press at 499ms must still be rejected");
        std::printf("  [PASS] Case 3: Key press at 499ms boundary still debounced\n");

        // Case 4: Key press at tick 1501 (+501ms) exceeds debounceThreshold (500ms) and is accepted
        sys.ProcessFrame(1501, 0x76, true);
        assert(sys.inputDlssNr && "Key press after debounce window must be accepted");
        assert(sys.lastInputTick == 1501 && "lastInputTick must be updated to new tick (1501)");
        std::printf("  [PASS] Case 4: Key press at 501ms accepted and updates lastInputTick\n");
    }

    // Case 5: Different shortcut keys respect debounce window (e.g. Menu pressed right after NR)
    {
        ShortcutDebounceSystem sys;
        sys.ProcessFrame(2000, 0x76, true); // NR toggle
        assert(sys.inputDlssNr);
        sys.inputDlssNr = false;

        // User immediately presses Menu key at tick 2100 (+100ms)
        sys.ProcessFrame(2100, 0x24, true);
        assert(!sys.inputMenu && "Menu key within 100ms of another shortcut must be debounced");

        // After cooldown expires at tick 2501 (+501ms)
        sys.ProcessFrame(2501, 0x24, true);
        assert(sys.inputMenu && "Menu key accepted after debounce window expires");
        std::printf("  [PASS] Case 5: Cross-shortcut debounce protection verified\n");
    }

    // Case 6: Capturing key mode continues to update lastInputTick
    {
        ShortcutDebounceSystem sys;
        sys.capturingKey = true;
        sys.ProcessFrame(3000, 0x76, true);
        assert(!sys.inputDlssNr && "Shortcuts must not fire while capturing keys");
        assert(sys.lastInputTick == 3000 && "Capturing mode must update lastInputTick");
        std::printf("  [PASS] Case 6: Capturing key mode behavior verified\n");
    }

    std::printf("=== All Shortcut Debounce Unit Tests PASSED ===\n");
    return 0;
}
