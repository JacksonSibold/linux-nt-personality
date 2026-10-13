/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ntpers - NT security model (ntsec workstream)
 *
 * Kernel-resident implementation of SIDs, access tokens, security descriptors,
 * ACLs/ACEs and SeAccessCheck for the Windows personality.
 *
 * Linux-safety rule (ARCH + research-4) — NON-NEGOTIABLE:
 *   NT tokens express rights over NT objects ONLY.  This module deliberately
 *   does NOT touch struct cred, uid/gid, capabilities, LSM hooks, or any
 *   POSIX permission path.  There is NO code path that converts an NT
 *   privilege (e.g. SeDebugPrivilege, SeTakeOwnershipPrivilege) into a POSIX
 *   capability, nor any path that maps an NT admin SID (S-1-5-32-544, S-1-5-18)
 *   into uid 0 or CAP_SYS_ADMIN.
 *
 *   How isolation is enforced:
 *     1) Storage: ACCESS_TOKEN lives in the NT personality's task->win_token
 *        (or as a handle-referenced object), completely separate from
 *        task->cred.  Linux cred is never read or written by this code.
 *     2) Enforcement scope: se_access_check() is called ONLY on NT object
 *        create/open paths (process, thread, file, section, key, named objects
 *        via ObCreateObject/ObOpenObjectByName).  Linux VFS, binder, net
 *        and other subsystems never consult it.
 *     3) Bridging rule for shared files: a PE open must pass BOTH the NT
 *        SeAccessCheck AND the normal Linux DAC/LSM check (additive deny);
 *        a Linux open never consults the NT SD.  This matches Samba's
 *        security.nt.sd xattr model and prevents privilege escalation.
 *
 *   PE processes keep the POSIX identity of the launching user; all NT rights
 *   live entirely inside the personality and cannot widen any Linux decision.
 *
 * The public API below is used by:
 *   - other NT personality subsystems (process/thread/file/section objects)
 *   - kernel-in-kernel unit tests compiled against kshim.h
 */

#ifndef _NTSEC_SECURITY_H
#define _NTSEC_SECURITY_H

#ifdef __KERNEL__
#include <linux/types.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/stddef.h>
#else
#include "kshim.h"
#endif

/* -------------------------------------------------------------------------- */
/* NT base scalar types (must match nt_types.h when both are included)        */
/* -------------------------------------------------------------------------- */

typedef u8  UCHAR;
typedef u16 USHORT;
typedef u32 ULONG;
typedef s32 LONG;
typedef u64 ULONGLONG;
typedef void *HANDLE;
typedef ULONG ACCESS_MASK;
typedef LONG  NTSTATUS;
typedef UCHAR BOOLEAN;

#ifndef FALSE
#define FALSE 0
#endif
#ifndef TRUE
#define TRUE  1
#endif

#define NT_SUCCESS(Status) (((NTSTATUS)(Status)) >= 0)

/* -------------------------------------------------------------------------- */
/* NTSTATUS codes used by this module                                         */
/* -------------------------------------------------------------------------- */

#define STATUS_SUCCESS                  ((NTSTATUS)0x00000000L)
#define STATUS_ACCESS_DENIED            ((NTSTATUS)0xC0000022L)
#define STATUS_BUFFER_TOO_SMALL         ((NTSTATUS)0xC0000023L)
#define STATUS_INVALID_PARAMETER        ((NTSTATUS)0xC000000DL)
#define STATUS_NO_MEMORY                ((NTSTATUS)0xC0000017L)
#define STATUS_INVALID_SID              ((NTSTATUS)0xC0000078L)
#define STATUS_INVALID_ACL              ((NTSTATUS)0xC0000077L)
#define STATUS_INVALID_SECURITY_DESCR   ((NTSTATUS)0xC0000079L)
#define STATUS_NOT_IMPLEMENTED          ((NTSTATUS)0xC0000002L)
#define STATUS_GENERIC_NOT_MAPPED       ((NTSTATUS)0xC00000E6L)
#define STATUS_NOT_ALL_ASSIGNED         ((NTSTATUS)0x00000106L)
#define STATUS_PRIVILEGE_NOT_HELD       ((NTSTATUS)0xC0000061L)
#define STATUS_NO_SUCH_PRIVILEGE        ((NTSTATUS)0xC0000060L)

