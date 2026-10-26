# Repository Guidelines

OpenCode project guidance. Shared with Claude via `CLAUDE.md`.

Main project context and rules are loaded via `opencode.jsonc` `instructions`
(`.agents/CONTEXT.md` and `.agents/rules/*.md`). Read them before changing code.

## Mandatory Planning Gate
Before any implementation plan or code change, read `.agents/rules/workflow-planning.md` and create/update the implementation plan in the brain directory for the active conversation.

The plan must:
- define a clear task sequence with atomic, independently verifiable steps
- include the reported issue, validity assessment, and solution approach
- be updated as tasks progress
- receive explicit user approval before implementation proceeds

## Operational & Architecture Rules
- `.agents/rules/cpp-msvc.md`
- `.agents/rules/optiscaler-architecture.md`
- `.agents/rules/ci-workflows.md`
- `.agents/rules/workflow-planning.md`
- `.agents/rules/pr-review.md`
- `.agents/rules/browser-tools.md`
- `.agents/rules/ai-commit-coauthor.md`
