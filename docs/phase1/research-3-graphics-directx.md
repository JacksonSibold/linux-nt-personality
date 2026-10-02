# Research 3 — Windows Graphics Stack, DirectX, GPU Interaction, and Linux GPU Infrastructure

**Status:** Phase 1 research report. Not authority (`DEFINITION-OF-DONE.md` §1); findings feed architecture and `criteria.json` (domains GFX, DX, PERF).
**Scope:** D3D9/10/11/12, DXGI, DWM/GDI presentation, WDDM, shader pipeline, DRM/KMS, and the native-execution architecture for the graphics personality.
**Framing constraint:** no user-mode Win32→POSIX compatibility server, no emulation/VM, no Proton/DXVK/vkd3d as the mechanism. Shared kernel infrastructure with Linux drivers is *not* translation; API-to-API mapping layers at runtime *are*.

---

## 1. Windows graphics stack layering — who owns what, and therefore what WE build

Canonical Windows layering (WDDM 2.x, D3D11/D3D12 era):

```
Application (game.exe)                    [third party]
  d3d12.dll → d3d12core.dll  (or d3d11.dll, d3d9.dll)   [Microsoft: "runtime"]
    dxgi.dll (adapters, swapchains, present)             [Microsoft]
      dxcore.dll (D3D12's thin DXG adapter)              [Microsoft]
        D3DKMT* system calls  (gdi32/win32u → dxgkrnl)   [Microsoft]
          dxgkrnl.sys  (graphics kernel)                 [Microsoft]
          dxgmms2.sys  (memory manager + GPU scheduler)  [Microsoft]
            DxgkDdi* callbacks                           [IHV KMD entry points]
              amdkmdag.sys / nvlddmkm.sys / igd10um64xe* [IHV kernel-mode driver]
                GPU hardware
Vendor UMDs (user-mode): amdxxmm.dll, nvwgf2umx.dll,igdusc/icd .dll — loaded by the
D3D runtimes/DXG user-mode thunk; they build the actual hardware command buffers.
[WARP: Microsoft's CPU rasterizer, d3d10warp.dll]
[DWM.exe: Microsoft compositor, a D3D11 client of the same stack]
```

Precise ownership split:

