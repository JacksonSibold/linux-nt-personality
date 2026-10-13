# Module: security (NT security model)

Sources: `src/kernel/nt/security.c`, `src/kernel/nt/security.h`
Related: `object.md` (security descriptor slot in object headers, the token object type, and the process and thread token slots), `registry.md` (Machine-hive write rule is path-based, not SD-based), `io.md` (file share checks), `security_test.c` and `security_extra_test.c` (tests), `debugfs_tests.c` (se_access_check checks).

## Purpose

Implements the NT security data model for the personality: SIDs, access tokens, ACLs and ACEs, self-relative and absolute security descriptors, generic-mapping, DACL inheritance and `SeAccessCheck`-style evaluation. The header states a hard isolation rule: NT tokens grant rights over NT objects only. The module does not read or write Linux credentials, uid/gid, capabilities or LSM state, and no NT privilege or admin SID maps to a POSIX identity.

## Public API (security.h)

SIDs:

- `PSID sid_create(const UCHAR *identifier_authority, UCHAR sub_authority_count, const ULONG *sub_authorities)` - build a SID.
- `void sid_free(PSID)`, `ULONG sid_length(PCSID)`, `BOOLEAN sid_valid(PCSID)`, `BOOLEAN sid_equal(PCSID, PCSID)`, `BOOLEAN sid_prefix_equal(PCSID prefix, PCSID sid)`.
- `NTSTATUS sid_to_string(PCSID, char *buf, size_t buf_size)` and `NTSTATUS sid_from_string(const char *str, PSID *out_sid)`.
- `PSID sid_well_known(enum ntsec_well_known_sid wk)` - well-known SIDs such as the medium integrity level used in token creation.
- `ULONG sid_get_last_subauthority(PCSID)`.

ACLs and ACEs:

- `PACL acl_create(USHORT max_size)` / `void acl_free(PACL)`.
- `acl_add_allowed_ace`, `acl_add_denied_ace` - add a simple ACE `(acl, mask, flags, sid, ULONG *ace_index)`.
- `acl_add_allowed_object_ace`, `acl_add_denied_object_ace`, `acl_add_allowed_callback_ace`, `acl_add_denied_callback_ace`, `acl_add_allowed_callback_object_ace`, `acl_add_denied_callback_object_ace`, `acl_add_mandatory_label_ace` - the object, callback and mandatory-label variants.
- `NTSTATUS acl_get_ace(const PACL acl, ULONG index, void **ace)` and `BOOLEAN acl_is_canonical(const PACL acl)`.

Long prototypes are abbreviated with `...` below; the header has the full parameter lists.

Security descriptors:

- `SECURITY_DESCRIPTOR *sd_create_absolute(PSID owner, PSID group, PACL dacl, PACL sacl, USHORT control)`.
- `void sd_free(SECURITY_DESCRIPTOR *)`.
- `NTSTATUS sd_to_self_relative(const SECURITY_DESCRIPTOR *sd, void **sr_buffer, ULONG *sr_length)` and `NTSTATUS sd_from_self_relative(const void *sr_buffer, ULONG sr_length, SECURITY_DESCRIPTOR **sd)`.
- `ULONG sd_length(const SECURITY_DESCRIPTOR *sd)`.

Tokens:

- `ACCESS_TOKEN *token_create_primary(PSID user, ... groups, privileges, owner, primary_group, default_dacl, integrity_level, session_id)` - build a primary token.
- `ACCESS_TOKEN *token_duplicate(const ACCESS_TOKEN *src)`.
- `void token_reference(ACCESS_TOKEN *)` and `void token_dereference(ACCESS_TOKEN *)`.
- `NTSTATUS token_add_group(ACCESS_TOKEN *, PSID sid, ULONG attributes)` and `token_add_restricted_sid(...)`.
- `NTSTATUS token_adjust_privileges(ACCESS_TOKEN *, BOOLEAN disable_all, const LUID *privileges, ULONG count, ...)`.
- `BOOLEAN token_has_privilege(const ACCESS_TOKEN *, const LUID *luid, ...)`, `BOOLEAN token_sid_enabled(const ACCESS_TOKEN *, PCSID sid)`, `BOOLEAN token_sid_restricted(const ACCESS_TOKEN *, PCSID sid)`.

