#!/usr/bin/env python3
"""Roadmap for the Definition-of-Done conformance criteria.

Reads tests/conformance/criteria.json and thresholds.json, the DoD register of
gated criteria (docs/phase0/DEFINITION-OF-DONE.md section 3.1), the test
registry and traceability classes (via tools/criteria_trace.py), and the
repository's status and decision documents. For each criterion it records:

  area, tier and dependency class   from the DoD
  needs                             what the criterion requires to pass
  work items                        repository work items it depends on (a DAG)
  blockers                          findings, decision records, owner decisions
  effort                            S, M, L, XL or unknown, with the reasoning
  order                             a dependency-respecting sequence in
                                    milestones M-A..M-F

Criteria that depend on a gated artifact or on an out-of-scope component are
never placed in a milestone. They are listed separately so the owner can decide
scope.

This is a plan, not a results report. The script runs no test. Every criterion
stays unknown until a result is recorded in the repository.

The work-item graph is curated at the granularity of an area plus a few keyword
refinements. Each work item names the repository facts it rests on, and
--selftest checks that every cited file, finding, decision record and owner
decision exists.

Standard library only.

Usage:
  criteria_roadmap.py                    plain-text summary on stdout
  criteria_roadmap.py --json             JSON document on stdout
  criteria_roadmap.py --markdown         Markdown document on stdout
  criteria_roadmap.py --markdown --out docs/status/CRITERIA-ROADMAP.md
  criteria_roadmap.py --selftest         built-in checks on synthetic data and repository references
"""
import argparse
import heapq
import importlib.util
import json
import re
import sys
from collections import OrderedDict, defaultdict
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
CRITERIA_PATH = REPO / "tests" / "conformance" / "criteria.json"
THRESHOLDS_PATH = REPO / "tests" / "conformance" / "thresholds.json"
DOD_PATH = REPO / "docs" / "phase0" / "DEFINITION-OF-DONE.md"
DECISIONS_INDEX = REPO / "docs" / "decisions" / "INDEX.md"
TRACE_PATH = REPO / "tools" / "criteria_trace.py"

REQUIRED_FIELDS = ("id", "domain", "tier", "dependency_class", "criterion", "status")
PLACEHOLDER_STATUSES = frozenset({"", "not_implemented"})
TIER_RANK = {"P0": 0, "P1": 1, "P2": 2, "P3": 3}
EFFORTS = ("S", "M", "L", "XL", "unknown")
EFFORT_POINTS = {"S": 1, "M": 2, "L": 3, "XL": 4}
POINTS_EFFORT = {1: "S", 2: "M", 3: "L", 4: "XL"}
STATUS_POINTS = {"implemented": 1, "partial": 2, "stub": 3, "missing": 4}
KERNEL_ENVS = frozenset({"kernel", "vm", "windows-differential+kernel"})
MILESTONES = ("M-A", "M-B", "M-C", "M-D", "M-E", "M-F")
NEVER = "never"
LABEL_ORDER = (
    "USERSPACE", "REPO-ONLY", "PE-LOADER", "WIN32-SHIM", "SCM", "NT-KERNEL", "GOLDEN-DIFF",
    "VM-HARNESS", "GUI-GFX-DX", "AUDIO", "NET-COM", "CLR", "HW-BOOT", "ANTI-CHEAT-DRM",
    "PERF-MEASURE", "LINUX-REF", "EVIDENCE", "EXT-GATED", "OUT-OF-SCOPE",
)
ID_RE = re.compile(r"DOD-[A-Z]+-[AB]\d{3}")
WILDCARD_RE = re.compile(r"DOD-([A-Z]+)-\*")
GATED_ID_RE = re.compile(r"`(DOD-[A-Z]+-[AB]\d{3})`")
DOC_ONLY_RE = re.compile(r"docs?-lint|capability matrix|generation \+ lint", re.I)
CANARY_RE = re.compile(r"canary", re.I)
OUT_OF_SCOPE_AREAS = frozenset({"TELEM", "GDK"})
NTSTATUS_RE = re.compile(r"NTSTATUS|RtlNtStatusToDosError|MS-ERREF|error mapping", re.I)

# ---------------------------------------------------------------------------
# Area facts: (status, basis, refs). Status is what the repository contains
# for the area, not whether it works. Refs are checked by --selftest.
# ---------------------------------------------------------------------------
AREA_FACTS = OrderedDict([
    ("ARCH", ("partial", "NT dispatch is partial and the kernel patch is not applied or built",
              ["DOC:docs/phase2/ARCHITECTURE-STATUS.md"])),
    ("API", ("partial", "kernel32 is a minimal Win32 surface; no API-surface denominator file",
             ["DOC:docs/phase2/ARCHITECTURE-STATUS.md"])),
    ("EXE", ("partial", "PE loader partial; no application corpus in the repository",
             ["DOC:docs/phase2/ARCHITECTURE-STATUS.md"])),
    ("PE", ("partial", "userspace parser and kernel binfmt_pe are partial; kernel build refuses SEC_IMAGE",
            ["DOC:docs/phase2/ARCHITECTURE-STATUS.md", "OD-15"])),
    ("PROC", ("partial", "process-creation design is M0 to M7; NtCreateUserProcess stubs refuse",
              ["ADR-0037", "OD-21"])),
    ("MEM", ("partial", "ntmm is partial; one global VAD table; the two mm.c copies differ",
             ["ADR-0020", "OD-08"])),
    ("FS", ("partial", "ntio is partial; no filesystem shim tests",
            ["DOC:docs/phase2/ARCHITECTURE-STATUS.md", "ADR-0022"])),
    ("REG", ("partial", "kernel registry.c exists; ntdll Registry* functions report not found",
             ["OD-40"])),
    ("DLL", ("stub", "the real loader is excluded from the MinGW build; DllMain and TLS callbacks never run",
             ["LDR-1", "ADR-0029"])),
    ("NET", ("missing", "no ntafd (AFD) or Winsock provider in the repository",
             ["DOC:docs/phase2/ARCHITECTURE-STATUS.md"])),
    ("SEC", ("partial", "security.c has SIDs and ACLs; object, handle, registry, io and mm paths do not call the access check",
             ["DOC:docs/phase2/ARCHITECTURE-STATUS.md", "OD-01"])),
    ("GFX", ("missing", "no gdi32; user32.c has DllMain and the wsprintf family only",
             ["DOC:docs/phase2/ARCHITECTURE-STATUS.md"])),
    ("DX", ("missing", "no d3d, dxgi, user-mode driver or /dev/dxg code",
            ["DOC:docs/phase2/ARCHITECTURE-STATUS.md"])),
    ("DRV", ("missing", "no device stack; the thresholds file says it is out of scope for this wave",
             ["DOC:tests/conformance/thresholds.json"])),
    ("AUD", ("missing", "no audio sources (no WASAPI, MME, DirectSound or MIDI code)", [])),
    ("INP", ("missing", "no input sources (no raw input, hooks or XInput code)", [])),
    ("SVC", ("missing", "no services.exe or SCM code",
             ["DOC:docs/phase2/ARCHITECTURE-STATUS.md"])),
    ("COM", ("missing", "no COM, OLE or oleaut32 code", [])),
    ("UWP", ("missing", "no WinRT or metadata code", [])),
    ("DOTNET", ("missing", "no CLR or mscoree code", [])),
    ("SYNC", ("partial", "ntsync is partial; WaitOnAddress appears only in test sources; timer APCs refused",
              ["DOC:docs/phase2/ARCHITECTURE-STATUS.md"])),
    ("IPC", ("partial", "LPC port.c exists; no named-pipe or ncalrpc code", ["ADR-0072"])),
    ("NT", ("partial", "dispatch is partial; services listed as MISSING in the inventory",
            ["DOC:docs/status/SERVICES.md"])),
    ("PERF", ("unknown", "no baseline timings (B1, B2) or soak data in the repository",
              ["DOC:docs/harness/OVERVIEW.md"])),
    ("PORT", ("missing", "no hardware matrix, aarch64 refusal path or CPUID-mask harness", [])),
    ("BOOTVM", ("partial", "the vm/ boot smoke is single-mode; the registry has three real harness entries",
                ["DOC:tests/harness/registry.yaml"])),
    ("BOOTPH", ("missing", "no installer, Secure Boot chain or MOK tooling in the repository", [])),
    ("LINUXR", ("partial", "the Linux patch exists but is not applied or built; no witness image",
                ["DOC:docs/phase2/ARCHITECTURE-STATUS.md"])),
    ("EVI", ("partial", "dodrun writes unsigned JSON and text summaries; no signed bundles",
             ["DOC:docs/harness/OVERVIEW.md"])),
    ("AC", ("missing", "os-identity-policy.json is absent; no anti-cheat support",
            ["DOC:docs/phase2/ARCHITECTURE-STATUS.md"])),
    ("DRM", ("missing", "no DRM or activation code", [])),
    ("TELEM", ("out-of-scope", "criterion text declares it out of scope (DoD Appendix A)",
               ["DOC:docs/phase0/DEFINITION-OF-DONE.md"])),
    ("GDK", ("out-of-scope", "criterion text declares it out of scope (DoD Appendix A)",
             ["DOC:docs/phase0/DEFINITION-OF-DONE.md"])),
])


