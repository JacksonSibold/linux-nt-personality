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
    PSECURITY_DESCRIPTOR    security_descriptor;
    ULONG                   flags;
    void                   *body;
    struct list_head        type_link;
    bool                    is_permanent;
    bool                    is_exclusive;
    bool                    exclusive_owned;
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

NTSTATUS ObMakeTemporaryObject(struct nt_object_header *obj);
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
};

NTSTATUS ObCreateProcessObject(struct nt_eprocess **out, ULONG pid, const char *name);
NTSTATUS ObCreateThreadObject(struct nt_ethread **out, struct nt_eprocess *proc, ULONG tid);
void     ObDestroyProcessObject(struct nt_eprocess *proc);
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