Access check and inheritance:

- `NTSTATUS se_access_check(const SECURITY_DESCRIPTOR *sd, const ACCESS_TOKEN *token, ACCESS_MASK desired_access, const GENERIC_MAPPING *generic_mapping, enum ntsec_object_class object_class, ACCESS_MASK *granted_access)` - evaluate a DACL against a token and return the granted mask.
- `NTSTATUS se_inherit_dacl(const PACL parent_dacl, BOOLEAN is_container, const GENERIC_MAPPING *generic_mapping, ...)` - compute a child DACL from a parent.

## Token object and token slots

- The token is an object-manager type. `nt_token_type` is named `"Token"` (`object.c:202`) and is registered in `object.c:1340`. Its body is `struct nt_token_body`, which holds one `ACCESS_TOKEN` reference (`object.h:116`). The object is created with `ObCreateObject` (`security.c:1022`), and `token_delete` drops the reference (`object.c:193`).
- Each process holds a primary token in `primary_token` (`object.h:281`). It is created with `token_create_default_primary` (`security.c:1034`, called from `object.c:1175`) and released in `ObDestroyProcessObject` (`object.c:1250`).
- Each thread has an `impersonation_token` slot (`object.h:318`). It is NULL unless the thread impersonates, and it is released in `object.c:1273`.
- `token_reference` and `token_dereference` use `__atomic` operations on `ref_count` (`security.c:1123`, `1128`).
- Token open: `NtOpenProcessToken`, `NtOpenProcessTokenEx`, `NtOpenThreadToken` and `NtOpenThreadTokenEx` (`security.c:1931-1951`) go through `ntsec_open_process_token` and `ntsec_open_thread_token`. Both reach `ntsec_open_token_for` (`security.c:1858`), which calls `ntsec_check_token_object_access` (`security.c:1826`) and then `se_access_check`. The caller's token is the primary token, or the thread's impersonation token when one is set (`security.c:1865-1873`).

## Services

Token and security services defined in `security.c` (from the inventory in `docs/status/SERVICES.md`):

- Implemented: `NtOpenProcessToken`, `NtOpenProcessTokenEx`, `NtOpenThreadToken`, `NtOpenThreadTokenEx`, `NtQueryInformationToken` (`security.c:2220`), `NtDuplicateToken` (`:2351`), `NtAdjustPrivilegesToken` (`:2586`), `NtPrivilegeCheck` (`:2596`), `NtAccessCheck` (`:2605`).
- Not defined (dispatch slot is NULL): `NtSetInformationToken`, `NtSetSecurityObject`, `NtQuerySecurityObject`, `NtCloseObjectAuditAlarm`, `NtAccessCheckAndAuditAlarm`.
Security services (`security.c`, service entry points `nt_svc_*`):

- `NtQuerySecurityObject(Handle, SecurityInformation, SecurityDescriptor, Length, LengthNeeded)` - copies the OWNER, GROUP, DACL and SACL parts selected by `SecurityInformation` into a self-relative buffer. Needs READ_CONTROL on the handle and a token check on the object descriptor (SACL also needs ACCESS_SYSTEM_SECURITY). A short buffer returns `STATUS_BUFFER_TOO_SMALL` with the required length. An object with no descriptor returns an empty header.
- `NtSetSecurityObject(Handle, SecurityInformation, SecurityDescriptor)` - replaces the selected parts and keeps the others. Needs WRITE_OWNER (owner, group), WRITE_DAC (DACL) or ACCESS_SYSTEM_SECURITY (SACL) on the handle and in the caller effective token. The new owner or primary group must be a SID the token holds. An object with no descriptor cannot be changed through this call.
- `NtSetInformationToken(TokenHandle, Class, Information, Length)` - classes `TokenOwner`, `TokenPrimaryGroup` and `TokenDefaultDacl`, under TOKEN_ADJUST_DEFAULT. The token keeps its own copies. A default DACL must be canonical (DENY before ALLOW), or it is `STATUS_INVALID_ACL`. Other classes return `STATUS_INVALID_INFO_CLASS`.
- `NtAccessCheckAndAuditAlarm(...)` - access check of the descriptor against the caller effective token (impersonation token first, else primary), through `se_access_check`. Audit is a documented no-op: no audit record is generated, `GenerateOnClose` is always FALSE, and the name strings and `ObjectCreation` are accepted and ignored.