# ---------------------------------------------------------------------------
# Work items. Effort is curated from the basis text. A sentinel marks a
# criterion that cannot pass without an out-of-scope or gated component.
# ---------------------------------------------------------------------------
def _wi(wid, title, effort, prereqs, refs, basis, sentinel=False):
    return OrderedDict([
        ("id", wid), ("title", title), ("effort", effort),
        ("prereqs", tuple(prereqs)), ("refs", tuple(refs)), ("basis", basis),
        ("sentinel", sentinel),
    ])


_WORK_ITEM_LIST = [
    # Repository-local work and owner decisions (level 0 unless a prerequisite is listed).
    _wi("WI-HARNESS", "Make dodrun run on this Windows host", "S", (),
        ["DOC:docs/harness/OVERVIEW.md"],
        "the lint gate fails on the absent golden corpus; selftest does not run; PyYAML is not declared; WSL is absent"),
    _wi("WI-GOLDEN", "Windows golden-output corpus under tests/differential/golden", "L", (),
        ["DOC:tests/differential/README.md", "DOC:docs/harness/OVERVIEW.md"],
        "the golden directory is absent and check_golden_integrity fails; the runner and probes exist"),
    _wi("WI-POLICY-DOCS", "Repository-local docs-lint and capability-matrix checks", "S", (),
        ["DOC:docs/status/TRACEABILITY.md"],
        "criteria_report and criteria_trace already generate repository docs; no policy lint exists"),
    _wi("WI-OSID-POLICY", "os-identity-policy.json declaring every version-reporting value", "S", (),
        ["DOC:docs/phase2/ARCHITECTURE-STATUS.md"],
        "ARCHITECTURE-STATUS section 4 lists the file as missing"),
    _wi("WI-API-SURFACE", "api-surface.json denominator and core500/core750 lists", "L", (),
        ["DOC:docs/phase2/ARCHITECTURE-STATUS.md"],
        "no api-surface file in the tree; the SDK metadata it is generated from is not in the repository"),
    _wi("WI-NTSTATUS-FULL", "Complete NTSTATUS to Win32 error table", "M", (),
        ["DOC:tests/harness/registry.yaml"],
        "the registry notes say the existing suite covers 83 pairs; ntstatus_to_dos.c exists"),
    _wi("WI-OD-P1", "Owner decisions OD-01 to OD-11 (security and data integrity)", "S", (),
        ["OD-01", "OD-02", "OD-03", "OD-04", "OD-05", "OD-06", "OD-07", "OD-08", "OD-09", "OD-10", "OD-11"],
        "the P1 rows of OWNER-DECISIONS: open security and data-integrity questions"),
    _wi("WI-OD-LDR", "Owner decisions OD-17 to OD-20 (loader build, order and failure)", "S", (),
        ["OD-17", "OD-18", "OD-19", "OD-20"],
        "MinGW build selection, DllMain order, second exception pass, missing-import behaviour"),
    _wi("WI-OD-PROC", "Owner decision OD-21 (process-creation design)", "S", (),
        ["OD-21", "ADR-0037"],
        "record 0037 is open and names option C"),
    _wi("WI-OD-REG", "Owner decision OD-40 (registry back-end design)", "S", (),
        ["OD-40"],
        "OWNER-DECISIONS lists the registry back end as not recorded"),
    _wi("WI-OD-MM", "Owner decisions OD-03 and OD-08 (W^X sections, VAD table)", "S", (),
        ["OD-03", "OD-08", "ADR-0011", "ADR-0020"],
        "W^X section policy and a global versus per-process VAD table"),
    _wi("WI-OD-FS", "Owner decisions OD-04 to OD-06 (file handles, rights, supersede order)", "S", (),
        ["OD-04", "OD-05", "OD-06", "ADR-0007", "ADR-0023", "ADR-0024"],
        "file-handle ownership, rights per information class, FILE_SUPERSEDE order"),
    _wi("WI-OD-SYNC", "Owner decisions OD-32 to OD-34 (exclusive open, timeout, ACL table)", "S", (),
        ["OD-32", "OD-33", "OD-34", "ADR-0033", "ADR-0034", "ADR-0035"],
        "synchronization and capacity decisions that are blocked"),
    _wi("WI-OD-P2", "Owner decisions OD-22 and OD-26 to OD-28 (contracts and C runtime)", "S", (),
        ["OD-22", "OD-26", "OD-27", "OD-28"],
        "contract, C-runtime and Windows-reference choices recorded as needing an owner"),
    _wi("WI-LDR-CTX", "Process-wide loader context created at startup", "M", (),
        ["LDR-1", "K32-4", "ADR-0029"],
        "record 0029 says the MinGW fix needs the process-wide context (K32-4) first"),
    _wi("WI-KBOOT", "Apply the Linux kernel patch, build bzImage and initramfs, boot under QEMU", "L", (),
        ["DOC:docs/phase2/ARCHITECTURE-STATUS.md", "DOC:docs/harness/OVERVIEW.md"],
        "patch 0001 is not applied or built here; the vm/ scripts need Linux or WSL and a kernel tree"),
    _wi("WI-PROVENANCE", "Provenance scan of the release image and SBOM", "M", (),
        ["DOC:tools/provenance_scan.py", "DOC:docs/phase2/ARCHITECTURE-STATUS.md"],
        "the scanner runs in CI in --check mode; the release image is not in the repository"),
    _wi("WI-OUT-OF-SCOPE", "Component the DoD declares out of scope", "unknown", (),
        ["DOC:docs/phase0/DEFINITION-OF-DONE.md"],
        "sentinel: the criterion is declared out of scope in the DoD", sentinel=True),
    _wi("WI-EXT-GATED", "Third-party artifact named in the DoD gated register", "unknown", (),
        ["DOC:docs/phase0/DEFINITION-OF-DONE.md"],
        "sentinel: the DoD section 3.1 register; passes only if the artifact is obtained", sentinel=True),
    # Level 1: the in-VM harness and the first loader and kernel objects.
    _wi("WI-KHARNESS", "In-VM kernel harness run through dodrun", "M", ("WI-HARNESS", "WI-KBOOT"),
        ["DOC:tests/harness/registry.yaml"],
        "only three registry entries run real commands, and they need the VM build"),
    _wi("WI-LDR-1", "Real LdrLoadDll, LdrGetDllHandle and LdrGetProcedureAddress in the MinGW build",
        "M", ("WI-LDR-CTX", "WI-OD-LDR"), ["LDR-1", "ADR-0029", "OD-17"],
        "the implementation exists but is excluded from the MinGW build; the Linux test build compiles it"),
    _wi("WI-MISSING-IMPORT", "Terminate or continue on a missing import, as decided", "S", ("WI-OD-LDR",),
        ["OD-20", "MAIN-3", "MAIN-6"],
        "the load continues with an unpatched IAT today"),
    _wi("WI-CRT", "C runtime behaviour: printf floats, errno ABI, signal, _exit", "M", ("WI-OD-P2",),
        ["OD-26", "OD-39", "CRT-1"],
        "several C-runtime areas behave as stubs and need a product choice"),
    _wi("WI-OBJ-ACCESS", "Access checks in NtOpenProcess, NtOpenThread and object opens", "M", ("WI-OD-P1",),
        ["OD-01", "OD-02", "ADR-0002", "ADR-0021", "ADR-0025"],
        "any process can open any other by ID today"),
    _wi("WI-EXC-SECOND-PASS", "Exception second pass and RtlUnwindEx transfer", "L",
        ("WI-LDR-1", "WI-OD-LDR"), ["OD-19", "UNW-2", "EXC-3"],
        "termination handlers never run; RtlUnwindEx runs no unwind handlers"),
    _wi("WI-LDR-3", "DllMain and TLS callbacks run in the documented order", "M",
        ("WI-LDR-1", "WI-OD-LDR"), ["LDR-3", "ADR-0031", "OD-18"],
        "record 0031 is open and says the calls need the VM"),
    _wi("WI-MM-VAD", "Per-process VAD, commit rollback and section policy", "L",
        ("WI-OD-MM", "WI-KBOOT"), ["OD-03", "OD-08", "OD-29", "OD-30"],
        "one VAD table for all processes; a failed commit leaves VAD and mapping apart"),
    _wi("WI-NT-DISPATCH-FULL", "Wire the missing NT services and unlinked wrappers", "L", ("WI-KBOOT",),
        ["DOC:docs/status/SERVICES.md", "DOC:docs/modules/dispatch.md"],
        "the services inventory lists MISSING and unlinked entries; nt_can_sysret is a stub"),
    # Level 2 and up.
    _wi("WI-WIN32-CORE", "Core kernel32 and ntdll Win32 surface", "L",
        ("WI-LDR-1", "WI-NTSTATUS-FULL"), ["DOC:docs/phase2/ARCHITECTURE-STATUS.md"],
        "kernel32.c is a minimal surface of about one hundred definitions; no RtlAllocateHeap"),
    _wi("WI-HANDLE-OWNER", "Per-process file and object handle tables", "L",
        ("WI-OBJ-ACCESS", "WI-OD-FS"), ["ADR-0007", "ADR-0022", "OD-04"],
        "the file-handle table is global and shared across processes"),
    _wi("WI-IMAGE-KERNEL", "Kernel-side PE image mapping (SEC_IMAGE in the kernel build)", "L",
        ("WI-MM-VAD", "WI-KHARNESS"), ["OD-15", "DOC:docs/architecture/loading.md"],
        "NtCreateSection(SEC_IMAGE) returns STATUS_NOT_IMPLEMENTED in the kernel build"),
    _wi("WI-SYNC-KERNEL", "Kernel wait paths exercised; keyed events; timer APCs", "M",
        ("WI-KHARNESS", "WI-OD-SYNC"), ["DOC:docs/modules/sync.md"],
        "the kernel wait-queue path is not exercised by sync_test.c; timer APCs are refused"),
    _wi("WI-DRIVER-STACK", "Personality virtual device stack: bus and device interfaces", "XL",
        ("WI-KHARNESS",), ["DOC:tests/conformance/thresholds.json"],
        "no driver or bus code; the thresholds file says out of scope for this wave"),
    _wi("WI-REG-USER", "Registry back end and the Registry* API", "L",
        ("WI-OD-REG", "WI-WIN32-CORE"), ["OD-40", "ADV-1"],
        "Registry* functions report not found"),
    # Level 3.
    _wi("WI-WIN32-FILE", "Win32 file, temp-file, mapping and critical-section primitives", "L",
        ("WI-WIN32-CORE",), ["OD-38", "K32-8", "K32-12", "K32-14"],
        "file and mapping calls fail or report false success today"),
    _wi("WI-SEC-ENFORCE", "Call se_access_check from object, handle, registry, io and mm paths", "L",
        ("WI-HANDLE-OWNER", "WI-MM-VAD"), ["DOC:docs/phase2/ARCHITECTURE-STATUS.md", "OD-09"],
        "the access check is called only by the token and descriptor services"),
    _wi("WI-IO-FS", "Filesystem personality: share modes, locks, rename rules, shim tests", "L",
        ("WI-HANDLE-OWNER", "WI-KHARNESS", "WI-OD-FS"), ["OD-05", "OD-06", "OD-41", "ADR-0023", "ADR-0024"],
        "no filesystem shim tests; access rights per information class are blocked"),
    _wi("WI-PROC-CREATE", "NtCreateUserProcess and CreateProcess in the kernel build", "XL",
        ("WI-IMAGE-KERNEL", "WI-OD-PROC"), ["ADR-0037", "OD-21", "DOC:docs/architecture/process-creation.md"],
        "design milestones M0 to M7 are documented; the stubs refuse"),
    _wi("WI-IPC-PORTS", "Named pipes and ncalrpc on port.c", "L",
        ("WI-HANDLE-OWNER", "WI-SYNC-KERNEL"), ["ADR-0072"],
        "port.c exists; no named-pipe or ncalrpc code"),
    _wi("WI-GDI-USER", "user32 and gdi32: windows, messages, GDI raster", "XL",
        ("WI-WIN32-CORE", "WI-SYNC-KERNEL"), ["DOC:docs/phase2/ARCHITECTURE-STATUS.md"],
        "user32.c is 137 lines of DllMain and wsprintf; there is no gdi32 source"),
    # Level 4.
    _wi("WI-WOW64", "WOW64: 32-bit PE on a 64-bit host, thunks and redirection", "XL", ("WI-PROC-CREATE",),
        ["DOC:src/kernel/nt/registry.c"],
        "registry view names exist; there is no 32-bit execution or thunk code"),
    _wi("WI-NET-AFD", "AFD and Winsock provider", "XL", ("WI-IO-FS", "WI-HANDLE-OWNER"),
        ["DOC:docs/phase2/ARCHITECTURE-STATUS.md"], "no ntafd or socket sources"),
    _wi("WI-AUDIO", "WASAPI, MME and DirectSound on virtual endpoints", "XL",
        ("WI-IO-FS", "WI-SYNC-KERNEL"), [], "no audio sources"),
    _wi("WI-INPUT", "Keyboard, mouse, raw input and gamepads", "XL", ("WI-GDI-USER",), [],
        "no input sources"),
    _wi("WI-DXGCORE", "D3DKMT, /dev/dxg and the D3D and DXGI user-mode drivers", "XL",
        ("WI-KBOOT", "WI-GDI-USER"), ["DOC:docs/phase2/ARCHITECTURE-STATUS.md"],
        "no dxgcore, D3D, DXGI or user-mode driver sources"),
    _wi("WI-SVC-SCM", "services.exe SCM and service hosting", "XL",
        ("WI-PROC-CREATE", "WI-REG-USER", "WI-IPC-PORTS"), ["DOC:docs/phase2/ARCHITECTURE-STATUS.md"],
        "no services.exe"),
    _wi("WI-COM", "COM and OLE: class store, marshaling, oleaut32, structured storage", "XL",
        ("WI-IPC-PORTS", "WI-REG-USER", "WI-WIN32-CORE"), [], "no COM sources"),
    _wi("WI-DOTNET", "CLR hosting through mscoree and runtime support", "XL",
        ("WI-PROC-CREATE", "WI-WIN32-CORE"), [], "no CLR or mscoree sources"),
    _wi("WI-BOOT-INSTALL", "Installer, Secure Boot with project keys, measured-boot verification", "XL",
        ("WI-KBOOT", "WI-PROC-CREATE"), ["OD-15", "DOC:docs/phase0/DEFINITION-OF-DONE.md"],
        "no installer, MOK or PCR tooling in the repository"),
    # Level 1 and up, independent of the kernel chain above.
    _wi("WI-LINUX-REF", "Reference Ubuntu witness image and LTP result-set comparison", "L", ("WI-KBOOT",),
        ["DOC:docs/phase2/ARCHITECTURE-STATUS.md"],
        "the stock-Ubuntu witness image is not in the repository"),
    _wi("WI-REPRO-BUILD", "Byte-equal reproducible build of the image and modules", "L", ("WI-KBOOT",),
        ["DOC:docs/architecture/build-and-test.md"],
        "no reproducibility harness; the build tree is not a release image"),
    _wi("WI-PORT-ARM", "Non-x86 refusal path and CPUID-masked boot arm", "M", ("WI-KBOOT",),
        ["DOC:src/loader/pe_format.h"],
        "only a machine-type constant exists; there is no aarch64 refusal path or CPUID mask"),
    _wi("WI-EVIDENCE", "Signed, time-stamped evidence bundles and independent replay", "L", ("WI-HARNESS",),
        ["DOC:docs/harness/OVERVIEW.md"],
        "dodrun writes unsigned results; the bundle check is a lint over a directory layout"),
    _wi("WI-PERF-BASE", "Windows baseline timings (B1, B2) and the measurement harness", "L",
        ("WI-GOLDEN", "WI-HARNESS"), ["DOC:docs/harness/OVERVIEW.md"],
        "no baseline or timing data; needs the reference Windows host"),
    _wi("WI-HW-MATRIX", "Physical hardware matrix and rack automation", "XL",
        ("WI-KBOOT", "WI-BOOT-INSTALL", "WI-DRIVER-STACK"), [],
        "no rack automation or hardware data in the repository; needs physical machines"),
    _wi("WI-WINRT", "WinRT: ECMA-335 metadata and RoInitialize activation", "XL", ("WI-COM",), [],
        "no metadata or WinRT sources"),
]

