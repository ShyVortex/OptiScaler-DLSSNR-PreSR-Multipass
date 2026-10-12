---
trigger: always_on
description: Implementation planning, task progression, testing, and explicit approval requirements
---

# Implementation Planning & Task Progression Rules

1. **Task Division**:
   - Every implementation plan MUST divide work into clear, discrete, sequential tasks (e.g. Task 1, Task 2, etc.).

2. **Mandatory Dedicated Implementation Plan File in Brain**:
   - Every implementation plan MUST be written to and maintained in a dedicated file in the agent's brain directory (`<appDataDir>/brain/<conversation-id>/`, e.g., `implementation_plan.md` or a descriptive task plan file) before execution begins.
   - The agent MUST keep this plan file continuously updated, marking tasks as completed (`[x]`) as work progresses, so that the agent and user can always reference it across turns, model changes, and context compactions without losing execution state or confusing ongoing initiatives.

3. **Mandatory Issue Triage & Validity Documentation**:
   - Every implementation plan MUST explicitly document:
     - **Reported Issue**: The exact bug, defect, or unexpected behavior reported by the user or discovered in the field.
     - **Issue Validity Assessment**: A rigorous analysis verifying whether the issue is a genuine defect/bug in the codebase vs. a user misconfiguration, unsupported environment, or operational error.
     - **Solution Applied / Proposed**: The precise technical mechanism and architectural changes designed to solve the underlying defect.

4. **Mandatory Issue Documentation in `docs/issues/`**:
   - Every resolved or investigated defect/issue MUST be preserved in a dedicated markdown document under the `docs/issues/` directory, organized into domain subfolders:
     - `docs/issues/SR/` — Super Resolution (DLSS, FSR, XeSS)
     - `docs/issues/FG/` — Frame Generation (DLSS-G, FSR-FG, XeFG, Smooth Motion, external proxies)
     - `docs/issues/RR/` — Ray Reconstruction (DLSS-D)
     - `docs/issues/NR/` — Neural Rendering (DLSS-NR)
   - The document must record the environment, reported symptoms, validity assessment, root cause, applied solution, and test verification.
   - **Mandatory Hardware & OS Accuracy Verification**:
     - When writing the Overview & Environment section of any report under `docs/issues/`, the agent MUST ALWAYS actively check and verify real system and hardware details using terminal commands (e.g. `uname -r`, `/etc/os-release`, `nvidia-smi`) before authoring the document.
     - Never guess, estimate, or copy stale/placeholder OS distributions, kernel versions, GPU models, or driver versions into issue documentation. Accurate, verified environmental metadata is mandatory.

5. **Mandatory Unit Tests**:
   - Every implementation plan MUST feature automated unit tests to validate each component or enhancement.
   - All existing and new unit tests must compile and pass cleanly before considering a task or implementation complete.

6. **Explicit Approval Requirements**:
   - **Pre-execution Approval**: The agent MUST NOT proceed to execution without explicit user approval of the implementation plan.
   - **Inter-task Approval**: The agent MUST pause and obtain explicit user approval between tasks before proceeding to subsequent tasks.

7. **Mandatory User Confirmation Gate Before Updating `.agents` Architecture & Context**:
   - `.agents/rules/optiscaler-architecture.md` and `.agents/CONTEXT.md` represent verified institutional memory and permanent architectural invariants.
   - **Strict Prohibition on Premature Updates**: The agent MUST NEVER update `.agents/rules/optiscaler-architecture.md` or `.agents/CONTEXT.md` during development, triage, or unverified task completion.
   - **Explicit User Confirmation Required**: These files MUST ONLY be updated **after** the user explicitly tests and confirms in real gameplay/execution that the solution or feature is working as intended.
   - **Active Investigation Artifacts**: During implementation and testing, all triage, findings, root cause analysis, and test results belong strictly in `docs/issues/`, unit tests (`tests/`), and the implementation plan.
