/*
 * object.h - NT object manager (kernel/nt/object.c).
 *
 * Provides object headers, object types, the global namespace, name lookup,
 * reference counting, and the link to the per-process handle table.
 *
 * Kernel-style C: fixed-width types, no VLAs, no alloca.
 */

#ifndef NTOBJECT_H
#define NTOBJECT_H

#include "kshim.h"
#include "../../ntdll/nt_types.h"
#include "../../ntdll/ntstatus.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/* Object attribute flags (from WDM/ntdef.h)                                  */
/* -------------------------------------------------------------------------- */

#define OBJ_INHERIT             0x00000002UL
#define OBJ_PERMANENT           0x00000010UL
#define OBJ_EXCLUSIVE           0x00000020UL
#define OBJ_CASE_INSENSITIVE    0x00000040UL
#define OBJ_OPENIF              0x00000080UL
#define OBJ_OPENLINK            0x00000100UL
#define OBJ_KERNEL_HANDLE       0x00000200UL
#define OBJ_FORCE_ACCESS_CHECK  0x00000400UL
#define OBJ_PROTECT_FROM_CLOSE  0x00002000UL
#define OBJ_DONT_RESOLVE_LINK   OBJ_OPENLINK
#define OBJ_VALID_ATTRIBUTES    (OBJ_INHERIT|OBJ_PERMANENT|OBJ_EXCLUSIVE|OBJ_CASE_INSENSITIVE|OBJ_OPENIF|OBJ_OPENLINK|OBJ_KERNEL_HANDLE|OBJ_FORCE_ACCESS_CHECK|OBJ_PROTECT_FROM_CLOSE)

/* OBJECT_DIRECTORY_INFORMATION (winternl.h): one record per entry, with the
 * strings stored after the record array (see NtQueryDirectoryObject). */
typedef struct _OBJECT_DIRECTORY_INFORMATION {
    UNICODE_STRING Name;
    UNICODE_STRING TypeName;
} OBJECT_DIRECTORY_INFORMATION, *POBJECT_DIRECTORY_INFORMATION;

/* Object directory access rights (winnt.h). Values are not in security.h, so
 * they are defined here for the namespace services in nt_obj.c. */
#define DIRECTORY_QUERY                 0x0001UL
#define DIRECTORY_TRAVERSE              0x0002UL
#define DIRECTORY_CREATE_OBJECT         0x0004UL
#define DIRECTORY_CREATE_SUBDIRECTORY   0x0008UL
#define DIRECTORY_ALL_ACCESS            0x000F000FUL

/* Symbolic link object access rights (winnt.h). */
#define SYMBOLIC_LINK_QUERY             0x0001UL
#define SYMBOLIC_LINK_ALL_ACCESS        0x000F0001UL

/* STATUS for a create that found an existing object and opened it (ntstatus.h). */
#ifndef STATUS_OBJECT_NAME_EXISTS
#define STATUS_OBJECT_NAME_EXISTS ((NTSTATUS)0x40000000L)
#endif

/* STATUS for a create that found an existing object and opened it. */
#ifndef STATUS_OBJECT_NAME_EXISTS
#define STATUS_OBJECT_NAME_EXISTS ((NTSTATUS)0x40000000L)
#endif

/* STATUS for protected handle close */
#ifndef STATUS_HANDLE_NOT_CLOSABLE
#define STATUS_HANDLE_NOT_CLOSABLE ((NTSTATUS)0xC0000235L)
#endif

/* -------------------------------------------------------------------------- */
/* Forward declarations                                                       */
/* -------------------------------------------------------------------------- */

struct nt_object_header;
struct nt_object_type;
struct nt_directory;
struct nt_eprocess;
struct nt_ethread;
struct nt_handle_table;
struct nt_job;
struct nt_debug_obj;

/* -------------------------------------------------------------------------- */
/* Object type callbacks                                                      */
/* -------------------------------------------------------------------------- */