WORK_ITEMS = OrderedDict((w["id"], w) for w in _WORK_ITEM_LIST)

# Area work items: each criterion in an area gets these.
AREA_WI = {
    "ARCH": ("WI-PROC-CREATE",),
    "API": ("WI-API-SURFACE", "WI-WIN32-CORE"),
    "EXE": ("WI-PROC-CREATE", "WI-WIN32-CORE", "WI-LDR-3"),
    "PE": ("WI-LDR-3", "WI-IMAGE-KERNEL"),
    "PROC": ("WI-PROC-CREATE", "WI-SYNC-KERNEL"),
    "MEM": ("WI-MM-VAD", "WI-WIN32-CORE"),
    "FS": ("WI-IO-FS", "WI-WIN32-FILE"),
    "REG": ("WI-REG-USER",),
    "DLL": ("WI-LDR-3", "WI-MISSING-IMPORT"),
    "NET": ("WI-NET-AFD",),
    "SEC": ("WI-OBJ-ACCESS", "WI-SEC-ENFORCE"),
    "GFX": ("WI-GDI-USER",),
    "DX": ("WI-DXGCORE", "WI-GDI-USER"),
    "DRV": ("WI-DRIVER-STACK",),
    "AUD": ("WI-AUDIO",),
    "INP": ("WI-INPUT",),
    "SVC": ("WI-SVC-SCM",),
    "COM": ("WI-COM",),
    "UWP": ("WI-WINRT",),
    "DOTNET": ("WI-DOTNET",),
    "SYNC": ("WI-SYNC-KERNEL",),
    "IPC": ("WI-IPC-PORTS",),
    "NT": ("WI-WIN32-CORE",),
    "PERF": ("WI-PROC-CREATE", "WI-PERF-BASE"),
    "PORT": ("WI-HW-MATRIX", "WI-PORT-ARM"),
    "BOOTVM": ("WI-KBOOT", "WI-KHARNESS"),
    "BOOTPH": ("WI-BOOT-INSTALL", "WI-HW-MATRIX"),
    "LINUXR": ("WI-LINUX-REF",),
    "EVI": ("WI-EVIDENCE",),
    "AC": ("WI-OSID-POLICY", "WI-WIN32-CORE"),
    "DRM": ("WI-PROC-CREATE", "WI-IO-FS"),
    "TELEM": ("WI-OUT-OF-SCOPE",),
    "GDK": ("WI-OUT-OF-SCOPE",),
}

