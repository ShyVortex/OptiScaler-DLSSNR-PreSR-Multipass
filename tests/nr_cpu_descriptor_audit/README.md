# CPU-only descriptor-slot ownership regression

These tests compile production `DlssNr_DescriptorSlots.h`, the allocator used by
both NR dispatch methods, and the actual production `DlssNr_GpuLifetime.cpp`.
The D3D12/COM boundary is a CPU fake, with interface shims shared from the adjacent
`nr_cpu_threaded_audit` directory. No D3D12 library, device, WARP or GPU work is used.

The regression began by moving the existing modulo allocation code from the two
dispatch methods into the helper and wiring those methods to it. This preserved
the original allocation behavior; the CPU test then observed that allocation 97
returned an already-owned slot instead of refusing it. All seven initial ownership
checks failed. This tests the extracted production allocator, not the complete
GPU dispatch functions or a parallel test-only copy of the algorithm.

The replacement keeps 96 physical slots. It groups their leases by recording, polls
actual completion, and returns no slot while a prior recording can still use it.
Tracker objects are reused so GUID/fence allocation does not grow each frame.
Opaque immutable handles encode physical index and generation, reject invalid or
stale handles and cross-recording reuse, and never wrap generation identifiers.
They are published only after descriptor and constant-buffer initialization, so a
failed setup leaves a retry on the initialization path.

Cases cover capacity exhaustion, reset-before-notification, completion versus reset,
immutable handle reuse/rejection, replay on another queue, failed signals, device
removal, abandoned transactions, and destruction with a reused command-list address.
The destruction fixture drops private-data watch references (triggering the real
production destruction callback), then reuses the same fake object's address.
That final case was separately observed failing before the live-recording check.

Observed final result: MSVC C++20 `/W4` compilation succeeded without warnings;
all nine cases passed and the executable exited 0. The allocator only calls the
live-recording query with the current live `Acquire` argument, never a stored
identity pointer that may refer to a destroyed object.

Build from a Visual Studio x64 developer shell at the repository root:

```powershell
cl.exe /nologo /std:c++20 /EHsc /W4 /Itests\nr_cpu_descriptor_audit /Fotests\nr_cpu_descriptor_audit\DescriptorSlotsTests.obj /Fetests\nr_cpu_descriptor_audit\DescriptorSlotsTests.exe tests\nr_cpu_descriptor_audit\DescriptorSlotsTests.cpp ole32.lib
.\tests\nr_cpu_descriptor_audit\DescriptorSlotsTests.exe
```

Limitations: the harness tests ownership and allocator behavior, not actual resource
transitions, shader-visible heap writes, NGX execution, hook/global-lock integration,
GPU performance, or the reported Windows watchdog crash. The full application build
and combined integration validation are separate checks.