/*
 * delete_obj is called when the last reference (PointerCount) is dropped.
 * It must perform type-specific teardown but must NOT free the header; the
 * object manager frees the contiguous header+body allocation afterwards.
 */
typedef void (*nt_object_delete_t)(struct nt_object_header *hdr);

/*
 * close is called each time a handle to the object is closed.
 */
typedef void (*nt_object_close_t)(struct nt_object_header *hdr, HANDLE handle);

/*
 * parse is invoked when a path still has components after reaching a
 * non-directory object.  Symbolic links and devices use this.
 */
typedef NTSTATUS (*nt_object_parse_t)(struct nt_object_header *parse_obj,
                                       void *context,
                                       PUNICODE_STRING remaining_path,
                                       ULONG attributes,
                                       struct nt_object_header **found);

/*
 * okay_to_close is consulted before a handle is closed.  Returning an error
 * status prevents the close (e.g. protected handles).
 *
 * Contract: the handle table spinlock is held while this runs.  An
 * implementation must not block, must not call ObReferenceObjectByHandle or
 * any other handle-table operation on the same process, and must not call
 * ObCloseHandle.
 */
typedef NTSTATUS (*nt_object_okay_to_close_t)(struct nt_object_header *hdr,
                                               HANDLE handle,
                                               KPROCESSOR_MODE mode);

struct nt_object_type_ops {
    nt_object_delete_t      delete_obj;
    nt_object_close_t       close;
    nt_object_parse_t       parse;
    nt_object_okay_to_close_t okay_to_close;
};

struct nt_object_type {
    struct list_head        list;
    const char             *name;
    struct nt_object_type_ops ops;
    ULONG                   pool_tag;
};

extern struct nt_object_type nt_directory_type;
extern struct nt_object_type nt_symlink_type;
extern struct nt_object_type nt_process_type;
extern struct nt_object_type nt_thread_type;
extern struct nt_object_type nt_test_type; /* generic test object type */
extern struct nt_object_type nt_token_type; /* access token object (security.c) */

/* Token object body: one reference to an ACCESS_TOKEN, released when the     */
/* object is deleted (see security.c, token_create_primary).                  */
struct _ACCESS_TOKEN;
struct nt_token_body {
    struct _ACCESS_TOKEN   *token;
};

/* -------------------------------------------------------------------------- */
/* Object header                                                              */
/* -------------------------------------------------------------------------- */

struct nt_object_header {
    struct nt_object_type  *type;
    atomic64_t              ref_count;
    atomic64_t              handle_count;
    struct nt_directory    *parent_dir;
    UNICODE_STRING          name;
    ULONG                   flags;
    void                   *body;
    struct list_head        type_link;
    bool                    is_permanent;
    bool                    is_exclusive;
    bool                    exclusive_owned;
    /* Owned self-relative security descriptor copy, NULL when the object has
     * none. Set with ObSetObjectSecurityDescriptor; freed on the final
     * ObDereferenceObject. Shared by every handle, since handles point at
     * this header. Keep this field last: nothing outside object.c relies on
     * the offsets, but new fields are appended at the end. */
    PSECURITY_DESCRIPTOR    security_descriptor;
    ULONG                   security_descriptor_length;
};

static inline void *nt_object_body(struct nt_object_header *hdr)
{
    return hdr->body;
}

static inline struct nt_object_header *nt_body_to_header(void *body)
{
    return (struct nt_object_header *)((char *)body - sizeof(struct nt_object_header));
}

/* -------------------------------------------------------------------------- */
/* Directory body                                                             */
/* -------------------------------------------------------------------------- */

struct nt_dir_entry {
    UNICODE_STRING          name;
    struct nt_object_header *obj;
    struct list_head        link;
};

struct nt_directory {
    rwlock_t                lock;
    struct list_head        entries;
};

static inline bool nt_object_is_directory(struct nt_object_header *hdr)
{
    return hdr && hdr->type == &nt_directory_type;
}

/* -------------------------------------------------------------------------- */
/* Symbolic link body                                                         */
/* -------------------------------------------------------------------------- */