/* -------------------------------------------------------------------------- */
/* Access-mask bits                                                           */
/* -------------------------------------------------------------------------- */

#define DELETE                      0x00010000
#define READ_CONTROL                0x00020000
#define WRITE_DAC                   0x00040000
#define WRITE_OWNER                 0x00080000
#define SYNCHRONIZE                 0x00100000
#define STANDARD_RIGHTS_REQUIRED    0x000F0000
#define STANDARD_RIGHTS_READ        (READ_CONTROL)
#define STANDARD_RIGHTS_WRITE       (READ_CONTROL)
#define STANDARD_RIGHTS_EXECUTE     (READ_CONTROL)
#define STANDARD_RIGHTS_ALL         0x001F0000
#define SPECIFIC_RIGHTS_ALL         0x0000FFFF

#define ACCESS_SYSTEM_SECURITY      0x01000000

/* SECURITY_INFORMATION classes for NtQuerySecurityObject / NtSetSecurityObject. */
#define OWNER_SECURITY_INFORMATION      0x00000001UL
#define GROUP_SECURITY_INFORMATION      0x00000002UL
#define DACL_SECURITY_INFORMATION       0x00000004UL
#define SACL_SECURITY_INFORMATION       0x00000008UL
#define LABEL_SECURITY_INFORMATION      0x00000010UL
#define MAXIMUM_ALLOWED             0x02000000

#define GENERIC_READ                0x80000000
#define GENERIC_WRITE               0x40000000
#define GENERIC_EXECUTE             0x20000000
#define GENERIC_ALL                 0x10000000

/* -------------------------------------------------------------------------- */
/* SID                                                                        */
/* -------------------------------------------------------------------------- */

#define SID_REVISION                1
#define SID_MAX_SUB_AUTHORITIES     15

/* Identifier authorities (big-endian 48-bit) */
#define SECURITY_NULL_SID_AUTHORITY        {0,0,0,0,0,0}
#define SECURITY_WORLD_SID_AUTHORITY       {0,0,0,0,0,1}
#define SECURITY_LOCAL_SID_AUTHORITY       {0,0,0,0,0,2}
#define SECURITY_CREATOR_SID_AUTHORITY     {0,0,0,0,0,3}
#define SECURITY_NON_UNIQUE_AUTHORITY      {0,0,0,0,0,4}
#define SECURITY_NT_SID_AUTHORITY          {0,0,0,0,0,5}
#define SECURITY_MANDATORY_LABEL_AUTHORITY {0,0,0,0,0,16}
#define SECURITY_APP_PACKAGE_AUTHORITY     {0,0,0,0,0,15}

/* Relative identifiers */
#define SECURITY_NULL_RID                   0
#define SECURITY_WORLD_RID                  0
#define SECURITY_LOCAL_RID                  0
#define SECURITY_CREATOR_OWNER_RID          0
#define SECURITY_CREATOR_GROUP_RID          1
#define SECURITY_DIALUP_RID                 1
#define SECURITY_NETWORK_RID                2
#define SECURITY_BATCH_RID                  3
#define SECURITY_INTERACTIVE_RID            4
#define SECURITY_SERVICE_RID                6
#define SECURITY_ANONYMOUS_LOGON_RID        7
#define SECURITY_PROXY_RID                  8
#define SECURITY_ENTERPRISE_CONTROLLERS_RID 9
#define SECURITY_PRINCIPAL_SELF_RID         10
#define SECURITY_AUTHENTICATED_USER_RID     11
#define SECURITY_RESTRICTED_CODE_RID        12
#define SECURITY_TERMINAL_SERVER_RID        13
#define SECURITY_REMOTE_LOGON_RID           14
#define SECURITY_THIS_ORGANIZATION_RID      15
#define SECURITY_IUSER_RID                  17
#define SECURITY_LOCAL_SYSTEM_RID           18
#define SECURITY_LOCAL_SERVICE_RID          19
#define SECURITY_NETWORK_SERVICE_RID        20
#define SECURITY_BUILTIN_DOMAIN_RID         32
#define SECURITY_DOMAIN_ALIAS_RID_ADMINS    544
#define SECURITY_DOMAIN_ALIAS_RID_USERS     545

