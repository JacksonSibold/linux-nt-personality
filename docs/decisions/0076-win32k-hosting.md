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
- Known limits, not fixed here: `user32.dll` and `gdi32.dll` still hold separate copies of the model, so a DC from
  `GetDC` cannot be used by a gdi32 call. Only the S12 probe's gdi32 path is verified (DIB created and used inside
  gdi32). Calls from threads other than the loading thread act on the loading thread's queue. The model has no
  locking.
- `MsgWaitForMultipleObjects` supports `fWaitAll` FALSE only. `dwWakeMask` is not narrowed.
- Clipboard and CreateFontW remain unexported. They need a global memory service that the personality does not have.

The decision is revisited when cross-process messages or a second process need the window tree: that is option 1
or 3, and it needs its own record.