struct nt_symlink {
    UNICODE_STRING          target;
};

/* Generic test body (used only by unit tests)                               */
struct nt_test_body {
    ULONG                   value;
    bool                    close_pending;
    bool                    delete_called;
};

/* -------------------------------------------------------------------------- */
/* Object manager API                                                         */
/* -------------------------------------------------------------------------- */

NTSTATUS ObInitObjectManager(void);
void     ObCleanupObjectManager(void);

/* Test seam for the namespace race tests (object_extra_test.c). NULL in
 * production. The object manager calls it at named points in the directory
 * insert and remove paths, with the object involved. */
extern void (*ObTestPauseHook)(const char *point, struct nt_object_header *obj);

NTSTATUS ObCreateObjectType(const char *name,
                              const struct nt_object_type_ops *ops,
                              struct nt_object_type **out_type);

NTSTATUS ObRegisterObjectType(struct nt_object_type *type);

NTSTATUS ObCreateObject(struct nt_object_type *type,
                         POBJECT_ATTRIBUTES attrs,
                         SIZE_T body_size,
                         struct nt_object_header **out_hdr,
                         void **out_body);

NTSTATUS ObInsertObject(struct nt_object_header *obj,
                         POBJECT_ATTRIBUTES attrs,
                         ACCESS_MASK access,
                         struct nt_eprocess *proc,
                         PHANDLE out_handle);

void     ObReferenceObject(struct nt_object_header *obj);
void     ObDereferenceObject(struct nt_object_header *obj);

NTSTATUS ObReferenceObjectByName(PUNICODE_STRING path,
                                  struct nt_object_type *type,
                                  KPROCESSOR_MODE mode,
                                  ACCESS_MASK access,
                                  struct nt_object_header **out);

NTSTATUS ObOpenObjectByName(POBJECT_ATTRIBUTES attrs,
                             struct nt_object_type *type,
                             KPROCESSOR_MODE mode,
                             ACCESS_MASK access,
                             struct nt_eprocess *proc,
                             PHANDLE out_handle);

#ifdef __KERNEL__
/* Copies a user OBJECT_ATTRIBUTES, its name and the name buffer into kernel storage. */
NTSTATUS ObCopyObjectAttributesFromUser(POBJECT_ATTRIBUTES uptr, OBJECT_ATTRIBUTES *k_oa,
                                        UNICODE_STRING *k_name, WCHAR *k_buf,
                                        size_t k_buf_bytes, POBJECT_ATTRIBUTES *out_oa);
#endif
NTSTATUS ObMakeTemporaryObject(struct nt_object_header *obj);

/* Sets OBJ_PERMANENT on obj. Does not check SeCreatePermanentPrivilege; the
 * caller does. */
NTSTATUS ObMakePermanentObject(struct nt_object_header *obj);

/* New, unnamed or named, directory object with an empty entry list. The caller
 * inserts it with ObInsertOrOpenNamedObject. */
NTSTATUS ObCreateDirectoryObject(POBJECT_ATTRIBUTES oa, struct nt_object_header **out_hdr);

/* New symbolic link object whose target is a copy of target (absolute path,
 * even length, non-empty). The caller inserts it with
 * ObInsertOrOpenNamedObject. */
NTSTATUS ObCreateSymbolicLinkObject(POBJECT_ATTRIBUTES oa, PCUNICODE_STRING target,
                                    struct nt_object_header **out_hdr);

/* Inserts obj under oa's name (or leaves it unnamed when oa has no name) and
 * creates a handle for it in proc. Create-or-open rules (NT):
 *  - the name exists and has another object type: STATUS_OBJECT_TYPE_MISMATCH;
 *  - the name exists and OBJ_OPENIF is not set: STATUS_OBJECT_NAME_COLLISION;
 *  - the name exists and OBJ_OPENIF is set: a handle to the existing object,
 *    STATUS_OBJECT_NAME_EXISTS. obj is not inserted and the caller still
 *    releases it.
 * A missing parent directory gives STATUS_OBJECT_PATH_NOT_FOUND. The name
 * lookup does not follow a symbolic link at the last component. */
