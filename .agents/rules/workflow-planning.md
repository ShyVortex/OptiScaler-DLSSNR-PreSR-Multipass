---
trigger: always_on
description: Implementation planning, task progression, testing, and explicit approval requirements
---

# Implementation Planning & Task Progression Rules

1. **Task Division**:
   - Every implementation plan MUST divide work into clear, discrete, sequential tasks (e.g. Task 1, Task 2, etc.).

2. **Mandatory Issue Triage & Validity Documentation**:
   - Every implementation plan MUST explicitly document:
     - **Reported Issue**: The exact bug, defect, or unexpected behavior reported by the user or discovered in the field.
     - **Issue Validity Assessment**: A rigorous analysis verifying whether the issue is a genuine defect/bug in the codebase vs. a user misconfiguration, unsupported environment, or operational error.
     - **Solution Applied / Proposed**: The precise technical mechanism and architectural changes designed to solve the underlying defect.

3. **Mandatory Issue Documentation in `docs/issues/`**:
   - Every resolved or investigated defect/issue MUST be preserved in a dedicated markdown document under the `docs/issues/` directory, organized into domain subfolders:
     - `docs/issues/SR/` — Super Resolution (DLSS, FSR, XeSS)
     - `docs/issues/FG/` — Frame Generation (DLSS-G, FSR-FG, XeFG, Smooth Motion, external proxies)
     - `docs/issues/RR/` — Ray Reconstruction (DLSS-D)
     - `docs/issues/NR/` — Neural Rendering (DLSS-NR)
   - The document must record the environment, reported symptoms, validity assessment, root cause, applied solution, and test verification.

4. **Mandatory Unit Tests**:
   - Every implementation plan MUST feature automated unit tests to validate each component or enhancement.
   - All existing and new unit tests must compile and pass cleanly before considering a task or implementation complete.

5. **Explicit Approval Requirements**:
   - **Pre-execution Approval**: The agent MUST NOT proceed to execution without explicit user approval of the implementation plan.
   - **Inter-task Approval**: The agent MUST pause and obtain explicit user approval between tasks before proceeding to subsequent tasks.