#define SECURITY_MANDATORY_UNTRUSTED_RID    0
#define SECURITY_MANDATORY_LOW_RID          0x00001000
#define SECURITY_MANDATORY_MEDIUM_RID       0x00002000
#define SECURITY_MANDATORY_HIGH_RID         0x00003000
#define SECURITY_MANDATORY_SYSTEM_RID       0x00004000

#define SYSTEM_MANDATORY_LABEL_NO_WRITE_UP   0x00000001
#define SYSTEM_MANDATORY_LABEL_NO_READ_UP    0x00000002
#define SYSTEM_MANDATORY_LABEL_NO_EXECUTE_UP 0x00000004

/* Well-known SID selector for sid_well_known() */
enum ntsec_well_known_sid {
    SID_WK_NULL,
    SID_WK_EVERYONE,
    SID_WK_CREATOR_OWNER,
    SID_WK_CREATOR_GROUP,
    SID_WK_AUTHENTICATED_USERS,
    SID_WK_LOCAL_SYSTEM,
    SID_WK_LOCAL_SERVICE,
    SID_WK_NETWORK_SERVICE,
    SID_WK_ADMINISTRATORS,
    SID_WK_USERS,
    SID_WK_IL_UNTRUSTED,
    SID_WK_IL_LOW,
    SID_WK_IL_MEDIUM,
    SID_WK_IL_HIGH,
    SID_WK_IL_SYSTEM,
};

typedef struct _SID {
    UCHAR  Revision;
    UCHAR  SubAuthorityCount;
    UCHAR  IdentifierAuthority[6];
    ULONG  SubAuthority[1];        /* variable */
} SID, *PSID;

typedef const SID *PCSID;

/* -------------------------------------------------------------------------- */
/* GUID / LUID                                                                */
/* -------------------------------------------------------------------------- */

#ifndef _NT_GUID_DEFINED
#define _NT_GUID_DEFINED
typedef struct _GUID {
    ULONG  Data1;
    USHORT Data2;
    USHORT Data3;
    UCHAR  Data4[8];
} GUID;
#endif

typedef struct _LUID {
    ULONG LowPart;
    LONG  HighPart;
} LUID, *PLUID;

/* -------------------------------------------------------------------------- */
/* Generic mapping                                                            */
/* -------------------------------------------------------------------------- */

#ifndef NT_GENERIC_MAPPING_DEFINED /* ntdll/nt_types.h may already define it */
#define NT_GENERIC_MAPPING_DEFINED 1
typedef struct _GENERIC_MAPPING {
    ACCESS_MASK GenericRead;
    ACCESS_MASK GenericWrite;
    ACCESS_MASK GenericExecute;
    ACCESS_MASK GenericAll;
} GENERIC_MAPPING;
#endif

/* -------------------------------------------------------------------------- */
/* ACL / ACE                                                                  */
/* -------------------------------------------------------------------------- */

#define ACL_REVISION     2
#define ACL_REVISION_DS  4

typedef struct _ACL {
    UCHAR  AclRevision;
    UCHAR  Sbz1;
    USHORT AclSize;
    USHORT AceCount;
    USHORT Sbz2;
} ACL, *PACL;

typedef struct _ACE_HEADER {
    UCHAR  AceType;
    UCHAR  AceFlags;
    USHORT AceSize;
} ACE_HEADER;

/* ACE types */
#define ACCESS_ALLOWED_ACE_TYPE                 0x0
#define ACCESS_DENIED_ACE_TYPE                  0x1
#define SYSTEM_AUDIT_ACE_TYPE                   0x2
#define ACCESS_ALLOWED_OBJECT_ACE_TYPE          0x5
#define ACCESS_DENIED_OBJECT_ACE_TYPE           0x6
#define ACCESS_ALLOWED_CALLBACK_ACE_TYPE        0x9
#define ACCESS_DENIED_CALLBACK_ACE_TYPE         0xA
#define ACCESS_ALLOWED_CALLBACK_OBJECT_ACE_TYPE 0xB
#define ACCESS_DENIED_CALLBACK_OBJECT_ACE_TYPE  0xC
#define SYSTEM_MANDATORY_LABEL_ACE_TYPE         0x11

