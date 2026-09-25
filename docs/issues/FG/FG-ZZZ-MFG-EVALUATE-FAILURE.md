# Issue Investigation: Screen Flickering in Zenless Zone Zero with Multi-Frame Generation (Streamline EvaluateFeature 0xBAD00005)

- **Issue Reference**: [ShyVortex/dlss-unlocked #40](https://github.com/ShyVortex/dlss-unlocked/issues/40)
- **Target Component**: OptiScaler Ada Multi-Frame Generation (MFG), Streamline (`sl.dlss_g.dll`), and NVIDIA DLSS-G (`nvngx_dlssg.dll`)
- **Status**: Analyzed & Solution Planned (Ready for Implementation)

---

## 1. Problem Statement & Reported Symptoms

In *Zenless Zone Zero* (Unity engine with Streamline 2.11 / 2.14 interposer and DLSS-G plugin `sl.dlss_g.dll`) running on an RTX 4060 Laptop (Ada Lovelace, GPU arch `0x190`, NVIDIA driver 616.56, DLSS-G 310.9.1.0):
- OptiScaler's Ada MFG patch applies successfully:
  `MfgUnlock::TryApply MFG unlock: nvngx_dlssg.dll patched for 5 generated frames (kernels rewritten: 31)`
- Streamline detects 5-frame multi-frame capability and, when Multi-Frame Generation is enabled (e.g. 2 generated frames / 3X FG), allocates 3 output buffers:
  `dlfgPresent.cpp:1021[presentCommon] DLSS-G interpolation state changed from disabled to enabled (mode=sl::DLSSGMode::eOn, numFramesToGenerate=2, SyncInterval=1)`
  `generic.cpp:753[manageVRAM] vram alloc ... resource ... 'm_pDLFGOutputs_0'`
  `generic.cpp:753[manageVRAM] vram alloc ... resource ... 'm_pDLFGOutputs_1'`
  `generic.cpp:753[manageVRAM] vram alloc ... resource ... 'm_pDLFGOutputs_2'`
- However, immediately upon running the evaluation loop, every evaluation fails with `0xBAD00005` (`NVSDK_NGX_Result_FAIL_InvalidParameter`):
  `CommitDlssgEvaluationResult DLSSG evaluation rejected submitted interpolation count 2, result: BAD00005`
  `commonEntry.cpp:723[evaluateNGXFeature] NVSDK_NGX_D3D12_EvaluateFeature((ID3D12GraphicsCommandList*)cmdList, handle, ctx.ngxContext.params, nullptr) failed 0xbad00005`
- The game screen flickers violently, alternates between native and un-interpolated black/stale frames, and fails to generate any multi-frame interpolation.

---

## 2. Issue Validity Assessment

**Status: 100% Genuine Defect** stemming from a protocol mismatch between Streamline's Multi-Pass frame generation pipeline (`sl.dlss_g.dll`) and `nvngx_dlssg.dll`'s resource manager validation when `DLSSG.OutputReal` is set to `nullptr`.

---

## 3. Deep Dive Binary Reverse Engineering & Root Cause Analysis

### 3.1 Streamline Multi-Pass Evaluation Behavior (`sl.dlss_g.dll`)

In `sl.dlss_g.dll` (Streamline 2.11 / 2.14) at VA `0x1800609ab` - `0x1800609db`:
```assembly
180060991: leaq "DLSSG.OutputInterpolated", %rdx
1800609a4: movq 0x28(%r8), %r8               ; m_pDLFGOutputs[i]
1800609a8: callq InParameters->Set           ; DLSSG.OutputInterpolated = m_pDLFGOutputs[i]

1800609ab: testb %dil, %dil                  ; dil is flag indicating if this pass produces real frame output
1800609ae: je 0x1800609cb                    ; For intermediate generated frames, dil == 0!
; When dil != 0 (final subframe pass):
1800609c5: movq 0x28(%rax), %r8              ; m_pDLFGOutputs[real_idx]
1800609c9: jmp 0x1800609ce
; When dil == 0 (intermediate generated frames):
1800609cb: movq %rbx, %r8                    ; rbx == 0 (nullptr)!
1800609ce: movq (%r14), %rcx                 ; InParameters
1800609d1: leaq "DLSSG.OutputReal", %rdx
1800609d8: movq (%rcx), %rax
1800609db: callq *(%rax)                     ; InParameters->Set("DLSSG.OutputReal", nullptr)
```
- In traditional 2X Frame Generation (`numFramesToGenerate == 1`), `dil != 0`, so `DLSSG.OutputReal` is set to a valid 2560x1440 destination texture.
- In Multi-Frame Generation (`numFramesToGenerate >= 2`), Streamline loops over the generated subframes. For intermediate passes, only the interpolated frame is created. Streamline explicitly calls `InParameters->Set("DLSSG.OutputReal", (ID3D12Resource*)nullptr)`.

### 3.2 NGX DLSS-G Resource Manager & Validation Failure (`nvngx_dlssg.dll` v310.9.1.0)

1. **Parameter Ingestion (`EndpointResourceManager::ReadResource` at VA `0x180063249`)**:
   ```assembly
   180063249: movq (%rbx), %rcx              ; InParameters
   18006324c: leaq "DLSSG.OutputReal", %rdx
   180063253: movq %rdi, %r8
   180063256: movq (%rcx), %rax
   180063259: movq 0x40(%rax), %rax          ; InParameters->Get
   18006325d: callq *0x2f1bd(%rip)           ; Call InParameters->Get("DLSSG.OutputReal", &res)
   180063263: andl $0xfff00000, %eax
   180063268: cmpl $0xbad00000, %eax
   18006326d: je 0x18006340f                 ; Jump only if Get failed (key not found)!
   ```
   Because Streamline explicitly called `Set("DLSSG.OutputReal", nullptr)`, the key exists in the NGX parameter dictionary. `InParameters->Get` returns `NVSDK_NGX_Result_Success` (`0`) and writes `nullptr` into `res`.
   `ReadResource` sees `eax == 0`, marks Tag `0x4c` (`DLSSG.OutputReal`) as **PRESENT** in the resource manager, and records an extent of `(0, 0)`.

2. **Extent Validation (`EndpointCore::ValidateApplicationResources` at VA `0x18003815e`)**:
   ```assembly
   180038161: movl $0x4c, %edx               ; Tag 0x4c (OutputReal)
   180038169: movq 0x30(%rax), %rax          ; HasResource(0x4c)
   18003816d: callq *%rax
   180038173: testb %al, %al
   180038175: je 0x1800381e6                 ; If NOT present, skips OutputReal check!
   18003817f: movl $0x4c, %r8d
   180038188: movq 0x88(%rax), %rax          ; GetResourceExtent(0x4c)
   18003818f: callq *%rax
   180038195: movq 0x40(%rsp), %rax          ; Backbuffer width (e.g. 2560)
   18003819a: movq 0x50(%rsp), %rcx          ; OutputReal width (0)
   18003819f: movl 0x44(%rsp), %edx          ; Backbuffer height (e.g. 1440)
   1800381a3: movl 0x54(%rsp), %r8d          ; OutputReal height (0)
   1800381a8: cmpl %ecx, %eax                ; 2560 == 0 ?
   1800381aa: jne 0x1800381b1                ; MISMATCH!
   ...
   1800381e1: movl $0xbad00005, %edi         ; Returns NVSDK_NGX_Result_FAIL_InvalidParameter!
   ```
   Because `HasResource(0x4c)` returned `true`, `ValidateApplicationResources` compares the null resource dimensions `(0, 0)` against the real backbuffer dimensions `(2560, 1440)`. The comparison fails, immediately aborting evaluation with `0xBAD00005`.

3. **Behavior when Tag `0x4c` is absent**:
   Notice line `180038175: je 0x1800381e6` and lines `18003831f`, `180038cb1`, `180039127`. Whenever `HasResource(0x4c)` is `false`, `nvngx_dlssg.dll` gracefully skips `OutputReal` extent validation and skips the real-frame output blit.

---

## 4. Proposed Technical Solution

In `OptiScaler/inputs/NVNGX_DLSS_Dx12.cpp`:
1. When intercepting `NVSDK_NGX_D3D12_EvaluateFeature` for `NVSDK_NGX_Feature_FrameGeneration`:
   - Inspect `InParameters` for `"DLSSG.OutputReal"`.
   - If `InParameters->Get("DLSSG.OutputReal", &outputReal)` succeeds and `outputReal == nullptr`:
     - Wrap `InParameters` with a lightweight stack-allocated `DlssgParameterSanitizer : public NVSDK_NGX_Parameter` that forwards all calls to `InParameters`, but for `"DLSSG.OutputReal"` returns `NVSDK_NGX_Result_FAIL_FeatureNotFound` (`0xBAD00002`).
   - When `nvngx_dlssg.dll`'s `ReadResource` receives `0xBAD00002` from `InParameters->Get("DLSSG.OutputReal", ...)`:
     - It jumps directly to `0x18006340f` (success).
     - Tag `0x4c` is not added to the resource manager.
     - `HasResource(0x4c)` returns `false`.
     - `ValidateApplicationResources` skips the extent validation.
     - Evaluation succeeds cleanly with `NVSDK_NGX_Result_Success`.
2. Add diagnostic logging in `NVNGX_DLSS_Dx12.cpp` to report when null `DLSSG.OutputReal` is filtered for multi-frame subframe evaluations.

---

## 5. Implementation Plan

- **Task 1**: Create `docs/issues/FG/FG-ZZZ-MFG-EVALUATE-FAILURE.md` documenting the complete reverse engineering and root cause analysis.
- **Task 2**: Implement `DlssgParameterSanitizer` in `OptiScaler/inputs/NVNGX_DLSS_Dx12.cpp` to filter null `DLSSG.OutputReal` on intermediate MFG evaluation passes.
- **Task 3**: Create unit test in `tests/mfg_parameter_sanitization_unit.cpp` verifying that null `DLSSG.OutputReal` returns `NVSDK_NGX_Result_FAIL_FeatureNotFound` and passes non-null resources untouched.
- **Task 4**: Verify all tests compile and pass, verify `.clang-format`, and maintain UTF-8 BOM.
