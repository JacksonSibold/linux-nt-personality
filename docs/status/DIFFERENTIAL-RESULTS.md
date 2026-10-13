# Differential probes inside the NT personality kernel: results

Date: 2026-10-13. Kernel: Linux 6.18.56 with the NT personality (`docs/status/KERNEL-BOOT.md`), booted under
host QEMU with `-kernel`. Oracle: host-captured Windows goldens `tests/differential/golden/windows-26200.9457/`.
Comparator: the repo's own `tests/differential/dualrun.py` (`normalize_json`, `diff_jsons`, `evaluate_probe`),
called by `tools/linux-env/probe_results.py compare`.

## Five-line report

1. Probes run in the personality: 8 of 8 VM-capable probes (`*_direct`). All 8 ran to exit status 0 in the final boot. Not run: the 14 non-direct probes, which have no VM branch and need ntdll (`binfmt_pe`: "ntdll not mapped").
2. Probes matching goldens: 0 of 8 at the verdict level (the repo's policy allows 0 S1). Field level, `probe_sharemode_direct` matches 462437 of 462451 fields and `probe_sync_direct` could not be compared (see below).
3. Top divergence causes: (a) the NT namespace has no `C:\tmp`, so every scratch-file test returns `STATUS_OBJECT_PATH_NOT_FOUND` (0xC000003A); (b) `NtCreateProcessEx` and other services are not implemented (`STATUS_NOT_IMPLEMENTED`), which empties the cross-process and handle-query sections; (c) VirtualQuery region coalescing and status codes differ (`probe_virtualquery_direct`: 1279 S1 fields).
4. Fixed one kernel bug (`src/kernel/fs/binfmt_pe.c`): the second PE in one boot always failed to load (`pe_map_sections failed after exec commit (err -99)`). Before: 1 probe ran, 7 killed by SIGSEGV. After: 8 of 8 ran. M0 still passes (`m0-serial-after-binfmt-fix.log`).
5. Harness: `tools/linux-env/probes-initramfs.sh` builds the probes and the initramfs; `boot-m0-host.ps1 -Initrd -Serial -Append` boots them; `probe_results.py split|compare` produces the verdicts.

## Progress after the divergence fixes (2026-10-13)

Before = run 4 (`probes-serial-run4.log`, the kernel of the table below). After = run 6 (`probes-divfix2-serial.log`, the kernel built from the commits listed underneath). Matched/total are `diff_jsons` field counts against the Windows golden. Run 5 (`probes-divfix-serial.log`, sync fix only, no `/tmp`, no exit fix) changed only `probe_sync_direct`, so the other rows are the same as run 4 there.

| probe | matched before | matched after | mismatched before to after | verdict after |
|---|---|---|---|---|
| probe_handles_direct | 93 / 121 | 114 / 121 | 28 to 7 | FAIL |
| probe_ntstatus_direct | 3510 / 3977 | 2904 / 3977 | 467 to 1073 (regression) | FAIL |
| probe_path_direct | 121507 / 237589 | 122713 / 237589 | 116082 to 114876 | FAIL |
| probe_process_thread_direct | 177 / 184 | 177 / 184 | 7 to 7 | FAIL |
| probe_section_direct | 106 / 129 | 103 / 129 | 23 to 26 (regression) | FAIL |
| probe_sharemode_direct | 462437 / 462451 | 462439 / 462451 | 14 to 12 | FAIL |
| probe_sync_direct | not compared (guard) | 74 / 74 | 0 after (5 in run 5) | **PASS** |
| probe_virtualquery_direct | 47902 / 49181 | 47902 / 49181 | 1279 to 1279 | FAIL |

Probes matching their goldens at the verdict level: 1 of 8 (`probe_sync_direct`). All 8 exit 0 and reach `NTPERS_ALL_DONE`.

Commits in the "after" kernel, in order:

- `8d23a6a` comparator: `probe_sync_direct` mandatory keys match the probe (the sync probe is now compared).
- `d4dbbb3` kernel: the sync thread is per task (`src/kernel/nt/sync.h`). It was one process-wide global, so threads shared one sync thread.
- `2673694` kernel: a task's owned mutants are abandoned at exit (`arch/x86/nt/dispatch.c` `nt_thread_exit_abandon`, called from `do_exit` by `tools/ntpers_kpatch.py`). Nothing abandoned them on a normal thread return. This is the change that makes the sync probe match.
- `db518ef` harness: `probes-initramfs.sh` mounts tmpfs on `/tmp`. The kernel maps the NT path `C:\tmp` to `/tmp`; the initramfs had no `/tmp`, so scratch creates returned `STATUS_OBJECT_PATH_NOT_FOUND` (0xC000003A).

The `/tmp` and exit-abandon changes were measured together, so the handles, path and ntstatus rows cannot be attributed to one of them. The two regressions (ntstatus, section) are not explained yet. The likely cause is the `/tmp` mount changing which paths succeed, but this is a hypothesis.

Regression checks on the same kernels: M0 exit 42 (PASS) and S1 `hello from NT` exit 0 (PASS), before and after the exit-abandon change.

Not done: the 14 non-VM probes (ntdll packing), VirtualQuery (1279 S1 fields, unchanged), and the two regressions.

## Fix cycles after the first table (runs 7 to 10, 2026-10-13)

| run | kernel (commit) | initramfs | result |
|---|---|---|---|
| 7 | build 2 + exit abandon (`2673694`) | no `/tmp` (old) | ntstatus 3510, section 106: the same as run 4. The two regressions come from the `/tmp` mount, not the exit change. |
| 8 | build 4 (adds `3cd5e5f`) | `/tmp` | 7 of 8 probes killed (SIGKILL). Cause: a kernel SMAP fault on the user `AllocationSize` read in `NtCreateFile`. Fixed in `87087ff` and `510d8d4`. |
| 9 | build 5 (SMAP read fixed) | `/tmp` | all 8 exit 0. Same numbers as run 6. |
| 10 | build 6 (adds `236e741`) | `/tmp` | all 8 exit 0. `probe_section_direct` 104/129 (was 103); `sec_file_commit` now matches the golden. |

Run 10 per probe (matched / golden leaves): handles 114/121, ntstatus 2904/3977, path 122713/237589, process_thread 177/184, section 104/129, sharemode 462439/462451, sync 74/74 (PASS), virtualquery 47902/49181.

Still open:

- **ntstatus regression (467 to 1073 mismatches):** after `/tmp` exists, the probe reaches real NT statuses where it used to hit `OBJECT_PATH_NOT_FOUND` everywhere, so its distinct-status list is positionally shifted. This is a real S1 divergence exposed by `/tmp`, not a kernel regression. Not fixed.
- **section (26 mismatches):** pagefile `NtMapViewOfSection` at offset 8192 (Windows 0xC0000220, VM 0); `map_file_commit.base` (an address the normaliser does not ignore; ASLR); `sec_image` (Windows 0xC000012F, VM 0xC0000002). Not fixed.
- **Non-VM probes (`tools/linux-env/probes-nonvm-initramfs.sh`):** ntdll now maps (`/windows/system32/ntdll.dll`), and the loader runs. All 15 probes then segfault: the probes import `ADVAPI32.dll`, which the initramfs does not provide. The loader leaves an unresolved import's RVA in the IAT and jumps there (`ip` = 0xf5fc, 0x10566). Needs an advapi32 shim, and the loader should fail the load when an import is unresolved, as Windows does (`STATUS_ENTRYPOINT_NOT_FOUND`). Not done.
- **VirtualQuery** (1279 S1 fields), **NtCreateProcessEx / sharemode** (12 fields): not started.

Commits added in this cycle: `2834e75` (exit helper moved into `src/kernel/nt/sync.c`), `3cd5e5f` and `cba9ff9` (create AllocationSize, src and arch mirror), `87087ff` and `510d8d4` (SMAP-safe read of the user pointer, src and arch mirror), `236e741` (explicit zero maximum size), and the non-VM initramfs script.

## How results leave the VM

- Each probe is built in the guest with `-DNTPERS_VM -DVM_BUILD` and `-Dlinux_open=ntpers_vm_open -Dlinux_write=ntpers_vm_write -Dlinux_close=ntpers_vm_close`. The probe source is unchanged. The shim `tools/linux-env/ntpers_vm_stdout.c` routes the probe's stdout to NT console handle -11, which the kernel prints through `printk` (`arch/x86/nt/nt_io.c`, `ntio_console_write`). The probe's own `linux_write(1, ...)` is a raw `syscall` with rax=1, which the NT ABI dispatches as an NT service number, so it cannot reach stdout.
- `/init` runs each probe under `timeout 900` and frames it with `NTPERS_PROBE_BEGIN/RC/END` markers written to `/dev/kmsg`, so the markers share the printk sequence with the probe output.
- Limits found on the way: printk truncates a record at about 1 KB, so stdout is written in chunks of 700 bytes. The printk ring is 128 KB by default (`CONFIG_LOG_BUF_SHIFT=17`). Boot with `log_buf_len=16M`. Probes writing more than one copy of their JSON (`probe_virtualquery_direct` writes it through `linux_write(1)` and again through `NtWriteFile(-11)`) are handled by taking the first complete JSON object.

## Per-probe results (final boot, `probes-serial-run4.log`)

Golden leaves are the normalised leaf count of the Windows golden. Matched and mismatched are field counts from `diff_jsons` (S4 normalisations excluded).

| probe | ran | exit | JSON | matched / golden leaves | mismatched fields | verdict |
|---|---|---|---|---|---|---|
| probe_handles_direct | yes | 0 | yes | 93 / 121 | 28 | FAIL (8 S1) |
| probe_ntstatus_direct | yes | 0 | yes | 3510 / 3977 | 467 | FAIL (463 S1) |
| probe_path_direct | yes | 0 | yes | 121507 / 237589 | 116082 | FAIL (42363 S1) |
| probe_process_thread_direct | yes | 0 | yes | 177 / 184 | 7 | FAIL (1 S1) |
| probe_section_direct | yes | 0 | yes | 106 / 129 | 23 | FAIL (4 S1) |
| probe_sharemode_direct | yes | 0 | yes | 462437 / 462451 | 14 | FAIL (6 S1) |
| probe_sync_direct | yes | 0 | yes | not compared | 1 | FAIL (comparator guard, see below) |
| probe_virtualquery_direct | yes | 0 | yes | 47902 / 49181 | 1279 | FAIL (1279 S1) |

No probe crashed or hung in the final boot. Before the kernel fix, 7 of 8 died with exit status 139.

### First mismatches (from `compare.json`)

- **probe_handles_direct**: `create_status.h1` (Windows 0, VM 0xC000003A), `create_status.h1_hex`, `create_status.h2`, `create_status.h2_hex`, `create_status.hFile2`. Then `duplicate_handle` and `nt_query_object` arrays are shorter on the VM side (len 1 vs 7 and 1 vs 6), and `handles[0..3].value` differ (Windows 236/240/244/248, VM 0/0/200/204).
- **probe_ntstatus_direct**: `distinct_summary.distinct_list` (len 15 vs 1), `distinct_list[0]` (0xC0000003 vs 0xC000003A), `distinct_via_real_syscalls` (15 vs 1), `total_real_cases` (15 vs 1), `fallback_tests[2].dos` (128 vs 317).
- **probe_path_direct**: `tests[0].dos` (2 vs 3), `tests[0].hex` (0xC0000034 vs 0xC000003A), `tests[0].nt_path` (`\??\<root>\...` vs `\??\C:\tmp\...`), `tests[0].ntstatus`, `tests[10000].dos` (3 vs 123).
- **probe_process_thread_direct**: `process_info[9].retLen` (202 vs 120), `exit_codes.create_suspended_after` (777 vs 0), `exit_codes.create_suspended_before` (259 vs 0), `exit_codes.thread_after` (777 vs 0), `exit_codes.thread_before_wait` (259 vs 0).
- **probe_section_direct**: `map_commit_offset_8192.hex` (0xC0000220 vs 0x0), `map_commit_offset_8192.ntstatus`, `map_real_image.viewSize` (2519040 vs 2514944), `vquery_commit[1].RegionSize` (61440 vs 65536), `map_commit.base` (0x23662f20000 vs 0x20000, an address field not in the normaliser's key list).
- **probe_sharemode_direct**: `cross_process` (Windows len 512, VM len 0), `cross_process_missing_hex` (0 vs 0xC0000002), `cross_process_missing_service` (empty vs `NtCreateProcessEx`), `cross_process_missing_ssn` (0 vs 77), `cross_process_missing_status` (0 vs -1073741822).
- **probe_sync_direct**: the comparator's vacuous guard fires on both sides, so no field was compared. `MANDATORY_KEYS["probe_sync_direct"]` is `["tests"]`, but the probe emits `events` and `mutant` and no `tests`. This is a comparator configuration gap in `tests/differential/dualrun.py`, not a kernel result.
- **probe_virtualquery_direct**: `negatives[0].status` and `negatives[1].status` (Windows 0, VM 0xC000000D), `release.status` (0xC00000A0 vs 0), `release.success` (False vs True), `transitions[1281].mbi.RegionSize` (4096 vs 8192).

## Root-cause hypotheses (not yet proven)

1. **No `C:\tmp` in the NT namespace** (handles, ntstatus, path). The scratch path `\??\C:\tmp\...` is not found, so the create returns `STATUS_OBJECT_PATH_NOT_FOUND` (0xC000003A) where Windows returns success or `STATUS_OBJECT_NAME_NOT_FOUND` (0xC0000034). This cascades into the handle and distinct-status arrays. Likely the kernel FS/namespace layer (`src/kernel/fs`, `src/kernel/nt/io.c`). Not fixed.
2. **Missing services** (sharemode cross-process: `NtCreateProcessEx`, SSN 77, returns `STATUS_NOT_IMPLEMENTED`; handle query: `NtQueryObject` and `DuplicateHandle` arrays truncated). `docs/status/KERNEL-BOOT.md` already records 43 listed services as unimplemented.
3. **Probe-side table, not kernel** (ntstatus `fallback_tests[2].dos`). The VM build of `probe_ntstatus_direct` maps `RtlNtStatusToDosError` through a hard-coded switch with default 317. The VM value is the probe's own stub, so it cannot be a kernel result. Do not attribute it to the kernel.
4. **Thread exit codes** (process_thread). The kernel reports 0 where Windows reports `STILL_ACTIVE` (259) or 777 for a live thread. Likely `GetExitCodeThread` returns the stored exit code before the thread has exited. Not fixed.
5. **VirtualQuery coalescing and argument validation** (virtualquery, section). Region sizes and statuses differ at specific transitions, and invalid arguments are accepted (`negatives`). Likely `src/kernel/nt/mm.c` VAD coalescing and argument checks. Not fixed.
6. **Comparator gap** (sync). Listed above; this is in the repo's comparator, not the kernel.

## The kernel fix (before and after)

`src/kernel/fs/binfmt_pe.c`: `load_pe_binary` now calls `ntmm_reset()` right after `begin_new_exec` commits, under `CONFIG_NT_PERSONALITY`.

Cause: the kernel keeps one NT VAD map (`ntmm_global_space` in `src/kernel/nt/mm.c`). Only the userspace shim destroys a process space on exit. The second PE in a boot found the first image's header and section regions still registered, so `ntmm_add_region` returned `STATUS_CONFLICTING_ADDRESSES` (reported as `-EADDRNOTAVAIL`, -99) and the exec was killed after commit.

- Before (`probes-serial-run2.log`, old bzImage): `probe_handles_direct` exit 0, the other 7 exit 139 at `pe_map_sections failed after exec commit (err -99)`.
- After (`probes-serial-run4.log`, new bzImage): all 8 exit 0.
- M0 regression, after the fix (`m0-serial-after-binfmt-fix.log`): `M0 exit status: 42`, `M0 PASS`.

Limit: the VAD map is still global. Two PEs running at the same time would share it. Per-process VAD maps in the kernel are the real fix and are not done here.

## Not run

The 14 probes without an NTPERS_VM branch (`probe_directory`, `probe_direct_simple`, `probe_exception`, `probe_fileinfo`, `probe_handles`, `probe_namedpipe`, `probe_ntstatus`, `probe_path`, `probe_peloader`, `probe_process_thread`, `probe_registry`, `probe_section`, `probe_sharemode`, `probe_sync`, `probe_teb_peb`) resolve ntdll through `GetProcAddress`. The personality maps no ntdll, so they cannot run until ntdll is loaded. Their `*_direct` variants are the ones run here.

## Reproduce

Guest (SSH via `tools/linux-env/ssh-guest.sh`, repo synced with `GUEST_DEST=dualrun tools/linux-env/sync-repo.sh`):

```sh
sh tools/linux-env/probes-initramfs.sh ~/dualrun ~/dualrun-probes.cpio.gz
python3 tools/ntpers_kpatch.py --tree ~/ntpers/linux-6.18.56 && cd ~/ntpers/linux-6.18.56 && make -j2 ARCH=x86_64 bzImage
python3 tools/linux-env/probe_results.py split <serial.log> <outdir>
python3 tools/linux-env/probe_results.py compare <outdir> tests/differential/golden/windows-26200.9457
```

Host (copy `bzImage` and `probes-initramfs.cpio.gz` into `C:\Users\Jackson\repos\linux-env\kernel-6.18.56\`):

```powershell
powershell -ExecutionPolicy Bypass -File tools/linux-env/boot-m0-host.ps1 -TimeoutSec 1500 -Initrd probes-initramfs.cpio.gz -Serial probes-serial.log -Append 'console=ttyS0,115200 earlyprintk=serial,ttyS0,115200 panic=-1 log_buf_len=16M'
```

Serial logs are kept on the host in `C:\Users\Jackson\repos\linux-env\kernel-6.18.56\` (`probes-serial-run1..4.log`, `m0-serial-after-binfmt-fix.log`). They are not committed (about 60 MB).