/* ACE flags */
#define OBJECT_INHERIT_ACE          0x01
#define CONTAINER_INHERIT_ACE       0x02
#define NO_PROPAGATE_INHERIT_ACE    0x04
#define INHERIT_ONLY_ACE            0x08
#define INHERITED_ACE               0x10
#define SUCCESSFUL_ACCESS_ACE_FLAG  0x40
#define FAILED_ACCESS_ACE_FLAG      0x80

/* Object ACE flags */
#define ACE_OBJECT_TYPE_PRESENT             0x00000001
#define ACE_INHERITED_OBJECT_TYPE_PRESENT   0x00000002

typedef struct _ACCESS_ALLOWED_ACE {
    ACE_HEADER  Header;
    ACCESS_MASK Mask;
    ULONG       SidStart;
} ACCESS_ALLOWED_ACE;

typedef struct _ACCESS_DENIED_ACE {
    ACE_HEADER  Header;
    ACCESS_MASK Mask;
    ULONG       SidStart;
} ACCESS_DENIED_ACE;

typedef struct _ACCESS_ALLOWED_OBJECT_ACE {
    ACE_HEADER  Header;
    ACCESS_MASK Mask;
    ULONG       Flags;
    GUID        ObjectType;
    GUID        InheritedObjectType;
    ULONG       SidStart;
} ACCESS_ALLOWED_OBJECT_ACE;

typedef struct _ACCESS_DENIED_OBJECT_ACE {
    ACE_HEADER  Header;
    ACCESS_MASK Mask;
    ULONG       Flags;
    GUID        ObjectType;
    GUID        InheritedObjectType;
    ULONG       SidStart;
} ACCESS_DENIED_OBJECT_ACE;

typedef struct _ACCESS_ALLOWED_CALLBACK_ACE {
    ACE_HEADER  Header;
    ACCESS_MASK Mask;
    ULONG       SidStart;
    /* UCHAR ApplicationData[]; */
} ACCESS_ALLOWED_CALLBACK_ACE;

typedef struct _ACCESS_DENIED_CALLBACK_ACE {
    ACE_HEADER  Header;
    ACCESS_MASK Mask;
    ULONG       SidStart;
    /* UCHAR ApplicationData[]; */
} ACCESS_DENIED_CALLBACK_ACE;

typedef struct _ACCESS_ALLOWED_CALLBACK_OBJECT_ACE {
    ACE_HEADER  Header;
    ACCESS_MASK Mask;
    ULONG       Flags;
    GUID        ObjectType;
    GUID        InheritedObjectType;
    ULONG       SidStart;
    /* UCHAR ApplicationData[]; */
} ACCESS_ALLOWED_CALLBACK_OBJECT_ACE;

typedef struct _ACCESS_DENIED_CALLBACK_OBJECT_ACE {
    ACE_HEADER  Header;
    ACCESS_MASK Mask;
    ULONG       Flags;
    GUID        ObjectType;
    GUID        InheritedObjectType;
    ULONG       SidStart;
    /* UCHAR ApplicationData[]; */
} ACCESS_DENIED_CALLBACK_OBJECT_ACE;

typedef struct _SYSTEM_MANDATORY_LABEL_ACE {
    ACE_HEADER  Header;
    ACCESS_MASK Mask;           /* policy: NO_WRITE_UP / NO_READ_UP / NO_EXECUTE_UP */
    ULONG       SidStart;
} SYSTEM_MANDATORY_LABEL_ACE;

/* -------------------------------------------------------------------------- */
/* Security descriptor                                                        */
/* -------------------------------------------------------------------------- */