# Environment work items (thresholds.json required_environment).
ENV_WI = {
    "userspace": (),
    "kernel": ("WI-KBOOT", "WI-KHARNESS"),
    "vm": ("WI-KBOOT", "WI-KHARNESS"),
    "windows-differential": ("WI-GOLDEN",),
    "windows-differential+kernel": ("WI-GOLDEN", "WI-KBOOT", "WI-KHARNESS"),
    "EXT-GATED": ("WI-EXT-GATED",),
}

# Keyword refinements: (areas or None for all, regex over criterion and verification, work item).
KEYWORD_WI = [
    (("NT",), re.compile(r"syscall|SSN", re.I), "WI-NT-DISPATCH-FULL"),
    (("NT",), re.compile(r"object manager|NtQueryObject|handle attributes|OBJ_", re.I), "WI-OBJ-ACCESS"),
    (("NT",), re.compile(r"SEC_IMAGE|image|section|PEB|TEB", re.I), "WI-IMAGE-KERNEL"),
    (("NT", "PE", "PROC"), re.compile(r"SEH|vectored|unwind|RtlUnwind", re.I), "WI-EXC-SECOND-PASS"),
    (None, re.compile(r"WOW64|SysWOW64|LARGEADDRESSAWARE|x86 PEs?|32-bit", re.I), "WI-WOW64"),
    (("BOOTPH", "BOOTVM"), re.compile(r"Secure Boot|MOK|PCR|install", re.I), "WI-BOOT-INSTALL"),
    (("ARCH",), re.compile(r"provenance|lineage|SBOM|fingerprint", re.I), "WI-PROVENANCE"),
    (("ARCH", "BOOTVM", "PORT", "LINUXR"), re.compile(r"reproducib|repro-build", re.I), "WI-REPRO-BUILD"),
    (("ARCH", "LINUXR"), re.compile(r"\bLTP\b|Ubuntu", re.I), "WI-LINUX-REF"),
    (("ARCH", "PERF"), re.compile(r"PMU|perf stat|soak|72 ?h|baseline|median", re.I), "WI-PERF-BASE"),
    (("PORT",), re.compile(r"aarch64|ISA floor|x86-64-v2|CPUID", re.I), "WI-PORT-ARM"),
    (("DRV",), re.compile(r"GPU|DX|D3D|Vulkan|mesa", re.I), "WI-DXGCORE"),
    (("EVI",), re.compile(r"signed|RFC3161|replay|bundle", re.I), "WI-EVIDENCE"),
]

LABEL_BY_AREA = {
    "PE": ("PE-LOADER",), "DLL": ("PE-LOADER",),
    "EXE": ("WIN32-SHIM",), "API": ("WIN32-SHIM",), "PROC": ("WIN32-SHIM",),
    "MEM": ("WIN32-SHIM",), "FS": ("WIN32-SHIM",), "REG": ("WIN32-SHIM",),
    "SYNC": ("WIN32-SHIM",), "IPC": ("WIN32-SHIM",), "SEC": ("WIN32-SHIM",),
    "NT": ("WIN32-SHIM",), "SVC": ("WIN32-SHIM", "SCM"),
    "NET": ("NET-COM",), "COM": ("NET-COM",), "UWP": ("NET-COM",),
    "DOTNET": ("CLR",), "GFX": ("GUI-GFX-DX",), "DX": ("GUI-GFX-DX",), "INP": ("GUI-GFX-DX",),
    "AUD": ("AUDIO",), "DRV": ("HW-BOOT",), "BOOTPH": ("HW-BOOT",), "PORT": ("HW-BOOT",),
    "AC": ("ANTI-CHEAT-DRM",), "DRM": ("ANTI-CHEAT-DRM",),
    "PERF": ("PERF-MEASURE",), "BOOTVM": ("VM-HARNESS",), "LINUXR": ("LINUX-REF",),
    "EVI": ("EVIDENCE",), "TELEM": ("OUT-OF-SCOPE",), "GDK": ("OUT-OF-SCOPE",),
}
LABEL_BY_ENV = {
    "userspace": (),
    "kernel": ("NT-KERNEL",),
    "vm": ("VM-HARNESS",),
    "windows-differential": ("GOLDEN-DIFF",),
    "windows-differential+kernel": ("GOLDEN-DIFF", "NT-KERNEL"),
    "EXT-GATED": ("EXT-GATED",),
}


# ---------------------------------------------------------------------------
# Loading
# ---------------------------------------------------------------------------
def _read(path):
    try:
        return Path(path).read_text(encoding="utf-8")
    except OSError:
        return ""


def load_criteria(path=CRITERIA_PATH):
    """Load and minimally validate the criteria list. Raises ValueError."""
    data = json.loads(Path(path).read_text(encoding="utf-8"))
    if not isinstance(data, list):
        raise ValueError("criteria file must hold a JSON list")
    seen = set()
    for i, entry in enumerate(data):
        missing = [k for k in REQUIRED_FIELDS if k not in entry]
        if missing:
            raise ValueError(f"entry {i} ({entry.get('id', '?')}) lacks {missing}")
        if entry["id"] in seen:
            raise ValueError(f"duplicate id {entry['id']}")
        seen.add(entry["id"])
    return data


def load_thresholds(path=THRESHOLDS_PATH):
    data = json.loads(Path(path).read_text(encoding="utf-8"))
    if not isinstance(data, dict):
        raise ValueError("thresholds file must hold a JSON object")
    return data