| Layer | Owner | Notes |
|---|---|---|
| D3D9/10/11/12 API runtime (COM objects, validation, lifetime, shader *container* parsing, state objects) | **Microsoft** | d3d9.dll/d3d10.dll/d3d11.dll/d3d12core.dll. API semantics are *de facto* driver-visible: apps observe quirks of this code. |
| DXGI runtime (factory, swapchain, output enumeration, presentation model state machine, waitable objects) | **Microsoft** | Lives in dxgi.dll; owns present queues, MPO decisions, FSO. |
| DirectX Graphics Kernel: `dxgkrnl.sys` (DDI dispatch, handle tables, video-memory mgmt API D3DKMT, TDR, VSync) + `dxgmms1/2.sys` (paging, GPU scheduler, preemption) | **Microsoft** | This is the "second ABI" spine. |
| User-mode driver (UMD): consumes D3D runtime DDI (`DxgkDdiRender*` for D3D9/10/11) or vendor-proprietary escape payloads (D3D12), emits hardware command buffers, compiles shaders to ISA | **IHV** (AMD/Intel/NVIDIA; also Microsoft's UMD for basic display + WARP) | Documented interface: WDK "User-Mode Driver Components" (D3DUMDDI / DxgkDdi* UMD entry table). |
| Kernel-mode driver (KMD): `DxgkDdiStartDevice`, allocation create/destroy, paging packet build, submit, sync objects, interrupts | **IHV** | Documented WDDM DDI. |
| DWM, GDI rasterizer, cursor, monitor/HDR/colormgmt policy | **Microsoft** | DWM is a client of the stack above. |

**Consequence — what WE must build:** every layer above the GPU. The GPU vendor ships us *only* a Linux kernel-mode driver (amdgpu, xe, nouveau/NVK-era, nova) plus possibly an open or closed *Linux userspace* (Mesa ICD). Windows ships no Linux UMD/KMD pair we can consume. There is no IHV-provided "Windows graphics on Linux" — the entire Microsoft stack (runtime + dxgkrnl equivalent) and the entire IHV UMD equivalent are our engineering scope (or re-hosted Microsoft binaries, §4.5).

---

## 2. WDDM in detail

### 2.1 dxgkrnl ↔ KMD interface (`DxgkDdi*`)
The KMD exports an `DXGK_INTERFACE` table; dxgkrnl/dxgmms call into it:

- **Device lifecycle:** `DxgkDdiInit`, `DxgkDdiStartDevice` (returns capabilities struct `DXGK_GLOBAL_CAPS`, engine info incl. preemption granularity), `DxgkDdiStopDevice`, `DxgkDdiRemoveDevice`, `DxgkDdiQueryChildStatus` (monitor connect/disconnect EDID polls).
- **Video memory management:** `DxgkDdiGetStandardAllocationDriverData`, `DxgkDdiCreateAllocation` (one call, N `DXGK_ALLOCATIONS`, tiling/format constraints returned), `DxgkDdiDestroyAllocation`, `DxgkDdiDescribeAllocation`, `DxgkDdiGetStandardAllocationDriverData`. CPU access: `DxgkDdiAcquireSwizzleRange` (legacy swizzle), and `DxgkDdiSystemMemoryRead/Write` (1.x; gone in 2.0).
- **Paging (WDDM 1.x model, still alive in 2.x as "paging queues"):** dxgmms asks the KMD to `DxgkDdiBuildPagingPacket` — the KMD emits *hardware-specific page-table update command packets* (AMD VM PTB3 chunks, Intel PTE writes, NVIDIA channel push) into a protected queue. dxgmms owns the virtual address space model and decides *what*; the KMD encodes *how*.
- **Command submission:** `DxgkDdiSubmitCommand` (batch of driver-private chunks), `DxgkDdiSubmitCommandVirtual` (kernel-accessed user ring, WDDM 1.3+), `DxgkDdiWaitForEvent`/`DxgkDdiWaitForPt`. For D3D9/10/11 the UMD calls `DxgkDdiRender(RenderNm)` — a *device context* rendering packet (this is where patch lists live, §2.4).
- **DMA buffers:** `DxgkDdiCreateDmaBuffer` / `DestroyDmaBuffer` — pinned host memory regions for CPU↔device streams.
- **Sync objects:** `DxgkDdiCreateSynchronizationObject`, `Destroy`, `AcquireFromCPU`/`ReleaseFromCPU`, `SignalFromCPU`, `SignalFromGPU`, `IsSignaled`. These are *event-style* (D3D11) or *fence-value* (D3D12: `D3DKMTCreateSynchObject2` with 64-bit value, `D3DKMTSignalSynchObjectEx`, `D3DKMTWaitForSynchronizationObjectFromCpu/Gpu`). WDDM 1.2+: cross-engine waits.
- **Scheduling/queues:** `DxgkDdiCreateHwQueue`/`CreateHwContext` (D3D12 queues/contexts), `DxgkDdiSubmitHwQueueProgress`, `DxgkDdiQueryVideoMemoryInfo`, `DxgkDdiSetSchedulingProperty` (priority nodes), `DxgkDdiGetDeviceState` (the "is my device dead?" query that powers `DXGI_ERROR_DEVICE_*`).
- **Present/display (KMD side):** `DxgkDdiPresent`, `DxgkDdiSetVidPnSourceAddress`, `DxgkDdiDwmRenderPresent`-era DDIs, MPO: `DxgkDdiPresent` swap to overlay planes; `DxgkDdiCreateOverlay`, `DxgkDdiSetPointerShape` (hardware cursor).
- **Escape:** `DxgkDdiEscape` — vendor-private in-band channel; D3D12's entire submission path on Windows runs through *documented generic* (`D3DKMTCreateAllocation`, `D3DKMTHWQueue`, `D3DKMTUpdateResidencyStatus`) + *undocumented payload* escapes.
- **TDR debug:** `DxgkDdiCollectDbgInfo`, `DxgkDdiRecommendFunctionalState`, `DxgkDdiResetFromTimeout`, `DxgkDdiDXGKkmdCrashData`. WDDM 3.2 adds `DxgkDdiCollectDbgInfo2` (improved TDR forensics).

### 2.2 GPU virtual memory & allocations
- **WDDM 1.x:** a single global GPU address space (or per-prim); dxgmms paged resources in/out, UMD commands contained **relocatable addresses** → patch lists (§2.4). Expensive.
- **WDDM 2.0 (Win10):** per-process GPU VA space (48-bit canonical-ish, driver-defined page sizes); a process's allocations have *stable* GPU addresses for their lifetime → "GPUVA" visible in D3D12 (apps can even read `ID3D12Resource::GetGPUVirtualAddress`). Paging still happens (residency: `D3DKMTUpdateResidencyStatus`, MakeDiskResident), but command streams no longer need patching for VA; only *descriptor* updates touch known GPU VAs. This is the model D3D12's whole explicitness depends on.
- `DXGKGPUVIRTUALADDRESS`, `D3DKMTGPUVAINFO`, `D3DKMTEnumResidentMemories`. Guarded VA (resource ranges that fault on access — `DXGK_GUARDED_RESOURCE_TYPE`) — vendor support AMD/Intel only.

### 2.3 Command submission models
- D3D9/10/11: UMD builds a command list in user memory → `DxgkDdiRender` → dxgmms schedules *TCBs* (Translation Control Buffers) per render context → engine. One extra kernel hop per "packet" batch, ~µs, amortized by batching.
- D3D12 + WDDM 2.x + "hardware queue" (HAGS path): command queues are `DXGKHWQUEUE` with a user-mode *doorbell page* and an in-memory ring; the UMD writes vendor commands and rings doorbell via MMIO mapped into the process; kernel involvement drops to submission-time bookkeeping or vanishes (HAGS). This is exactly the shape of Linux `amdgpu` user queues (§3) — a key precedent for us.

### 2.4 Patch lists (why they existed and why we mostly avoid them)
In WDDM 1.x, `DXGKDDI_RENDER` direct/indirect command lists contain *patch locations*: kernel-poked GPU addresses written by dxgmms at submit time (because the resource's physical VA was only known then). Cost: a kernel pass over every indirect list, per draw. D3D12-era (stable GPUVA) removed patching from the hot path entirely. **Our architecture must likewise guarantee stable per-process GPUVA before D3D11/D3D12 UMD paths — Linux per-fd GPUVM gives us this natively (§3).**

### 2.5 Fences / sync
Two families: event-style `DXGKSYNCOBJECT` (manual-reset events) and fence-style (monotonic 64-bit value). D3D11 fences map to events, D3D12 fences map to fence-style; `D3D11Sync`-style cross-API sharing goes through NT handles (→ `D3DKMTShareObjects`), and WSL2's dxgkrnl maps NT handles to **Linux file descriptors** and signals via **dma-fence/sync_file** (`dxgsyncfile.c`) — proof this object model translates cleanly to kernel dma-fence semantics.

### 2.6 GPU scheduling & preemption
dxgmms2's GPU scheduler owns per-engine queues of TCBs/context slices, round-robins with priority, handles *pageable* preemption (engine state save/restore via `DXGKDXGPAGING` + device `DxgkDdiPreemptCommandBuffer`-style callbacks — vendor detail), watchdog for stuck contexts, and the DWM's high-priority path. **HAGS (WDDM 2.7-era, Oct 2020+; DDI present from 2.6/2.7)** moves scheduling *into* the GPU (firmware scheduler: AMD "SOC" queue, NVIDIA "GPU host scheduler") and collapses CPU submission overhead — measurably reduces latency in CPU-bound titles.

### 2.7 TDR (timeout detection & recovery)
dxgkrnl watchdog (~2s default) → freeze submits, `DxgkDdiCollectDbgInfo` → device reset (`DxgkDdiResetFromTimeout` fast path, else full `StopDevice/StartDevice`) → mark `dxgdevice` execution state (`D3DKMTDEVICEEXECUTIONSTATE` live/reset/error) → all waitable objects/`GetDeviceState` calls fail → runtime surfaces `DXGI_ERROR_DEVICE_HUNG/REMOVED/RESET`. WDDM 3.2 improved TDR forensics. **All apps rely on this state machine existing and being per-device.**

### 2.8 Version requirements (Microsoft's own table, condensed)
1.0 Vista: scheduling, VM mgmt, TDR. 1.1 Win7: D3D11, GDI HW accel, multi-adapter. 1.2 Win8: preemptive multitasking. 1.3 8.1: GPU scheduler, MPO. 2.0 Win10: **per-process GPUVA, driver residency model, D3D12**. 2.1: SM6.0/HDR10. 2.4: GPU paravirtualization (the D3D12-on-WDDM escape contract WSL uses), IOMMU. 2.5: DXR. 2.6: HAGS groundwork/VRS. 2.7: mesh/DXR1.1/sampler feedback. 3.0 Win11: WSL GPU, D3D12 video encode, enhanced barriers, hardware flip queue. 3.1: SM6.7, IOMMU remapping. 3.2: SM6.8, work graphs, generic programs, `DxgkDdiCollectDbgInfo2`.
**Minimum target for us:** "WDDM 2.x semantics" for a credible D3D12 personality: stable GPUVA, fence-style sync objects, hardware queues+doorbells, residency queries, TDR state machine, escape passthrough.

---

## 3. Linux GPU infrastructure — the equivalent detail

### 3.1 Core DRM/GEM
- `struct drm_device`, `drm_file` (per-fd state — Linux's analog of a WDDM dxgprocess), `struct drm_gem_object` (+ `drm_gem_object_funcs`) — the universal buffer object; handles: `GEM_CREATE`, `GEM_CLOSE`, PRIME `DUP/EXPORT_FD/IMPORT_FD` (cross-process/cross-driver zero-copy = `D3DKMTShareObjects`).
- **TTM** (`drivers/gpu/drm/ttm`): `ttm_buffer_object`, placements (VRAM/GTT), `ttm_bo_validate`, pin refcount, shrinker, swap-out via shmem — Linux's dxgmms paging + residency manager, except eviction is opportunistic (LRU/shrinker) not explicit.
- **dma-buf** — exported GEM as `struct dma_buf`/`dma_buf_attachment`; `drm_gem_prime_*`; this is the cross-process/CPU-sharing substrate every modern feature hangs on.
- **dma-fence / dma-resv**: monotonic signaling fence objects; `dma_resv` per-BO with one exclusive + shared list; `ww_mutex` deadlock-free multi-object locking (`drm_exec` helper since ~6.10). This *is* dxgmms' "allocation ready for GPU" tracking.
- **sync_file / drm_syncobj**: fence↔fd (`SYNC_FILE_CREATE`/`MERGE`); `DRM_IOCTL_SYNCOBJ_CREATE` incl. **timeline syncobjs** with 64-bit points (`DRM_SYNCOBJ_CREATE_FLAGS_TIMELINE`), `SYNCOBJ_TIMELINE_WAIT/SIGNAL/TIMELINE_QUERY`, import/export to sync_file. Timeline syncobj ≈ `D3DKMTCreateSynchObject2` fence, 1:1.

### 3.2 Scheduler & recovery
- **`drm_sched`** (`gpu/drm/scheduler`): multi-priority entity/ring model, job timeout → driver `recover` callback, fence-based dependencies. Used by amdgpu, nouveau (now), panthor, v3d, xe.
- GPU fault/recovery: amdgpu `amdgpu_device_gpu_recover` (full reset), **per-queue reset on RDNA2+/GFX10+ (Linux 6.12)** with per-process isolation; i915/xe GuC-context reset + devcoredump snapshots. There is **no OS-level TDR policy layer** — only driver-local reset; a dxg-style device-state machine must sit on top (§9).

### 3.3 Per-driver UAPI
- **amdgpu**: `AMDGPU_GEM_CREATE` (domains GTT/VRAM/CPU_ACCESS/ WC), `AMDGPU_VM_OP` (map/unmap/clear in the per-fd `amdgpu_vm` VA space, page tables in VRAM), `AMDGPU_CS` with chunked submissions (`AMDGPU_CHUNK_ID_IB` raw PM4/CSIB, `DEPENDENCIES`/`SCHED_DEPENDENCIES` syncobj in/out w/ timeline values, handles), `AMDGPU_CTX` create with priority (ICS 0–4) and per-fd `pasid`. **User queues (userq, 6.15+)**: gfx/compute/SDMA rings fully in userptr/MMIO with firmware validation + protected fence packets — i.e. **Linux now offers the exact HAGS-style submission model D3D12 wants**, and Mesa 25.1+ can run RadeonSI on it.
- **i915** (Gen≤12 + early DG): `DRM_IOCTL_I915_GEM_EXECBUFFER2`, relocations vs soft-pin vs full `PPGTT` per-fd, batch pools, `I915_CONTEXT_CREATE` with priority/scheduling class, syncobj fences.
- **xe** (TGL+ incl. Battlemage/Arrow Lake): clean modern model — `DRM_IOCTL_XE_VM_CREATE`, `XE_VM_BIND` (async bind/unbind/signal-ops, UVM-friendly), `XE_EXEC` with per-engine-class queues + user fence + array of timeline syncobjs; GPU fault via `XE_DEVICE_STATE` queries. Intel's compute stack (NEO) and ANV use it.
- **nouveau/NVK**: long-standing CPU-push path (NVK pushes directly into user channels via `DRM_IOCTL_NOUVEAU_GEM_NEW` + `IFM` doorbells, mostly no submit ioctl for graphics); the new **userspace submission + uvmm** (`DRM_IOCTL_NOUVEAU_VM_*` bind, `NOUVEAU_EXEC`, timeline syncobjs) landed for Turing+ GSP path; GSP firmware handles most engine init.
- **nova** (NVIDIA's new Rust driver, upstream 6.15+, Hopper/Blackwell enablement in progress as of mid-2026) + **Devex** (device-execution user API) — NVIDIA's committed upstream future; not yet a full graphics submit surface.
- **KMS/display**: atomic UAPI (`DRM_IOCTL_MODE_ATOMIC`, nonblocking, `PAGE_FLIP_EVENT`, IN_FENCE_FD, OUT_FENCE_PTR on properties), connectors/CRTC/planes, VRR via `VRR_ENABLED` connector prop (DP adasink/HDMI FreeSync), HDR via `HDR_OUTPUT_METADATA` + `BT.2020`/encoding props, `CTM`/degamma/gamma LUTs, `drm_fb` via `ADDFB2` (fd or handles). Vblank: `WAIT_VBLANK`, `DRM_VBLANK_EVENT`, high-precision in-kernel timestamps.
- **Userspace**: Mesa = RADV/ANV (conformant **Vulkan 1.4**, conformant 1.3/1.4 since 2025), **NVK conformant Vulkan 1.4 back to Maxwell** (docs.mesa3d.org; mesh shaders Mesa 26.2, Vulkan Video 26.3), `libdrm` as ioctl wrapper, Zink. AMD PAL (GPUOpen) is MIT-licensed, is the shared engine under AMD's *Windows* Vulkan (and informs D3D UMD), and on Linux talks to **the same `AMDGPU_CS` UAPI** → PAL is effectively a documented reference for "how to drive AMD graphics hardware from userspace, DDI-shaped".

**Mapping summary (WDDM concept → Linux facility):**

| WDDM | Linux |
|---|---|
| dxgprocess | `drm_file` (+ our handle table) |
| dxgdevice/dxgcontext | vendor ctx (`amdgpu_ctx` w/ priority, `xe_exec_queue`) |
| dxgresource/dxgallocation | `drm_gem_object`/`amdgpu_bo` (TTM) |
| GPUVA per process | per-fd VM (`amdgpu_vm`, `xe_vm`, `nouveau_uvmm`) / `drm_gpuvm` helper (generic GPU VA manager, kernel 6.12+; amdgpu & nouveau converted) |
| Paging | TTM validate + `drm_exec`/`dma_resv`; VM_BIND ops |
| TCB / hw queue | `drm_sched_entity` / vendor queue; amdgpu userq doorbell |
| Fence synch object | timeline `drm_syncobj` |
| Event synch object | syncobj point / sync_file fd |
| NT handle sharing | PRIME fd / `fcntl FDUP` |
| DxgkDdiEscape | vendor ioctl or escape on our service |
| TDR | driver reset + `drm_sched` timeout (policy layer missing) |

---

## 4. THE CENTRAL QUESTION — native Direct3D+DXGI architecture

### 4.1 Option (a): native D3D runtimes + per-vendor "UMDs" emitting hardware command streams over DRM UAPIs
This is building the IHV driver ourselves, API-shape on top.

**Feasibility per vendor (what's documented in 2026):**
- **AMD: HIGH.** RDNA1/2/3 public ISA PDFs; gfx12 (RDNA4) already in upstream LLVM (encoding de-facto public); RADV + PAL + LLPC are open, MIT-like; PM4 command stream fully known (RADV emits raw CS chunks via `AMDGPU_CS`); tiling/swizzle models documented (PAL, Vulkan memory allocation docs); SR-IOV/GFX12 register docs via amd-gpu-doc on GitHub. The compiler path (DXIL→…→AMDGPU ISA) has LLVM's AMDGPU backend upstream. **AMD is where we ship first.**
- **Intel: MEDIUM-HIGH.** PRMs published through Xe1/Alchemist (dgpu-docs.intel.com; mirror at kiwitree/intel-gfx-docs); ANV open (conformant) proving userspace emission is fully reconstructable; `xe` UAPI is *designed* for modern UMDs (VM_BIND + async binds); GuC/CT interface documented in kernel source. Gap: **Xe2/Xe3 (Battlemage, Lunar Lake+) PRMs not publicly released** (community request unanswered as of Aug 2025; Intel's new "Jay" compiler passes Vulkan CTS on Xe2/Xe3, i.e., knowledge exists but isn't in the open). Mitigation: Xe2+ initially via command emission reverse-engineered from ANV behavior (ANV is open — legal and complete on Linux) and cooperation with Intel.
- **NVIDIA: LOW-MEDIUM (improving).** No ISA license; but NVK is a conformant userspace Vulkan driver with its own SASS assembler (Maxwell–Blackwell), `open-gpu-doc` has been growing (12k lines of Blackwell 3D headers, DMA-class headers, pushed by NVIDIA engineers in 2025), NVK+Zink is default GL for Turing+, and Nova/Devex is NVIDIA's upstream commitment. Realistic: NVIDIA graphics personality via NVK-derived internals (userspace push buffers, documented classes + Mesa-internal assembler) for Turing–Ada now; Blackwell follow NVK; SASS *DXIL* compilation via our own path (see §5 — the one honest hard part for NVIDIA is the *shader backend*, not command submission, because NVK already proves the ISA side). Protected features (some Vulkan caps tied to signed firmware paths) and tiled-resource details remain friction.

**Cost:** this is the largest single work item in the project — three vendor rasterizers' worth of "RADV without the Vulkan API in front". RADV+PAL scale ≈ >500k LOC each; our subset (no Vulkan-specific semantics like renderpasses-as-objects, but *plus* D3D-specific bindings model, guarded VA, swapchain, DXR BVH management) is comparable. No way to shrink this with translation tricks without violating the framing (§4.4).

**Performance:** optimal — identical to what an IHV driver does; steady-state submission is doorbell/ring writes (tens of ns–hundreds ns/draw CPU-side), zero ioctl per draw on userq, one ioctl per ~frame or per submission in classic `AMDGPU_CS` mode.

### 4.2 Option (b): a WDDM-compatible kernel service ("dxgkrnl personality") over DRM
**The existence proof:** Microsoft's own `drivers/hv/dxgkrnl` (WSL2 kernel, also in Linux-msft 6.18 tree) is *a Linux kernel driver that exposes the WDDM D3DKMT object model to Linux userspace* via `/dev/dxg` miscdevice: `LX_DX*` ioctls for `dxgadapter/dxgprocess/dxgdevice/dxgcontext/dxgpagingqueue/dxgallocation/dxgresource/dxgsyncobject/dxghwqueue` (`dxgkrnl.h`, handle table `hmgr.c`), NT handles as fds, `LX_DXSHAREOBJECTS`, scheduling-priority ioctls, clock calibration, escape ioctls, and **dma-fence integration via `dxgsyncfile.c`**. The UAPI mirrors the documented D3DKMT surface, shipped MIT in **`microsoft/libdxg`** ("official WDDM kernel API headers" + `libdxg.so` translating D3DKMT calls to `/dev/dxg` ioctls) and `DirectX-Headers/include/wsl/winadapter.h`. Microsoft's stated architecture explicitly enumerates our layering: app → API runtime → **UMD** → libdxg → dxgkrnl(Linux) → (host dxgkrnl.sys + IHV KMD).

In WSL2 the last hop is VMBus to a Windows host. **A non-virtualized version replaces the vpipe backend with a DRM backend:** each `dxgallocation` wraps a GEM object of a real adapter; `dxghwqueue`/`dxgcontext` wrap vendor sched entities (or raw amdgpu userq); `LX_DXCREATEALLOCATION` → `TTM`/`GEM_CREATE` + VM map; `DX_WAIT_SYNCH_OBJECT_*` → syncobj timeline waits; `LX_DXESCAPE` → vendor ioctl or our UMD's channel; device execution state + watchdog → our TDR machine (§2.7); present → §6.
Note WSL2's variant today is *compute-only-graphics* — Microsoft's D3D12-for-WSL stack (closed, dual-compiled from Windows sources per their XDC 2020 talk "the D3D stack is same code that runs on Windows… modified to dual-compile") has rasterization but **no swapchains/window integration**. So the graphics half of the D3DKMT surface (present, vblank, MPO, DWM) does not exist in WSL2's subset — we write it. Also: as of 2026-03 Microsoft is re-submitting dxgkrnl to mainline for "GPU-accelerated workloads inside Linux containers on Windows" — it is still not merged into Linus' tree; plan for out-of-tree module initially.

**Assessment:** (b) is **necessary but not sufficient**. It defines the *contract* between our D3D runtime(s), vendor UMDs, and the kernel — object model, handles, sharing, fences, residency, device state, escapes. It does *not* define the command stream: someone must still build PM4/render-native SASS/Media — i.e. (b) needs (a) behind it. But adopting (b) *as the contract* buys: MIT headers we're allowed to implement, handle-table semantics apps/inspectors expect, `D3DKMT`-level debug tool parity, and a migration path where a real IHV could later ship a *Linux* UMD over /dev/dxg exactly as they ship one for Windows.

### 4.3 Option (c): D3D12 API implemented over Vulkan/Mesa in-process
Honest assessment against the stated rule: vkd3d/DXVK-style *runtime* mapping (ID3D12CommandQueue literally becomes a VkQueue at runtime; D3D12 objects ↔ Vulkan objects 1:1) **is the prohibited mechanism** — it is "the Wine model applied to the graphics ABI": a translation personality instead of a second native personality, and it re-imports every classic translation cost (see §8). It would fail DoD architecture tests on inspection (process address space full of `struct VkDevice`, libvulkan.so dependency, VK validation overhead).
However, two narrower uses are *driver-model choices, not translations*, and we should exploit them:
  1. **Compiler/winsys library reuse without Vulkan objects at runtime:** using NIR→ISA backends (ACO, AMDGPU LLVM, NVK's assembler) as libraries, or Mesa's `libdrm` usage patterns, is the same lower-level-infrastructure sharing that §Framing explicitly permits (RADV itself is just a policy writer over them).
  2. **Bring-up/fallback ICD:** while vendor-native paths (a) land, a *clearly flagged* fallback D3D9/11 shim over Vulkan (internally, behind a separate "compat adapter" that enumerates with `D3D11_SB`-style vendor flags or fails P0 DoD criteria) buys time. It must not be the product path for D3D12 and must be opt-out + telemetry-visible.
**Verdict:** (c) as mechanism = NO (prohibited by project charter and by the physics of §8 overhead). As compiler library + transitional safety net for legacy 9/11 = yes, explicitly bounded.

### 4.4 Option (d) (variant of (a)+(b) worth stating): re-host Microsoft's own runtime binaries
Microsoft's WSL2 slide deck is direct evidence that d3d12core/dxgi/dxcore are *OS-portable* code compiling against a thin PAL (Windows constructs replaced, dual-compile). Our PE personality can already run Windows DLLs; if our gdi32/win32u surface exports the D3DKMT* set onto `/dev/dxg`-equivalent and our kernel service implements it, a **Windows d3d12core.dll could in principle run unmodified above our stack** (same trick as WSL but local GPU). Technical risk: medium (import list is mostly documented D3DKMT + base APIs; the hard part is dxcore's undocumented-but-enumerable imports; spike: `dumpbin /imports` + early loader experiments). Legal risk: fatal for redistribution — Windows license doesn't permit shipping these binaries in our OS; Agility-SDK redistribution clauses cover app folders, not an OS component. **Decision: not the product path; keep as a diagnostics spike only.** (The API semantics learned from re-implementation remain our source of truth: official docs + DirectX-Specs + observed behavior.)

### 4.5 RECOMMENDATION
**Hybrid: (b) as the kernel contract + (a) as the driver model, with (c)-bounded-as-library.**

```
d3d9/d3d11/d3d12.dll + dxgi.dll        ← our native runtimes (PE personality)
        │ D3D runtime DDI (ours, documented to ourselves)
   vendor UMD engines ("dxgmd-umds"):
   amdgpu-native emitter  |  xe-native emitter  |  nvk-derived emitter   ← user space
        │ D3DKMT ioctls (MIT libdxg/d3dkmthk.h UAPI)
   dxgcore.ko  ← our "dxgkrnl personality": handle tables, allocations,
   (new kernel module)   hw queues, fences, device state/TDR, present queue
        │ maps onto existing infra
   DRM/GEM/TTM · dma-fence/syncobj · drm_sched · drm_gpuvm · KMS atomic
```

- Ship the **`d3dkmthk.h`-compatible ioctl surface verbatim where sane** (adapt the miscdevice model; keep `LX_DX*` numbering compatible with libdxg so the MIT userspace lib + headers, tooling, and potential IHV UMDs work unmodified).
- The **UMDs live in user space** (like Windows): per-vendor engines building real command streams. Start AMD (PM4 via `AMDGPU_CS`/userq; reuse Mesa `ac`/`radv` *libs* for tiling math and ACO/LLVM for shaders, which is permitted library reuse, not translation), then Intel (xe VM_BIND+EXEC), then NVIDIA (NVK winsys patterns).
- **D3D11/D3D9 runtimes built over our D3D12 core** the way Microsoft builds d3d10/9on12 — precedent *open-sourced by Microsoft* (github.com/microsoft/D3D9On12, D3D11on12 spec in DirectX-Specs) and legitimate (it's our runtime mapping to our own core — same choice Microsoft made internally).
- **Performance expectation:** steady-state frame CPU cost equal to Windows driver (doorbell path), present latency equal to native KMS scanout flip, memory overhead ± allocator only. Option (c)-style would cost 10–30% CPU in descriptor/state rewrites and double bookkeeping on every API object.

---

## 5. Shaders: DXBC, DXIL, HLSL → ISA

**The good news (unlike pre-2020 assumptions): the shader story is mostly open now.**
- **DXC** (HLSL→DXIL front end + validator + DXIL container reader) is fully open (microsoft/DirectXShaderCompiler, UIUC license) — includes the **DXIL spec** (`docs/DXIL.rst`), the **validator sources and hash algorithm (open-sourced July 2024)**, and **`dxilconv`/DxbcConverter — the Microsoft DXBC→DXIL converter source** (the formerly-internal `dxbc2dxil.dll` is in-tree!).
- DXIL *is* LLVM bitcode (7.0-era pinned) with a defined intrinsic/opcode ABI → consume with LLVM's `llvm-dxil` upstream (decompile passes included), then **lower DXIL→NIR or DXIL→LLVM-IR→vendor backend**. RADV-style: NIR→ACO (AMD GCN/RDNA) or NIR→LLVM AMDGPU; NVK→SASS assembler; ANV→vec4/FS. This gives us *validated, conformant* SM6.0+ on every GPU **without any vendor cooperation**, because we own the entire back end like Mesa drivers already do.
- **Legacy DXBC (SM ≤ 5.1; all D3D9/10/11-era content)**: the container formats and opcode streams are documented enough via open-source readers (DXVK's `dxil_conv`-opposite `dxbc` parser exists as a *reference we must not copy as mechanism*, but we can implement readers from `dxilconv` in DXC + Noesis/SharpDX-era docs + D3D9 "Shader Bytecode" SDK docs; SM1–3 is documented in the old DX9 headers). Path: **DXBC → (dxilconv, open MS source) → DXIL → our back ends.** Remaining originals: fidelity tables for fxc's SM4/SM5 float/precision quirks (D3DCOMPILE flags, `DXBC` vs DXIL `dx.op` behavior differences) — differential-testing corpus work, not RE wall.
- **Signing/validation:** apps hand us *already-compiled* bytecode blobs. DXIL container validation (hash/signature) is only an SDK-time gate; runtime must tolerate/validate-but-not-enforce (real Windows drivers accept unsigned DXIL from shipped binaries because it was signed by SDK; our runtime should *parse and verify structure*, not reject on missing MS signature — behavior check required: do any shipped games re-require `dxil.dll`? Some D3D12 titles call `D3D12SerializeRootSignature`-adjacent validation but not signing at runtime. Experiment: corpus run with signature enforcement off).
- **Shader caching:** D3D has no spec'd API cache; drivers (and apps via `DXGI`/driver store) cache ISA keyed by shader blob hash + state key. Ours: on-disk cache exactly like Mesa (`MESA_SHADER_CACHE_DIR`, disk_cache), keyed (DXIL/DXBC SHA-256, ISA hash, engine gen). D3D12 apps also use `D3D12_STATE_OBJECT`/PSO blob caches — our `CreatePipelineState`/`CreateStateObject` (DXR) returns *opaque* blobs; we define their content (vendor ISA + metadata), and app-level "driver store" equivalents (`$ProgramData` cache) just work if the blob is stable.

## 6. DXGI: swapchains, presentation, outputs

**API surface:** `IDXGIFactory*`, `IDXGIAdapter*` (→ PCI enumeration via `/sys/bus/pci`, DRM driver names), `IDXGIOutput` (+1.x/1.3 dup modes, `FindClosestMatchingMode`), `IDXGIOutputDuplication`, `IDXGISwapChain(1..7)`, presentation models: legacy bitblt (D3D≤10), legacy-flip (D3D9Ex), **FLIP_SEQUENTIAL/FLIP_DISCARD (D3D11.2+/12 — today's only relevant models)**; flags `ALLOW_TEARING`, `ALPHA_MODE`, `PRESENT_DO_NOT_WAIT`, `PRESENT_ALLOW_TEARING`, frame-latency-waitable objects (`DXGI_WAITOBJECT` ← the `D3DKMT`-level sync object; must integrate with the PE personality's wait/sync model — dependency on Agent 1), DXGI formats incl. YUV `DXGI_FORMAT_…NV12` for video swapchains, HDR (`DXGI_COLOR_SPACE_*`, `IDXGISwapchain4::GetDesc`, `DXGI_SWAP_EFFECT_FLIP_DISCARD + DXGI_FORMAT_R16G16B16A16_FLOAT + colorspace 2020`), multi-monitor + `DXGI_SWAPCHAIN_FLAG_BIND_TARGET`, FSO (full-screen optimizations).

**Presentation design (native, compositor-round-trip-free):**
- Back buffers = GEM BOs created by our allocator service, **imported into KMS directly as framebuffers** (`ADDFB2` with prime fd) — zero-copy flip chain, identical to how RADV does Vulkan WSI direct scanout.
- `dxgcore.ko` owns a **present queue + display arbitrator** per adapter (our `DxgkDdiPresent` equivalent): when the flip-model swapchain is *visible and topmost*, do a nonblocking atomic page flip on the assigned CRTC plane (direct scanout = flip latency = one vblank, no GPU copy, no compositor hop); when occluded/windowed, insert into the desktop compositor's scanout queue (DWM-analog) as a *dma-buf plane* — compositor composes with MPO planes if the display engine allows, else GPU-composite once (same cost model as Windows DWM, which also copies/composites when occluded).
- vblank & present events: `DRM_IOCTL_WAIT_VBLANK`/atomic `PAGE_FLIP_EVENT` → our waitable-object table (frame pacing) + DXGI `WaitForPresent`. **Hardware flip queue** (WDDM 3.0) maps to a kernel-side present FIFO so `Present()` never blocks even under load.
- Fullscreen-exclusive (D3D9/DXGI≤1.1 `DXGI_SWAP_EFFECT` legacy + `SetFullscreenState`): the app becomes *KMS master* for its adapter momentarily (DRM leases / `drm_master` handoff primitive we add: only one personality-owned master — the arbitrator — so "exclusive" = arbitrator pins the CRTC mode + direct flip, bypasses compositor; this matches FSO-era Windows reality and avoids legacy mode-switch races).
- VRR: propagate `VRR_ENABLED` + `AllowTearing` to `Present`; HDR: map `DXGI_COLOR_SPACE_` to connector `HDR_OUTPUT_METADATA`+colorspace props; SDR paper-white per `IDXGIOutput5` semantics.
- Wayland/Linux-native surfaces: native apps go through the normal Wayland stack; the arbitrator may also *import* DirectX swapchain dma-bufs into Wayland surfaces (for hybrid desktop) — presentation for DirectX never *depends* on Wayland.
- **Multi-adapter (DXGI enumeration, `DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT`, cross-adapter `IDXGIOutput1::DuplicateOutput`, explicit-linked-adapter `IDXGIAdapter3`)**: expose all DRM cards via `EnumAdapters`; present on any; cross-adapter sync via PRIME+syncobj; "monitor attached to different GPU" = GPU copy/scanout-over-prime (same as Linux today). Linked-adapter SLI/Crossfire: report unsupported (as modern Windows does).

## 7. GDI / USER32 (graphics-facing slice)

USER32 window object model belongs to Agent 2; the graphics contracts we must provide:
- **HWND ↔ swapchain target** (`IDXGIFactory::CreateSwapchain(HWND)`) requires the window manager to expose a per-window visible-region/occlusion/alpha query (for the arbitrator's direct-scanout decision and for `DXGI_SWAP_EFFECT` resize semantics, `WM_SIZE`-driven `ResizeBuffers`).
- **GDI device contexts:** DC over (a) a DIB section (pure CPU raster — reimplement with our own CPU blitter; correctness > speed for GDI), (b) a window (render into its front buffer = the compositor's shadow surface; Windows itself hasn't GPU-accelerated GDI through dxgkrnl since Win8-era "GDI/D3D9 retirement" — GDI draws are CPU into surfaces, DWM composes), (c) `CreateDC` over printer (out of scope P2). Keep GDI entirely below the D3D personality: GDI never needs D3DKMT. This collapses the legacy surface area massively and matches how modern Windows actually behaves.
- **DWM contract:** our compositor (DWM-analog, built on our D3D11 runtime for GPU composition, or on the raw dxgcore present API) needs: per-window redirection surfaces shared via `D3DKMTShareObjects`-equivalent (prime fds), **Desktop Duplication** (`IDXGIOutputDuplication` = arbitrator snapshotting the composed frame — same mechanism as DXGI capture, so implement both over the arbitrator), magnifier/`DwmGetDxSharedSurface`-style internal hooks as P3.
- Legacy D3D9Ex shared primary / `D3DSURFACE` "SetRootSurface" path: map onto redirection surfaces, not real scanout. Mouse: hardware cursor via DRM plane (`DxgkDdiSetPointerShape` equivalent in arbitrator).

## 8. Performance: existing overheads vs native design

| Overhead source (DXVK/vkd3d/proton world) | Mechanism | Native design |
|---|---|---|
| API-object shadowing | every D3D object allocates + maps to a Vulkan object; lifecycle rules differ (barrier insertion, layout tracking) | 1:1 real objects (our runtime *is* the API) |
| Descriptor model impedance (CBV/SRV/UAV heaps + root sigs → VkDescriptorSetLayouts, dynamic sets, per-draw set binding, view rewrites for buffer views w/ different format rules) | O(hundreds of ns–µs)/draw in busy titles; memory for descriptor staging | D3D12's GPU-visible descriptor heaps are *hardware-native* on AMD (raw GPU VA tables) and NVIDIA (constant buffers) — RADV does exactly this for Vulkan; our UMD writes descriptors directly, no rewrite pass |
| State-object churn: PSO/D3D11-state → pipeline cache lookups, pipeline compile on first draw | hitches | same cache, but keyed once (no VK translation layer hashing) and same ACO/LLVM backend as RADV; D3D12 PSO blob caches persist across runs |
| Double memory bookkeeping (D3D12 heaps + VMA + Vulkan memprop queries) | RAM + CPU | single TTM/GEM allocation per `dxgallocation`; residency = TTM pin + explicit MakeResident ioctls (cheap, correct) |
| Extra submissions (vkQueueSubmit per present/flush) + WSI path | µs-level per submit | userq/doorbell rings; per-frame submit cost like Windows HAGS |
| **Compositor round-trip** (game→WSI→Weston/Mutter→KMS: +1 frame queueing, GPU copy or extra plane commit, tearing-free forced) | 1–2 frames latency in the worst historical case | direct-scanout present queue (§6); flip = 1 vblank; latency parity w/ Windows flip model |
| Barrier validation in `vkd3d` (correct-but-conservative layout tracking) | GPU stalls | we emit vendor-native transitions (AMD: CS chunks; Intel: PIPE_SURFACE_STATE) exactly as IHV drivers |
| CPU: Win32→POSIX syscalls in compat server (not ours) | — | ARCH non-negotiable satisfied: graphics ioctls are direct syscalls from the PE process |

**Draw-call budget target:** ≤ 150 ns/draw CPU for D3D12 recording (RADV-class), ≤ 5 µs/frame present→vblank queueing (direct scanout). We can measure Windows vs our stack directly because both end in the same `AMDGPU_CS`/userq format — differential *command stream comparison* is even possible (same PM4 as Windows driver).

## 9. Direct use / new facilities / adaptation (kernel-side split)

**Directly usable, zero new kernel code:** GEM/TTM/dma-buf/dma-resv/dma-fence/syncobj timelines (fences!), `drm_sched` + per-ctx priority, `drm_gpuvm`/`drm_exec`, `AMDGPU_CS`/`XE_EXEC`/`NOUVEAU_EXEC` submission, KMS atomic (present, cursors, MPO), `VRR_ENABLED`, `HDR_OUTPUT_METADATA`, PRIME sharing, amdgpu **userq** (our HAGS path, already exists upstream), per-queue reset (RDNA2+, 6.12), vblank ioctls, DRI_PRIME-style multi-card, sysfs adapter info.

**Needs NEW kernel facilities (our module work, upstreamable as a coherent story):**
1. **`dxgcore.ko`** — the D3DKMT object service: miscdevice `/dev/dxg` (libdxg-compatible numbering), handle table (port the WSL2 `hmgr.c` model — clean room not needed, it's GPLv2 and we can write a compatible one or upstream-collab with MS's own submission), adapter aggregation per DRM card, allocation/lock/mmap, fence objects, hw queues, **escape channel** to vendor drivers, device-state machine. *(Largest single kernel item.)*
2. **Present/display arbitration:** one-owner KMS master with policy API: direct-scanout flip queues from processes, vblank+present event routing to *multiple* waiters, exclusive-mode handoff, HDR/VRR property mapping to "current primary app". (DRM leases are close but wrong fit — single-lessee; extend.)
3. **TDR policy:** global watchdog, `execution_state` (live/reset/error), broadcast to all fences/queues, reset sequencing via vendor recover entry points, app-visible `GetDeviceState` semantics. (Drivers have reset; no one has the *OS state machine*.)
4. **Multi-syncobject wait ("WaitMultipleObjects" for fences, any/all semantics, with NT-style priority):** sync_file merges AND-only today; add an in-kernel "any-of" wait + eventfd integration so the PE executive's object waits and GPU fences share one wakeup path. Small, well-scoped patchset.
5. **Guarded resources / protected session parity (P2):** guarded VA exists in amdgpu for compute via PASID semantics; CDM-style protected media paths (amdgpu TA, PXP) are per-vendor; declare unsupported until tested (DoD: P3/EXT-GATED for PlayReady anyway).

**Needs ADAPTATION (user space mostly):** residency API (`D3DKMTUpdateResidencyStatus`) → TTM pin/unpin + batched validation; WDDM `DXGK_ALLOCATIONINFO` tiling semantics → per-vendor tiling queries (radv `ac_surface` reused); DX12 "core" concepts (command signature, cross-adapter fences) → syncobj + prime; D3D9Ex `SetRootSurface` legacy present → redirection path.

## 10. Kernel / user split, justified

- **Kernel (`dxgcore.ko` + existing DRM):** protection boundaries (handle translation, object lifetimes, cross-process sharing, device-state, memory residency, display arbitration) — because the Windows ABI promises *these* invariants (NT-handle sharing semantics, TDR state machine, flip timing) and only the kernel can enforce them across processes; and because keeping the GPU command doorbell page and protected-fence memory trusted requires kernel setup (precedent: exactly what WSL2 dxgkrnl + amdgpu userq do).
- **User space (runtimes + UMDs + compiler):** everything non-authoritative — API objects, validation, command emission, shaders. Same split as Windows (d3d11.dll + nvwgf2umx.dll), same split as Mesa (radv + amdgpu.ko), justified identically: crash containment (TDR instead of kernel oops), performance (no ioctl in draw path), and debuggability (RenderDoc-style tools intercept user DLLs).
- The only *new* kernel surface is the D3DKMT object service + arbitration; the alternative (user-mode daemon holding `/dev/dri` on behalf of apps) is precisely the "user-mode compatibility server" shape the DoD bans for architecture tests (kill-the-helper test §14.1): graphics must survive via in-kernel personality even if policy helpers die.

---

## 11. DELIVER: consolidated plan

### 11.1 Proposed architecture
As §4.5: **dxgcore.ko (D3DKMT over DRM) + our D3D runtimes + native per-vendor UMD engines + Mesa-compiler-library reuse + dxg-owned present arbitrator; (c)-shims only as bounded fallback; (d) MS-binary hosting only as a diagnostics spike.**

### 11.2 Existing Linux facilities used (named)
`drm_file`, `drm_gem_object`, TTM (`ttm_bo_validate`), dma-buf/PRIME (`DRM_IOCTL_PRIME_HANDLE_TO_FD`), `dma_resv`/`ww_mutex`/`drm_exec`, `DRM_IOCTL_SYNCOBJ_{CREATE,TIMELINE_WAIT,TIMELINE_SIGNAL}`, `sync_file`, `drm_sched`, `drm_gpuvm`/`drm_gpuva`, `AMDGPU_GEM_CREATE`/`AMDGPU_VM_OP`/`AMDGPU_CS`(+USERQ)/`AMDGPU_CTX`, `I915_GEM_EXECBUFFER2`, `XE_VM_CREATE`/`XE_VM_BIND`/`XE_EXEC`, `NOUVEAU_VM_BIND`/`NOUVEAU_EXEC`, KMS: `DRM_IOCTL_MODE_ATOMIC`, `ADDFB2`, `PAGE_FLIP_EVENT`, `WAIT_VBLANK`, `VRR_ENABLED`, `HDR_OUTPUT_METADATA`, IN_FENCE_FD, CTM/degamma props; Mesa libs: `ac` (surfinfo), NIR/ACO, LLVM AMDGPU backend, (NVK internals as reference), `libdrm`; userspace WSL artifacts: `libdxg` + `d3dkmthk.h` (MIT), `DirectX-Headers`, `DXC` incl. `dxilconv` + open validator.

### 11.3 Required NEW kernel facilities
§9 list 1–5 (dxgcore.ko; present arbitrator; TDR state machine; any-of fence waits; protected/guarded parity), plus initial out-of-tree packaging (Ubuntu DKMS + downstream patch-set) with upstream track (amdgpu userq-adjacent code will make DRM reviewers demand upstream fit — budget for it).

### 11.4 Dependencies
Agent 1: NT object manager (HANDLE↔fd bridge for DX shared objects & waitables), section objects (upload heaps ↔ `dma_buf` mmap, WC caching), PE loader DLL mapping (d3d12.exe import redirection), thread model (D3D12 multithreaded recording ↔ per-thread ctx binding). Agent 2: HWND queries (occlusion, DPI, fullscreen requests), DWM-analog composition surface contract, HDR/colormgmt policy. Outside: libdxg/dxgkrnl collaboration opportunity (MS is actively upstreaming; our DRM backend differs but UAPI is shared); Mesa/amdgpu/Intel contacts for Xe2+ documentation; NVIDIA engagement for Nova/Devex timeline.

### 11.5 Technical risks (ranked)
1. **API-compatibility wall of the D3D runtimes** (esp. D3D9/11 float rules, D3D12 validation behaviors, Agility-SDK version matrix — apps ship *their own* d3d12.dll expecting OS-side dxcore/D3DKMT to satisfy it). Mitigation: version-gate `D3D12_SDK_VERSION`, prioritize criteria mapping to `criteria.json` DX domain.
2. **NVIDIA long-term path** (DXIL→SASS backend maturity; tiling/prot features; Nova/Devex graphics completion uncertain). Mitigation: vendor order AMD→Intel→NVIDIA; NVK-derived winsys lowers risk.
3. **Tiled resource / swizzle + reserved (tiled) resources** for D3D11/12 (sparse) — heavily vendor-documented on AMD/Intel, near-zero on NVIDIA. Mitigation: feature-level reporting (games probe FL11_0/12_0 and adapt); sparse = P2 per-vendor.
4. **Kernel acceptance/maintenance of dxgcore + arbitrator vs Ubuntu's "no Linux regression" constraint** (GPU stack churn). Mitigation: DKMS + upstream-first policy for the generic bits (syncobj any-wait, present lease).
5. **Shader numerical fidelity** (DXIL semantics vs ACO/NIR assumptions; fxc DXBC quirks) → visual regressions in specific titles; mitigated by differential pixel test farm + documented "strict accuracy" compile flags (dxo `dx.compiler.version`).
6. Present latency regressions from hybrid graphics (prime offload) on laptops — flip must be on the scanout GPU. Design: swapchain migrates or GPU-copies explicitly; measure.

### 11.6 Implementation order (parallelizable with other agents' work)
1. `dxgcore.ko` skeleton: /dev/dxg + handle table + adapter/device + allocation over GEM + syncobj fences; libdxg-compatible ioctls; IGT tests. (Proof: libdxgtest compute sample runs on *local* AMD card.) 
2. **Spike-0 (resolving experiment, see §12):** end-to-end "D3D12-shaped device → PM4 → scanout triangle" with 500-line toy runtime — validates the entire §4.5 chain before committing to runtimes.
3. D3D12 runtime core (device/heap/resource/buffer, cmd queue/alloc/list, fence, descriptor heaps, PSO) + AMD UMD engine + dxg present queue + direct-scanout flip. *Gate: DirectX-Graphics-Samples D3D12 demos pass pixel-diff.*
4. DXIL/DXBC pipeline (dxilconv import → DXIL→NIR lowering → ACO/LLVM), shader disk cache.
5. DXGI full (enumeration, flip models, waitables, FSO, tearing, VRR) + HDR properties.
6. D3D11 runtime over core + D3D9 over D3D11-core (D3D9On12 as published architectural reference for the mapping) + GDI/DWM-analog integration.
7. TDR machine + device state semantics (before broad app testing — everything depends on credible device-lost paths).
8. Intel (xe) engine; then NVIDIA (NVK-pattern engine + DXIL→NVK backend); sparse/tiled, DXR (AMD via PAL/gpurt open docs first).
9. Perf hardening: userq enablement, HAGS-parity queues, hardware flip queue.

### 11.7 Testing methodology
- **Differential pixel/state testing:** same D3D program + shader corpus rendered on Windows reference host (PIX/SnapCaptures or RT-screenshot harness) vs ours → per-frame hash/pixel diff (tolerance per precision class). This is the only honest way to police "behavioral parity" (DoD §5–6 harness hooks).
- **Corpus:** DirectX-Graphics-Samples (MIT), DirectXTK12 tests, D3D12 translation-layer tests (Microsoft ships its own D3D12→D3D9/11 unit tests with D3D9On12/D3D11On12 repos — direct, licensed parity tests for *our* runtime!), dxc `tests/` + DXIL validator suite, piglit-style native C tests we author per API area (criteria.json driven), then real-game capture/replay for smoke.
- **Kernel:** IGT extension suite for dxgcore (handle/fence/TDR injection via fault-injection), kselftests for syncobj any-wait; `amdgpu` CI matrices (GFX10/11/12), `xe` CI for Intel.
- **Perf gates in CI:** draw-call ns counters (perf counters around `ExecuteCommandLists`), present→vblank latency (DRM timestamp), CPU-bound FPS vs RADV/native-Vulkan baselines + Windows baseline where hardware parity allows; regression ratchet in criteria.json PERF domain.
- **Anti-counterfeit alignment:** process introspection must show no libvulkan/Wine lineage; DoD PMU fingerprint checks apply to the UMD loops.

### 11.8 Known compatibility traps (graphics slice)
DXBC-era rounding/precision (fxc vs dxc; `DXBC` SM4/5 ops w/ NaN behaviors on GCN vs NV); premultiplied-alpha swapchains + desktop color keying; `DXGI_FORMAT_DX10`↔`DXGI_FORMAT_*` reinterpreted views (format aliasing rules per vendor tiling); YUV swapchains + `DXVA` video paths; `DXGI_WAITOBJECT` + `MsgWaitForMultipleObjects` integration (Agent 1 coupling); frame-latency-waitable pacing assumptions (many games mis-pace without it); D3D11 "deferred context" multithread ordering; D3D9Ex "create device reset" semantics and lost-device loops (must match Windows' present-then-reset dance); HDR SDR-paper-white; VRR+tearing `AllowTearing` gating (DWM historically refused tearing presents until flag opt-in — match: present fails `-DXGI_ERROR_UNSUPPORTED` unless app sets flag); NVAPI calls (P2, declare unsupported), Intel "D3D11on12" edge paths; drivers' `D3D11_1` "frame latency" quirks baked into games (e.g., some games rely on Windows driver bugs — need quirk DB, `Application Specific Driver State` spec is literally a published quirk channel to copy).

### 11.9 Areas requiring original implementation (no upstream source to lean on)
D3D9/10/11/12 + DXGI runtime binaries; the AMD/Intel/NVIDIA UMD command-emission engines (pattern knowledge from RADV/ANV/NVK is reference, code is original); dxgcore.ko; present arbitrator + TDR machine; **DXIL→NIR/LLVM front-end** (the single most valuable novel OSS artifact of this project — dxil-spirv exists as precedent but our DXIL→NIR is cleaner and ours); legacy DXBC readers for SM1–5 (using dxilconv as primary reference); WARP-analog software rasterizer for D3D11/D3D12 FL12 (original, or adopt OpenWARP with a health check on its status — **UNVERIFIED** maturity); D3D9 fixed-function on modern hardware; GDI raster stack; DWM-analog compositor; state-object/DXR acceleration structures (BVH builders: reference `gpurt` open docs on AMD; original elsewhere); `IDXGIOutputDuplication` capture; the driver-quirk DB.

## 12. Open questions → resolving experiments
| Question | Experiment |
|---|---|
| Can a D3DKMT-object kernel service ride DRM without fighting DRM mastership/lease rules? | Spike-0 (above) + `drm_master` arbitration design note to DRM list. |
| DXIL signing enforcement expectations of shipped titles | Corpus: 50 D3D12 titles, launch with/without `dxil.dll`-equivalent checks; observe `D3D12SerializeRootSignature`/shader-creation error codes on Windows capture vs ours. |
| fxc DXBC numerical parity on RDNA via our pipeline | dxc `-T vs_5_0`→dxilconv vs fxc DXBC golden shader set, pixel-diff on GPU farm. |
| Xe2+ command formats without PRMs | Build against ANV behavior with `IGT`-level capture of ANV batches; decision gate for Intel cooperation. |
| NVK-derived D3D12 backend viability | Port RADV `ac_surf`-equivalent tiling tests + DXIL→NVK backend toy; measure vs Windows RTX card. |
| userq vs `AMDGPU_CS` perf delta | Bench both submission paths under our UMD (draw-call storm + present latency). |
| libdxg UAPI drift (it's compute-subset) | Gap-analysis `d3dkmthk.h` ioctl list vs graphics DDI needs (present/vblank/display already known gaps). |
