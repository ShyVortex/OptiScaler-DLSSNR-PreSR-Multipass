---
trigger: always_on
description: Implementation planning, task progression, testing, and explicit approval requirements
---

# Implementation Planning & Task Progression Rules

1. **Task Division**:
   - Every implementation plan MUST divide work into clear, discrete, sequential tasks (e.g. Task 1, Task 2, etc.).

2. **Mandatory Unit Tests**:
   - Every implementation plan MUST feature automated unit tests to validate each component or enhancement.
   - All existing and new unit tests must compile and pass cleanly before considering a task or implementation complete.

3. **Explicit Approval Requirements**:
   - **Pre-execution Approval**: The agent MUST NOT proceed to execution without explicit user approval of the implementation plan.
   - **Inter-task Approval**: The agent MUST pause and obtain explicit user approval between tasks before proceeding to subsequent tasks.