def parse_gated_register(text):
    """{id: controlling party} from the EXT-GATED register table (DoD section 3.1)."""
    start = text.find("### §3.1")
    if start == -1:
        return OrderedDict()
    end = text.find("\n## ", start)
    section = text[start:end if end != -1 else len(text)]
    out = OrderedDict()
    for line in section.splitlines():
        if not line.startswith("|") or set(line) <= set("|-: "):
            continue
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        if len(cells) < 3:
            continue
        for cid in GATED_ID_RE.findall(cells[0]):
            out[cid] = cells[2]
    return out


def parse_decision_status(text):
    """{'0029': 'open', ...} from the decision-record index table."""
    out = {}
    for line in text.splitlines():
        m = re.match(r"^\|\s*\[(\d{4})\]\([^)]*\)\s*\|[^|]*\|\s*([^|]*?)\s*\|", line)
        if m:
            words = m.group(2).split()
            out[m.group(1)] = words[0].lower().rstrip(";,") if words else "unknown"
    return out


def load_trace_module():
    spec = importlib.util.spec_from_file_location("criteria_trace", TRACE_PATH)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def trace_classes(repo=REPO):
    """{id: traceability class} from tools/criteria_trace.py. Read-only."""
    trace = load_trace_module()
    criteria, registry_text, sources = trace.collect(repo)
    rows, _ = trace.build_matrix(criteria, registry_text, sources)
    return {row["id"]: row["classification"] for row in rows}


# ---------------------------------------------------------------------------
# Classification
# ---------------------------------------------------------------------------
def status_of(entry):
    raw = (entry.get("status") or "").strip()
    return "unknown" if raw in PLACEHOLDER_STATUSES else raw


def is_doc_only(entry):
    verification = str(entry.get("verification", ""))
    return bool(DOC_ONLY_RE.search(verification)) and not CANARY_RE.search(verification)


def area_status(area):
    fact = AREA_FACTS.get(area)
    return fact[0] if fact else "unknown"


def labels_for(entry, env):
    """Capability labels a criterion needs, in LABEL_ORDER."""
    if is_doc_only(entry):
        return ["REPO-ONLY"]
    found = set(LABEL_BY_ENV.get(env, ()))
    found.update(LABEL_BY_AREA.get(entry["domain"], ()))
    if env == "userspace" and not found:
        found.add("USERSPACE")
    if entry["dependency_class"] == "EXT-GATED":
        found.add("EXT-GATED")
    return [label for label in LABEL_ORDER if label in found]


def work_items_for(entry, env):
    """Direct work items a criterion depends on, in a stable order."""
    area = entry["domain"]
    if entry["dependency_class"] == "EXT-GATED":
        return ["WI-EXT-GATED"] + [w for w in AREA_WI.get(area, ()) if w != "WI-EXT-GATED"]
    if area in OUT_OF_SCOPE_AREAS:
        return ["WI-OUT-OF-SCOPE"]
    if is_doc_only(entry):
        return ["WI-POLICY-DOCS"]
    wis = ["WI-HARNESS"]
    text = f"{entry.get('criterion', '')} {entry.get('verification', '')}"
    if area == "NT" and NTSTATUS_RE.search(text):
        wis.append("WI-NTSTATUS-FULL")
    else:
        wis.extend(AREA_WI.get(area, ()))
    wis.extend(ENV_WI.get(env, ()))
    for areas, pattern, wid in KEYWORD_WI:
        if areas is not None and area not in areas:
            continue
        if pattern.search(text):
            wis.append(wid)
    out = []
    for w in wis:
        if w not in out:
            out.append(w)
    return out


def closure_of(wids):
    """Every work item needed by wids, including wids themselves."""
    out = set()
    stack = list(wids)
    while stack:
        w = stack.pop()
        if w in out:
            continue
        out.add(w)
        stack.extend(WORK_ITEMS[w]["prereqs"])
    return out


def work_item_level(wid, memo=None):
    """0 when a work item has no prerequisite; else one more than the deepest prerequisite.

    Sentinels and anything that needs a sentinel have no level (None)."""
    if memo is None:
        memo = {}
    if wid in memo:
        return memo[wid]
    item = WORK_ITEMS[wid]
    if item["sentinel"]:
        memo[wid] = None
        return None
    levels = [work_item_level(p, memo) for p in item["prereqs"]]
    memo[wid] = None if any(lv is None for lv in levels) else (0 if not levels else 1 + max(levels))
    return memo[wid]


def milestone_of_level(level):
    if level is None:
        return NEVER
    return MILESTONES[min(level, len(MILESTONES) - 1)]


def criterion_effort(entry, env, volume, gated, out_of_scope, doc_only):
    """(effort bucket, reasoning). Facts from the repository only; unknown when the repository cannot say."""
    area = entry["domain"]
    if gated:
        return "unknown", ("EXT-GATED: needs a third-party artifact; the effort depends on it and "
                           "is not estimable from the repository")
    if out_of_scope:
        return "unknown", "declared out of scope in the DoD; no effort applies"
    if doc_only:
        return "S", ("repository-local documentation or policy check (verification names a docs-lint); "
                     "no runtime or Windows reference needed")
    status = area_status(area)
    if status == "unknown":
        return "unknown", f"area {area} has no status in the repository"
    points = STATUS_POINTS[status]
    parts = [f"area {area} {status}"]
    if env in KERNEL_ENVS:
        points += 1
        parts.append(f"env {env} needs a booted personality kernel (+1)")
    if isinstance(volume, int) and volume >= 1_000_000 and env in KERNEL_ENVS:
        points += 1
        parts.append(f"volume {volume} needs long kernel runs (+1)")
    points = min(points, 4)
    bucket = POINTS_EFFORT[points]
    return bucket, "; ".join(parts) + f" => {bucket}"


_DECISIONS = {}


def _decisions():
    if not _DECISIONS:
        _DECISIONS.update(parse_decision_status(_read(DECISIONS_INDEX)))
    return _DECISIONS


def ref_text(ref):
    if ref.startswith("ADR-"):
        number = ref[4:]
        return f"ADR-{number} ({_decisions().get(number, 'unknown')})"
    if ref.startswith("DOC:"):
        return ref[4:]
    return ref


def ref_list(wids):
    """Blocker references of the given work items, de-duplicated and in order."""
    out = []
    for wid in wids:
        for ref in WORK_ITEMS[wid]["refs"]:
            text = ref_text(ref)
            if text not in out:
                out.append(text)
    return out


def build_model(criteria, thresholds, trace_map, gated_register):
    """Pure model from the given inputs. Returns rows (ordered), never, and work-item rows."""
    by_id = {c["id"]: c for c in criteria}
    wi_levels = {w: work_item_level(w) for w in WORK_ITEMS}

    info = {}
    for c in criteria:
        cid = c["id"]
        env = thresholds.get(cid, {}).get("required_environment", "unknown")
        volume = thresholds.get(cid, {}).get("required_volume")
        wis = work_items_for(c, env)
        gated = c["dependency_class"] == "EXT-GATED"
        out_of_scope = c["domain"] in OUT_OF_SCOPE_AREAS
        doc_only = is_doc_only(c)
        closure = closure_of(wis)
        closure_points = sum(EFFORT_POINTS[WORK_ITEMS[w]["effort"]] for w in closure
                             if not WORK_ITEMS[w]["sentinel"])
        own_effort, reason = criterion_effort(c, env, volume, gated, out_of_scope, doc_only)
        never = any(WORK_ITEMS[w]["sentinel"] for w in wis)
        levels = [wi_levels[w] for w in wis if wi_levels[w] is not None]
        level = None if never else (max(levels) if levels else 0)
        gates = sorted((w for w in closure if wi_levels[w] is not None),
                       key=lambda w: (wi_levels[w], w))
        deps = set()
        for match in ID_RE.findall(c.get("criterion", "")):
            if match != cid and match in by_id:
                deps.add(match)
        for area in WILDCARD_RE.findall(c.get("criterion", "")):
            for other in criteria:
                if other["domain"] == area and other["id"] != cid:
                    deps.add(other["id"])
        info[cid] = {
            "entry": c, "env": env, "wis": wis, "closure": closure,
            "closure_points": closure_points, "own_effort": own_effort,
            "effort_reason": reason, "never": never, "level": level,
            "effective_level": None, "last_gate": gates[-1] if gates else None,
            "deps": deps, "labels": labels_for(c, env),
            "gated": gated, "out_of_scope": out_of_scope,
        }

    # Never propagates to anything that depends on a never criterion.
    changed = True
    while changed:
        changed = False
        for meta in info.values():
            if not meta["never"] and any(info[d]["never"] for d in meta["deps"]):
                meta["never"] = True
                meta["level"] = None
                changed = True

    ordered = _topo_order(info)
    rows = []
    for index, cid in enumerate(ordered, start=1):
        meta = info[cid]
        rows.append(_row(index, meta, milestone_of_level(meta["effective_level"]), trace_map, gated_register))
    never_rows = [_row(None, meta, NEVER, trace_map, gated_register)
                  for meta in info.values() if meta["never"]]
    never_rows.sort(key=lambda r: (TIER_RANK.get(r["tier"], 9), r["id"]))
    return {"rows": rows, "never": never_rows, "work_items": _wi_rows(wi_levels, info)}


