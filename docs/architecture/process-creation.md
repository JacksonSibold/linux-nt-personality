# Process creation

Status: design note, 2026-10-12. No VM path creates a process yet. The host-testable parts of M1, M4 and M5 are built (see the status table after the milestone plan in section 6). Prerequisite P1 is partly done: file-backed data sections are built, and `SEC_IMAGE` is not. It
Prerequisite P1 is partly done: file-backed data sections are built, and `SEC_IMAGE` sections are built in the userspace build only (the kernel build still refuses them). It
describes the code as it is, what `NtCreateUserProcess` would need on top of
it, three implementation options, and a plan. The decision is recorded in
[0037](../decisions/0037-process-creation.md), which is open.

## Summary

- A PE process starts as an ordinary Linux `execve` of a PE file. The
  `binfmt_pe` handler replaces the task's address space, maps the image,
  builds a PEB and TEB, maps ntdll, and sets the GS base. Only then does the
  task get the NT personality.
- The NT process-creation services are not implemented in the kernel build.
  `NtCreateProcessEx`, `NtCreateProcess` and `NtCreateUserProcess` return
  `STATUS_NOT_IMPLEMENTED`. `kernel32!CreateProcessW` is a stub that returns
  NULL with `ERROR_FILE_NOT_FOUND`.
- The userspace shim has an eprocess-only `NtCreateProcessEx`, and its
  `NtCreateUserProcess` builds a thread on a dummy entry address. Neither
  creates a PE image. Neither is a model for the kernel path.
- The main blockers are: no kernel `SEC_IMAGE` sections (the userspace build has them, see P1), one global
  kernel address space and VAD table (the userspace shim has one per process, see P3), one global file-handle table, an empty
  `ProcessParameters`, and a process object that is created lazily for the
  first task.

## 1. How a PE process starts today