#define SE_OWNER_DEFAULTED              0x0001
#define SE_GROUP_DEFAULTED              0x0002
#define SE_DACL_PRESENT                 0x0004
#define SE_DACL_DEFAULTED               0x0008
#define SE_SACL_PRESENT                 0x0010
#define SE_SACL_DEFAULTED               0x0020
#define SE_DACL_AUTO_INHERIT_REQ        0x0100
#define SE_SACL_AUTO_INHERIT_REQ        0x0200
#define SE_DACL_AUTO_INHERITED          0x0400
#define SE_SACL_AUTO_INHERITED          0x0800
#define SE_DACL_PROTECTED               0x1000
#define SE_SACL_PROTECTED               0x2000
#define SE_RM_CONTROL_VALID             0x4000
#define SE_SELF_RELATIVE                0x8000

typedef struct _SECURITY_DESCRIPTOR {
    UCHAR  Revision;
    UCHAR  Sbz1;
    USHORT Control;
    PSID   Owner;
    PSID   Group;
    PACL   Sacl;
    PACL   Dacl;
} SECURITY_DESCRIPTOR;

typedef SECURITY_DESCRIPTOR *PSECURITY_DESCRIPTOR_INTERNAL;

/* Self-relative layout ( Revision + Sbz1 + Control + four offsets ) */
typedef struct _SECURITY_DESCRIPTOR_RELATIVE {
    UCHAR  Revision;
    UCHAR  Sbz1;
    USHORT Control;
    ULONG  Owner;
    ULONG  Group;
    ULONG  Sacl;
    ULONG  Dacl;
} SECURITY_DESCRIPTOR_RELATIVE;

/* -------------------------------------------------------------------------- */
/* Token                                                                      */
/* -------------------------------------------------------------------------- */

#define SE_GROUP_MANDATORY              0x00000001
#define SE_GROUP_ENABLED_BY_DEFAULT     0x00000002
#define SE_GROUP_ENABLED                0x00000004
#define SE_GROUP_OWNER                  0x00000008
#define SE_GROUP_USE_FOR_DENY_ONLY      0x00000010
#define SE_GROUP_LOGON_ID               0xC0000000
#define SE_GROUP_RESOURCE               0x20000000
#define SE_GROUP_INTEGRITY              0x00000020
#define SE_GROUP_INTEGRITY_ENABLED      0x00000040

#define SE_PRIVILEGE_ENABLED_BY_DEFAULT 0x00000001
#define SE_PRIVILEGE_ENABLED            0x00000002
#define SE_PRIVILEGE_REMOVED            0x00000004
#define SE_PRIVILEGE_USED_FOR_ACCESS    0x80000000

#define SE_CREATE_TOKEN_PRIVILEGE            2
#define SE_ASSIGNPRIMARYTOKEN_PRIVILEGE      3
#define SE_LOCK_MEMORY_PRIVILEGE             4
#define SE_INCREASE_QUOTA_PRIVILEGE          5
#define SE_MACHINE_ACCOUNT_PRIVILEGE         6
#define SE_TCB_PRIVILEGE                     7
#define SE_SECURITY_PRIVILEGE                8
#define SE_TAKE_OWNERSHIP_PRIVILEGE          9
#define SE_LOAD_DRIVER_PRIVILEGE            10
#define SE_SYSTEM_PROFILE_PRIVILEGE         11
#define SE_SYSTEMTIME_PRIVILEGE             12
#define SE_PROF_SINGLE_PROCESS_PRIVILEGE    13
#define SE_INC_BASE_PRIORITY_PRIVILEGE      14
#define SE_CREATE_PAGEFILE_PRIVILEGE        15
#define SE_CREATE_PERMANENT_PRIVILEGE       16
#define SE_BACKUP_PRIVILEGE                 17
#define SE_RESTORE_PRIVILEGE                18
#define SE_SHUTDOWN_PRIVILEGE               19
#define SE_DEBUG_PRIVILEGE                  20
#define SE_AUDIT_PRIVILEGE                  21
#define SE_SYSTEM_ENVIRONMENT_PRIVILEGE     22
#define SE_CHANGE_NOTIFY_PRIVILEGE          23
#define SE_REMOTE_SHUTDOWN_PRIVILEGE        24
#define SE_UNDOCK_PRIVILEGE                 25
#define SE_SYNC_AGENT_PRIVILEGE             26
#define SE_ENABLE_DELEGATION_PRIVILEGE      27
#define SE_MANAGE_VOLUME_PRIVILEGE          28
#define SE_IMPERSONATE_PRIVILEGE            29
#define SE_CREATE_GLOBAL_PRIVILEGE          30
#define SE_TRUSTED_CREDMAN_ACCESS_PRIVILEGE 31
#define SE_RELABEL_PRIVILEGE                32
#define SE_INC_WORKING_SET_PRIVILEGE        33
#define SE_TIME_ZONE_PRIVILEGE              34
#define SE_CREATE_SYMBOLIC_LINK_PRIVILEGE   35

