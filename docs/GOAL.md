# Goal: an offline Rainbow Six Siege on Linux, and the NVIDIA display driver

Status date: 2026-10-14. This page states the end goal, the layers it needs and the
honest current state. Current state is taken from the repository documents named in
each row. Nothing here is a result of a game or driver run.

## The end goal

The project has two linked goals. Both use the same native personality: Windows
NT kernel-side behaviour and the Win32 userspace surface, implemented inside the
Linux kernel as a native personality. It is not a translation layer and this
document does not compare it with one.

1. **Play Rainbow Six Siege offline on Linux** with the owner's own local install
   of the game, running on this personality.
2. **Run the vendor's native NVIDIA Windows kernel-mode display driver** (the
   display miniport, `nvlddmkm.sys`, and its user-mode components) on the
   personality, as the GPU path for games.

Goal 2 is the stricter of the two. Goal 1 needs a GPU path, and the GPU path is
the native driver stack.

## Current state, in one paragraph

Early stage. Some NT services are implemented, and the VM probe results recorded
in [ROADMAP](ROADMAP.md) are the only boot evidence the documents give. The
README records no VM boot in this snapshot, so this page makes no boot claim. The
personality does not run Siege, no third-party driver loads or executes, and there
is no GPU path. The conformance record is 3 of 423 criteria with a recorded
passing result, and that count is historical, not reproduced (see
[README](../README.md)). The claim boundary for third-party drivers is in
[CLAIM-BOUNDARY.md](policy/CLAIM-BOUNDARY.md): no third-party `.sys` executes today.

## Layers

Each layer names what it needs, its state in the repository, and the measure that
tracks it. The import gap tool is the measuring stick for layers 1 to 8.

| # | Layer | What it needs | State in the repository | Measure |
|---|---|---|---|---|
| 0 | Core NT | Loader, object manager, handles, virtual memory, synchronization, I/O, registry, processes and threads | Partial. Components are rated in [ARCHITECTURE-STATUS](phase2/ARCHITECTURE-STATUS.md). 7 of 171 services are not implemented ([SERVICES](status/SERVICES.md)). | [CRITERIA](status/CRITERIA.md), [SERVICES](status/SERVICES.md) |
| 1 | Win32 breadth | Exports of kernel32, ntdll, msvcrt, advapi32, user32 and the API-set DLLs that a game's imports name | Partial export tables in `src/ntdll/*.def` | `tools/import_gap.py` on the local install |
| 2 | Graphics, D3D11 and DXGI | D3D11, DXGI and the shader compiler DLLs, translated to the Linux GPU stack | Not started. No D3D or DXGI DLL is provided. | Import gap, category `graphics` |
| 3 | Audio | The audio DLLs the game imports (XAudio2, DirectSound or WASAPI, to be confirmed from the import report) | Not started | Import gap, category `audio` |
| 4 | Input | Raw input, XInput and DirectInput as the game imports them | Not started | Import gap, category `input` |
| 5 | Window, user32 and gdi32 | Window classes, messages, GDI and the shell | Partial exports only | Import gap, categories `window` and `shell` |
| 6 | Networking | Winsock and the HTTP and DNS APIs, if the game's offline mode imports them | Not started | Import gap, category `networking` |
| 7 | Launcher and licensing | The game's own launcher and any licence check, as shipped in the owner's install | Not assessed. The project does not implement or bypass licensing or DRM checks. | Import gap, and the owner's own review of the install |
| 8 | Anti-cheat constraints | Kernel-mode anti-cheat modules, if the local install loads them | Not started. The project does not evade, patch or bypass anti-cheat. A required anti-cheat driver is a blocker that is recorded, not worked around. | Import gap, category `anti-cheat/driver` |
| 9 | Native GPU driver | See the section below | Not started. No driver runs today. | Import gap against the kernel surface, and the driver criteria |

### Native GPU driver (layer 9)

This layer is what goal 2 needs. Each part is a separate piece of work, and none
of them is started.

- **Kernel-mode driver hosting.** The personality must load a kernel-mode driver
  image, resolve its imports against the ntoskrnl export surface, call its
  `DriverEntry`, and unload it. Today the kernel surface is the set of functions
  the personality defines itself, listed in
  [NTOSKRNL-SURFACE](status/NTOSKRNL-SURFACE.md). It is not a Windows kernel
  export table.
- **WDM I/O model.** IRP creation, dispatch and completion, device objects, the
  device stack, and the I/O manager calls that a driver makes.
- **PnP and power.** Device enumeration, start and stop, resource assignment, and
  the power state transitions a display driver receives.
- **PCI configuration and BARs.** Reading the configuration space, sizing and
  mapping BARs, and enabling bus mastering for the GPU.
- **Interrupts and DPCs.** Connecting an interrupt, the interrupt service routine,
  and deferred procedure calls at the right IRQL.
- **DMA.** Common buffers and scatter-gather mapping for the GPU's transfers.
- **MmMapIoSpace and register access.** Mapping device memory and the failure
  paths that go with it.
- **Registry and service control.** The driver's registry keys, the service
  entries that start it, and the service control calls.
- **WDDM and dxgkrnl.** The display miniport interfaces (VidPN, the DXGKRNL
  callbacks, scheduling and memory segments) that the vendor driver expects from
  the Windows display stack. This is a large surface, and the import gap against
  the dxgkrnl imports is the way to size it.
- **User-mode driver.** Loading the NVIDIA Direct3D user-mode driver (`nvwgf2umx.dll`)
  and the other user-mode components, and the D3D11 and DXGI paths they call.
  This depends on layers 2 and 3.
- **Firmware.** Any GSP firmware image that the driver package carries, loaded at
  the time the driver asks for it.
- **Signing and tamper checks.** The driver package's signature and the checks
  that run on load. The project's claim boundary applies. A signature check that
  the personality cannot satisfy is a blocker, and the project does not disable it.

### Safety approach for the GPU driver

Hardware faults can hang the machine, and a broken display driver can leave the
host without a screen. The approach is therefore fixed before any driver work
starts:

1. Develop first in a QEMU guest with VFIO passthrough of a dedicated GPU that is
   not the host's display GPU.
2. If passthrough is not available, use a second, non-primary GPU in the host and
   keep it away from the host's display. Never use the host's only display GPU.
3. Keep a recovery path: a snapshot of the guest disk before each run, a watchdog
   that resets the guest, and a host console that does not depend on the GPU.
4. Do not write VBIOS or any other firmware to a device. Read only, unless the
   owner approves a specific write in writing.
5. Record each run's hardware, driver package hash and outcome before the next run.

## Using the gap tool

The tool reads PE files as bytes. It does not run, load or copy them, and it does
not download anything.

```
python tools/import_gap.py "<owner's local install directory>"
python tools/import_gap.py --write-ntoskrnl-surface
```

The first command writes `docs/status/GAME-IMPORT-GAP.md` and
`docs/status/GAME-IMPORT-GAP.json` in the repository. Those files list imports of
the owner's local binaries, so the owner decides whether to commit them. The same
command with a driver package's directory measures kernel-mode imports, against
the kernel surface. Details are in [tools/README.md](../tools/README.md).

## What is not in this repository

- No game binaries, no driver binaries and no firmware images are committed.
- The tools do not download any of them. The owner supplies a local path, and the
  tools read only what that path contains.
- No claim is made here that either goal is reached. A layer is complete only
  when its measure says so, and the measures are the repository documents above.
