---
trigger: always_on
description: AI-assisted commit co-authorship requirements
---

# AI-Assisted Commit Co-Authorship

When an AI coding assistant contributes to a commit, the commit message MUST include a `Co-authored-by` trailer identifying the tool (and model, when known) that assisted.

## Required trailers by tool

- **Antigravity (Gemini)**:
  `Co-authored-by: Antigravity <noreply@google.com>` (or the active Gemini model name)
- **Claude (Anthropic)**:
  `Co-authored-by: Claude <noreply@anthropic.com>` (optionally with model, e.g. `Claude Opus 4.7`)
- **OpenCode**:
  `Co-authored-by: OpenCode <noreply@opencode.ai>`
- **GitHub Copilot**:
  `Co-authored-by: GitHub Copilot <noreply@github.com>`
- **Cursor**:
  `Co-authored-by: Cursor <noreply@cursor.com>`

## Rules

1. Include one trailer per distinct AI tool that contributed to the change.
2. Human authors remain the primary `Author`/`Committer`; AI tools are credited via trailers only.
3. Do not omit the trailer when AI assistance was used, even for small edits.
4. When multiple tools are used in the same change, add a trailer for each.
5. Keep trailers at the end of the commit message, after a blank line.

## Example

```
fix(FG): neutralise Streamline swapchain hooks in external MFG mode

Co-authored-by: OpenCode <noreply@opencode.ai>
Co-authored-by: Claude <noreply@anthropic.com>
```
