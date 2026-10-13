# Module: security (NT security model)

Sources: `src/kernel/nt/security.c`, `src/kernel/nt/security.h`
Related: `object.md` (the security descriptor slot in object headers, the token object type, and the process and thread token slots), `registry.md` (the Machine-hive write rule is path-based, not descriptor-based), `io.md` (file share checks, separate from this module), `security_test.c` and `security_extra_test.c` (tests).

## Purpose

Implements the NT security data model for the personality: SIDs, access tokens, ACLs and ACEs, self-relative and absolute security descriptors, generic mapping, DACL inheritance, and `SeAccessCheck`-style evaluation. The header states a hard isolation rule: NT tokens grant rights over NT objects only. The module does not read or write Linux credentials, uid/gid, capabilities or LSM state.

## Public API (security.h)

SIDs, ACLs and ACEs: `sid_create`, `sid_free`, `sid_length`, `sid_valid`, `sid_equal`, `sid_prefix_equal`, `sid_to_string`, `sid_from_string`, `sid_well_known`; `acl_create`, `acl_free`, `acl_add_allowed_ace`, `acl_add_denied_ace`, the object, callback and mandatory-label ACE adders, `acl_get_ace` and `acl_is_canonical`.

Security descriptors: `sd_create_absolute`, `sd_free`, `sd_to_self_relative`, `sd_from_self_relative`, `sd_length`.

Tokens: `token_create_primary`, `token_duplicate`, `token_reference`, `token_dereference`, `token_add_group`, `token_add_restricted_sid`, `token_adjust_privileges`, `token_has_privilege`, `token_sid_enabled`, `token_sid_restricted`. `token_create_default_primary` (`security.c:1047`) builds the default primary token used for each new process.

Access check and inheritance: `se_access_check(sd, token, desired_access, generic_mapping, object_class, &granted)` (`security.c:1485`) evaluates a DACL against a token. `se_inherit_dacl` computes a child DACL from a parent. `se_caller_has_privilege` (`security.c:2648`) tests one privilege on a process's effective token; the process-information and object services use it.

## Token object and token slots

- The token is an object-manager type. `nt_token_type` is named `"Token"` (`object.c:205`) and is registered by `ObInitObjectManager`. Its body (`struct nt_token_body`, `object.h:145`) holds one `ACCESS_TOKEN` reference. Tokens are created with `ObCreateObject` in `security.c:1035`. `token_delete` drops the reference (`object.c:196`).
- Each process holds a primary token in `primary_token` (`object.h:363`). `ObCreateProcessObject` sets it with `token_create_default_primary` (`object.c:1395`). `ObDestroyProcessObject` releases it.
- Each thread has an `impersonation_token` slot (`object.h:406`). It is NULL unless the thread impersonates, and it is released with the thread.
- Token reference counts use `__atomic` operations on `ref_count` (`security.c:1137` and `:1142`).
- Token IDs. Each token has a LUID `token_id` from a counter (`ntsec_next_token_id`, `security.c:911`), set on create and on duplicate (`security.c:937`, `:1073`). `TokenStatistics` returns it. It is not a kernel address (audit pass 3 item 4, `222f64d`).

## Token open path

`NtOpenProcessToken`, `NtOpenProcessTokenEx`, `NtOpenThreadToken` and `NtOpenThreadTokenEx` go through `ntsec_open_process_token` (`security.c:1897`) and `ntsec_open_thread_token` (`:1919`), which reach `ntsec_open_token_for` (`:1872`). That calls `ntsec_check_token_object_access` (`:1840`) and then `se_access_check`. The checked token is the caller's primary token, or the thread's impersonation token when one is set.

## Services

Every service below has a typed definition in the userspace branch and a raw-argument definition in the kernel branch. Dispatch uses the raw form. Line numbers are the typed definition, then the raw one.

| Service | Typed / raw (`security.c`) | Notes |
|---|---|---|
| `NtOpenProcessToken`, `NtOpenProcessTokenEx`, `NtOpenThreadToken`, `NtOpenThreadTokenEx` | `1945`-`1965` / `1985`-`2019` | Token open path above. |
| `NtQueryInformationToken` | `2233` / `2256` | Classes via `ntsec_query_token` (`:2214`) and `ntsec_fill_token_info` (`:2089`). |
| `NtDuplicateToken` | `2364` / `2373` | `ntsec_duplicate_token` (`:2330`). |
| `NtAdjustPrivilegesToken` | `3032` / `3131` | `ntsec_adjust_core` (`:2416`). |
| `NtPrivilegeCheck` | `3042` / `3176` | `ntsec_privilege_check_core` (`:2485`). |
| `NtAccessCheck` | `3051` / `3207` | `ntsec_access_check_core` (`:2563`). |
| `NtQuerySecurityObject` | `3073` / `3258` | `ntsec_query_sd_core` (`:2725`). |
| `NtSetSecurityObject` | `3081` / `3287` | `ntsec_set_sd_core` (`:2787`). |
| `NtSetInformationToken` | `3091` / `3314` | `ntsec_set_token_core` (`:2916`). |
| `NtAccessCheckAndAuditAlarm` | `3102` / `3355` | Access check only; see below. |