typedef struct _SID_AND_ATTRIBUTES {
    PSID   Sid;
    ULONG  Attributes;
} SID_AND_ATTRIBUTES;

typedef struct _LUID_AND_ATTRIBUTES {
    LUID   Luid;
    ULONG  Attributes;
} LUID_AND_ATTRIBUTES;

typedef enum _TOKEN_TYPE {
    TokenPrimary = 1,
    TokenImpersonation
} TOKEN_TYPE;

typedef enum _SECURITY_IMPERSONATION_LEVEL {
    SecurityAnonymous,
    SecurityIdentification,
    SecurityImpersonation,
    SecurityDelegation
} SECURITY_IMPERSONATION_LEVEL;

typedef struct _ACCESS_TOKEN {
    LONG                       ref_count;
    TOKEN_TYPE                 token_type;
    SECURITY_IMPERSONATION_LEVEL impersonation_level;
    PSID                       user;
    ULONG                      group_count;
    SID_AND_ATTRIBUTES         *groups;
    ULONG                      privilege_count;
    LUID_AND_ATTRIBUTES        *privileges;
    PSID                       owner;
    PSID                       primary_group;
    PACL                       default_dacl;
    PSID                       integrity_level;     /* S-1-16-... label SID */
    ULONG                      restricted_sid_count;
    SID_AND_ATTRIBUTES         *restricted_sids;
    struct _ACCESS_TOKEN       *parent;
    ULONG                      session_id;
    LUID                       token_id;            /* from ntsec_next_token_id, never an address */
} ACCESS_TOKEN;

/* Object class hint for privilege overrides */
enum ntsec_object_class {
    NTSEC_OBJECT_GENERIC,
    NTSEC_OBJECT_PROCESS,
    NTSEC_OBJECT_FILE,
};

/* -------------------------------------------------------------------------- */
/* SID API                                                                    */
/* -------------------------------------------------------------------------- */

PSID     sid_create(const UCHAR *identifier_authority,
                    UCHAR sub_authority_count,
                    const ULONG *sub_authorities);
void     sid_free(PSID sid);
ULONG    sid_length(PCSID sid);
BOOLEAN  sid_valid(PCSID sid);
BOOLEAN  sid_equal(PCSID a, PCSID b);
BOOLEAN  sid_prefix_equal(PCSID prefix, PCSID sid); /* prefix is left arg */
NTSTATUS sid_to_string(PCSID sid, char *buf, size_t buf_size);
NTSTATUS sid_from_string(const char *str, PSID *out_sid);
PSID     sid_well_known(enum ntsec_well_known_sid wk);
ULONG    sid_get_last_subauthority(PCSID sid);

/* -------------------------------------------------------------------------- */
/* ACL / ACE API                                                              */
/* -------------------------------------------------------------------------- */

PACL     acl_create(USHORT max_size);
void     acl_free(PACL acl);
NTSTATUS acl_add_allowed_ace(PACL acl, ACCESS_MASK mask, UCHAR flags,
                             PSID sid, ULONG *ace_index);
NTSTATUS acl_add_denied_ace(PACL acl, ACCESS_MASK mask, UCHAR flags,
                            PSID sid, ULONG *ace_index);
NTSTATUS acl_add_allowed_object_ace(PACL acl, ACCESS_MASK mask, UCHAR flags,
                                    const GUID *object_type,
                                    const GUID *inherited_object_type,
                                    PSID sid, ULONG *ace_index);
NTSTATUS acl_add_denied_object_ace(PACL acl, ACCESS_MASK mask, UCHAR flags,
                                   const GUID *object_type,
                                   const GUID *inherited_object_type,
                                   PSID sid, ULONG *ace_index);
