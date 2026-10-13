# 0031: DllMain and TLS callbacks never run (LDR-3)

Status: Partly implemented in 10f42c8 (LDR-3). The MinGW build calls TLS callbacks, then `DllMain(hinst, DLL_PROCESS_ATTACH, NULL)`, through their function pointers (`ctx->run_entry_points`). A FALSE from attach returns `STATUS_DLL_INIT_FAILED` and removes the module. Detach calls `DllMain`, then the TLS callbacks. Host-tested: the attach result rule (`ldr_attach_result_ok`, `test_ldr_sched`). Needs VM: that PE code runs, and the failure path on a real image (the Linux build never calls entry points, by design).
Finding: `docs/review/ntdll.md` LDR-3 (High).
Date: 2026-10-12

## Context

The attach path is `ldr_process_attach` (`src/ntdll/ldr.c:1518`):

1. Cycle check on `ctx->init_stack` (lines 1522-1525).
2. `ldr_run_tls_callbacks(ctx, mod, DLL_PROCESS_ATTACH)` (line 1529).
3. `ldr_run_dll_main(ctx, mod, DLL_PROCESS_ATTACH)` (line 1530).
4. `mod->attached = 1`.

Neither step calls the image. `ldr_run_tls_callbacks` (line 1485) walks the table and calls only `ctx->tls_callback_hook` (line 1503). `ldr_run_dll_main` (line 1509) calls only `ctx->dll_main_hook` (line 1512). The entry point is computed at line 962 (`mod->entry.EntryPoint = mod->image + mod->pe.entry_point`, when `entry_point != 0`) and never called.

Two more defects sit on the same path:

- The hook type returns `void`, so a test cannot make attach fail. `ldr_load_dll_internal` ignores the result of `ldr_process_attach` (line 1690), so a `FALSE` from `DllMain` could not stop the load even after the call is made.
- `ldr_process_detach` (line 1538) calls the DllMain hook with `DLL_PROCESS_DETACH` but runs no TLS callbacks for detach. Windows runs them for thread and process detach as well.

Effect: no module initialises its globals, no DllMain can fail a load, and no TLS data is set up. Every Win32 DLL that relies on `DllMain` or TLS initialisation is broken. This is independent of LDR-1, because the MinGW build has no loader at all, but the fix is needed for the MinGW build too.

## Options

1. **Call the image directly in the existing functions.** In `ldr_run_tls_callbacks`, call each callback as `cb((PVOID)mod->image, reason, NULL)`. In `ldr_run_dll_main`, call `((BOOL (*)(HINSTANCE, DWORD, LPVOID))mod->entry.EntryPoint)(mod->image, reason, NULL)`. Keep the hooks as observers, called before the real call. Tradeoff: the Linux build cannot execute PE code, so this path is compiled but not run on the host.
2. **Gate the real call on a context flag.** Add `ctx->run_entry_points`, set by `winldr_ctx_create` to 1 for the MinGW build and to 0 for the host build. The host build sets it only in tests that use fixture code. Tradeoff: one more flag, but the host behaviour stays as now.
3. **Do the calls in the platform layer.** Move the calls into the MinGW-only layer from 0029. Tradeoff: ties the ordering to the platform layer, which is the wrong place for it.

## Recommendation

Option 2, with option 1's calls. The order is the one Windows documents: TLS callbacks with `DLL_PROCESS_ATTACH`, then `DllMain(hinst, DLL_PROCESS_ATTACH, NULL)`, with `mod->image` as `hinst`. Make the hook a `BOOL`-returning observer, so a test can set `FALSE` and check the failure path. On `FALSE` from attach, undo the load: remove the module from the lists and hash, decrement the dependencies it loaded, and return `STATUS_DLL_INIT_FAILED`.

This needs the MinGW build (0029) to exist before it can run, so the code is only partly verifiable now. It is recorded as needing VM for the execution part.

## Patch sketch

- `ldr.h`: `typedef BOOL (*ldr_dll_main_hook_t)(winldr_ctx_t *, PLDR_DATA_TABLE_ENTRY, ULONG, PVOID, void *);` (or keep `void` and add a second result hook; choose one and update `ldr_test.c`).
- `ldr_run_tls_callbacks`: after the observer hook, if `ctx->run_entry_points`, call `cb((PVOID)mod->image, reason, NULL)`.
- `ldr_run_dll_main`: if `ctx->run_entry_points && mod->entry.EntryPoint`, call the entry with `mod->image`, `reason`, `NULL`, and return the BOOL. For `DLL_PROCESS_ATTACH` only, a FALSE result fails the attach.
- `ldr_load_dll_internal` (line 1690): check the attach result, and on failure run the unwind path described above.
- Keep `DLL_PROCESS_DETACH` calls in `ldr_process_detach`. Add TLS callbacks with `DLL_PROCESS_DETACH` to the same path.
- The calling convention is `__stdcall`-equivalent on x64 (one convention), so a plain function pointer call is correct on MinGW x64. The Linux build must not reach the call, which the flag guarantees.

## Test plan

- Host (needs the test harness from 0030): with `run_entry_points = 0` and a hook that returns FALSE on attach, `LdrLoadDll` returns `STATUS_DLL_INIT_FAILED`, the module is absent from `winldr_find_module`, and dependencies loaded for it are released.
- Host: hook order. Record the sequence of observer calls for a module with two TLS callbacks and an entry point. Expect TLS, TLS, DllMain.
- Host: a dependency attaches before its importer, under the current init order (regression).
- Needs VM: a MinGW test DLL that sets a global in `DllMain` and a TLS callback that sets another. The test exe reads both through its imports. Also check that a DLL whose `DllMain` returns FALSE fails `LoadLibrary`.

## Not verified

- The Windows order (TLS callbacks before `DllMain`) is taken from the PE/COFF and Windows loader behaviour as commonly documented. It was not checked against the current spec text in this review.
- The unwind on attach failure was not traced through all paths, including `ldr_unload_internal`. It needs review when implemented.

## Outcome (10f42c8)

- Hook type unchanged. The observer hooks stay `void`, so `ldr_test.c` is unchanged. The result is the `DllMain` return value, which the host cannot exercise, because `run_entry_points` is 0 on the host.
- Not done: releasing the dependencies a failed module loaded (their `LoadCount` stays raised). The same gap exists on the old resolve-imports failure path.
- Ordering: `ldr.c` still attaches by recursion (dependencies first). `ldr_sched_attach_order` is tested in `test_ldr_sched` but is not called from `ldr.c` yet.
