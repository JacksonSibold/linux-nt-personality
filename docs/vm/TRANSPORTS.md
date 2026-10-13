# VM transports: 9p share and initrd-serial

The VM rig moves data between the host and the guest in two ways. `VM_TRANSPORT`
selects one of them. The default is `9p`, which is the behaviour the repository
was built on. `initrd-serial` exists for hosts whose QEMU has no 9p and no KVM,
such as QEMU for Windows in TCG mode (docs/status/LINUX-ENV.md, blocker 4).

| `VM_TRANSPORT` | Test scripts and binaries in | Result files out | Host needs |
|---|---|---|---|
| `9p` (default) | the 9p share at `/mnt/share` (`-virtfs`, tag `share`) | the same share, `.dodresult/` | QEMU with 9p, KVM optional |
| `initrd-serial` | packed into the initramfs at the same paths (`/mnt/share/...`) | framed on the serial console, rebuilt on the host by `tools/vm_serial_collect.py` | QEMU with `-kernel`/`-initrd`, TCG is enough |

Guest paths do not change between the two modes. A test script named
`/mnt/share/vm/test_m0.sh` runs at that path in both, and it writes its results to
`/mnt/share/.dodresult/`. Only the transport differs.

## How to run each transport

9p, on a Linux host with QEMU and the 9p share (unchanged, and the only mode with the freshness gate):

```
vm/runvm.sh --test /mnt/share/vm/test_m0.sh          # or: vm/check_m0.sh
```

initrd-serial, on a Linux host, through the gated runner. Build the initramfs with the payload, then run:

```
VM_TRANSPORT=initrd-serial vm/mkinitramfs.sh          # packs vm/ and build/probes under /mnt/share
VM_TRANSPORT=initrd-serial vm/runvm.sh --test /mnt/share/vm/test_m0.sh
```

`runvm.sh --transport initrd-serial` does the same. Both run the freshness gate first.

initrd-serial, on the Windows host (no 9p, no C toolchain, no cpio), with no gate. It is for
boot-and-collect work, not verification:

```
python3 tools/vm_initrd_pack.py --base <kernel-dir>/m0-initramfs.cpio.gz --out <out>/initrd.cpio.gz \
    --add tools/linux-env/init-serial.sh=init --add vm/emit-results.sh=emit-results.sh --add vm=mnt/share/vm \
    --link bin/cat=busybox --link bin/tee=busybox --link bin/date=busybox --link bin/mkdir=busybox \
    --link bin/rm=busybox --link bin/sync=busybox --link bin/base64=busybox --link bin/sha256sum=busybox \
    --link bin/find=busybox --link bin/sort=busybox --link bin/cut=busybox --link bin/tr=busybox --link bin/wc=busybox
powershell -ExecutionPolicy Bypass -File tools\linux-env\run-vm.ps1 -Initrd <out>\initrd.cpio.gz -OutDir <out>\run1 -Test /mnt/share/vm/test_m0.sh
```

`tools/linux-env/run-vm.sh` is the same runner for a Linux or WSL host with `qemu-system-x86_64`.
`run-vm.ps1` needs `-Python` (or `NTPERS_PYTHON`) set to a real interpreter, because `python` may
resolve to the Microsoft Store alias.

The collector can also be run alone on any saved serial log:
`python3 tools/vm_serial_collect.py --log serial.log --out <dir>`.

## Inventory of 9p share use

Inputs (host to guest) read from `/mnt/share`:

- Test scripts passed as `--test` and run by `init.c` through `dodtest=`:
  `vm/test_*.sh`, `vm/diag_m0.sh`, `vm/run_section.sh`, `vm/check_fs.sh`, the
  default `/mnt/share/vmtests/smoke.sh` (not in the repository), and the
  host-written `.dodresult/dualrun_guest_<probe>.sh` used by `tests/differential/dualrun.py`.
- Test binaries copied from the share into the guest: `/mnt/share/build/probes/*.exe`
  (`vm/check_r3.sh`, `vm/check_fs.sh`), and the probe JSON helpers that open
  `/mnt/share/.dodresult/...` in `tests/differential/probes/*_direct.c`.
- The repository tree itself, mounted read-only in intent but writable in practice
  (`security_model=none`).

Outputs (guest to host) written under `/mnt/share/.dodresult/`:

- `result.exitcode`: written by `init.c` (`write_result`) and by the test
  scripts (`test_m0.sh`, `run_section.sh`, `check_fs.sh`, `check_r3.sh`, `diag_m0.sh`).
  This is the one file `runvm.sh` must read back.
- Per-test JSON and logs: `test_m0.json`, `test_m0.log`, `test_reg.json`,
  `smoke.marker`, `m0_diag_dmesg.log`, `probe_*.json`, `dualrun_vm_*.json`, and similar.
- Serial logs: `serial-*.log` and `m0_gate_serial.log` are written by the host
  (`runvm.sh --log`), not by the guest.

Host-side readers of the share: `runvm.sh` (creates `.dodresult/`, removes and
polls `result.exitcode`, writes `current_run_id`), `check_m0.sh`, `check_reg.sh`,
`smoke.sh`, `guest-bootstrap.sh`, `tests/differential/dualrun.py`.

Not share users: `mkinitramfs.sh` (it only creates the `mnt/share` mount point and
packs `/tests_kernel`, `/tests_kernel/*`, the DLLs and `busybox` into the initramfs),
`freshness.sh`, `preflight.sh`, `kbuild.sh`, `make-kernel-config.sh` (the config
option comment only).

The table above is what the transport switch has to cover. Anything that is not in
the table keeps its present behaviour.

## Design

