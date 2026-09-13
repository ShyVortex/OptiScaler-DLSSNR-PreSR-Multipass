# NR initialization diagnostic build

Use this build when the menu reports that the NVIDIA NGX driver could not create NR. A game crash is not required.

1. Close the game. Replace its loaded OptiScaler proxy with this package's DLL, keeping the existing proxy name and INI.
2. Set `[Log] LogToFile=true` and `LogLevel=2` (Info). Trace logging is unnecessary.
3. Start the game, enable NR and wait for the failure. Press **Retry** once if needed, then exit normally.
4. Send the complete `OptiScaler.log` from that run.

`NR diagnostic` entries include the driver dispatcher, NGX search paths, candidate/loaded NR files with versions and SHA-256, command-list device/LUID, device-removal status, creation settings, returned handle and error. A candidate file is not proof the driver accepted it. File hashes are cached until size or modification time changes.

NVIDIA logging is captured around driver initialization, capability lookup and NR creation, including callbacks from worker threads. Capture is limited to 512 lines per window. The game's callback and other logging sinks are retained; ordinary NR evaluations do not trigger this capture. No runtime is loaded or replaced by the inspector.

The patch adds diagnostics, not a claimed fix for `0xBAD0000B`. It retains the Starfield tracking fix. Choose the RTX 40 MFG package only if the unlock is wanted; its runtime toggle still defaults off.