NTSTATUS ObInsertOrOpenNamedObject(struct nt_object_header *obj,
                                   POBJECT_ATTRIBUTES oa,
                                   ACCESS_MASK access,
                                   struct nt_eprocess *proc,
                                   PHANDLE out_handle);

/* Attaches a copy of the self-relative descriptor sd (length bytes) to obj,
 * replacing and freeing any previous one. sd == NULL clears it. The object
 * takes ownership of the copy; the caller's buffer is not retained. */
NTSTATUS ObSetObjectSecurityDescriptor(struct nt_object_header *obj,
                                        const void *sd, ULONG length);

/* Returns a heap copy of obj's descriptor (*out, *out_len), or *out == NULL when
 * the object has none. The caller frees the copy with kfree. Safe against a
 * concurrent ObSetObjectSecurityDescriptor. */
NTSTATUS ObCopyObjectSecurityDescriptor(struct nt_object_header *obj,
                                        void **out, ULONG *out_len);
/* Creates a Token object that takes its own reference on tok and inserts it
 * into proc's handle table. The caller keeps its own reference. */
NTSTATUS ObCreateTokenHandle(struct nt_eprocess *proc,
                             struct _ACCESS_TOKEN *tok,
                             ACCESS_MASK access,
                             ULONG attributes,
                             PHANDLE out_handle);

struct nt_object_header *ObGetRootDirectory(void);
struct nt_object_header *ObGetDosDevicesDirectory(void);
struct nt_object_header *ObGetBaseNamedObjects(void);
struct nt_object_header *ObGetDeviceDirectory(void);
struct nt_object_header *ObGetKnownDllsDirectory(void);
struct nt_object_header *ObGetSessionsDirectory(void);

NTSTATUS ObpIncrementHandleCount(struct nt_object_header *obj);
void     ObpDecrementHandleCount(struct nt_object_header *obj, HANDLE handle);

const UNICODE_STRING *ObGetObjectName(struct nt_object_header *obj);
struct nt_object_type *ObGetObjectType(struct nt_object_header *obj);

/* For handle.c internal use */
struct nt_object_header *ObpLookupDirectoryEntry(struct nt_directory *dir, const UNICODE_STRING *comp);

/* -------------------------------------------------------------------------- */
/* Process / thread minimal objects                                           */
/* -------------------------------------------------------------------------- */

struct nt_eprocess {
    struct nt_object_header *obj;
    struct nt_handle_table  *handle_table;
    ULONG                    pid;
    char                     name[32];
    NTSTATUS                 exit_status;
    bool                     terminated;
    ULONG_PTR                peb_base;
    ULONG_PTR                affinity_mask;
    LONG                     base_priority;
    char                     image_path[512];
    WCHAR                    image_path_w[512];
    size_t                   image_path_wlen;
#ifdef __KERNEL__
    spinlock_t               lock;
    wait_queue_head_t        exit_wq;
    atomic_t                 ref_ext;
#else
    pthread_mutex_t          lock;
    pthread_cond_t           exit_cond;
    atomic_int               ref_ext;
#endif
    /* PEB allocation */
    struct _PEB             *peb;
    void                    *peb_teb_reservation;
    /* Primary access token (one reference held). Created with the process
     * and released in ObDestroyProcessObject. */
    struct _ACCESS_TOKEN    *primary_token;
    /* Process information set state (NtSetInformationProcess). Added at the
     * end of the struct; defaults are set in ObCreateProcessObject. */
    UCHAR                    priority_class;        /* PROCESS_PRIORITY_CLASS_*, default NORMAL */
    BOOLEAN                  priority_foreground;   /* PROCESS_PRIORITY_CLASS.Foreground */
    ULONG                    default_hard_error_mode; /* SEM_* flags */
    BOOLEAN                  break_on_termination;  /* ProcessBreakOnTermination */
    /* Job membership (job.c). NULL when the process is not in a job. Set and
     * cleared under the job lock; nt_job_remove_process clears it on destroy. */
    struct nt_job           *job;
    /* Debug object attachment (debugobj.c). NULL when no debugger is attached.
     * Set and cleared under the debug object lock and proc->lock. */
    struct nt_debug_obj     *debug_obj;
};