NTSTATUS acl_add_allowed_callback_ace(PACL acl, ACCESS_MASK mask, UCHAR flags,
                                      PSID sid,
                                      const void *callback_data,
                                      USHORT callback_size,
                                      ULONG *ace_index);
NTSTATUS acl_add_denied_callback_ace(PACL acl, ACCESS_MASK mask, UCHAR flags,
                                     PSID sid,
                                     const void *callback_data,
                                     USHORT callback_size,
                                     ULONG *ace_index);
NTSTATUS acl_add_allowed_callback_object_ace(PACL acl, ACCESS_MASK mask,
                                             UCHAR flags,
                                             const GUID *object_type,
                                             const GUID *inherited_object_type,
                                             PSID sid,
                                             const void *callback_data,
                                             USHORT callback_size,
                                             ULONG *ace_index);
NTSTATUS acl_add_denied_callback_object_ace(PACL acl, ACCESS_MASK mask,
                                            UCHAR flags,
                                            const GUID *object_type,
                                            const GUID *inherited_object_type,
                                            PSID sid,
                                            const void *callback_data,
                                            USHORT callback_size,
                                            ULONG *ace_index);
NTSTATUS acl_add_mandatory_label_ace(PACL acl, ACCESS_MASK policy_mask,
                                     UCHAR flags, PSID sid, ULONG *ace_index);
NTSTATUS acl_get_ace(const PACL acl, ULONG index, void **ace);
BOOLEAN  acl_is_canonical(const PACL acl);
USHORT   acl_used_size(const PACL acl);

/* -------------------------------------------------------------------------- */
/* Security-descriptor API                                                    */
/* -------------------------------------------------------------------------- */

SECURITY_DESCRIPTOR *sd_create_absolute(PSID owner, PSID group,
                                        PACL dacl, PACL sacl,
                                        USHORT control);
void     sd_free(SECURITY_DESCRIPTOR *sd);
NTSTATUS sd_to_self_relative(const SECURITY_DESCRIPTOR *sd,
                             void **sr_buffer, ULONG *sr_length);
NTSTATUS sd_from_self_relative(const void *sr_buffer, ULONG sr_length,
                               SECURITY_DESCRIPTOR **sd);
ULONG    sd_length(const SECURITY_DESCRIPTOR *sd);
/* Grows *total to cover [off, off + size); refuses a result past the 64 KiB descriptor limit. */
NTSTATUS ntsec_sr_extend(ULONG *total, ULONG off, ULONG size);

/* Token access rights (winnt.h) */
#define TOKEN_ASSIGN_PRIMARY        0x0001
#define TOKEN_DUPLICATE             0x0002
#define TOKEN_IMPERSONATE           0x0004
#define TOKEN_QUERY                 0x0008
#define TOKEN_QUERY_SOURCE          0x0010
#define TOKEN_ADJUST_PRIVILEGES     0x0020
#define TOKEN_ADJUST_GROUPS         0x0040
#define TOKEN_ADJUST_DEFAULT        0x0080
#define TOKEN_ADJUST_SESSIONID      0x0100
#define TOKEN_ALL_ACCESS            (STANDARD_RIGHTS_REQUIRED | 0x1FFUL)
#define TOKEN_READ                  (STANDARD_RIGHTS_READ | TOKEN_QUERY)
#define TOKEN_WRITE                 (STANDARD_RIGHTS_WRITE | TOKEN_ADJUST_PRIVILEGES | \
                                     TOKEN_ADJUST_GROUPS | TOKEN_ADJUST_DEFAULT)
#define TOKEN_EXECUTE               (STANDARD_RIGHTS_EXECUTE)

/* TOKEN_INFORMATION_CLASS values used by NtQueryInformationToken (winnt.h) */
typedef enum _TOKEN_INFORMATION_CLASS {
    TokenUser = 1,
    TokenGroups,
    TokenPrivileges,
    TokenOwner,
    TokenPrimaryGroup,
    TokenDefaultDacl,
    TokenSource,
    TokenType,
    TokenImpersonationLevel,
    TokenStatistics,
    TokenRestrictedSids,
    TokenSessionId,
    TokenGroupsAndPrivileges,
    TokenSessionReference,
    TokenSandBoxInert,
    TokenAuditPolicy,
    TokenOrigin,
    TokenElevationType,
    TokenLinkedToken,
    TokenElevation,
    TokenHasRestrictions,
    TokenAccessInformation,
    TokenVirtualizationAllowed,
    TokenVirtualizationEnabled,
    TokenIntegrityLevel = 25
} TOKEN_INFORMATION_CLASS;

