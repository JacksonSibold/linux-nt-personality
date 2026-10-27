/* drvhost_int.h - internal state and platform layer shared by the driver host.
 * Not part of the driver-facing ABI; included only by drvhost_*.c.
 */
#ifndef DRVHOST_INT_H
#define DRVHOST_INT_H

#ifndef __KERNEL__
#include <setjmp.h>
#endif
#ifdef __KERNEL__
#include <linux/stddef.h>
#include <linux/types.h>
#else
#include <stddef.h>
#include <stdint.h>
#endif

#include "drvhost_libc.h"
#include "drvhost.h"
#include "drvhost_types.h"
#include "drvhost_ntos.h"

/* -------------------------------------------------------------------------- */
/* Platform layer (drvhost_plat.c)                                            */
/* -------------------------------------------------------------------------- */
void  *dh_alloc(size_t n);            /* zeroed, 16-byte aligned, or NULL */
void   dh_free(void *p);
void  *dh_map_image(size_t n);        /* page-aligned, RW, or NULL */
int    dh_protect_rx(void *p, size_t n, int exec, int write);
void   dh_unmap_image(void *p, size_t n);
int    dh_read_file(const char *path, unsigned char **out, size_t *len);
void   dh_free_file(unsigned char *p);
uint64_t dh_now_ns(void);             /* CLOCK_MONOTONIC */
uint64_t dh_system_time_100ns(void);  /* 100ns since 1601-01-01 */
void   dh_sleep_ns(uint64_t ns);

void   dh_lock(void);                 /* the single executive monitor */
void   dh_unlock(void);
void   dh_wait(void);                 /* cond wait, releases the monitor */
int    dh_timedwait_until(uint64_t deadline_ns); /* 0 signalled/woken, 1 timed out */
void   dh_notify_all(void);

typedef void *(*dh_thread_fn)(void *);
void  *dh_thread_start(dh_thread_fn fn, void *arg, int *err);
void   dh_thread_join(void *handle);

/* -------------------------------------------------------------------------- */
/* Per-thread executive state                                                 */
/* -------------------------------------------------------------------------- */
typedef struct dh_thread {
    DISPATCHER_HEADER hdr;     /* signalled when the thread exits (DISPATCHER_THREAD_OBJECT) */
    void     *native;          /* pthread handle, or NULL for an adopted thread */
#ifndef __KERNEL__
    jmp_buf   exit_jb;         /* PsTerminateSystemThread longjmps here (host only) */
#endif
    int       exit_armed;
    NTSTATUS  exit_status;
    PKSTART_ROUTINE start;
    PVOID     start_ctx;
    int       owned_by_pool;   /* created by PsCreateSystemThread */
    struct dh_thread *next;
} dh_thread_t;

dh_thread_t *dh_current_thread(void);

/* Per-thread executive state. Host: __thread variables. Kernel: a per-task
 * slot (plat.c), looked up by the current task on each access. */
#ifdef __KERNEL__
KIRQL       *dh_tls_irql_ptr(void);
struct dh_thread **dh_tls_thread_ptr(void);
#define dh_irql       (*dh_tls_irql_ptr())
#define dh_tls_thread (*dh_tls_thread_ptr())
/* Kernel thread exit. Releases the calling kthread's TLS slot and signals its
 * completion, then kthread_exit. Never returns. Host builds use longjmp. */
void dh_thread_exit_current(void) __attribute__((noreturn));
#else
extern __thread KIRQL dh_irql;
extern __thread dh_thread_t *dh_tls_thread;
#endif

/* Bugcheck: records the code and aborts.  Drivers reach it through KeBugCheckEx
 * and through the host's own IRQL and lock-order checks. */
void dh_bugcheck(ULONG code, ULONG_PTR p1, ULONG_PTR p2, ULONG_PTR p3, ULONG_PTR p4) __attribute__((noreturn));

/* -------------------------------------------------------------------------- */
/* Pool bookkeeping (drvhost_ex.c)                                            */
/* -------------------------------------------------------------------------- */
extern SIZE_T g_pool_outstanding;
extern SIZE_T g_pool_bytes;

/* -------------------------------------------------------------------------- */
/* Handle table (drvhost_ex.c)                                                */
/* -------------------------------------------------------------------------- */
enum dh_kind { DH_KIND_NONE = 0, DH_KIND_FILE = 1, DH_KIND_THREAD = 2 };
typedef struct dh_handle {
    int   kind;
    void *object;
} dh_handle_t;

HANDLE dh_handle_insert(int kind, void *object);
void  *dh_handle_lookup(HANDLE h, int kind);
int    dh_handle_remove(HANDLE h, void **object_out);

/* -------------------------------------------------------------------------- */
/* Timers and DPCs (drvhost_ex.c, run by the worker thread)                   */
/* -------------------------------------------------------------------------- */
void dh_worker_run(void);
void dh_dpc_enqueue_locked(PKDPC dpc, PVOID a1, PVOID a2);

/* -------------------------------------------------------------------------- */
/* I/O manager objects (drvhost_io.c)                                         */
/* -------------------------------------------------------------------------- */
/* A DEVICE_OBJECT is always embedded at offset 0 of a dh_device, so host code
 * can reach its bookkeeping from the driver's pointer. */
