# Issue & PR Cross-Repository References

This repository is a fork of `wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass`. GitHub resolves a bare `#N` written in this fork (commit subjects and bodies, PR titles and descriptions, issue and PR comments, release notes) against the **upstream** repository. Words in front of it do not change that: "DLSS Unlocked #61" renders as "DLSS Unlocked wilsjo2#61" and links an unrelated upstream issue.

## Required forms

| Target | In commits, PRs, comments, release notes | In repository Markdown files |
|---|---|---|
| DLSS Unlocked (`ShyVortex/DLSS-Unlocked`) | `ShyVortex/DLSS-Unlocked#61` | `[DLSS Unlocked #61](https://github.com/ShyVortex/DLSS-Unlocked/issues/61)` |
| This fork (`ShyVortex/OptiScaler-NR-PreSR-MFG`) | `ShyVortex/OptiScaler-NR-PreSR-MFG#31` | `[PR #31](https://github.com/ShyVortex/OptiScaler-NR-PreSR-MFG/pull/31)` |
| Upstream (`wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass`) | `wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass#12` | `[upstream #12](https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass/issues/12)` |

The text `DLSS Unlocked #N` (a space, then `#` and digits) must never appear in a commit message, PR title or PR description: not in the subject, not in the body, not in parentheses.

## Commit subjects

Earlier commits end their subject with `(DLSS Unlocked #N)`. That form is wrong; do not copy it from `git log`. Use:

```
docs(FG): document generation-A DLSS-G runtimes under sdli1995 0.4.1 (ShyVortex/DLSS-Unlocked#61)
```

The same applies to body references, e.g. `A loader error keeps priority (ShyVortex/DLSS-Unlocked#61).`

## Check before committing, not after

Write the message to a file, check it, then commit with it:

```bash
grep -nE '(^|[^A-Za-z0-9_./-])#[0-9]+' "$MSG_FILE" && echo "qualify these references first"
git commit -F "$MSG_FILE"
```

Apply the same check to PR titles and descriptions before `gh pr create` / `gh pr edit`. A match must be rewritten into one of the required forms. If a bare reference reaches a local commit anyway, amend it before pushing. Pushed commits are not rewritten; correct the reference in the related PR or issue text instead.
