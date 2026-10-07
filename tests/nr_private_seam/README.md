# NR private seam CPU regression tests

Run from the repository:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tests/nr_private_seam/run.ps1
```

`-Repo` selects the production repository; `-DenoiseSource` selects a separate
DenoiseFirst source snapshot (useful before the feature has been ported).
`-RoutingSource` selects a separate `IFeature_Dx12.cpp` source snapshot.
`-OutputDirectory` selects the generated methods, MSVC build, executable, and
`test-run.txt` transcript and `test-results.txt` assertion log directory. Visual Studio C++ Build Tools are required;
the runner discovers them through vswhere and calls VsDevCmd. No GPU, game,
driver API, or installed third-party test framework is used.

The runner extracts complete, unchanged production bodies for
`DenoiseFirstContext::After`, `State::RetryAfterFailure`, and both private
contexts' `Cancel`, `RetireCurrent`, and `ReleaseResources`. The CPU adapter
substitutes the D3D command/resource boundary, configuration, and GPU completion.
Resource transitions validate their incoming states; the copy boundary validates
both states and the active extent. Retirement retains failed generations until
the test advances completion, matching the lifetime contract.

Regression expectations:

- Failed game upscale on the default path invalidates private history after
  returning the handed-off input to its resting state.
- Successful default upscale retains history; replacement failure and mismatched
  handoffs invalidate history. Successful replacement restores copy states.
- Explicit retry clears ordinary NR failure and releases the ordinary enlarger,
  cancels both private contexts' pending handoffs, and retires their failed
  generations. A subsequent allocation opportunity must produce usable history.

The allocation opportunity in the retry fixture models Before's `!current`
allocation gate, rather than executing GPU allocation. Thus these tests prove
CPU reset/retirement decisions and resource-boundary sequencing, not GPU execution
or private upscaler creation. Failed-generation flags and pending state are
observed through real extracted methods, not source-text assertions.

Scheduling cases execute the actual denoise-ownership and pre/post NR routing
expressions extracted from `IFeature_Dx12.cpp`. Empty warmup or unsupported
handoffs preserve ordinary NR placement; successful color/replacement handoffs
own both seams. Disabled NR, absent shader and specialized placement retain their
existing precedence. A request flag alone must not suppress ordinary NR.

Late-failure traces also execute Before's actual failed-generation gate, successful
NR check segment, and composition/private-upscale failure blocks. The GPU NR
boundary advances successfulDispatches once, then the extracted routing decisions
must prevent a second model evaluation in the same epoch despite an empty output
handoff. Sticky composition failure must reset ordinary NR and allow its placement
to resume on the next frame. These traces cover both ordinary pre- and post-SR
placement and validate composition scratch restoration.

Final review cases execute Before's abandoned-handoff prefix and require both
abandoned and mismatched handoffs to retire their generation without transitioning
the previously lent texture on a different command list. Retirement holds the
generation until completion. The actual Run model-evaluation segment and Before
Run invocation are also extracted: a successful GPU model evaluation followed by
failed NR final composition must report consumed history despite unchanged
successfulDispatches and suppress ordinary NR in that same epoch.

Exit codes: 0 passes, 1 assertion failure, 2 invalid adapter boundary. Build or
source-extraction errors abort the runner and are not evidence of a regression.