typedef struct dh_device {
    DEVICE_OBJECT dev;               /* must stay first */
    char   name[128];                /* \Device\NAME, or empty */
    int    name_registered;
    PDEVICE_OBJECT pdo_of_fdo;       /* unused when 0 */
    int    io_timer_active;
    PIO_TIMER_ROUTINE io_timer_routine;
    PVOID  io_timer_ctx;
    uint64_t io_timer_next_ns;
    int    deleted;
    struct dh_device *all_next;      /* g_all_devices list */
    int    io_timer_inflight;        /* IoTimer calls not yet returned */
} dh_device_t;

#define DH_FROM_DEV(d) ((dh_device_t *)(d))

/* Symbolic-link and device namespace (drvhost_io.c). */
dh_device_t *dh_device_by_name(const char *nt_name);
int   dh_symlink_lookup(const char *nt_link, char *target, size_t cap);
void  dh_ascii_from_unicode(const UNICODE_STRING *u, char *out, size_t cap);

/* IRP helpers. */
void  dh_irp_set_stack(PIRP irp, CCHAR location_count);
PIO_STACK_LOCATION dh_irp_current(PIRP irp);

/* IO timers are driven by the worker thread. Collect due ones under the monitor,
 * then call them unlocked (see dh_worker_run). */
int   dh_io_timer_collect(uint64_t now, dh_device_t **out, int cap);
uint64_t dh_io_timer_next_deadline_locked(void);
#define DH_IRP_OWNS_SYSBUF 0x80000000u   /* IoFreeIrp frees SystemBuffer */

/* Calls into the driver with a bugcheck on a NULL dispatch slot. */
DRVCALL NTSTATUS dh_invalid_device_request(PDEVICE_OBJECT DeviceObject, PIRP Irp);

/* -------------------------------------------------------------------------- */
/* Driver objects and service control (drvhost_svc.c)                         */
/* -------------------------------------------------------------------------- */
#define DH_NAME_MAX 256

typedef struct dh_image dh_image_t;

typedef struct dh_driver {
    DRIVER_OBJECT drv;               /* must stay first */
    DRIVER_EXTENSION ext;
    char   service[DH_NAME_MAX];     /* service key name */
    char   image_path[DH_NAME_MAX];
    dh_image_t *image;
    UNICODE_STRING name_u;           /* \Driver\<service> */
    WCHAR  name_buf[DH_NAME_MAX];
    UNICODE_STRING key_u;            /* service key */
    WCHAR  key_buf[DH_NAME_MAX];
    int    loaded;
    struct dh_driver *next;
} dh_driver_t;

dh_driver_t *dh_driver_by_service(const char *service);
void dh_driver_free_image(dh_driver_t *drv);

/* -------------------------------------------------------------------------- */
/* PE image loader (drvhost_pe.c)                                             */
/* -------------------------------------------------------------------------- */
struct dh_image {
    unsigned char *mem;      /* mapped image */
    size_t         size;     /* SizeOfImage, page multiple */
    void          *entry;    /* DriverEntry, or NULL */
    unsigned char *file;     /* file bytes kept only during load */
    size_t         file_len;
};

/* Maps, relocates and binds a .sys.  Missing imports fail the load with
 * STATUS_PROCEDURE_NOT_FOUND and are reported through DbgPrint, so a
 * caller can see every unresolved name at once. */
NTSTATUS dh_image_load(const char *path, dh_image_t **out);
void     dh_image_unload(dh_image_t *img);
/* Looks up one exported symbol of the host export table by name (for
 * MmGetSystemRoutineAddress). */
void    *dh_host_export(const char *name, size_t len);

/* -------------------------------------------------------------------------- */
/* Debug output (drvhost_fmt.c)                                               */
/* -------------------------------------------------------------------------- */
void dh_emit(const char *text, SIZE_T len);

#endif /* DRVHOST_INT_H */

/* -------------------------------------------------------------------------- */
/* Cross-file counters and lifecycle hooks                                    */
/* -------------------------------------------------------------------------- */
extern ULONG g_devices_live;
extern ULONG g_irps_live;
extern ULONG g_images_loaded;
extern ULONG g_dpcs_run;
extern ULONG g_timers_fired;
extern ULONG g_io_timer_ticks;

int  dh_ex_init(void);       /* starts the executive worker thread */
void dh_ex_shutdown(void);   /* stops and joins it */
void dh_ex_join_threads(void); /* joins system threads and frees their objects */
void dh_symlinks_clear(void);
ULONG dh_devices_registered(void);
void *dh_host_export_module(const char *module, const char *name, size_t len);

/* Closes an NT file handle's object (drvhost_nt.c); called by ZwClose. */
void dh_nt_file_closed(void *file_object);

/* Host bugcheck codes (not Windows values, except 0xC2 and 0x0A). */
#define DH_BUGCHECK_BAD_POOL_CALLER     0x000000C2u
#define DH_BUGCHECK_IRQL                0x0000000Au
#define DH_BUGCHECK_LOCK_RECURSION      0xD0000001u
#define DH_BUGCHECK_BAD_IRP             0xD0000002u
#define DH_BUGCHECK_BAD_HOST_STATE      0xD0000003u
#define DH_BUGCHECK_TERMINATE_NONPOOL   0xD0000004u
