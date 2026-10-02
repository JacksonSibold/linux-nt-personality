# ntpers — Windows execution personality research

An experimental personal systems project investigating a native Windows PE execution personality alongside Linux's ABI. The source explores PE loading, NT syscall dispatch, object/handle management, virtual memory, synchronization and compatibility probing.

**Status: incomplete research prototype.** This is not a finished Windows compatibility layer, replacement for Windows, or production operating system. Development used AI-assisted implementation and independent verification.

## Repository map

- `src/`: kernel-personality components, loader, ntdll and Win32 experiments.
- `arch/`: architecture-specific integration.
- `docs/`: architecture, research notes and definition of done.
- `tests/`: conformance harness, differential probes and regression source.
- `vm/`: experimental build/run and artifact-freshness tooling.

The original development handoff records a minimal PE reaching exit code 42 and only **3 of 423** full conformance criteria passing. These are historical project records, not results reproduced during this publication. Large generated evidence bundles, Windows binaries, VM images, build products and local Git history are excluded from this source snapshot. Some evidence-dependent commands will therefore lack their inputs.

## Engineering lesson

Independent verification found tests measured against stale kernel images. Source-to-image freshness checks and artifact identity stamping were added before accepting results. Other defects remain, including loader and virtual-memory behavior; the design documentation describes goals, not completed capabilities.

## Build and validation

Kernel/VM work requires a separately configured Linux environment. Read the scripts before running them: some rebuild a local kernel tree. Publication checks cover Python syntax and repository hygiene only; a clean kernel build, VM boot and full conformance run were not performed here.

## Source and licensing

This snapshot retains source notices. Linux-derived integration material remains subject to its upstream license; this publication does not relicense third-party code or include proprietary Windows binaries. A complete file-by-file provenance/license review has not been established. No new blanket license grant is made for the project's original code.