The object owner has READ_CONTROL and WRITE_DAC implicitly, as in NT. WRITE_OWNER and ACCESS_SYSTEM_SECURITY are not implicit.

## Key data structures

- `struct _SID`, `struct _GUID`, `struct _LUID`, `struct _GENERIC_MAPPING`, `struct _ACL`, `struct _ACE_HEADER`, plus typed ACE structures (`ACCESS_ALLOWED_ACE`, `ACCESS_DENIED_ACE`, object ACEs, callback ACEs).
- `struct _SECURITY_DESCRIPTOR` (`security.h:364`) and `struct _ACCESS_TOKEN` (`security.h:463`), which holds groups, restricted SIDs, privileges, owner, primary group, default DACL, integrity level, session id and a `parent` link.
- `enum ntsec_object_class` (`security.h:483`) selects the object-class-specific rights handling in `se_access_check`.
- `enum ntsec_well_known_sid` (`security.h:172`).
- A file-scope capacity table in `security.c` (`g_acl_caps`, `ACL_CAP_MAX` = 256) records the allocated size of ACLs made with `acl_create`.

## Invariants and locking (as implemented)

- Isolation rule (`security.h` header): tokens live in the NT personality, separate from Linux `cred`. `se_access_check` is intended only for NT object create and open paths, and a PE open must pass both the NT check and Linux DAC/LSM checks.
- `acl_capacity_lookup` falls back to `AclSize` when an ACL was not registered, which covers imported ACLs.
- `security.c` has no spinlock or mutex calls. Its only atomics are the two token reference-count operations (`security.c:1123`, `1128`). Other shared structures are not synchronised here.

## Known gaps (evident in code)

- `se_access_check`, `token_create_primary`, `sd_create_absolute` and `se_inherit_dacl` are called only from inside `security.c`, which uses them for the token-open path (`security.c:1851`) and an access-check helper behind `NtAccessCheck` (`security.c:2564`). No call site exists in `object.c`, `handle.c`, `registry.c`, `io.c` or `mm.c`, so the enforcement the header describes ("called ONLY on NT object create/open paths") is not wired there. `debugfs_tests.c` mentions the fail-closed check only in a comment.
- Process open (`NtOpenProcess`, `proc_thread.c:629`) does not reach this check, and no process or thread open path performs an access check (see `proc_thread.md` and decision record 0002).
- The ACL capacity side table holds 256 entries (`ACL_CAP_MAX`, `security.c:45`). `acl_capacity_register` (`security.c:62`) silently drops registrations beyond that, so the lookup falls back to `AclSize` for those ACLs.
- The module is unsynchronised apart from the token reference count. Concurrent use of ACLs, SIDs or the capacity table is unsafe.
- Security descriptors are stored in the object header (`object.h:130`, set from the object attributes at `object.c:521`). No service reads or sets them, because `NtQuerySecurityObject` and `NtSetSecurityObject` are missing.
- `se_access_check` is called by the access-check and security-descriptor services above, and by `ntsec_object_access`. Object create and open paths in `object.c`, `handle.c`, `registry.c`, `io.c` and `mm.c` do not yet apply a descriptor. `token_create_primary`, `sd_create_absolute` and `se_inherit_dacl` are still used only in `security.c`, the test files and token creation.
- The ACL capacity side table holds 256 entries. `acl_capacity_register` (`security.c:57-60`) silently drops registrations beyond that, so the lookup falls back to `AclSize` for those ACLs.
- The module is unsynchronised (see above). Concurrent use of tokens, ACLs or the capacity table is unsafe.
