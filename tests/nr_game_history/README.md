# Main game upscaler history transitions

Run `powershell.exe -NoProfile -ExecutionPolicy Bypass -File tests/nr_game_history/run.ps1`.
Optional `-Repo`, `-Source` and `-OutputDirectory` select production input and
generated evidence. Visual Studio C++ Build Tools are required. No GPU or game
calls occur.

The runner compiles the actual game evaluation segment from IFeature_Dx12.cpp,
including temporal Reset and jitter overrides, against a recording NGX/GPU
boundary. The fixture does not implement route reset behavior. Literal frame
sequences verify ordinary/composite/clean/replacement transitions, stable frames,
failed transitions and their retries, caller Reset preservation, unset Reset's
semantic zero, and scoped jitter restoration.