def _topo_order(info):
    """Dependency-respecting order. Ties: milestone, then summed work-item effort, then tier, then ID."""
    nodes = [cid for cid, meta in info.items() if not meta["never"]]
    node_set = set(nodes)
    indeg = {n: 0 for n in nodes}
    children = defaultdict(list)
    for n in nodes:
        for d in info[n]["deps"]:
            if d in node_set:
                indeg[n] += 1
                children[d].append(n)

    def key(n):
        meta = info[n]
        return (MILESTONES.index(milestone_of_level(meta["effective_level"])),
                meta["closure_points"] + EFFORT_POINTS.get(meta["own_effort"], 0),
                TIER_RANK.get(meta["entry"]["tier"], 9), n)

    heap = []
    for n in nodes:
        if indeg[n] == 0:
            info[n]["effective_level"] = info[n]["level"]
            heapq.heappush(heap, (key(n), n))
    ordered = []
    while heap:
        _, n = heapq.heappop(heap)
        ordered.append(n)
        for ch in children[n]:
            indeg[ch] -= 1
            if indeg[ch] == 0:
                eff = info[ch]["level"]
                for d in info[ch]["deps"]:
                    if d in node_set:
                        eff = max(eff, info[d]["effective_level"])
                info[ch]["effective_level"] = eff
                heapq.heappush(heap, (key(ch), ch))
    if len(ordered) != len(nodes):
        raise ValueError("dependency cycle among criteria")
    return ordered


def _row(index, meta, milestone, trace_map, gated_register):
    c = meta["entry"]
    cid = c["id"]
    return OrderedDict([
        ("order", index),
        ("id", cid),
        ("milestone", milestone),
        ("tier", c["tier"]),
        ("area", c["domain"]),
        ("dependency_class", c["dependency_class"]),
        ("env", meta["env"]),
        ("needs", meta["labels"]),
        ("work_items", meta["wis"]),
        ("last_gate", meta["last_gate"]),
        ("closure_size", len(meta["closure"])),
        ("depends_on", sorted(meta["deps"])),
        ("blockers", ref_list(meta["wis"])),
        ("effort", meta["own_effort"]),
        ("effort_reason", meta["effort_reason"]),
        ("evidence", trace_map.get(cid, "unknown")),
        ("result", status_of(c)),
        ("gated_register", gated_register.get(cid, "")),
        ("description", " ".join(str(c["criterion"]).split())),
    ])


def _wi_rows(wi_levels, info):
    out = []
    for wid, item in WORK_ITEMS.items():
        level = wi_levels.get(wid)
        unlocks = sorted(cid for cid, meta in info.items() if wid in meta["closure"] and not meta["never"])
        gates = sorted(cid for cid, meta in info.items() if meta["last_gate"] == wid and not meta["never"])
        out.append(OrderedDict([
            ("id", wid),
            ("title", item["title"]),
            ("effort", item["effort"]),
            ("prereqs", list(item["prereqs"])),
            ("refs", ref_list([wid])),
            ("basis", item["basis"]),
            ("sentinel", item["sentinel"]),
            ("level", level),
            ("milestone", milestone_of_level(level)),
            ("unlocks", unlocks),
            ("last_gate_for", gates),
        ]))
    return out


def _count(items):
    out = defaultdict(int)
    for item in items:
        out[item] += 1
    return dict(out)


def summarize(model):
    rows = model["rows"]
    milestone_counts = _count(r["milestone"] for r in rows)
    effort_counts = _count(r["effort"] for r in rows + model["never"])
    return OrderedDict([
        ("criteria", len(rows) + len(model["never"])),
        ("ordered", len(rows)),
        ("never", len(model["never"])),
        ("milestones", OrderedDict((m, milestone_counts.get(m, 0)) for m in MILESTONES)),
        ("effort", OrderedDict((e, effort_counts.get(e, 0)) for e in EFFORTS)),
        ("first_ten", [r["id"] for r in rows[:10]]),
    ])


# ---------------------------------------------------------------------------
# Rendering
# ---------------------------------------------------------------------------
EXTERNAL_WIS = OrderedDict([
    ("WI-GOLDEN", "needs a reference Windows host to produce the golden outputs"),
    ("WI-HW-MATRIX", "needs physical machines and rack automation"),
    ("WI-API-SURFACE", "needs the public Windows SDK import libraries and API-set metadata"),
    ("WI-LINUX-REF", "needs a stock Ubuntu reference image"),
    ("WI-BOOT-INSTALL", "needs project Secure Boot keys and an enrolment step"),
    ("WI-PERF-BASE", "needs timings taken on the same hardware as the Windows reference"),
])

MILESTONE_MEANING = OrderedDict([
    ("M-A", "repository-local work: docs and policy checks, the runner, the golden corpus, owner decisions"),
    ("M-B", "first loader and kernel foundations: real MinGW loader context, the in-VM harness, the VAD and NT service gaps"),
    ("M-C", "core loader and kernel objects: DllMain order, access checks, handle tables, the Win32 core"),
    ("M-D", "process creation, filesystem, synchronization, named pipes and the windowing base"),
    ("M-E", "services, COM, networking, graphics, input, audio, the CLR and WOW64"),
    ("M-F", "WinRT, physical hardware, and the boot and installer chain"),
])


def md_cell(text):
    return str(text).replace("|", "\\|").replace("\n", " ")


def _table(header, body):
    lines = ["| " + " | ".join(header) + " |", "|" + "|".join("---" for _ in header) + "|"]
    for row in body:
        lines.append("| " + " | ".join(md_cell(c) for c in row) + " |")
    return "\n".join(lines)


def _unlock_text(w):
    """Criteria this item unlocks: those it gates last, and every criterion that needs it."""
    ids = w["last_gate_for"]
    parts = []
    if ids:
        shown = ", ".join(f"`{cid}`" for cid in ids[:12])
        more = f" and {len(ids) - 12} more" if len(ids) > 12 else ""
        parts.append(f"last gate for {len(ids)}: {shown}{more}")
    parts.append(f"needed by {len(w['unlocks'])} criteria")
    return "; ".join(parts)


def _never_note(row):
    if row["dependency_class"] == "EXT-GATED":
        return "gated: needs a third-party artifact the project cannot produce"
    return "declared out of scope by the DoD"


def render_json(model, summary):
    payload = OrderedDict([
        ("summary", summary),
        ("milestones", OrderedDict((m, [r["id"] for r in model["rows"] if r["milestone"] == m])
                                   for m in MILESTONES)),
        ("work_items", model["work_items"]),
        ("criteria", model["rows"]),
        ("never", model["never"]),
    ])
    return json.dumps(payload, indent=2, ensure_ascii=False) + "\n"