struct nt_ethread {
    struct nt_object_header *obj;
    struct nt_eprocess      *process;
    ULONG                    tid;
    NTSTATUS                 exit_status;
    bool                     terminated;
    ULONG_PTR                teb_base;
    struct _TEB             *teb;
    void                    *stack_base;
    void                    *stack_limit;
    size_t                   stack_reserve;
    size_t                   stack_commit;
#ifndef __KERNEL__
    pthread_t                pthread;
    bool                     pthread_valid;
    pthread_mutex_t          lock;
    pthread_cond_t           exit_cond;
    pthread_cond_t           suspend_cond;
    atomic_int               suspend_count;
    void                    *gs_saved;
#endif
#ifdef __KERNEL__
    spinlock_t               lock;
    wait_queue_head_t        exit_wq;
    wait_queue_head_t        suspend_wq;
    atomic_t                 suspend_count;
    unsigned long            tif_flags;
#endif
    PVOID                    start_routine;
    PVOID                    start_arg;
    ULONG                    create_flags;
    struct nt_thread        *sync_thread;
    /* Impersonation token (one reference held), NULL unless the thread is
     * impersonating. Released in ObDestroyThreadObject. */
    struct _ACCESS_TOKEN    *impersonation_token;
    /* Scheduling and alert state (proc_thread.c, iocp.c). All zero by default:
     * priority 0 is THREAD_PRIORITY_NORMAL relative to the process, affinity 0
     * means "inherit the process affinity mask". The alert flag is not kept
     * here: it lives on sync_thread, the flag every alertable wait tests, so
     * NtAlertThread and NtTestAlert see the same state. Written under the
     * thread lock. */
    LONG                     priority;       /* relative, -15..15 */
    ULONG_PTR                affinity_mask;  /* 0 = inherit process mask */
    /* NtContinue / NtContinueEx (proc_thread.c): the register state the thread
     * asked to resume with, a copy of the caller's CONTEXT. Written under the
     * thread lock. pending_continue_valid is set only when a context has been
     * recorded. The userspace shim has no CPU state to restore, so nothing
     * consumes this field yet. Zero (not valid) by default. */
    CONTEXT                  pending_continue;
    bool                     pending_continue_valid;
};

NTSTATUS ObCreateProcessObject(struct nt_eprocess **out, ULONG pid, const char *name);
NTSTATUS ObCreateThreadObject(struct nt_ethread **out, struct nt_eprocess *proc, ULONG tid);
void     ObDestroyProcessObject(struct nt_eprocess *proc);
/* Removes proc from its job (job.c). Called first by ObDestroyProcessObject. */
void     nt_job_remove_process(struct nt_eprocess *proc);
void     ObDestroyThreadObject(struct nt_ethread *thr);

struct nt_eprocess *ObFindProcessByHeader(struct nt_object_header *hdr);
struct nt_eprocess *ObFindProcessByPid(ULONG pid);
struct nt_ethread  *ObFindThreadByHeader(struct nt_object_header *hdr);
struct nt_ethread  *ObFindThreadByTid(ULONG tid);
NTSTATUS ObRegisterThread(struct nt_ethread *t);
void     ObUnregisterThread(struct nt_ethread *t);
void ObForEachThreadOfProcess(struct nt_eprocess *proc, void (*fn)(struct nt_ethread *thr, void *ctx), void *ctx);
void ObSuspendProcessThreads(struct nt_eprocess *proc);
void ObResumeProcessThreads(struct nt_eprocess *proc);
void ObTerminateProcessThreads(struct nt_eprocess *proc, NTSTATUS status);

#ifdef __cplusplus
}
#endif

#endif /* NTOBJECT_H */