The path below is verified against `src/kernel/fs/binfmt_pe.c`, the NT
personality code in `src/kernel/nt/` and `arch/x86/nt/`, and the VM scripts.
The same flow is drawn in [overview.md](overview.md#how-a-pe-executable-starts).

1. **Launch.** A PE file is started by the Linux `execve` path. The VM test
   scripts run the PE by path, for example `timeout 30 "$PE"` in
   `vm/test_n4.sh` with `PE=/tests_kernel/n4_thread.exe`. `vm/init.c:129`
   (`run_test`) forks and runs the test through `/bin/sh` or directly with
   `execl`. Nothing in the repo calls an NT service to start a PE.
2. **Handler registration.** `binfmt_pe.c:1183-1202` registers
   `pe_format` with `register_binfmt` from a `core_initcall`. The handler
   takes over any file the kernel's binfmt walk offers it.
3. **Entry check.** `load_pe_binary` (`binfmt_pe.c:975`) refuses with
   `-EPERM` when `uid != euid` or `gid != egid`. This is before any commit.
4. **Headers.** `pe_read_file_headers` (`:142`) reads the headers with
   `kernel_read` from `bprm->file`. The file is not mapped as a section. The
   image bytes are read later by `pe_map_sections` (`:403`).
5. **Point of no return.** `begin_new_exec` and `setup_new_exec` are called.
   The old mm is gone after this. Each later failure goes through
   `pe_post_commit_fail` (`:968`), which kills the task rather than returning
   an error (see [0012](../decisions/0012-kernel-post-commit-exec-failure.md)).
6. **Stack and arguments.** `setup_arg_pages` places the Linux argv and envp on
   the stack. `binfmt_pe` does not build a PE command line. A search for
   `argv`, `envp` and `CommandLine` in `binfmt_pe.c` finds only a comment.
7. **Image.** `pe_pick_base` (`:344`), `pe_map_sections` (`:403`),
   `pe_apply_relocs` (`:489`). The stack, parameter, PEB and TEB mappings are
   also recorded in the kernel NT VAD model through `ntmm_add_region` when
   `CONFIG_NT_PERSONALITY` is set (`:940`, `:562`, `:583`, `:608`). This note
   did not confirm the same for every image section.
8. **Stack.** `pe_setup_stack` (`:940`) maps the reserve as read-write. The
   reserve is clamped to 8 MiB. The commit size is ignored.
9. **ProcessParameters.** `pe_build_process_parameters` (`:562`) maps one
   zeroed page and stores its address in `st->params`. No field is set. The
   comment "Set CommandLine buffer empty" describes code that does not exist.
   The page is not an `RTL_USER_PROCESS_PARAMETERS` in the NT layout.
10. **PEB.** `pe_init_peb` (`:583`) maps a page. It writes `ImageBaseAddress` at
    `+0x10`, `ProcessParameters` at `+0x20`, and zero at `+0xbc`. `Ldr` is not
    built. The kernel never builds module lists (as
    [research-1](../phase1/research-1-nt-architecture.md) intends).
11. **TEB.** `pe_init_teb` (`:608`) maps two pages. It writes `NtTib.Self` at
    `+0x30`, `StackBase` at `+0x08`, `StackLimit` at `+0x10`, a zero TLS pointer
    at `+0x58`, the PEB at `+0x60`, `LastError` at `+0x68`, and `ClientId` at
    `+0x40` and `+0x48`. Both `ClientId` fields are set to `current->pid`, so
    the thread id is not separate from the process id. Static TLS is not
    copied.
12. **ntdll.** `pe_map_ntdll` (`:641`) tries `/ntdll.dll` and then
    `/windows/system32/ntdll.dll`. There is no `/tmp` fallback (see
    [0013](../decisions/0013-kernel-ntdll-trusted-path.md)). If ntdll is found,
    the entry point is `LdrInitializeThunk`. If not, the entry point is the exe
    entry point and the task has no loader.
13. **Commit to the personality.** `finalize_exec`, then
    `nt_enter_personality` (`binfmt_pe.c:1092`). The function sets `TIF_NTABI`
    (`arch/x86/nt/dispatch.c:274`; a second copy is at
    `src/kernel/nt/dispatch.c:381`). It also attaches an NT thread object to the
    task.
14. **Enter user mode.** `start_thread(regs, entry, sp)`. `RCX` is set to the
    image base when ntdll is present, and to the PEB otherwise. The GS base is
    set to the TEB with `x86_gsbase_write_cpu_inactive`. A TEB at or above
    `TASK_SIZE_MAX` is refused (`pe_post_commit_fail`).
15. **Process object.** `binfmt_pe` does not create the NT process object. The
    first NT call from the task reaches `nt_current_eprocess`
    (`src/kernel/nt/object.c:1072-1084`). That function looks the object up by
    `tgid`, then by `pid`, and creates it lazily with
    `ObCreateProcessObject(..., current->tgid, current->comm)`. The primary
    token is set in `ObCreateProcessObject` (`object.c:1175`, from
    `token_create_default_primary()`).

The `fork`/`vfork` restriction described in
[research-2](../phase1/research-2-pe-process-memory.md) (line 196, "Decision:
`fork`/`vfork`/`clone(SIGCHLD)` from a PE process return `-ENOSYS`") is not in
the code. The only `MMF_PE` mention is a comment at `binfmt_pe.c:1142` that
says `MMF_PE_IMAGE` is not defined in vanilla. A PE task can call Linux `fork`
today.

## 2. What the NT process-creation services do today

| Entry point | Kernel build (`arch/x86/nt/proc_thread.c`, `src/kernel/nt/proc_thread.c`) | Shim build (`proc_thread.c`, `#ifndef __KERNEL__`) |
|---|---|---|
| `NtCreateProcessEx` | `STATUS_NOT_IMPLEMENTED` (`arch` `:1353`, `src/kernel/nt` `:1441`) | `nt_create_process_internal` (`src/kernel/nt` `:407`, `arch` `:347`). Creates an eprocess and a handle. No image. |
| `NtCreateProcess` | `STATUS_NOT_IMPLEMENTED` (`arch` `:1354`, `src/kernel/nt` `:1442`) | Forwards to `NtCreateProcessEx` (`src/kernel/nt` `:482`). `InheritObjectTable` is ignored. |
| `NtCreateUserProcess` | Validates `CreateInfo` and `AttributeList` (`proc_create_args.h`), then `STATUS_NOT_IMPLEMENTED` (`arch` `:1366`, `src/kernel/nt` `:1454`) | Creates a process as above, then a thread on the dummy address `0x140001000` (`src/kernel/nt` `:491`). `AttributeList` is used as the start address. This is a test hack and must not be kept. |
| `kernel32!CreateProcessW/A` | Not applicable | Stub in `src/ntdll/kernel32.c:395-402`: `SetLastError(2)`, returns NULL |

The kernel build has a working thread path that could serve as a model for the
initial thread. `nt_svc_NtCreateThreadEx` uses `kernel_clone` through a
`kernel_clone_args` structure and calls `copy_process` (`arch/x86/nt/proc_thread.c:1159-1270`).
That path is for threads, not processes. The header comment (`proc_thread.c:43-56`)
says the kernel "uses `kernel_clone(CLONE_NEWPID|VM)` with an emptied mm". That
plan is not in the code (see [proc_thread.md](../modules/proc_thread.md), Known
gaps).

The shim's `nt_create_process_internal` calls `ObInheritHandles(parent, newproc)`
on every call. `ObInheritHandles` (`src/kernel/nt/handle.c:574-605`) copies only
entries with `HANDLE_FLAG_INHERIT`, so the filter is in place. The shim does
not check `InheritObjectTable` or the attribute list before calling it.

## 3. What NtCreateUserProcess would need on top

The Windows contract is in [research-2](../phase1/research-2-pe-process-memory.md),
section on `CreateProcessW` (around line 184). One `NtCreateUserProcess` call
creates the process and its initial thread, both suspended. The steps and
what each needs in this repo:

1. **Image source.** Windows passes a section handle that was created from the
   file (`SEC_IMAGE`). Here `NtCreateSection` builds file-backed data sections
   (`src/kernel/nt/mm.c:1820`) and, in the userspace build, `SEC_IMAGE` sections
   (`nt_create_image_section`, `mm.c:1938`). The kernel build still returns
   `STATUS_NOT_IMPLEMENTED` (`mm.c:1893`). Without the kernel side, the only image
   source is a path that `binfmt_pe` reads itself. See prerequisite P1.
2. **Child task.** Two ways to create a new task that runs a PE:
   - *Clone then exec.* Create a task with a new mm, then run the PE loader in
     that task's context. This is what `binfmt_pe` does in the current task.
     Running `load_pe_binary` in another task needs a kernel path that is not
     `execve` from userspace.
   - *Spawn helper.* Create the task with `CLONE_VFORK` (or equivalent), then
     call the exec path from the child. Linux's `posix_spawn` works this way.
     Parent and child share no mm after the exec.
   In either case the child must have a process identity (tgid) that
   `nt_current_eprocess` finds at creation time, not lazily on the first
   syscall. See P3 and P6.
3. **RTL_USER_PROCESS_PARAMETERS.** The caller's parameters live in the
   caller's address space. The child needs a copy. Absolute pointers inside
   the struct (image path, command line, environment, current directory) must
   be relocated into the child's mapping. `binfmt_pe` builds an empty page
   today (`binfmt_pe.c:562-581`). See P7. The NT layout is in
   [research-1](../phase1/research-1-nt-architecture.md) (around line 257): the
   parameter block sits in the PEB region, and the standard handles are
   synthesized from host fds 0, 1 and 2.
4. **PEB and TEB.** `binfmt_pe` already writes both (section 1, steps 10 and
   11). For `NtCreateUserProcess` they must be written for the child, not the
   caller. The `ClientId` deviation noted above (process id stored as thread
   id) must be fixed before any test compares thread ids.
5. **Handle inheritance.** NT handle values are kept in the child at the same
   index (`ObInheritHandles`). Windows uses the `PS_ATTRIBUTE_HANDLE_LIST`
   attribute and `bInheritHandles` on `CreateProcess`. The kernel path does not
   read either. The handle table lives per task and is destroyed on task exit
   (`src/kernel/nt/dispatch.c:401-405`). This repo does not show what happens to
   the NT handle table on an `execve` of an existing task. That needs checking
   before option A is chosen. See P2.
6. **Initial thread.** The child needs one thread, created suspended when
   `ThreadFlags` has `THREAD_CREATE_FLAGS_CREATE_SUSPENDED`. Its TEB and
   stack must exist before the thread is resumed. The kernel `NtCreateThreadEx`
   path is the model. The thread has to start at the ntdll entry, not at a
   stub.
7. **Suspend semantics.** `NtResumeThread` on the initial thread starts
   execution. Suspended creation means the child has been fully set up and is
   blocked before user code runs. The kernel suspend gate is described in
   `proc_thread.c:1-40` and in [proc_thread.md](../modules/proc_thread.md). It is
   implemented in both builds.
8. **Tokens.** Each process object gets a default primary token in
   `ObCreateProcessObject` (`object.c:1175`). The child should inherit the
   parent's primary token, or take one from the attribute list
   (`PS_ATTRIBUTE_TOKEN`). Token slots exist (`security.h:17` mentions
   `task->win_token`) but no duplication path is wired into process creation.
9. **Job and session.** Not implemented. No job-object type or header was
   found in `src/kernel/nt` or `arch/x86/nt`. Job and
   session fields in the create arguments are ignored. This is acceptable for
   the first milestone and must be listed as ignored in the result.
10. **Startup info.** The parent's `STARTUPINFO` equivalent (window flags,
    standard handles, desktop) is carried in the parameter block. It is not
    read today.
11. **Return values.** `ProcessHandle` and `ThreadHandle` are returned. The
    `PS_CREATE_INFO` structure (`CreateInfo`) is in/out: on success Windows
    writes a success state and image information. The kernel stub ignores it.
    The shim ignores it too.
12. **Failure.** A failure before the child task is committed can return an
    error. A failure after exec commits the child cannot be rolled back. The
    child has to be killed and the status reported through the parent's
    handles. This is the same problem as
    [0012](../decisions/0012-kernel-post-commit-exec-failure.md), applied to a
    new task.

## 4. Options

### Option A: userspace-assisted CreateProcess in ntdll

`CreateProcessW` in ntdll builds the parameter block and the handle list in a
memory file (`memfd`) that is inherited across exec. It then calls a new
kernel helper syscall, which runs the spawn and the `binfmt_pe` exec in the
child. The child's ntdll reads the block from the inherited fd. `NtCreateUserProcess`
stays unsupported.

- Pros: no NT create semantics in the kernel. The existing `binfmt_pe` path
  is reused unchanged apart from reading the block. No file-backed sections
  are needed, because the PE is still read by `binfmt_pe`. Testable with the
  VM scripts from the first milestone.
- Cons: the spawn still needs a kernel fork exception. The design decision that
  PE tasks cannot `fork` (research-2) is not in the code, and A needs a
  narrow spawn primitive anyway. Process creation is not an NT object
  operation: the parent gets a Linux pid, and `NtCreateUserProcess` remains
  refused, so Windows binaries that call it directly still fail. Handle
  inheritance and token inheritance are conventions carried in the block, not
  kernel state. A suspended start needs a flag in the spawn helper.

### Option B: kernel-side full NtCreateUserProcess

`NtCreateUserProcess` is implemented in the kernel. It takes a section handle or
a path, creates the child task with its own mm and VAD table, runs the PE load
in that task, copies and relocates the parameter block, inherits handles and the
token, creates a suspended initial thread, and returns handles and `PS_CREATE_INFO`.

- Pros: this is the Windows contract. Callers see the same API, and suspended
  start, handle inheritance, tokens and `PS_CREATE_INFO` come from the kernel.
  `CreateProcessW` becomes a thin wrapper.
- Cons: it depends on most of the prerequisites in section 5 (P1 to P6). A
  second address space and VAD table per process is needed first. The kernel
  would write into another task's user memory (the parameter block) at create
  time. That is a new write path, not the exec-time path that is reviewed
  today. Host testing is likely limited, since the `kshim` model was not checked for a second mm.
  Largest change and largest risk.

### Option C: hybrid, kernel spawn primitive with NT wrapper

The kernel gets one primitive: create a child task, run the PE exec in it
(as in B step 2), take the parameter block from a memfd or a user buffer, and
leave the thread suspended. `NtCreateUserProcess` is a wrapper over that
primitive that reads the arguments and maps them to handles. Parameter
marshalling and attribute parsing are done in ntdll, so the kernel sees a
small, checked format. Handle inheritance is done with `ObInheritHandles`
after the child exists, gated by the caller's list.

- Pros: the kernel change is one primitive, which is reviewable and testable
  in the VM. Both `CreateProcessW` and `NtCreateUserProcess` use it, so there is
  one code path. File-handle sources come later, when P1 is done, without
  changing the primitive.
- Cons: a new kernel primitive and a new user/kernel format (the block)
  to review. Still needs the spawn exception of A and the child-mm work of B,
  so it is not cheaper than B for the kernel side. Two entry points must agree
  on errors.

## 5. Recommendation

Choose option C, and build it in the order of section 6. The first milestone
uses only the existing `binfmt_pe` exec path and a memfd block, which is
testable in the VM without file-backed sections. Option A is the fallback if
the child-mm work (P3) cannot start for a long time, because it gives
`CreateProcessW` a working path with less kernel change. It is not a substitute
for `NtCreateUserProcess`.

Option B is the target semantics, reached through C. Do not start B's full
kernel path before P1 to P3 are decided.

## 6. Incremental plan

Each milestone is testable on the host (`tests/host`, `tests/kernel-host`, and
the `kshim` model) or in the VM (`vm/` scripts that run `/tests_kernel/*.exe`).

| Milestone | Work | Test |
|---|---|---|
| M0 | Record the current behaviour as tests: `NtCreateProcessEx` and `NtCreateUserProcess` return `STATUS_NOT_IMPLEMENTED` in the kernel build; `CreateProcessW` returns NULL. | Host: the existing `src/kernel/nt/proc_thread_test.c` already calls `NtCreateProcessEx`. Add an assertion for the kernel-build status. VM: run a PE by path (`vm/test_n4.sh` pattern) and confirm exit code and dmesg. |
| M1 | `binfmt_pe` reads an optional parameter block from an inherited memfd and fills `ProcessParameters` (image path, command line, environment, current directory). Empty block keeps today's behaviour. | VM: a PE that prints its command line and exits with a fixed code. Host: encode and decode the block in `tests/host`. |
| M2 | Kernel spawn primitive: create a child task, exec the PE in it, leave it blocked before user code. No NT handle or token inheritance yet. | VM: spawn a child PE from a parent PE, wait for it, check the exit code. A failure after exec must kill the child (see 0012). |
| M3 | Suspended start: the child's initial thread is created blocked, and `NtResumeThread` releases it. | VM: a parent checks that the child has not run (for example, it writes a file only after resume). |
| M4 | Handle and token inheritance from the attribute list, with `ObInheritHandles` gated by the caller's list. | Host: `handle.c` inheritance test with an explicit list. VM: a handle created by the parent is usable in the child at the same value. |
| M5 | `NtCreateUserProcess` wrapper over M2 to M4, path-based only. Returns `STATUS_INVALID_PARAMETER` for a file-handle image source. `CreateProcessW` calls it. | Host: argument validation. VM: `CreateProcessW` on a PE path. |
| M6 | `SEC_IMAGE` sections (P1; file-backed data sections are done, and userspace `SEC_IMAGE` sections are done: planner `image_plan.h`, base relocation `image_reloc.h`, and tests `mm_test.c`, `image_plan_test.c`, `image_reloc_test.c`). Open: the kernel-side `SEC_IMAGE` mapping, and a file-handle image source in `NtCreateUserProcess`. | Host: section creation and mapping with a file (done for the userspace build). VM: `NtCreateUserProcess` from a file handle. |
| M7 | Remove the shim's dummy-address use of `NtCreateUserProcess`, and the eprocess-only path, once the kernel path covers them. | Shim build: existing tests pass, and the dummy address no longer appears. |

### Milestone status (2026-10-12)

| Milestone | State | Built | Not built |
|-----------|-------|-------|-----------|
| M0 | Not started | Nothing. The kernel-build stubs are unchanged and have no status assertions. | Status assertions for the kernel build. These need a VM run or a kernel-branch build. |
| M1 | Host part done | `src/kernel/nt/process_params.h`: builder and validator for a relocatable block (offsets, not pointers), with normalization. Host tests: `tests/kernel-host/process_params_test.c`. | `binfmt_pe` reads the block from a memfd and fills `ProcessParameters` (P7). VM test. |
| M2 | Not started | Nothing. | Spawn primitive, suspended child, and kill on a failed exec (P3, P6, P11). |
| M3 | Not started | Nothing. | Suspended initial thread and resume. |
| M4 | Host part done | `ObInheritHandleTable` in `handle.c`: copies OBJ_INHERIT entries at the same indices with the same access. All or nothing, with undo on failure. `ObInheritHandles` wraps it. Host tests: `tests/kernel-host/handle_inherit_test.c`. | No kernel path calls it (P2). VM test. |
| M5 | Partial | `src/kernel/nt/proc_create_args.h`: probe and copy-in of PS_CREATE_INFO and PS_ATTRIBUTE_LIST with size checks and a double-fetch check. Both kernel-branch `NtCreateUserProcess` stubs call it, then return `STATUS_NOT_IMPLEMENTED`. Host tests: `tests/kernel-host/proc_create_args_test.c`. | The wrapper over M2 to M4. `CreateProcessW` calling it. The file-handle source (M6). ProcessParameters is not checked by the stub. |
| M6 | Partial (userspace build) | `SEC_IMAGE` sections in the userspace build: the planner `image_plan.h`, relocations `image_reloc.h`, and `nt_map_image_view` in `mm.c`. Tests: `image_plan_test.c`, `image_reloc_test.c`, `mm_test.c`. | The kernel-side `SEC_IMAGE` mapping (the kernel build returns `STATUS_NOT_IMPLEMENTED`, `mm.c:1893`). A file-handle image source in `NtCreateUserProcess`. VM test. |
| M7 | Not started | Nothing. | Removal of the shim's dummy-address path. |

## 7. Prerequisites in this repo

Each prerequisite lists the findings or decisions that already cover it.

- **P1. File-backed and `SEC_IMAGE` sections.** File-backed data sections
  (`SEC_COMMIT` with a `FileHandle`) are built (`mm.c:1817`), with access checks,
  size rules, and write-back on the host shim. `SEC_IMAGE` sections are built
  in the userspace build (planned by `src/kernel/nt/image_plan.h`, mapped whole
  with per-segment protections). A relocatable image mapped away from its
  preferred base gets its fixups applied (`image_reloc.h`). The kernel build
  still refuses `SEC_IMAGE`. See [memory.md](memory.md) and
  [mm.md](../modules/mm.md). Related: [0004](../decisions/0004-section-object-lifetime.md)
  (section object lifetime, open), [0011](../decisions/0011-kernel-wx-sections.md)
  (sections mapped writable and executable, blocked), and
  [0021](../decisions/0021-arch-section-handle-pointer.md) (section handle is a
  kernel pointer, open).
- **P2. Handle inheritance.** `ObInheritHandles` exists (`handle.c:574`), but
  nothing in the kernel build calls it. The shim calls it unconditionally. M4 added
  `ObInheritHandleTable`, an all-or-nothing copy with host tests. No kernel path calls it yet.
  Handle values are per task and the table is not shown to be reset on exec. See
  [handle.md](../modules/handle.md), [0003](../decisions/0003-handle-slot-reuse.md)
  (slot reuse, one option fixed), [0007](../decisions/0007-file-handle-ownership.md)
  (file handle table has no owner, open), and
  [0022](../decisions/0022-arch-global-file-handle-table.md) (global file handle
  table, open). A child must not see a handle the parent did not mark for
  inheritance.
- **P3. One address space and VAD table per process.** Partly done (ADR 0074).
  Each `nt_eprocess` owns a `struct nt_mm` (created on first use). The memory
  services resolve `ProcessHandle` through the object manager with access checks,
  and cross-process reads and writes copy between spaces. In the userspace shim a
  foreign space is a host arena. The kernel build still has one real mm, bound to
  the current task, and returns `STATUS_NOT_SUPPORTED` for a foreign process. A
  child task needs its own real address space there (a per-process `mm_struct`)
  before a loader can map into it. See [memory.md](memory.md),
  [mm.md](../modules/mm.md) and [0074](../decisions/0074-per-process-address-space.md).
  Related: [0020](../decisions/0020-arch-global-vad-table.md) (one global VAD table,
  open) and [0005](../decisions/0005-vad-commit-rollback.md) (VAD rollback, blocked).
- **P4. Process and thread access checks.** `NtOpenProcess` and `NtOpenThread`
  do not check access, and a zero `DesiredAccess` grants `0x1FFFFF`. A creator
  would hand out handles with these rights. Related:
  [0002](../decisions/0002-process-open-access-check.md) and
  [0025](../decisions/0025-arch-process-thread-open-access.md), both open.
- **P5. TEB location and disclosure.** The TEB is kernel heap memory used as the
  GS base (see [0026](../decisions/0026-arch-teb-kernel-heap.md), open), and
  `NtQueryInformationThread` returns the TEB address
  ([0008](../decisions/0008-teb-address-disclosure.md), open). The child's TEB
  must be in user memory before it can be exposed to a suspended child.
- **P6. Process identity at creation.** `nt_current_eprocess` creates the
  eprocess lazily from `tgid` (`src/kernel/nt/object.c:1072-1084`). The child's
  eprocess must exist before the child runs, and it must carry the parent link
  and the parameter block. See the module note in [proc_thread.md](../modules/proc_thread.md).
- **P7. Parameter block in `binfmt_pe`.** `pe_build_process_parameters`
  (`binfmt_pe.c:562`) produces an empty page. It needs fields and the
  block reader from M1. No prior decision covers this. The M1 block format and
  validator now exist in `process_params.h` and are host-tested. `binfmt_pe` does not use them yet.
- **P8. Fork policy.** Research-2 decides that a PE task's `fork`, `vfork` and
  `clone` return `-ENOSYS`. The code does not do this. Option A and option C both
  add a spawn exception, so the policy must be written down first. No ADR
  covers it yet.
- **P9. Loader correctness for a child that runs.** DllMain and TLS callbacks
  are never called ([0031](../decisions/0031-loader-dllmain-tls-order.md), open),
  and the MinGW build compiles only loader stubs
  ([0029](../decisions/0029-loader-mingw-build-selection.md), open). A child
  that depends on DllMain cannot be tested end to end until 0031 is decided.
- **P10. Test helper.** `binfmt_pe_test.c` calls helpers that do not exist
  ([0028](../decisions/0028-binfmt-pe-test-missing-helpers.md), open). Adding
  M1 tests to that file is blocked until 0028 is decided.
- **P11. Post-commit failure.** A child that fails after exec must be killed,
  not returned as an error. [0012](../decisions/0012-kernel-post-commit-exec-failure.md)
  (fixed) is the pattern. Related: [0014](../decisions/0014-kernel-ntmm-add-region-results.md)
  (fixed) and [0016](../decisions/0016-kernel-clear-user-return.md) (fixed).
- **P12. Documentation drift.** [proc_thread.md](../modules/proc_thread.md)
  cites `proc_thread.c:1379-1381` for the kernel stubs. In `arch/x86/nt/proc_thread.c` they are at `1352-1354`, and in
  `src/kernel/nt/proc_thread.c` at `1416-1418`. The line numbers should be
  corrected when those files are next edited.

## 8. Test plan

- **Host, existing.** `src/kernel/nt/proc_thread_test.c` (`test_process_exit`)
  runs `NtCreateProcessEx` on the `kshim`. Keep it and add status assertions for the
  kernel build.
- **Host, new (with M1).** Encode and decode the parameter block, with
  truncated, oversized and misaligned inputs. These are cases the parser must
  reject.
- **Host, new (with M4).** Handle inheritance: only handles with
  `HANDLE_FLAG_INHERIT` are copied, and the values are the same in the child.
- **VM, spawn (M2 and M3).** A parent PE spawns a child PE and waits for it. The
  test checks the exit code, that the child is blocked before resume, and that
  a failed child exec kills the child.
- **VM, negative (M5 and M6).** An invalid image source, a missing path, and a
  file handle before P1 is done each return a documented status.
- **Regression.** `tools/check_syscall_tables.py` must still agree on the SSN
  table after any new service number.

## 9. Open questions

- Does `execve` reset the NT handle table of an existing task? If not, option A
  inherits too much. `src/kernel/nt/dispatch.c:401-405` destroys it on exit only.
- Which Linux syscall number, or NT service number, should the spawn helper
  use? The NT table is in `arch/x86/nt/nt_service_list.h`.
- Should a suspended child exist as a Linux task before its thread is resumed?
  Option C says yes. That makes the child visible to `ps` and to `wait`.
- Who owns the decision on the fork policy (P8)? It is a policy, not a code
  fix.

## Not verified

- The VM run of any new spawn path. Nothing here was built or booted. Every
  claim is from reading the source.
- The kernel `copy_process` call in `NtCreateThreadEx` (`proc_thread.c:1257-1265`)
  was read, not run.
- Whether the shim's dummy thread entry in `NtCreateUserProcess` is ever reached
  by a test. It appears to be used only by the shim tests.