- `runvm.sh` keeps the freshness gate, timeout, exit-code contract and result
  polling. With `initrd-serial`, it does not pass `-virtfs`. It appends
  `dodtransport=initrd-serial` to the kernel command line and, after QEMU exits,
  runs `tools/vm_serial_collect.py` on the serial log. The collector rebuilds
  `.dodresult/` in the share directory, and the existing result read-back runs unchanged.
- `init.c` in `initrd-serial` mode skips the 9p mount. `/mnt/share` is the
  writable initramfs tree, so `.dodresult/` is created in RAM. After the test it
  runs `/emit-results.sh /mnt/share`, then powers off.
- The emitter (`vm/emit-results.sh`) uses only busybox applets. It prints, per file
  under `.dodresult/`, a header line `@@NTPERS-FILE <path> <size> <sha256>@@`, the
  file as base64 (default 76-column lines), and `@@END@@`. The stream is bounded by
  `@@NTPERS-EMIT-BEGIN@@` and `@@NTPERS-EMIT-END@@`.
- The collector accepts the stream only when every frame verifies: the byte count
  matches, the sha256 matches, and the base64 decodes. Any failure writes nothing
  and exits 1. An emit without its end marker (the guest did not finish, or the
  VM was killed) exits 3, and `runvm.sh` then reports TIMEOUT or missing result as it
  does for 9p.
- Packing: `mkinitramfs.sh` (Linux, with `cpio`) and `tools/vm_initrd_pack.py`
  (any host with Python, used when the Linux tools are missing) both enforce a size
  cap on the uncompressed payload, default 32 MiB, `VM_PAYLOAD_MAX_KB`.

## Dry run: M0 payload through initrd-serial on the Windows host

Run on the Windows host with the kernel at `C:\Users\Jackson\repos\linux-env\kernel-6.18.56\bzImage`
and the M0 initramfs from the same directory. The artifacts are in `tests/host/out/vmdryrun/`
(gitignored).

1. Pack the M0 base with the serial `/init`, the emitter, and the `vm/` payload:

   ```
   python3 tools/vm_initrd_pack.py --base <kernel-dir>/m0-initramfs.cpio.gz \
       --out tests/host/out/vmdryrun/serial-initrd.cpio.gz \
       --add tools/linux-env/init-serial.sh=init --add vm/emit-results.sh=emit-results.sh \
       --add vm=mnt/share/vm --link bin/cat=busybox --link bin/tee=busybox \
       --link bin/date=busybox --link bin/mkdir=busybox --link bin/rm=busybox \
       --link bin/sync=busybox --link bin/base64=busybox --link bin/sha256sum=busybox \
       --link bin/find=busybox --link bin/sort=busybox --link bin/cut=busybox \
       --link bin/tr=busybox --link bin/wc=busybox
   ```

   Result: 91 entries (13 from the base), 2148720 bytes uncompressed, 1069580 bytes gzipped.
   `init` replaced the M0 init. `/mnt/share/vm/test_m0.sh` is the unchanged repository script.

2. Boot and collect, with the Windows interpreter named because `python` resolves to the Store alias:

   ```
   powershell -ExecutionPolicy Bypass -File tools\linux-env\run-vm.ps1 \
       -Initrd tests\host\out\vmdryrun\serial-initrd.cpio.gz -OutDir tests\host\out\vmdryrun\run1 \
       -Test /mnt/share/vm/test_m0.sh -TimeoutSec 240 -Python <WinPython>\python.exe
   ```

   QEMU exited 0 (TCG, no timeout, no Application Control refusal). The collector verified 3 frames
   and wrote 3 files. The launcher exited with the guest's `result.exitcode`.

Guest behaviour, from the serial log: `init: transport: initrd-serial (sh init)`, then
`PE exit RC=42 (masked 42)` from `/tests_kernel/minimal.exe`, then `"status": "PASS"`, then
`init: test exited with code 0`, then the emit between `@@NTPERS-EMIT-BEGIN@@` and `@@NTPERS-EMIT-END@@`.
The `result.exitcode` value is the test script's exit status (0 for PASS), as in 9p mode. The PE exit
code 42 is in `test_m0.json`, which is what `check_m0.sh` reads.

Match check, computed on the host:

| Rebuilt file | Bytes | Frame header sha256 | sha256 of rebuilt file |
|---|---|---|---|
| `.dodresult/result.exitcode` | 2 | `9a271f2a...ab86aa` | same |
| `.dodresult/test_m0.json` | 155 | `9b6b7708...414c85b` | same |
| `.dodresult/test_m0.log` | 244 | `c6d47d4c...a3bafb` | same |

Negative runs on the real log (`tests/host/out/vmdryrun/neg/`):

- Log cut inside the `test_m0.json` frame: collector exit 3, nothing written.
- One base64 character changed in that frame: exit 1, `sha256 mismatch`, nothing written.
- Four kernel log lines inserted between and inside frames: exit 0, `noise_lines=3`, and
  `test_m0.json` byte-identical to the clean run.
- `vm/runvm.sh --transport initrd-serial` on this host: refused by the freshness gate with exit 125,
  because `$HOME/ntpers/linux-6.18` has no build manifest. The gate was not bypassed.

What the dry run does not cover:

- `vm/init.c` (the C init) is not compiled here: this host has no C toolchain. `init.c` serial mode
  has the same contract as `tools/linux-env/init-serial.sh`, which the dry run used. Build and test
  `init.c` with `vm/mkinitramfs.sh` on a Linux host.
- `vm/mkinitramfs.sh`, `vm/check_m0.sh` and `vm/runvm.sh` were not run end to end: they need cpio,
  busybox, gcc and the freshness manifest, none of which this host has. Their serial branches pass
  `bash -n` and `vm/lint.sh`.
- The emitter is exercised here through busybox-compatible applets (GNU coreutils on Git for Windows)
  in the unit tests, and through busybox in the guest run.