def render_markdown(model, summary):
    rows = model["rows"]
    never = model["never"]
    out = [
        "# Conformance criteria roadmap",
        "",
        "Generated by `tools/criteria_roadmap.py` from `tests/conformance/criteria.json`, "
        "`tests/conformance/thresholds.json`, the DoD register in "
        "`docs/phase0/DEFINITION-OF-DONE.md`, `tests/harness/registry.yaml`, and the status and "
        "decision documents. Do not edit by hand; regenerate with the script.",
        "",
        "This is a plan, not a results report. No criterion is shown as passing: every result stays "
        "`unknown` until a repository record states one. Effort buckets, order and milestone sizes come "
        "from the facts cited in the tables. `unknown` means the repository does not contain enough to "
        "estimate.",
        "",
        "## Summary",
        "",
        f"{summary['criteria']} criteria: {summary['ordered']} are placed in milestones M-A to M-F, and "
        f"{summary['never']} cannot pass without an out-of-scope or gated component (section 5).",
        "",
        _table(["Milestone", "Criteria", "Work items", "Meaning"],
               [[m, summary["milestones"][m],
                 sum(1 for w in model["work_items"] if w["milestone"] == m and not w["sentinel"]),
                 MILESTONE_MEANING[m]] for m in MILESTONES]
               + [["never", summary["never"], "",
                   "needs a gated artifact or an out-of-scope component"]]),
        "",
        "Effort buckets over all criteria: " + ", ".join(f"{e} {summary['effort'][e]}" for e in EFFORTS) + ".",
        "",
        "First ten criteria to attack (section 3 order):",
        "",
    ]
    for i, row in enumerate(rows[:10], start=1):
        out.append(f"{i}. `{row['id']}` ({row['tier']}, {row['milestone']}, effort {row['effort']}): "
                   f"needs {', '.join(row['needs']) or 'none'}; last gate `{row['last_gate']}`.")
    out += [
        "",
        "## 1. Milestones",
        "",
        "A work item's level is 0 when it has no prerequisite, and otherwise one more than its deepest "
        "prerequisite. A criterion's milestone is the level of its deepest work item (level 5 and above is "
        "M-F). A criterion that names another criterion is placed no earlier than that criterion.",
        "",
    ]
    for m in MILESTONES:
        out.append(f"### {m}")
        out.append("")
        wis = [w for w in model["work_items"] if w["milestone"] == m and not w["sentinel"]]
        if wis:
            out.append(_table(["Work item", "Effort", "Prerequisites", "Repository basis", "Unlocks"],
                              [[f"`{w['id']}` {w['title']}", w["effort"],
                                ", ".join(f"`{p}`" for p in w["prereqs"]) or "none",
                                w["basis"], _unlock_text(w)] for w in wis]))
        else:
            out.append("No work items at this level.")
        out.append("")
        count = sum(1 for r in rows if r["milestone"] == m)
        out.append(f"{count} criteria are placed in {m}.")
        out.append("")
    out += [
        "## 2. Work items",
        "",
        _table(["Work item", "Level", "Milestone", "Effort", "Prerequisites", "Blocker references"],
               [[f"`{w['id']}` {w['title']}", "" if w["level"] is None else w["level"], w["milestone"],
                 w["effort"], ", ".join(w["prereqs"]) or "none", "; ".join(w["refs"]) or "none"]
                for w in model["work_items"]]),
        "",
        "## 3. Dependency-respecting order",
        "",
        "Criteria are ordered by milestone, then by the summed effort of their work items, then by tier, "
        "then by ID. A criterion that names another criterion appears after it.",
        "",
        _table(["#", "Criterion", "Tier", "Area", "Milestone", "Needs", "Last gate", "Effort",
                "Why this effort"],
               [[r["order"], f"`{r['id']}`", r["tier"], r["area"], r["milestone"], ", ".join(r["needs"]),
                 f"`{r['last_gate']}`", r["effort"], r["effort_reason"]] for r in rows]),
        "",
        "## 4. Blockers for each criterion",
        "",
        "Each criterion's blockers are the references of its direct work items. Section 2 lists the "
        "references for each work item.",
        "",
        _table(["Criterion", "Work items", "Blocker references", "Traceability class"],
               [[f"`{r['id']}`", ", ".join(f"`{w}`" for w in r["work_items"]),
                 "; ".join(r["blockers"]) or "none", r["evidence"]] for r in rows]),
        "",
        "## 5. Criteria that cannot pass without out-of-scope or gated components",
        "",
        "For the owner to decide scope. The first group is the DoD gated register (section 3.1). The DoD "
        "says those may be waived with evidence but never reported as passing unless the controlling "
        "artifact is obtained. The second group is declared out of scope in the DoD.",
        "",
        _table(["Criterion", "Tier", "Reason", "Controlling party or DoD note", "Note"],
               [[f"`{r['id']}`", r["tier"],
                 "gated" if r["dependency_class"] == "EXT-GATED" else "out of scope",
                 r["gated_register"] or "DoD Appendix A", _never_note(r)] for r in never]),
        "",
        "## 6. Gated register check",
        "",
    ]
    mismatch = [r for r in rows + never if r["gated_register"] and r["dependency_class"] != "EXT-GATED"]
    if mismatch:
        for r in mismatch:
            out.append(f"- `{r['id']}` is in the DoD section 3.1 register, but `criteria.json` gives dependency "
                       f"class `{r['dependency_class']}`. It is placed by the other rules, and the owner should "
                       f"confirm which is right.")
    else:
        out.append("The DoD register and `criteria.json` agree.")
    out += [
        "",
        "## 7. External inputs the owner must supply",
        "",
        _table(["Work item", "Why it is external"],
               [[f"`{w['id']}` {w['title']}", EXTERNAL_WIS[w["id"]]]
                for w in model["work_items"] if w["id"] in EXTERNAL_WIS]),
        "",
        "## 8. Method and limits",
        "",
        "- Area status comes from `docs/phase2/ARCHITECTURE-STATUS.md`, `docs/status/SERVICES.md`, "
        "`docs/KNOWN-ISSUES.md`, the review reports and the decision index. A status describes what the code "
        "contains, not whether it works.",
        "- Effort starts from the area status and adds one step when the environment needs a booted personality "
        "kernel or a VM. Documentation-only checks are S. Gated and out-of-scope criteria are `unknown`. The "
        "reasoning for each criterion is in section 3.",
        "- The graph is at the granularity of an area plus keyword refinements. Criteria in the same area share "
        "work items unless a keyword narrows them, so the order is a plan, not a measurement.",
        "- The last gate is the deepest work item of a criterion. When two items are equally deep, the one later "
        "in ID order is named; every work item in the criterion's closure must be done before it can pass.",
        "- Traceability classes come from `tools/criteria_trace.py`. A registry entry is not a pass.",
        "",
    ]
    return "\n".join(out)


def render_text(model, summary):
    lines = [f"criteria {summary['criteria']}  placed {summary['ordered']}  never {summary['never']}"]
    for m in MILESTONES:
        lines.append(f"  {m}: {summary['milestones'][m]}")
    lines.append("first ten: " + ", ".join(summary["first_ten"]))
    return "\n".join(lines) + "\n"


# ---------------------------------------------------------------------------
# Selftest
# ---------------------------------------------------------------------------
def repo_reference_problems(repo=REPO):
    """Citations in WORK_ITEMS and AREA_FACTS that do not resolve. Empty when all resolve."""
    problems = []
    owner = _read(repo / "docs" / "OWNER-DECISIONS.md")
    review_text = "\n".join(_read(p) for p in sorted((repo / "docs" / "review").glob("*.md")))
    review_text += "\n" + _read(repo / "docs" / "KNOWN-ISSUES.md")
    decisions_dir = repo / "docs" / "decisions"
    for wid, item in WORK_ITEMS.items():
        for ref in item["refs"]:
            if ref.startswith("DOC:"):
                if not (repo / ref[4:]).exists():
                    problems.append(f"{wid}: missing file {ref[4:]}")
            elif ref.startswith("OD-"):
                if not re.search(r"\|\s*" + re.escape(ref) + r"\s*\|", owner):
                    problems.append(f"{wid}: owner decision {ref} is not in OWNER-DECISIONS.md")
            elif ref.startswith("ADR-"):
                if not list(decisions_dir.glob(f"{ref[4:]}-*.md")):
                    problems.append(f"{wid}: decision record {ref[4:]} not found")
            else:
                pattern = r"(?<![A-Za-z0-9])" + re.escape(ref) + r"(?![0-9])"
                if not re.search(pattern, review_text):
                    problems.append(f"{wid}: reference {ref} is not in the review or known-issues text")
    for area, (_, _, refs) in AREA_FACTS.items():
        for ref in refs:
            if ref.startswith("DOC:") and not (repo / ref[4:]).exists():
                problems.append(f"area {area}: missing file {ref[4:]}")
            if ref.startswith("OD-") and not re.search(r"\|\s*" + re.escape(ref) + r"\s*\|",
                                                     _read(repo / "docs" / "OWNER-DECISIONS.md")):
                problems.append(f"area {area}: owner decision {ref} missing")
            if ref.startswith("ADR-") and not list(decisions_dir.glob(f"{ref[4:]}-*.md")):
                problems.append(f"area {area}: decision record {ref[4:]} missing")
    return problems