typedef GENERIC_MAPPING *PGENERIC_MAPPING;
typedef ACCESS_MASK *PACCESS_MASK;

/* Privilege structures (winnt.h) */
typedef struct _TOKEN_PRIVILEGES {
    ULONG               PrivilegeCount;
    LUID_AND_ATTRIBUTES Privileges[1];
} TOKEN_PRIVILEGES, *PTOKEN_PRIVILEGES;

typedef struct _PRIVILEGE_SET {
    ULONG               PrivilegeCount;
    ULONG               Control;
    LUID_AND_ATTRIBUTES Privilege[1];
} PRIVILEGE_SET, *PPRIVILEGE_SET;

#define PRIVILEGE_SET_ALL_NECESSARY 1

/* -------------------------------------------------------------------------- */
/* Token API                                                                  */
/* -------------------------------------------------------------------------- */

ACCESS_TOKEN *token_create_primary(PSID user,
                                   const SID_AND_ATTRIBUTES *groups,
                                   ULONG group_count,
                                   const LUID_AND_ATTRIBUTES *privileges,
                                   ULONG privilege_count,
                                   PSID owner,
                                   PSID primary_group,
                                   PACL default_dacl,
                                   PSID integrity_level,
                                   ULONG session_id);
/* Default primary token for a new process: user S-1-5-21-0-0-0-1000, member
 * of the builtin Users group, no privileges, medium integrity. Caller owns it. */
ACCESS_TOKEN *token_create_default_primary(void);
ACCESS_TOKEN *token_duplicate(const ACCESS_TOKEN *src);
void         token_reference(ACCESS_TOKEN *token);
void         token_dereference(ACCESS_TOKEN *token);

NTSTATUS token_add_group(ACCESS_TOKEN *token, PSID sid, ULONG attributes);
NTSTATUS token_add_restricted_sid(ACCESS_TOKEN *token, PSID sid,
                                  ULONG attributes);
NTSTATUS token_adjust_privileges(ACCESS_TOKEN *token, BOOLEAN disable_all,
                                 const LUID *privileges, ULONG count,
                                 ULONG new_attributes);
BOOLEAN  token_has_privilege(const ACCESS_TOKEN *token, const LUID *luid,
                             BOOLEAN enabled_only);
BOOLEAN  token_sid_enabled(const ACCESS_TOKEN *token, PCSID sid);
BOOLEAN  token_sid_restricted(const ACCESS_TOKEN *token, PCSID sid);

/* Token that decides a request made by caller: the current thread's
 * impersonation token when it has one, else caller's primary token. NULL when
 * caller is NULL and the thread has no impersonation token. */
struct nt_eprocess;
const ACCESS_TOKEN *se_effective_token(struct nt_eprocess *caller);
/* True when the effective token of caller holds privilege (an SE_*_PRIVILEGE
 * value) enabled. */
BOOLEAN se_caller_has_privilege(struct nt_eprocess *caller, ULONG privilege);

/* -------------------------------------------------------------------------- */
/* Access check                                                               */
/* -------------------------------------------------------------------------- */

NTSTATUS se_access_check(const SECURITY_DESCRIPTOR *sd,
                         const ACCESS_TOKEN *token,
                         ACCESS_MASK desired_access,
                         const GENERIC_MAPPING *generic_mapping,
                         enum ntsec_object_class object_class,
                         ACCESS_MASK *granted_access);

/* -------------------------------------------------------------------------- */
/* DACL inheritance                                                           */
/* -------------------------------------------------------------------------- */

NTSTATUS se_inherit_dacl(const PACL parent_dacl,
                         BOOLEAN is_container,
                         const GENERIC_MAPPING *generic_mapping,
                         PACL *child_dacl);

#endif /* _NTSEC_SECURITY_H */