`NtCloseObjectAuditAlarm` has no definition (dispatch slot NULL).

Details:

- `NtQuerySecurityObject` copies the OWNER, GROUP, DACL and SACL parts selected by `SecurityInformation` into a self-relative buffer. It needs READ_CONTROL on the handle, and the caller's effective token must pass the object descriptor's check. SACL also needs ACCESS_SYSTEM_SECURITY. A short buffer returns `STATUS_BUFFER_TOO_SMALL` with the required length. An object with no descriptor returns an empty header, because there is nothing to disclose.
- `NtSetSecurityObject` replaces the selected parts and keeps the others. It needs WRITE_OWNER (owner, group), WRITE_DAC (DACL) or ACCESS_SYSTEM_SECURITY (SACL) on the handle and in the caller's effective token. The new owner or primary group must be a SID the token holds. An object with no descriptor is refused with `STATUS_ACCESS_DENIED` (the code comment says it is unprotected). This blocks the owner from attaching a first descriptor; audit pass 4 item 4 asks the owner to confirm that is intended.
- `NtSetInformationToken` accepts `TokenOwner`, `TokenPrimaryGroup` and `TokenDefaultDacl` (`security.c:2925`-`2931`), under TOKEN_ADJUST_DEFAULT. The token keeps its own copies. A default DACL must be canonical (DENY before ALLOW), or the call returns `STATUS_INVALID_ACL`. Other classes return `STATUS_INVALID_INFO_CLASS`.
- `NtAccessCheckAndAuditAlarm` checks the descriptor against the caller's effective token (impersonation token first, else primary) through `se_access_check`. Auditing is a no-op: no audit record is generated, `GenerateOnClose` is always FALSE, and the name strings and `ObjectCreation` are accepted and ignored.
- The object owner has READ_CONTROL and WRITE_DAC implicitly, as in NT. WRITE_OWNER and ACCESS_SYSTEM_SECURITY are not implicit.

User buffers are read through `ntsec_rd_user` (`security.c:2403`, 17 call sites) rather than dereferenced. The self-relative length is summed in 64 bits (`ntsec_sr_extend`, `security.c:2518`, which refuses a total past the 64 KiB limit).

## Key data structures

- `struct _SID`, `struct _GUID`, `struct _LUID`, `struct _GENERIC_MAPPING`, `struct _ACL`, `struct _ACE_HEADER`, and the typed ACE structures.
- `struct _SECURITY_DESCRIPTOR` (`security.h`) and `struct _ACCESS_TOKEN` (`security.h`), which holds groups, restricted SIDs, privileges, owner, primary group, default DACL, integrity level, session id, the LUID `token_id`, and a `parent` link.
- `enum ntsec_object_class` selects the object-class rules in `se_access_check`.
- `enum ntsec_well_known_sid`.
- `g_acl_caps[ACL_CAP_MAX]` (`security.c:45`, `ACL_CAP_MAX` = 256): a side table of the allocated size of each ACL made with `acl_create`.

## Invariants and locking (as implemented)

- Isolation rule (`security.h`): tokens live in the NT personality, separate from Linux `cred`. `se_access_check` is for NT object create and open paths. A PE open must pass both the NT check and the Linux DAC and LSM checks.
- `acl_capacity_lookup` falls back to `AclSize` when an ACL was not registered, which covers imported ACLs.
- The module has no spinlock or mutex calls. Its only atomics are the two token reference-count operations and the token ID counter. Concurrent use of ACLs, SIDs or the capacity table is not synchronised.

## Known gaps (evident in code)

- **Descriptors are not applied at object creation or open.** `se_access_check` is called from the token-open path (`security.c:1865`), the `NtAccessCheck` core (`security.c:2588`), the object access helper `ntsec_object_access` (`:2678`) and the token access helper (`:3018`). The object, handle, registry, io and mm paths do not call it, so the header's rule ("called ONLY on NT object create/open paths") is not implemented there.
- Process open (`NtOpenProcess`, `proc_thread.c`) does not check access. See `proc_thread.md` and decision record 0002.
- `acl_capacity_register` silently drops registrations beyond the 256-entry side table. The lookup then falls back to `AclSize` for those ACLs (review kernel-nt, decision record 0035, blocked).
- `ntsec_sr_length` adds the `ULONG` offset and size after the 64-bit comparison, so a sum can wrap. `sd_from_self_relative` then rejects the descriptor, so the result is fail-closed (audit pass 4 item 11). The 64-bit total is kept by `ntsec_sr_extend` in the copy path.
- `ntsec_set_sd_core` has no owner decision on the no-descriptor case (see above).
- The `NtAccessCheckAndAuditAlarm` audit fields are ignored, so no audit trail exists.
- `TokenStatistics` returns the token LUID. Its fill pass in `ntsec_query_token` relies on the token being immutable after creation, so the measured and written lengths agree; the code has no separate bound.
