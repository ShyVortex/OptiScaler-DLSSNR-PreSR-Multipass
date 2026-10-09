---
trigger: always_on
description: Mandatory standards and rigorous methodology for analyzing, reviewing, and validating Pull Requests
---

# Pull Request Review & Validation Standards

When analyzing, reviewing, or validating pull requests (whether submitted by external contributors, team members, or automated tools), the following rules MUST ALWAYS be followed. Never give a blanket approval or declare a PR "excellent / ready to merge" based solely on compilation, clean formatting, or isolated mock test passes.

---

## 1. Mandatory Cross-Referencing Against Issue Documentation (`docs/issues/`)

1. **Active Cross-Reference Required**:
   - Every PR review MUST search and cross-reference all touched files, subsystems, and logic against existing documentation in `docs/issues/`:
     - `docs/issues/FG/` — Frame Generation (DLSS-G, FSR-FG, XeFG, Smooth Motion, external proxies, pacing detours)
     - `docs/issues/NR/` — Neural Rendering (DLSS-NR, Pre-SR/Post-SR pipelines, multi-mip textures, format bouncing)
     - `docs/issues/SR/` — Super Resolution (DLSS, FSR, XeSS)
     - `docs/issues/RR/` — Ray Reconstruction (DLSS-D)
2. **Verify Reverse-Engineered Contracts**:
   - Check if any modified code was originally implemented to fix a documented defect, engine quirk, or hardware limitation.
   - If a PR alters timing, gating, pacing, admission, configuration defaults, or hooks, verify whether any previous issue resolutions or reverse-engineered fixes depend on those exact mechanisms.
   - Never accept a PR that weakens, comments out, or bypasses an architectural invariant established in `docs/issues/`.

---

## 2. Skepticism of Isolated Mock & Synthetic Unit Tests

1. **Mocks Prove Syntax, Not Runtime Reality**:
   - Standalone unit tests with synthetic mocks or isolated boundaries (e.g. tests using mock modules, simulated buffers, or fake state objects) prove only that the code compiles and satisfies the author's own mock assumptions.
   - Passing mock unit tests do **NOT** prove runtime validity or correctness in a real graphics engine, DirectX/Vulkan runtime, or GPU driver pipeline.
2. **Scrutinize What Mocks Abstract Away**:
   - Explicitly analyze what the mock omits:
     - Real GPU presentation worker threads (such as `libxess_fg.dll` internal presentation loops).
     - Asynchronous swapchain presentation pacing and frame deadline scheduling.
     - Driver resource tagging (`D3D12TagFrameResource`), motion vector availability, and buffer lifecycle transitions.
     - Dynamic user interaction via the in-game ImGui menu overlay.
3. **Never Accept "Tests Pass" as Proof of Correctness**:
   - Any PR asserting that a change is "verified" solely by newly added mock tests must be subjected to aggressive end-to-end trace analysis.

---

## 3. End-to-End Operational Trace Analysis

For every modified line of code in a PR, the review must trace the complete operational call graph from entry point down to presentation and driver execution:

1. **Default Configuration vs. Explicit Configuration**:
   - Trace the exact behavior when a setting is at its default value in `Config.h` and `OptiScaler.ini`, as well as when explicitly set by the user.
   - Watch out for semantic confusion where a PR author equates an optional outer feature (e.g., DXGI VBlank lock) with an internal provider repair (e.g., native thunk pacing).
2. **In-Game Toggle & Mode Switching Lifecycles**:
   - Trace what happens when a feature is toggled Off in-game (`eOff`, cutscenes, loading screens, menus):
     - Does the provider runtime remain enabled in the background while game inputs stop?
     - Does this cause error storms (`MISMATCH_INPUT_RESOURCES`, `INVALID_ARGUMENT`, missing motion vector errors)?
     - Does the pipeline cleanly disable the hardware swapchain context on `eOff` and re-enable it on `eOn`?
3. **Frame Timing Deltas & Provider Math**:
   - Trace how frame times and delta times are calculated and passed to provider libraries (e.g. `frameRenderTime`).
   - Does any code path collapse delta time to `0.0f` or unmetered ballooning?
   - If a provider is fed `0.0f`, can its interpolation/motion extrapolation models function? (If not, frames will be unextrapolated duplicates).
4. **Live In-Game Menu Adaptability**:
   - Can the user still change settings, multipliers, and features live in the in-game ImGui overlay during active gameplay?
   - Did the PR introduce static/session immutability locks (e.g. `static const bool`, stripping live memory patching via `VirtualProtect`, or ignoring live slider adjustments)?
   - If a setting claims to require a restart, verify whether live adaptation is technically possible and was previously supported.

---

## 4. Preservation of Core Functionality & Anti-Regression Invariants

Scrutinize all PR claims of "cleaning up", "enforcing immutability", or "honoring config defaults":

1. **Core Provider Repairs Must Never Be Optional**:
   - Distinguish internal engine repair detours from optional wrapper features:
     - Internal memory patches and thunk detours that fix vendor driver bugs (e.g., `XeFGPacing.h` fixing `libxess_fg.dll`'s unmetered burst clumping) are **mandatory** whenever the unlocker is active.
     - They must **never** be gated behind optional user switches (such as DXGI VBlank synchronization `ExtraPacing`).
2. **Preserve Dynamic Memory Patching**:
   - Features that support live in-memory updates (such as multiplier ceiling byte rewriting in `XeMfgLoader::SetMaxGeneratedFrames`) must never be gutted or converted to restart-only locks.
3. **Preserve Dynamic Multi-Frame Generation (DMFG)**:
   - Capability queries (`hkslDLSSGGetState`, `canEnableDynamic`) for synthetic or emulation paths must not be gated behind rigid native checks (e.g. `fg->GetDMFGSupport()`) that return false for non-native implementations.

---

## 5. Mandatory PR Review Report Structure

Every PR analysis, review, or validation presented to the user MUST contain the following structured sections:

1. **PR Overview & Scope**:
   - Summary of changes, touched files, and contributor's stated intent.
2. **Architectural & End-to-End Impact Analysis**:
   - Detailed call-trace analysis of each changed function, verifying behavior across all operating modes (default config, non-default config, in-game toggles, runtime menu changes).
3. **Issue Cross-Reference Matrix**:
   - Explicit audit against related issue documents in `docs/issues/` (citing specific files and defect numbers).
4. **Regression Risk Assessment**:
   - Concrete breakdown of potential breakages, runtime lockouts, frame clumping, error storms, or performance/visual degradations.
5. **Verdict with Actionable Recommendation**:
   - Explicitly conclude with:
     - **APPROVE**: Only when all end-to-end paths are verified, no documented issues are regressed, and live adaptability is preserved.
     - **REQUEST CHANGES**: List the precise code modifications, file paths, and lines the author must adjust before merging.
     - **REJECT**: Explain the fundamental architectural flaw why the PR cannot be accepted.
