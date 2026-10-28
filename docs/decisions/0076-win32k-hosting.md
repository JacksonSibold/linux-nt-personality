# 0076: Hosting of the win32k windowing layer

Finding: none in `docs/review/`. Gap found in the win32k spike (`src/ntdll/win32k`, `user32_win32k.c`,
`gdi32_win32k.c`): the model was never initialised in the DLL build, `GetMessageW` returned -1 with
ERROR_TIMEOUT (1460) instead of blocking, and `user32.dll` and `gdi32.dll` each linked a private copy of the
model.

## Options

1. **In-kernel win32k-style service table.** Windows, desktops, queues and DCs live in the kernel. Each user32
   and gdi32 export becomes an NT service (`nt_service_list.h`, `nt_svc_*`). Cross-process messages work by
   construction. Tradeoff: about 150 new services, kernel objects for every window class, and a second
   personality-side copy of the model. Largest change, and nothing in the kernel needs it for the S12 goal.
2. **Per-process userspace model in ntdll-hosted DLLs, blocking through the kernel wait path.** The model stays
   in `user32.dll` and `gdi32.dll`. Blocking uses existing NT services: one auto-reset event per thread
   (`NtCreateEvent`, `NtSetEvent`, `NtWaitForSingleObject`, `NtWaitForMultipleObjects`), and the model clock is
   read from `NtQuerySystemTime`. Tradeoff: messages and windows are per process, so `SendMessage` and
   `PostMessage` across processes do not work. The model is not thread-safe between threads of one process.
3. **Userspace model with state shared through a kernel object.** A named section holds the model. Tradeoff:
   a kernel-side lock protocol is needed for every call, and the section layout becomes an ABI.

## Decision

Option 2. The layer is a per-process service model, and the kernel stays as it is except for the NT services that
already exist (no new `nt_svc_*`). Layering: the kernel provides only waits, events and the system clock. user32
and gdi32 own the model.

Consequences:

- `GetMessageW`, `WaitMessage` and `MsgWaitForMultipleObjects` block on the calling thread's event. The model's wake
  hook (`w32k_set_wake_hook`) sets the event when a message is queued to the thread (`w32k_queue_push`) or a paint
  is invalidated. Auto-reset events keep a signal that arrives before the wait, so no wake is lost. A pending timer
  bounds the wait at its due time (`w32k_timer_wait_ms`). There is no -1/1460 path any more.
- `user32.dll` runs `w32k_init` on process attach (`DllMain` in `user32.c`, which calls `w32k_shim_attach`).
  `gdi32.dll` runs `w32k_init` in its own `DllMain`.
- Known limits, not fixed here: only the S12 probe's gdi32 path is verified in a guest (DIB created and used inside
  gdi32). Calls from threads other than the loading thread act on the loading thread's queue. The model has no
  locking.
- `MsgWaitForMultipleObjects` supports `fWaitAll` FALSE only. `dwWakeMask` is not narrowed.
- Clipboard and CreateFontW remain unexported. They need a global memory service that the personality does not have.

The decision is revisited when cross-process messages or a second process need the window tree: that is option 1
or 3, and it needs its own record.

## Addendum: one model per process, in `win32u.dll` (2026-10-28)

The first cut linked the model objects into both `user32.dll` and `gdi32.dll`, so each DLL had its own window tree,
handle table and DCs. A DC from `GetDC` was not a valid handle to a gdi32 call, which no real program can live with.

The model is now linked once, into `win32u.dll` (`src/ntdll/win32u.c`). `user32.dll` and `gdi32.dll` import it and
keep only their ABI shims. Windows itself puts the services in `win32u.dll` under both DLLs, so the layering matches.

- Exports: every file-scope `w32k_*` prototype in `win32k/w32k.h`, written to `out/win32u.def` by
  `build/gen_win32u_def.py` at build time (a unit test checks each one is defined in a `w32k_*.c`). It is a private
  interface, not the `NtUser*`/`NtGdi*` call surface of Windows' `win32u.dll`.
- Init: `w32k_init_once()` is idempotent and is called from the `DllMain` of `win32u`, `user32` and `gdi32`, so the
  load order does not matter and a second attach never resets state. `user32` still installs the wake hook and the
  loading thread.
- Packaging: `win32u.dll` and `gdi32.dll` are added to the DLL lists in `vm/mkinitramfs.sh`, `vm/freshness.sh` and
  `tools/linux-env/probes-initramfs.sh`. Build with `make -C src/ntdll all kernel32 msvcrt advapi32 win32u user32 gdi32`.
- Verified: the DLLs link (`user32` imports kernel32, ntdll and win32u; `gdi32` imports win32u) and the host suites
  pass. Not verified: a guest boot of S12 with the new DLL layout.

## Addendum: breadth added on the shared model (2026-10-28)

`user32.dll` grew from 85 to 190 exports on top of `win32u.dll`, and the model gained raw input:

- **Window state and tree** (`win32k/w32k_win.c`): show and enable state, `GetWindow` relations, a desktop pseudo window
  (class `#32769`, never listed or destroyable), class values, z-order, `EnumWindows` and `EnumChildWindows` (snapshotted, so a
  callback may destroy windows), hit test, update-region helpers, the cursor display counter and `GetMessageTime`/`GetMessagePos`.
- **Model-free logic** (`user32_pure.h`, host-tested): RECT arithmetic, `AdjustWindowRectEx` (a Windows 10 caption window grows
  8 on the sides and bottom and 31 on top), the Windows 10 system colours, case mapping for ASCII, Latin-1, Greek and Cyrillic,
  and the US layout tables behind `MapVirtualKey`, `VkKeyScan`, `ToUnicode` and `GetKeyNameText`.
- **Raw input** (`win32k/w32k_rawinput.c`): `RegisterRawInputDevices` for the mouse (1/2) and keyboard (1/6), `WM_INPUT` to the
  registered window with RIM_INPUT or, with `RIDEV_INPUTSINK`, RIM_INPUTSINK, `RIDEV_NOLEGACY` suppressing the legacy messages,
  and the x64 RAWINPUT byte layout (mouse 48 bytes, keyboard 40). Evdev relative motion reaches raw input unclamped, even at the
  screen edge. `GetRawInputData`, `GetRawInputDeviceList` and `GetRawInputDeviceInfoW` forward to it.
- **Honest stubs**: `MessageBoxW` shows nothing and returns the affirmative button; `LoadStringW` returns 0 with
  `ERROR_RESOURCE_NAME_NOT_FOUND` until module resources can be read; window regions and layered attributes are accepted and have no effect.
- **Limits**: one update region per window, no menus, no owner windows, no `SetParent`, the window model still runs on the loading
  thread's queue unless the call binds the thread (the message functions do).

