# Neural Rendering: Selected Merge of Upstream Improvements (commit 6125a75)

## Overview & Environment
- **Operating System**: CachyOS Linux (Kernel `7.3.0-4-cachyos`)
- **Hardware**: NVIDIA GeForce RTX 3080 Ti Laptop GPU (Ampere SM86)
- **Driver**: NVIDIA 580.178.04
- **Upstream commit**: `6125a75b` (wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass)
- **Merge branch**: `merge-upstream`

## Reported Issue / Goal
Upstream introduced several DLSS-NR improvements. This fork selectively merged low-risk, additive changes while preserving fork invariants (Pre-SR defaults, format-bouncing cache, presets, multi-mip guides, contextual driver resolution, buffer creation).

## Merged (Low-Risk) Improvements
1. **Teardown/shutdown fixes** + GPU lifetime tracking, proxy, capture, finished-picture bridge, StreamlinePicture.
2. **Vulkan NR support** (Vk feature, finished picture, image/parameter helpers) — additive.
3. **Peripheral spatial compression** — shader blobs only (invariant .cpp/.h skipped).
4. **Edge/history/halo filters** — shader blobs only.
5. **Finished-picture routing (KCD2)** — FinishedCompose.cpp.
6. **Exposure/pipeline improvements** — Exposure.h, Pipeline_Dx12/Vk (with fork dimension fallback preserved).
7. **Denoise-first** — shader blob only (invariant .cpp/.h skipped).

## Skipped (Medium/High-Risk) Items
- Model-rebuild consolidation (touches format-bouncing cache).
- Exposure/sharpness/colour .cpp/.h changes (menu + config).
- Denoise-first .cpp/.h changes (may affect Pre-SR).
- Guide/white-point/jitter .cpp/.h changes (touches contextual resolution).

## Verification
- clang-format --dry-run --Werror: PASS on all modified C++ files.
- UTF-8 BOM: restored on all modified C++ files.
- Unit tests: not run (Windows-only build); CI to validate.

## Notes
- `.agents/rules/optiscaler-architecture.md` invariants preserved.
- Medium/high-risk items remain for future file-by-file review.
EOF
echo done