def _has_cycle(prereq):
    state = {}

    def visit(node):
        if state.get(node) == 1:
            return True
        if state.get(node) == 2:
            return False
        state[node] = 1
        for p in prereq.get(node, ()):
            if visit(p):
                return True
        state[node] = 2
        return False

    return any(visit(n) for n in prereq)


def selftest():
    """Built-in checks on synthetic data, the work-item graph and repository references.

    Returns a list of failure messages. Empty means every check passed."""
    failures = []

    def check(cond, label):
        if not cond:
            failures.append(label)

    # Work-item graph.
    for wid, item in WORK_ITEMS.items():
        for p in item["prereqs"]:
            check(p in WORK_ITEMS, f"{wid}: unknown prerequisite {p}")
        check(item["effort"] in EFFORTS, f"{wid}: bad effort {item['effort']}")
    check(not _has_cycle({w: WORK_ITEMS[w]["prereqs"] for w in WORK_ITEMS}), "work-item graph has a cycle")
    check(work_item_level("WI-HARNESS") == 0, "WI-HARNESS must be level 0")
    check(work_item_level("WI-KHARNESS") == 1, "WI-KHARNESS must be level 1")
    check(work_item_level("WI-EXT-GATED") is None, "sentinels have no level")
    check(work_item_level("WI-PROC-CREATE") == 3, "WI-PROC-CREATE must be level 3")

    # Synthetic ordering: a dependency comes first even when it is more expensive.
    base = {"domain": "PE", "tier": "P3", "dependency_class": "SELF", "criterion": "x",
            "verification": "", "status": "not_implemented"}
    cheap = dict(base, id="DOD-PE-A001")
    needy = dict(base, id="DOD-PE-A002", tier="P0", criterion="needs DOD-PE-A001 first")
    info = {
        "DOD-PE-A001": {"entry": cheap, "deps": set(), "level": 0, "closure_points": 9,
                        "own_effort": "S", "never": False, "effective_level": None},
        "DOD-PE-A002": {"entry": needy, "deps": {"DOD-PE-A001"}, "level": 0, "closure_points": 1,
                        "own_effort": "S", "never": False, "effective_level": None},
    }
    order = _topo_order(info)
    check(order == ["DOD-PE-A001", "DOD-PE-A002"], f"dependency not respected: {order}")
    cyc = {
        "DOD-PE-A001": {"entry": cheap, "deps": {"DOD-PE-A002"}, "level": 0, "closure_points": 1,
                        "own_effort": "S", "never": False, "effective_level": None},
        "DOD-PE-A002": {"entry": needy, "deps": {"DOD-PE-A001"}, "level": 0, "closure_points": 1,
                        "own_effort": "S", "never": False, "effective_level": None},
    }
    try:
        _topo_order(cyc)
        failures.append("a dependency cycle must raise")
    except ValueError:
        pass

    # Effort rules.
    check(criterion_effort(base, "userspace", 1, False, False, False)[0] == "M", "PE userspace is M")
    check(criterion_effort(base, "kernel", 1_000_000, False, False, False)[0] == "XL",
          "PE kernel with a large volume is XL")
    check(criterion_effort(dict(base, domain="NOPE"), "userspace", 1, False, False, False)[0] == "unknown",
          "unknown area gives unknown effort")
    check(criterion_effort(base, "EXT-GATED", "x", True, False, False)[0] == "unknown",
          "gated effort is unknown")
    check(criterion_effort(base, "kernel", 1, False, False, True)[0] == "S", "doc-only effort is S")

    # Parsing helpers.
    check(is_doc_only({"verification": "`dod docs-lint --policy`"}), "docs-lint is doc-only")
    check(not is_doc_only({"verification": "docs-lint and canary sweep"}), "canary is not doc-only")
    sample_dod = ("### §3.1 EXT-GATED register (governance)\n\n"
                  "| Criteria | Artifact required | Controlling party |\n|---|---|---|\n"
                  "| `DOD-AC-A001`, `DOD-AC-B003` | kernel attestation | Riot / Epic |\n\n## §4 next\n"
                  "| `DOD-NOT-IN-SECTION` | x | y |\n")
    reg = parse_gated_register(sample_dod)
    check(list(reg) == ["DOD-AC-A001", "DOD-AC-B003"], f"register parse: {list(reg)}")
    decisions = parse_decision_status(
        "| [0029](0029-x.md) | Title | open | ntdll LDR-1 (High) | |\n"
        "| [0003](0003-y.md) | Title | fixed (option 1) | kernel H-2 | d7 |\n")
    check(decisions.get("0029") == "open" and decisions.get("0003") == "fixed",
          f"decision parse: {decisions}")
    check(milestone_of_level(0) == "M-A" and milestone_of_level(9) == "M-F", "milestone bounds")
    check(milestone_of_level(None) == NEVER, "never milestone")

    # Repository references and the real criteria set.
    failures.extend(repo_reference_problems())
    try:
        criteria = load_criteria()
        thresholds = load_thresholds()
        gated = parse_gated_register(_read(DOD_PATH))
        trace_map = trace_classes()
    except (OSError, ValueError) as exc:
        failures.append(f"cannot load repository inputs: {exc}")
        return failures
    check(len(criteria) == 423, f"criteria count is {len(criteria)}")
    check(all(c["id"] in thresholds for c in criteria), "every criterion has a thresholds entry")
    model = build_model(criteria, thresholds, trace_map, gated)
    everything = model["rows"] + model["never"]
    check(len(everything) == len(criteria), f"placed {len(everything)} of {len(criteria)}")
    check(len({r["id"] for r in everything}) == len(everything), "a criterion appears twice")
    gated_ids = {c["id"] for c in criteria if c["dependency_class"] == "EXT-GATED"}
    check({r["id"] for r in model["never"] if r["dependency_class"] == "EXT-GATED"} == gated_ids,
          "every EXT-GATED criterion must be in the never list")
    check(all(r["order"] == i for i, r in enumerate(model["rows"], start=1)), "order numbers are sequential")
    position = {r["id"]: r["order"] for r in model["rows"]}
    for r in model["rows"]:
        for dep in r["depends_on"]:
            if dep in position:
                check(position[dep] < r["order"], f"{r['id']} is listed before its dependency {dep}")
    check(all(r["effort"] in EFFORTS for r in everything), "effort values")
    check(all(r["milestone"] in MILESTONES for r in model["rows"]), "milestone values")
    check(all(w["level"] is not None for w in model["work_items"] if not w["sentinel"]),
          "non-sentinel work items have a level")
    return failures


def build_real_model(repo=REPO):
    criteria = load_criteria(repo / "tests" / "conformance" / "criteria.json")
    thresholds = load_thresholds(repo / "tests" / "conformance" / "thresholds.json")
    gated = parse_gated_register(_read(repo / "docs" / "phase0" / "DEFINITION-OF-DONE.md"))
    trace_map = trace_classes(repo)
    return build_model(criteria, thresholds, trace_map, gated)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    fmt = parser.add_mutually_exclusive_group()
    fmt.add_argument("--json", action="store_true", help="emit JSON")
    fmt.add_argument("--markdown", action="store_true", help="emit Markdown")
    parser.add_argument("--out", help="write output to this file instead of stdout")
    parser.add_argument("--selftest", action="store_true", help="run built-in checks and exit")
    args = parser.parse_args(argv)

    if args.selftest:
        failures = selftest()
        for failure in failures:
            print(f"FAIL {failure}", file=sys.stderr)
        print("criteria_roadmap selftest: " + ("ok" if not failures else f"{len(failures)} failure(s)"))
        return 1 if failures else 0

    try:
        model = build_real_model()
    except (OSError, ValueError) as exc:
        print(f"criteria_roadmap: error: cannot build the roadmap: {exc}", file=sys.stderr)
        return 1
    summary = summarize(model)
    if args.json:
        text = render_json(model, summary)
    elif args.markdown:
        text = render_markdown(model, summary)
    else:
        text = render_text(model, summary)

    if args.out:
        try:
            Path(args.out).write_text(text, encoding="utf-8", newline="\n")
        except OSError as exc:
            print(f"criteria_roadmap: error: cannot write {args.out}: {exc.strerror}", file=sys.stderr)
            return 1
    else:
        sys.stdout.buffer.write(text.encode("utf-8"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
