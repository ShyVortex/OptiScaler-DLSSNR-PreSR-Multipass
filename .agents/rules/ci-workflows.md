---
trigger: always_on
description: GitHub Actions & CI workflow standards for the repository
---

# GitHub Actions & CI Workflow Standards

1. **GitHub Action Checkout Version**:
   - **Always** use `actions/checkout@v6` for all workflows in `.github/workflows/`.
   - Never downgrade or use older versions (e.g. `v4` or `v5`) when creating or modifying workflow files.

2. **External Binaries & Dependencies**:
   - Do not commit large precompiled binary dependencies (such as `dlssg_for_sm86/version.dll`) directly into the git repository.
   - Workflows must dynamically clone or download external dependencies during execution on the runner if they are missing.

3. **Packaging & Artifact Verification**:
   - Workflows that package releases (`package_release.yml`) must support manual triggering via `workflow_dispatch` with appropriate options and license agreement inputs.
   - Ensure release archives verify expected file structures (e.g. `OptiScaler/dlssg_sm86/` directory contents) before publishing or uploading artifacts.

4. **PowerShell Script Invocation**:
   - When invoking `.ps1` scripts with named and `[switch]` parameters, use Hashtable splatting with direct native invocation (`& .\script.ps1 @params`).
   - Do not use array splatting (which binds elements positionally and breaks named flags) or nested CLI processes (`powershell.exe -File ... @params`, which converts switch booleans to strings).
