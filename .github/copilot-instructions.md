# Repository Guidelines

Primary project context is maintained in `.agents/CONTEXT.md`. Always review that file for overarching project goals.

## Mandatory Planning Gate
Before proposing a fix, drafting a patch, or executing any code changes, the agent must read and follow `.agents/rules/workflow-planning.md`.

This is a hard requirement:
- read the planning workflow before any code-change plan is made
- create or update the implementation plan in the app brain directory (for example `implementation_plan.md` in the conversation's brain folder)
- keep the plan task-scoped, sequential, and updated as work progresses
- wait for explicit user approval before executing implementation steps
- document the issue validity assessment and the solution approach in the plan and the relevant `docs/issues/` file when applicable

## Module Guidelines
When working on this codebase, adhere strictly to the rules defined in `.agents/rules/`:

- **C++ & Build System:** Refer to `.agents/rules/cpp-msvc.md`
- **Architecture & Hooks:** Refer to `.agents/rules/optiscaler-architecture.md`
- **CI & Workflows:** Refer to `.agents/rules/ci-workflows.md`
- **Task Planning:** Refer to `.agents/rules/workflow-planning.md`
- **Pull Requests:** Refer to `.agents/rules/pr-review.md`
- **Browser / Research Tools:** Refer to `.agents/rules/browser-tools.md`
- **AI Commit Co-Authorship:** Refer to `.agents/rules/ai-commit-coauthor.md`