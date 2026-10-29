# Issue & PR Cross-Repository References

This repository is a fork of `wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass`. GitHub resolves a bare `#N` written in this fork (commit messages, PR descriptions, issue and PR comments, release notes) against the **upstream** repository. Commit `917ecb76` wrote "DLSS Unlocked #57" and GitHub rendered it as `wilsjo2#57`, linking an unrelated upstream issue.

1. **DLSS Unlocked issues** (`https://github.com/ShyVortex/DLSS-Unlocked`):
   - Never write a bare `#N` for a DLSS Unlocked issue or PR.
   - In commit messages, PR descriptions, comments and release notes, use the fully qualified form `ShyVortex/DLSS-Unlocked#N`, e.g. `Groundwork for ShyVortex/DLSS-Unlocked#57`.
   - In Markdown files in the repository (`docs/issues/`, `.agents/`, README), where GitHub does not autolink, use a link: `[DLSS Unlocked #57](https://github.com/ShyVortex/DLSS-Unlocked/issues/57)`. Plain "DLSS Unlocked #57" prose is acceptable only alongside such a link in the same document.

2. **This fork's own issues and PRs** (`ShyVortex/OptiScaler-NR-PreSR-MFG`):
   - Also qualify them: `ShyVortex/OptiScaler-NR-PreSR-MFG#N`. A bare `#N` points upstream.

3. **Upstream issues and PRs** (`wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass`):
   - Write `wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass#N` so the target is explicit, not implied by GitHub's fork resolution.

4. **Before committing or posting**:
   - Check the message for any `#<digits>` not preceded by `owner/repo`. A quick check:
     ```bash
     git log -1 --format=%B | grep -nE '(^|[^A-Za-z0-9_./-])#[0-9]+'
     ```
     Any match must be qualified before the commit is pushed.
   - Already-pushed commits are not rewritten to fix a reference; correct it in the next commit or the related PR/issue text instead.
