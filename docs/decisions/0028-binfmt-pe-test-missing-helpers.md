# 0028: binfmt_pe_test.c calls helpers that are not defined anywhere

Status: Accepted (2026-10-14). Option 2 for the command-line, environment and UTF-16 cases. The test file is unchanged. See Outcome.
Finding: `tests/syntax/README.md` remaining failures; `docs/KNOWN-ISSUES.md` (binfmt_pe_test.c entry).
Date: 2026-10-12

## Context

`src/kernel/fs/binfmt_pe_test.c` calls four functions that have no definition or
declaration in the repository:

| Call | Test lines | Closest existing name |
|------|-----------|-----------------------|
| `pe_build_command_line(&bprm, buf, 256)` | 535 | none |
| `pe_build_environment(&bprm, buf, 256)` | 546 | none |
| `pe_utf8_to_utf16(...)` | 562, 570, 575 | none (`binfmt_pe.c:558` says "Minimal UTF16 helpers not needed for M0 test" and defines none) |
| `pe_prot_to_mmap(...)` | 252-256 | none (`src/loader/pe_prot.h` declares `pe_prot_from_section` and `PE_PROT_*` only) |

It also uses three types that are not defined: `PPEB` (line 588), `PTEB` (589)
and `RTL_USER_PROCESS_PARAMETERS` (627). `PEB`, `TEB` and
`RTL_USER_PROCESS_PARAMETERS` are defined in `src/ntdll/nt_types.h`, which the
kernel-side fs code does not include, and `PPEB`/`PTEB` typedefs are not in that
header.

### History

`git log -S` and `git log -G` on each name find only one commit that touches
these calls: `5eb8026` ("Publish sanitized personal project source snapshot"),
which added `binfmt_pe_test.c` with the calls already in it. No commit adds a
definition, and no commit removes one. The later commits that match
(`6d151fa`, `ec2cf71`, `3374621`, `202920a`) are documentation or the syntax
shim. So the helpers were never written in this history. The `pe_load_state`
member `pe` (used as `st.pe.subsystem`, lines 601, 672, 696) is also absent:
the struct has `peb`, `teb` and `params`, and no `pe` member.

Conclusion: these were never written. They were not removed or renamed, so no
rename in the test can make it correct.

## Options

1. Port the test to the existing API and drop the cases that have no
   counterpart. Use `pe_prot_from_section` for the protection cases, and remove
   the UTF-16 and command-line/environment cases until the functions exist.
   Tradeoff: smaller coverage now; no new loader code; the test compiles against
   what exists. Small change in the test file only.
2. Write the four helpers in `src/kernel/fs/binfmt_pe.c` (or `src/loader/`) and
   add the `PEB`/`TEB`/`RTL_USER_PROCESS_PARAMETERS` typedefs the kernel side
   needs. Tradeoff: real loader behaviour, so it needs design review of the
   argv/envp layout and UTF-16 conversion; it is not a rename. Largest change.
3. Move the test out of the build until option 1 or 2 is chosen. Tradeoff: no
   loss of code, but the test stops being built at all. Also needs the
   Makefile entry removed.

## Recommendation

Option 1 for the protection cases, which map onto `pe_prot_from_section`. For
the command-line, environment and UTF-16 cases, option 2 is needed before they
can run. Do that as a separate change with its own review.

## Why no fix was applied now

Renaming the calls is not possible: no existing function has the same
behaviour. The minimal fix in this branch was the `struct inode` stub in
`tests/syntax/shim/linux/fs.h` (`i_mode`, `i_uid_read`), which unblocks
`binfmt_pe.c`, not the test.

## Test plan

- `sh tests/syntax/check.sh` still reports `binfmt_pe_test.c` with the same
  missing-declaration errors, until option 1 or 2 is applied.
- After option 1: the `pe_prot_from_section` assertions run in the host test
  harness. Host and kernel-host tests are out of scope for this change.
- After option 2: `pe_build_command_line` and `pe_build_environment` return the
  lengths the test expects, and UTF-16 output for "ABC", U+00E9 and U+1F600 is
  checked.

## Outcome (2026-10-14)

Option 2 was taken. The change has four parts:

- The helpers are in `src/loader/pe_text.c`: `pe_utf8_to_utf16`,
  `pe_build_command_line`, `pe_build_environment`, and `pe_prot_to_mmap` in
  `pe_text.h`. `pe_text.h` now builds under `__KERNEL__` too, so the kernel
  build compiles the same code. `tools/ntpers_kpatch.py` copies `pe_text.[ch]`
  to `fs/` and builds `pe_text.o` with `binfmt_pe.o`.
- `pe_build_process_parameters` fills the exec's strings the way Windows does.
  ImagePathName is the exec filename. CommandLine is argv joined with spaces,
  quoting arguments that contain a space or tab. Environment is the envp block,
  with `EnvironmentSize` in bytes. The strings sit in a second mapping, and the
  struct points at them. `Length` and `MaximumLength` are the struct size,
  `0x470`, in every build (the test checks `MaximumLength == sizeof`). The old
  code set `PAGE_SIZE` under `CONFIG_NT_PERSONALITY` and left it unset without it.
- The arguments come from `bprm->argv`/`bprm->envp` in the userspace model
  (`kshim.h`). The kernel has no such arrays. It reads the strings from the new
  stack at `mm->arg_start`, argv then envp, which `create_elf_tables` walks the
  same way.
- `pe_build_environment` writes an empty block as two NULs, not one. A block
  with no variables is still double-NUL terminated.

`binfmt_pe_test.c` was not changed. It is built and run by `tests/kernel-host`
on Linux, and its two failing cases (`peb_teb_population`,
`process_parameters_layout`) now pass.
