# 0075: OS identity reports the accepted golden build 26200.9457 (client)

Status: Accepted by delegated authority; the owner may override. Implemented in the squashed commit that adds this record (see the publish report and `git log`).
Finding: none in `docs/review/`. Identity-policy decision under `tests/conformance/os-identity-policy.json`.
Date: 2026-10-14

## Context

The NT personality reported two different OS identities:

- `RtlGetVersion` in `src/ntdll/ntdll_main.c` reported 10.0, build 20348, `VER_PLATFORM_WIN32_NT`, product type 3 (server). That is the Windows Server 2022 persona. `docs/modules/ntdll-startup.md` called it deliberate.
- The PEB OS fields in `ensure_peb` in `src/kernel/nt/proc_thread.c` reported 10.0.26200.

A program that reads both sources sees two Windows versions. The owner delegated the choice of identity to us, with the rule that every value must match the accepted golden host build.

The accepted golden build is `26200.9457` (`tests/differential/golden/ACCEPTED_BUILDS.json`, record 0073). Its `version.json` and `manifest.json` report:

- `windows_version` 10.0.26200, `CurrentBuild` 26200, `UBR` 9457, `DisplayVersion` 25H2.
- `ProductName` "Windows 10 Pro", `EditionID` Professional, so a client edition.
- `probe_teb_peb.json` measures `OSMajorVersion` 10, `OSMinorVersion` 0, `OSBuildNumber` 26200 in the live PEB.

The golden corpus has no product-type probe, so `wProductType` cannot be read from it. On the live reference host, `Win32_OperatingSystem.ProductType` returns 1 (workstation). That is the only check outside the corpus, and it is recorded here so the value is not taken from memory.

## Options

1. Keep the 20348 server persona. It is internally inconsistent with the PEB and with every golden probe. Differential comparisons against 26200.9457 would report a false identity mismatch on every version-sensitive probe.
2. Report 26200.9457 as a client build everywhere. This matches the golden host on every measured field. The product type is the one field the corpus does not measure, so it is held to the live host value.
3. Report a different build, for example 26200.9168, which the pinned policy once named. It is not an accepted golden build and matches no captured corpus.

## Decision

Option 2. The identity is 10.0.26200, platform 2 (NT), product type 1 (workstation), for every identity-bearing source. The values are declared once in `os-identity-policy.json`. `tools/check_os_identity.py` enforces that the declared sites assign them and that no other source assigns an identity field a literal.

Changes in this record:

- `RtlGetVersion` reports build 26200 and product type 1 (was 20348 and 3).
- The PEB in `ensure_peb` already reported 10.0.26200 and needed no change.
- `src/kernel/fs/binfmt_pe_test.c` asserts build 26200 (was 20348).
- `docs/modules/ntdll-startup.md` describes the 26200 client identity.

## Rationale

The golden corpus is the accepted oracle for this project (record 0073). A personality that reports another build breaks every differential comparison, and it contradicts the PEB that the same process exposes. Choosing the golden host's own identity makes the reported version agree with the oracle on every field the corpus measures.

## Consequences

- A program that checks for a server edition (product type 3) now sees a client. This is intended. Any server-only behaviour must be reviewed against the client identity.
- The product type rests on the live-host check, not on the corpus. If a later golden capture measures product type, the policy basis must be updated to cite it.
- Resolved (2026-10-14): `binfmt_pe.c` `pe_init_peb` writes the OS version fields from `src/ntdll/os_identity.h`, so the PE loader's PEB reports the identity above. `binfmt_pe_test.c` is built and run by `tests/kernel-host` on Linux (`FS_TESTS`). The kernel build compiles `os_identity.h` through the `arch/ntdll/` copy that `tools/ntpers_kpatch.py` makes, and `vm/incremental-kernel.sh` stages every `src/ntdll/*.h`, so a change to the identity header reaches the guest tree. See [0028](0028-binfmt-pe-test-missing-helpers.md).
- No `KUSER_SHARED_DATA` or `GetVersionEx` implementation exists under `src/`. Those sources have nothing to change. The policy lists only the sites that exist.

## Test plan

- `python tools/check_os_identity.py` reports 0 violations.
- Unit suite (`python -m unittest discover tests/unit`) passes.
- Kernel-host (`tests/kernel-host`) and ntdll-host (`tests/ntdll-host`) suites run in the guest VM against the new build, and their results are recorded in the publish report.
