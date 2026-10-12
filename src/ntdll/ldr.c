/*
 * ldr.c - user-mode PE loader (winldr) implementation.
 *
 * This file intentionally uses a small, portable subset of C runtime functions
 * so that it can later be linked into ntdll.  For the current workstream it is
 * compiled as a normal Linux userspace object and tested with real PE DLLs.
 */

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#define _GNU_SOURCE
#include "ldr.h"
#include "pe_bounds.h"
#include "ldr_pure.h"

#ifndef __MINGW32__
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#ifdef __MINGW32__
/* Freestanding NT implementation for ntdll.dll - no POSIX, no imports, use NT syscalls */
#include <string.h>
#include <stdlib.h>
#include "nt_syscalls.h"
#ifndef O_BINARY
#define O_BINARY 0
#endif
typedef volatile LONG ldr_spinlock_t;
static inline void ldr_spin_init(ldr_spinlock_t *l){ *l = 0; }
static inline void ldr_spin_lock(ldr_spinlock_t *l){ while(__sync_lock_test_and_set(l,1)){ while(*l) __asm__ volatile("pause"); } }
static inline void ldr_spin_unlock(ldr_spinlock_t *l){ __sync_lock_release(l); }
typedef ldr_spinlock_t pthread_mutex_t;
typedef unsigned long pthread_t;
static inline int pthread_mutex_init(pthread_mutex_t *m, void *a){ (void)a; ldr_spin_init(m); return 0; }
static inline int pthread_mutex_lock(pthread_mutex_t *m){ ldr_spin_lock(m); return 0; }
static inline int pthread_mutex_unlock(pthread_mutex_t *m){ ldr_spin_unlock(m); return 0; }
static inline int pthread_mutex_destroy(pthread_mutex_t *m){ (void)m; return 0; }
static inline pthread_t pthread_self(void){ return 1; }
static inline int pthread_equal(pthread_t a, pthread_t b){ return a==b; }
#ifndef MAP_FAILED
#define MAP_FAILED ((void*)-1)
#endif
#ifndef MEM_COMMIT
#define MEM_COMMIT 0x1000
#define MEM_RESERVE 0x2000
#define MEM_RELEASE 0x8000
#define PAGE_NOACCESS 0x01
#define PAGE_READONLY 0x02
#define PAGE_READWRITE 0x04
#define PAGE_WRITECOPY 0x08
#define PAGE_EXECUTE 0x10
#define PAGE_EXECUTE_READ 0x20
#define PAGE_EXECUTE_READWRITE 0x40
#define PAGE_EXECUTE_WRITECOPY 0x80
#endif
#ifndef FILE_SUPERSEDE
#define FILE_SUPERSEDE 0
#define FILE_OPEN 1
#define FILE_CREATE 2
#define FILE_OPEN_IF 3
#define FILE_OVERWRITE 4
#define FILE_OVERWRITE_IF 5
#define FILE_SYNCHRONOUS_IO_NONALERT 0x00000020
#define FILE_NON_DIRECTORY_FILE 0x00000040
#define OBJ_CASE_INSENSITIVE 0x00000040
#define FILE_SHARE_READ 1
#define FILE_SHARE_WRITE 2
#define FILE_SHARE_DELETE 4
#endif
#endif

#ifndef O_BINARY
#define O_BINARY 0
#endif

#ifndef __MINGW32__
#ifndef RemoveTailList
static inline PLIST_ENTRY ldr_RemoveTailList(PLIST_ENTRY h) {
    PLIST_ENTRY e = h->Blink;
    if (e == h) return NULL;
    RemoveEntryList(e);
    return e;
}
#define RemoveTailList(h) ldr_RemoveTailList(h)
#endif

NT_STATIC_ASSERT(offsetof(LDR_DATA_TABLE_ENTRY, InLoadOrderLinks)           == 0x00,
                 "ldr: LDR_DATA_TABLE_ENTRY.InLoadOrderLinks offset");
NT_STATIC_ASSERT(offsetof(LDR_DATA_TABLE_ENTRY, InMemoryOrderLinks)          == 0x10,
                 "ldr: LDR_DATA_TABLE_ENTRY.InMemoryOrderLinks offset");
NT_STATIC_ASSERT(offsetof(LDR_DATA_TABLE_ENTRY, InInitializationOrderLinks)  == 0x20,
                 "ldr: LDR_DATA_TABLE_ENTRY.InInitializationOrderLinks offset");
NT_STATIC_ASSERT(offsetof(PEB_LDR_DATA, InLoadOrderModuleList)               == 0x10,
                 "ldr: PEB_LDR_DATA.InLoadOrderModuleList offset");
NT_STATIC_ASSERT(offsetof(PEB_LDR_DATA, InMemoryOrderModuleList)             == 0x20,
                 "ldr: PEB_LDR_DATA.InMemoryOrderModuleList offset");
NT_STATIC_ASSERT(offsetof(PEB_LDR_DATA, InInitializationOrderModuleList)     == 0x30,
                 "ldr: PEB_LDR_DATA.InInitializationOrderModuleList offset");

/* -------------------------------------------------------------------------- */
/* Internal module node                                                       */
/* -------------------------------------------------------------------------- */

typedef struct ldr_module {
    LDR_DATA_TABLE_ENTRY entry;

    /* Parsed PE image */
    pe_context_t pe;
    u8          *file_data;     /* owned file buffer */
    size_t       file_size;
    u8          *image;         /* owned mapped image */
    size_t       image_size;

    /* Module state */
    int loading;                /* cycle detection */
    int attached;               /* DllMain attach already run */
    int datafile;               /* LOAD_LIBRARY_AS_DATAFILE */
    int pinned;                 /* never unload */

    /* TLS */
    IMAGE_TLS_DIRECTORY64 tls;
    int has_tls;
    ULONG tls_slot;

    /* ANSI base name cache for fast lookups */
    char base_name_ansi[256];
} ldr_module_t;

#define LDR_HASH_BUCKETS 256

/* -------------------------------------------------------------------------- */
/* Loader context                                                             */
/* -------------------------------------------------------------------------- */

struct winldr_ctx {
    PEB_LDR_DATA ldr_data;
    LIST_ENTRY module_hash[LDR_HASH_BUCKETS];

    /* Search paths */
    char *exe_dir;
    char *system32_dir;
    char **path_dirs;
    size_t path_count;

    /* Known DLLs */
    char **known_dlls;
    size_t known_dll_count;

    /* API set schema */
    struct {
        char *virtual_name;
        char *host_dll;
    } *apiset;
    size_t apiset_count;

    /* Hooks */
    ldr_dll_main_hook_t dll_main_hook;
    void *dll_main_user;
    ldr_tls_callback_hook_t tls_callback_hook;
    void *tls_callback_user;

    /* Init log for tests */
    winldr_init_log_entry_t *init_log;
    size_t init_log_count;
    size_t init_log_capacity;

    /* TLS bitmap (first 64 slots) */
    RTL_BITMAP tls_bitmap;
    ULONG tls_bitmap_bits[2];

    /* Loader lock (recursive, implemented with pthread) */
    pthread_mutex_t lock;
    pthread_t lock_owner;
    int lock_depth;

    /* Current initialization stack (for cycle / attach ordering) */
    ldr_module_t *init_stack[64];
    size_t init_stack_depth;
};

/* -------------------------------------------------------------------------- */
/* Forward declarations                                                       */
/* -------------------------------------------------------------------------- */

static void ldr_lock_acquire(winldr_ctx_t *ctx);
static void ldr_lock_release(winldr_ctx_t *ctx);
static ldr_module_t *ldr_lookup_by_name(winldr_ctx_t *ctx, const char *name);
static NTSTATUS ldr_load_dll_internal(winldr_ctx_t *ctx,
                                      const char *search_path,
                                      ULONG flags,
                                      const char *name,
                                      HMODULE *out);
static NTSTATUS ldr_map_and_load(winldr_ctx_t *ctx,
                                 const char *path,
                                 ULONG flags,
                                 ldr_module_t **out);
static NTSTATUS ldr_resolve_imports(winldr_ctx_t *ctx, ldr_module_t *mod);
static NTSTATUS ldr_process_attach(winldr_ctx_t *ctx, ldr_module_t *mod);
static NTSTATUS ldr_process_detach(winldr_ctx_t *ctx, ldr_module_t *mod);
static void ldr_log_call(winldr_ctx_t *ctx, ldr_module_t *mod, ULONG reason,
                         int is_tls, ULONG_PTR cb_va);

/* -------------------------------------------------------------------------- */
/* Minimal wide/ansi helpers                                                  */
/* -------------------------------------------------------------------------- */

static size_t ldr_wcslen(const WCHAR *s) __attribute__((unused));
static size_t ldr_wcslen(const WCHAR *s)
{
    size_t n = 0;
    while (s && s[n]) ++n;
    return n;
}

static int ldr_wcs_to_ascii(const WCHAR *in, char *out, size_t outsz)
{
    size_t i;
    for (i = 0; i < outsz - 1 && in[i]; ++i)
        out[i] = (char)(in[i] & 0xFF);
    out[i] = '\0';
    return (int)i;
}

static void ldr_ascii_to_wcs(const char *in, WCHAR *out, size_t outsz)
{
    size_t i;
    for (i = 0; i < outsz - 1 && in[i]; ++i)
        out[i] = (WCHAR)(unsigned char)in[i];
    out[i] = 0;
}

static void ldr_str_to_lower(char *s)
{
    for (; *s; ++s) {
        if (*s >= 'A' && *s <= 'Z') *s = (char)(*s - 'A' + 'a');
    }
}

static int ldr_strcasecmp(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = *a++, cb = *b++;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) return (int)(unsigned char)ca - (int)(unsigned char)cb;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

static const char *ldr_basename(const char *path)
{
    const char *p = path;
    const char *last = path;
    while (*p) {
        if (*p == '/' || *p == '\\') last = p + 1;
        ++p;
    }
    return last;
}

static int ldr_ends_with_dll(const char *s)
{
    size_t len = strlen(s);
    if (len < 4) return 0;
    return ldr_strcasecmp(s + len - 4, ".dll") == 0;
}

static void ldr_set_base_name(ldr_module_t *mod, const char *name)
{
    const char *base = ldr_basename(name);
    strncpy(mod->base_name_ansi, base, sizeof(mod->base_name_ansi) - 1);
    mod->base_name_ansi[sizeof(mod->base_name_ansi) - 1] = '\0';
    ldr_str_to_lower(mod->base_name_ansi);
}

static void ldr_set_unicode_string(UNICODE_STRING *us, const WCHAR *buf, USHORT len)
{
    us->Buffer = (PWSTR)buf;
    us->Length = len;
    us->MaximumLength = (USHORT)(len + sizeof(WCHAR));
}

/* -------------------------------------------------------------------------- */
/* Loader lock (recursive)                                                    */
/* -------------------------------------------------------------------------- */

static void ldr_lock_acquire(winldr_ctx_t *ctx)
{
    pthread_t self = pthread_self();
    if (ctx->lock_depth > 0 && pthread_equal(ctx->lock_owner, self)) {
        ++ctx->lock_depth;
        return;
    }
    pthread_mutex_lock(&ctx->lock);
    ctx->lock_owner = self;
    ctx->lock_depth = 1;
}

static void ldr_lock_release(winldr_ctx_t *ctx)
{
    if (--ctx->lock_depth == 0) {
        ctx->lock_owner = 0;
        pthread_mutex_unlock(&ctx->lock);
    }
}

/* -------------------------------------------------------------------------- */
/* Init log                                                                   */
/* -------------------------------------------------------------------------- */

static void ldr_log_call(winldr_ctx_t *ctx, ldr_module_t *mod, ULONG reason,
                         int is_tls, ULONG_PTR cb_va)
{
    if (ctx->init_log_count == ctx->init_log_capacity) {
        size_t nc = ctx->init_log_capacity ? ctx->init_log_capacity * 2 : 64;
        winldr_init_log_entry_t *p = realloc(ctx->init_log,
                                             nc * sizeof(*p));
        if (!p) return;
        ctx->init_log = p;
        ctx->init_log_capacity = nc;
    }
    winldr_init_log_entry_t *e = &ctx->init_log[ctx->init_log_count++];
    e->name = mod->base_name_ansi;
    e->reason = reason;
    e->is_tls = is_tls;
    e->cb_va = cb_va;
}

const winldr_init_log_entry_t *winldr_get_init_log(winldr_ctx_t *ctx, size_t *count)
{
    if (count) *count = ctx->init_log_count;
    return ctx->init_log;
}

void winldr_clear_init_log(winldr_ctx_t *ctx)
{
    ctx->init_log_count = 0;
}

/* -------------------------------------------------------------------------- */
/* Module hash map                                                            */
/* -------------------------------------------------------------------------- */

static unsigned ldr_hash_name(const char *name)
{
    unsigned h = 5381;
    while (*name) {
        char c = *name++;
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        h = ((h << 5) + h) + (unsigned char)c;
    }
    return h % LDR_HASH_BUCKETS;
}

static void ldr_insert_hash(winldr_ctx_t *ctx, ldr_module_t *mod)
{
    unsigned h = ldr_hash_name(mod->base_name_ansi);
    InsertHeadList(&ctx->module_hash[h], &mod->entry.HashLinks);
}

static ldr_module_t *ldr_lookup_by_name(winldr_ctx_t *ctx, const char *name)
{
    char lower[256];
    strncpy(lower, name, sizeof(lower) - 1);
    lower[sizeof(lower) - 1] = '\0';
    ldr_str_to_lower(lower);

    unsigned h = ldr_hash_name(lower);
    LIST_ENTRY *cur = ctx->module_hash[h].Flink;
    while (cur != &ctx->module_hash[h]) {
        ldr_module_t *m = CONTAINING_RECORD(cur, ldr_module_t, entry.HashLinks);
        if (ldr_strcasecmp(m->base_name_ansi, lower) == 0)
            return m;
        cur = cur->Flink;
    }
    return NULL;
}

/* -------------------------------------------------------------------------- */
/* Context create / destroy                                                   */
/* -------------------------------------------------------------------------- */

winldr_ctx_t *winldr_ctx_create(void)
{
    winldr_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;

    InitializeListHead(&ctx->ldr_data.InLoadOrderModuleList);
    InitializeListHead(&ctx->ldr_data.InMemoryOrderModuleList);
    InitializeListHead(&ctx->ldr_data.InInitializationOrderModuleList);
    ctx->ldr_data.Length = sizeof(ctx->ldr_data);
    ctx->ldr_data.Initialized = 1;

    for (int i = 0; i < LDR_HASH_BUCKETS; ++i)
        InitializeListHead(&ctx->module_hash[i]);

    pthread_mutex_init(&ctx->lock, NULL);

    ctx->tls_bitmap.Buffer = ctx->tls_bitmap_bits;
    ctx->tls_bitmap.SizeOfBitMap = 64;
    ctx->tls_bitmap_bits[0] = 0;
    ctx->tls_bitmap_bits[1] = 0;

    /* Reserve slot 0 for the main executable (kernel allocates it). */
    ctx->tls_bitmap_bits[0] |= 1U;

    /* Built-in minimal API set fallback. */
    winldr_load_apiset_schema(ctx, NULL);

    return ctx;
}

static void ldr_free_paths(winldr_ctx_t *ctx)
{
    free(ctx->exe_dir);
    free(ctx->system32_dir);
    for (size_t i = 0; i < ctx->path_count; ++i)
        free(ctx->path_dirs[i]);
    free(ctx->path_dirs);
    ctx->exe_dir = NULL;
    ctx->system32_dir = NULL;
    ctx->path_dirs = NULL;
    ctx->path_count = 0;
}

static void ldr_free_known_dlls(winldr_ctx_t *ctx)
{
    for (size_t i = 0; i < ctx->known_dll_count; ++i)
        free(ctx->known_dlls[i]);
    free(ctx->known_dlls);
    ctx->known_dlls = NULL;
    ctx->known_dll_count = 0;
}

static void ldr_free_apiset(winldr_ctx_t *ctx)
{
    for (size_t i = 0; i < ctx->apiset_count; ++i) {
        free(ctx->apiset[i].virtual_name);
        free(ctx->apiset[i].host_dll);
    }
    free(ctx->apiset);
    ctx->apiset = NULL;
    ctx->apiset_count = 0;
}

void winldr_ctx_destroy(winldr_ctx_t *ctx)
{
    if (!ctx) return;

    /* Unload all modules in reverse load order. */
    while (!IsListEmpty(&ctx->ldr_data.InLoadOrderModuleList)) {
        PLIST_ENTRY e = RemoveTailList(&ctx->ldr_data.InLoadOrderModuleList);
        ldr_module_t *m = CONTAINING_RECORD(e, ldr_module_t, entry.InLoadOrderLinks);
        if (m->attached && !m->datafile)
            ldr_process_detach(ctx, m);
        if (m->entry.FullDllName.Buffer) free(m->entry.FullDllName.Buffer);
        if (m->entry.BaseDllName.Buffer) free(m->entry.BaseDllName.Buffer);
        if (m->image) munmap(m->image, m->image_size);
        free(m->file_data);
        free(m);
    }

    ldr_free_paths(ctx);
    ldr_free_known_dlls(ctx);
    ldr_free_apiset(ctx);
    free(ctx->init_log);
    pthread_mutex_destroy(&ctx->lock);
    free(ctx);
}

PPEB_LDR_DATA winldr_peb_ldr(winldr_ctx_t *ctx)
{
    return &ctx->ldr_data;
}

/* -------------------------------------------------------------------------- */
/* Configuration                                                              */
/* -------------------------------------------------------------------------- */

void winldr_set_exe_dir(winldr_ctx_t *ctx, const char *path)
{
    ldr_lock_acquire(ctx);
    free(ctx->exe_dir);
    ctx->exe_dir = path ? strdup(path) : NULL;
    ldr_lock_release(ctx);
}

void winldr_set_system32_dir(winldr_ctx_t *ctx, const char *path)
{
    ldr_lock_acquire(ctx);
    free(ctx->system32_dir);
    ctx->system32_dir = path ? strdup(path) : NULL;
    ldr_lock_release(ctx);
}

void winldr_register_known_dll(winldr_ctx_t *ctx, const char *name)
{
    ldr_lock_acquire(ctx);
    char **p = realloc(ctx->known_dlls,
                       (ctx->known_dll_count + 1) * sizeof(char *));
    if (p) {
        ctx->known_dlls = p;
        ctx->known_dlls[ctx->known_dll_count++] = strdup(name);
    }
    ldr_lock_release(ctx);
}

void winldr_set_dll_main_hook(winldr_ctx_t *ctx, ldr_dll_main_hook_t hook, void *user)
{
    ctx->dll_main_hook = hook;
    ctx->dll_main_user = user;
}

void winldr_set_tls_callback_hook(winldr_ctx_t *ctx, ldr_tls_callback_hook_t hook, void *user)
{
    ctx->tls_callback_hook = hook;
    ctx->tls_callback_user = user;
}

/* -------------------------------------------------------------------------- */
/* API set schema (minimal fallback + optional text schema file)              */
/* -------------------------------------------------------------------------- */

static void ldr_add_apiset(winldr_ctx_t *ctx, const char *virt, const char *host)
{
    size_t n = ctx->apiset_count;
    void *p = realloc(ctx->apiset, (n + 1) * sizeof(*ctx->apiset));
    if (!p) return;
    ctx->apiset = p;
    ctx->apiset[n].virtual_name = strdup(virt);
    ctx->apiset[n].host_dll = strdup(host);
    ctx->apiset_count = n + 1;
}

NTSTATUS winldr_load_apiset_schema(winldr_ctx_t *ctx, const char *path)
{
    ldr_lock_acquire(ctx);

    /* Clear any previously loaded schema but keep built-in fallback. */
    ldr_free_apiset(ctx);

    if (path) {
        FILE *f = fopen(path, "r");
        if (f) {
            char line[512];
            while (fgets(line, sizeof(line), f)) {
                char *p = strchr(line, '#');
                if (p) *p = '\0';
                char virt[256] = {0}, host[256] = {0};
                if (sscanf(line, "%255s %255s", virt, host) == 2) {
                    ldr_add_apiset(ctx, virt, host);
                }
            }
            fclose(f);
        }
    }

    /* Built-in fallback for common virtual DLLs. */
    ldr_add_apiset(ctx, "api-ms-win-core-synch-l1-1-0", "kernelbase.dll");
    ldr_add_apiset(ctx, "api-ms-win-core-synch-l1-2-0", "kernelbase.dll");
    ldr_add_apiset(ctx, "api-ms-win-core-processthreads-l1-1-0", "kernelbase.dll");
    ldr_add_apiset(ctx, "api-ms-win-core-processthreads-l1-1-1", "kernelbase.dll");
    ldr_add_apiset(ctx, "api-ms-win-core-libraryloader-l1-2-0", "kernelbase.dll");
    ldr_add_apiset(ctx, "api-ms-win-core-libraryloader-l1-2-1", "kernelbase.dll");
    ldr_add_apiset(ctx, "api-ms-win-core-rtlsupport-l1-2-0", "ntdll.dll");
    ldr_add_apiset(ctx, "api-ms-win-core-heap-l1-1-0", "kernelbase.dll");
    ldr_add_apiset(ctx, "api-ms-win-core-errorhandling-l1-1-0", "kernelbase.dll");
    ldr_add_apiset(ctx, "api-ms-win-core-file-l1-1-0", "kernelbase.dll");
    ldr_add_apiset(ctx, "api-ms-win-core-handle-l1-1-0", "kernelbase.dll");
    ldr_add_apiset(ctx, "api-ms-win-core-localization-l1-2-0", "kernelbase.dll");
    ldr_add_apiset(ctx, "api-ms-win-core-memory-l1-1-0", "kernelbase.dll");
    ldr_add_apiset(ctx, "api-ms-win-core-sysinfo-l1-1-0", "kernelbase.dll");
    ldr_add_apiset(ctx, "api-ms-win-core-string-l1-1-0", "kernelbase.dll");
    ldr_add_apiset(ctx, "api-ms-win-core-timezone-l1-1-0", "kernelbase.dll");
    ldr_add_apiset(ctx, "api-ms-win-core-console-l1-1-0", "kernelbase.dll");
    ldr_add_apiset(ctx, "api-ms-win-core-console-l2-1-0", "kernelbase.dll");
    ldr_add_apiset(ctx, "api-ms-win-core-datetime-l1-1-0", "kernelbase.dll");
    ldr_add_apiset(ctx, "api-ms-win-core-fibers-l1-1-0", "kernelbase.dll");

    ldr_lock_release(ctx);
    return STATUS_SUCCESS;
}

const char *winldr_apiset_resolve(winldr_ctx_t *ctx, const char *virtual_name)
{
    if (!ctx || !virtual_name) return NULL;
    char lower[256];
    strncpy(lower, virtual_name, sizeof(lower) - 1);
    lower[sizeof(lower) - 1] = '\0';
    ldr_str_to_lower(lower);
    // Strip .dll suffix for comparison
    size_t len = strlen(lower);
    if (len > 4 && strcmp(lower + len - 4, ".dll") == 0) lower[len - 4] = '\0';

    ldr_lock_acquire(ctx);
    for (size_t i = 0; i < ctx->apiset_count; ++i) {
        char host_lower[256];
        strncpy(host_lower, ctx->apiset[i].virtual_name, sizeof(host_lower)-1);
        host_lower[sizeof(host_lower)-1]='\0';
        ldr_str_to_lower(host_lower);
        size_t hlen = strlen(host_lower);
        if (hlen > 4 && strcmp(host_lower + hlen - 4, ".dll")==0) host_lower[hlen-4]='\0';
        if (ldr_strcasecmp(host_lower, lower) == 0) {
            const char *res = ctx->apiset[i].host_dll;
            ldr_lock_release(ctx);
            return res;
        }
    }
    ldr_lock_release(ctx);
    return NULL;
}

/* -------------------------------------------------------------------------- */
/* File helpers                                                               */
/* -------------------------------------------------------------------------- */

#ifdef __MINGW32__
static int ldr_read_file(const char *path, u8 **out_data, size_t *out_size)
{
    WCHAR wpath[1024];
    size_t plen = strlen(path);
    if (plen >= 1023) return -1;
    const char *npath = path;
    if (path[0]=='/') {
        if (strncmp(path, "/tmp/",5)==0) npath = path+5;
        else if (path[0]=='/') npath = path+1;
    }
    size_t nlen = strlen(npath);
    if (nlen >= 1023) return -1;
    for (size_t i=0;i<=nlen;i++) wpath[i]=(WCHAR)(unsigned char)npath[i];
    for (size_t i=0; wpath[i]; i++) if (wpath[i]=='/') wpath[i]='\\';
    WCHAR ntpath[1024];
    const WCHAR prefix[] = { '\\','?','?','\\',0 };
    size_t pre=4;
    for (size_t i=0;i<pre;i++) ntpath[i]=prefix[i];
    for (size_t i=0;i<=nlen;i++) ntpath[pre+i]=wpath[i];
    size_t total_len = pre + nlen;
    UNICODE_STRING us;
    us.Length = (USHORT)(total_len * sizeof(WCHAR));
    us.MaximumLength = us.Length + sizeof(WCHAR);
    us.Buffer = ntpath;
    OBJECT_ATTRIBUTES oa;
    oa.Length = sizeof(oa);
    oa.RootDirectory = NULL;
    oa.ObjectName = &us;
    oa.Attributes = OBJ_CASE_INSENSITIVE;
    oa.SecurityDescriptor = NULL;
    oa.SecurityQualityOfService = NULL;
    HANDLE h = NULL;
    IO_STATUS_BLOCK ios;
    NTSTATUS st = NtCreateFile(&h, 0x80000000|0x00100080, &oa, &ios, NULL, 0, FILE_SHARE_READ|FILE_SHARE_WRITE, FILE_OPEN, FILE_NON_DIRECTORY_FILE|FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0);
    if (!NT_SUCCESS(st) || !h) {
        for (size_t i=0;i<=plen;i++) wpath[i]=(WCHAR)(unsigned char)path[i];
        for (size_t i=0; wpath[i]; i++) if (wpath[i]=='/') wpath[i]='\\';
        us.Length = (USHORT)(plen * sizeof(WCHAR));
        us.MaximumLength = us.Length + sizeof(WCHAR);
        us.Buffer = wpath;
        oa.ObjectName = &us;
        st = NtCreateFile(&h, 0x80000000|0x00100080, &oa, &ios, NULL, 0, FILE_SHARE_READ|FILE_SHARE_WRITE, FILE_OPEN, FILE_NON_DIRECTORY_FILE|FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0);
        if (!NT_SUCCESS(st) || !h) return -1;
    }
    struct { LARGE_INTEGER AllocationSize; LARGE_INTEGER EndOfFile; ULONG NumberOfLinks; BOOLEAN DeletePending; BOOLEAN Directory; } finfo;
    st = NtQueryInformationFile(h, &ios, &finfo, sizeof(finfo), 5);
    size_t sz = 0;
    if (NT_SUCCESS(st)) sz = (size_t)finfo.EndOfFile.QuadPart;
    if (sz==0) sz = 8192;
    u8 *buf = (u8*)malloc(sz ? sz : 1);
    if (!buf) { NtClose(h); return -1; }
    size_t total = 0;
    while (1) {
        ULONG to_read = (ULONG)(sz - total);
        if (to_read==0) {
            size_t nsz = sz * 2;
            u8 *nbuf = (u8*)malloc(nsz);
            if (!nbuf) { free(buf); NtClose(h); return -1; }
            memcpy(nbuf, buf, total);
            buf = nbuf;
            sz = nsz;
            to_read = (ULONG)(sz - total);
        }
        IO_STATUS_BLOCK rios;
        LARGE_INTEGER off; off.QuadPart = (LONGLONG)total;
        st = NtReadFile(h, NULL, NULL, NULL, &rios, buf+total, to_read, &off, NULL);
        if (!NT_SUCCESS(st) && st != STATUS_SUCCESS) {
            if (total>0) break;
            free(buf); NtClose(h); return -1;
        }
        size_t got = (size_t)rios.Information;
        if (got==0) break;
        total += got;
        if (total < sz && got < to_read) break;
        if (finfo.EndOfFile.QuadPart && total >= (size_t)finfo.EndOfFile.QuadPart) break;
    }
    NtClose(h);
    *out_data = buf;
    *out_size = total;
    return 0;
}

static int ldr_file_exists(const char *path)
{
    WCHAR wpath[1024];
    size_t plen = strlen(path);
    if (plen >= 1023) return 0;
    const char *npath = path;
    if (path[0]=='/') {
        if (strncmp(path, "/tmp/",5)==0) npath = path+5;
        else if (path[0]=='/') npath = path+1;
    }
    size_t nlen = strlen(npath);
    for (size_t i=0;i<=nlen;i++) wpath[i]=(WCHAR)(unsigned char)npath[i];
    for (size_t i=0; wpath[i]; i++) if (wpath[i]=='/') wpath[i]='\\';
    WCHAR ntpath[1024];
    const WCHAR prefix[] = { '\\','?','?','\\',0 };
    size_t pre=4;
    for (size_t i=0;i<pre;i++) ntpath[i]=prefix[i];
    for (size_t i=0;i<=nlen;i++) ntpath[pre+i]=wpath[i];
    size_t total_len = pre + nlen;
    UNICODE_STRING us;
    us.Length = (USHORT)(total_len * sizeof(WCHAR));
    us.MaximumLength = us.Length + sizeof(WCHAR);
    us.Buffer = ntpath;
    OBJECT_ATTRIBUTES oa;
    oa.Length = sizeof(oa);
    oa.RootDirectory = NULL;
    oa.ObjectName = &us;
    oa.Attributes = OBJ_CASE_INSENSITIVE;
    oa.SecurityDescriptor = NULL;
    oa.SecurityQualityOfService = NULL;
    HANDLE h=NULL;
    IO_STATUS_BLOCK ios;
    NTSTATUS st = NtCreateFile(&h, 0x80000000|0x00100080, &oa, &ios, NULL, 0, FILE_SHARE_READ|FILE_SHARE_WRITE, FILE_OPEN, 0, NULL, 0);
    if (NT_SUCCESS(st) && h) { NtClose(h); return 1; }
    for (size_t i=0;i<=plen;i++) wpath[i]=(WCHAR)(unsigned char)path[i];
    for (size_t i=0; wpath[i]; i++) if (wpath[i]=='/') wpath[i]='\\';
    us.Length = (USHORT)(plen * sizeof(WCHAR));
    us.MaximumLength = us.Length + sizeof(WCHAR);
    us.Buffer = wpath;
    oa.ObjectName = &us;
    st = NtCreateFile(&h, 0x80000000|0x00100080, &oa, &ios, NULL, 0, FILE_SHARE_READ|FILE_SHARE_WRITE, FILE_OPEN, 0, NULL, 0);
    if (NT_SUCCESS(st) && h) { NtClose(h); return 1; }
    return 0;
}

/* -------------------------------------------------------------------------- */
/* Image mapping                                                              */
/* -------------------------------------------------------------------------- */

static void *ldr_alloc_image(size_t size, size_t align)
{
    if (align < 4096) align = 4096;
    size = (size + align - 1) & ~(align - 1);
    PVOID base = NULL;
    SIZE_T sz = size;
    NTSTATUS st = NtAllocateVirtualMemory((HANDLE)(intptr_t)-1, &base, 0, &sz, MEM_RESERVE|MEM_COMMIT, PAGE_READWRITE);
    if (!NT_SUCCESS(st) || !base) return NULL;
    return base;
}

static void ldr_free_image(void *p, size_t size)
{
    if (!p) return;
    PVOID base = p;
    SIZE_T sz = size;
    NtFreeVirtualMemory((HANDLE)(intptr_t)-1, &base, &sz, MEM_RELEASE);
}
#else
static int ldr_read_file(const char *path, u8 **out_data, size_t *out_size)
{
    int fd = open(path, O_RDONLY | O_BINARY);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) < 0) { close(fd); return -1; }
    size_t sz = (size_t)st.st_size;
    u8 *buf = malloc(sz ? sz : 1);
    if (!buf) { close(fd); return -1; }
    size_t total = 0;
    while (total < sz) {
        ssize_t n = read(fd, buf + total, sz - total);
        if (n <= 0) { free(buf); close(fd); return -1; }
        total += (size_t)n;
    }
    close(fd);
    *out_data = buf;
    *out_size = sz;
    return 0;
}

static int ldr_file_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

/* -------------------------------------------------------------------------- */
/* Image mapping                                                              */
/* -------------------------------------------------------------------------- */

static void *ldr_alloc_image(size_t size, size_t align)
{
    if (align < 4096) align = 4096;
    size = (size + align - 1) & ~(align - 1);
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return NULL;
    return p;
}

static void ldr_free_image(void *p, size_t size)
{
    if (p) munmap(p, size);
}
#endif

static NTSTATUS ldr_apply_relocs(ldr_module_t *mod)
{
    pe_context_t *ctx = &mod->pe;
    u8 *base = mod->image;
    LONG64 delta = (LONG64)(base - (u8 *)mod->entry.OriginalBase);
    if (delta == 0) return STATUS_SUCCESS;

    pe_data_directory_t dd;
    pe_status_t s = pe_directory_get(ctx, IMAGE_DIRECTORY_ENTRY_BASERELOC, &dd);
    if (s == PE_OK && dd.rva == 0 && dd.size == 0) return STATUS_SUCCESS;
    if (s != PE_OK) return STATUS_INVALID_IMAGE_FORMAT;

    u32 reloc_off;
    s = pe_rva_to_offset(ctx, dd.rva, &reloc_off);
    if (s != PE_OK) return STATUS_INVALID_IMAGE_FORMAT;
    /* LDR-5: the directory must lie inside the file. */
    if (!pe_range_fits(reloc_off, dd.size, ctx->size)) return STATUS_INVALID_IMAGE_FORMAT;

    u32 cursor = 0;
    while (cursor < dd.size) {
        IMAGE_BASE_RELOCATION block;
        s = pe_read_base_relocation(ctx, reloc_off + cursor, &block);
        if (s != PE_OK) return STATUS_INVALID_IMAGE_FORMAT;
        if (block.SizeOfBlock == 0) break;
        if (block.SizeOfBlock < sizeof(block)) return STATUS_INVALID_IMAGE_FORMAT;
        /* LDR-5: the block, and so each entry read from it, must lie inside
         * the directory. */
        if (!pe_range_fits(cursor, block.SizeOfBlock, dd.size)) return STATUS_INVALID_IMAGE_FORMAT;

        u32 n = (block.SizeOfBlock - sizeof(block)) / sizeof(u16);
        for (u32 i = 0; i < n; ++i) {
            u32 off = reloc_off + cursor + sizeof(block) + i * sizeof(u16);
            u16 e = pe_file_u16(ctx, off);
            u16 type = (e >> 12) & 0xF;
            u16 offset = e & 0xFFF;
            if (type == IMAGE_REL_BASED_ABSOLUTE) continue;
            if (type != IMAGE_REL_BASED_DIR64 &&
                type != IMAGE_REL_BASED_HIGHLOW) {
                return STATUS_INVALID_IMAGE_FORMAT;
            }
            /* LDR-5: the write target must lie inside the image mapping. */
            unsigned long long target = (unsigned long long)block.VirtualAddress + offset;
            unsigned long long width = (type == IMAGE_REL_BASED_DIR64) ? 8 : 4;
            if (!pe_range_fits(target, width, mod->image_size)) return STATUS_INVALID_IMAGE_FORMAT;
            u8 *addr = base + block.VirtualAddress + offset;
            if (type == IMAGE_REL_BASED_DIR64) {
                u64 v = pe_load_u64(addr);
                v = (u64)((LONG64)v + delta);
                memcpy(addr, &v, sizeof(v));
            } else {
                u32 v = pe_load_u32(addr);
                v = (u32)((LONG64)v + delta);
                memcpy(addr, &v, sizeof(v));
            }
        }
        cursor += block.SizeOfBlock;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS ldr_map_sections(ldr_module_t *mod)
{
    pe_context_t *ctx = &mod->pe;
    u8 *base = mod->image;

    /* Copy headers. */
    size_t hdr_bytes = ctx->size_of_headers;
    if (hdr_bytes > mod->file_size) hdr_bytes = mod->file_size;
    memcpy(base, mod->file_data, hdr_bytes);

    u16 nsec;
    pe_section_count(ctx, &nsec);
    for (u16 i = 0; i < nsec; ++i) {
        pe_section_view_t sv;
        pe_status_t s = pe_section_get(ctx, i, &sv);
        if (s != PE_OK) return STATUS_INVALID_IMAGE_FORMAT;

        /* LDR-6: the section must lie inside the image mapping, and the bytes
         * copied from the file must lie inside the file. A raw size larger
         * than the virtual size copies only the virtual size. */
        unsigned long long vs = sv.virtual_size ? sv.virtual_size : sv.file_size;
        if (!pe_range_fits(sv.virtual_address, vs, mod->image_size)) return STATUS_INVALID_IMAGE_FORMAT;
        unsigned long long copy = pe_copy_len(sv.file_size, sv.virtual_size);
        if (!pe_range_fits(sv.file_offset, copy, mod->file_size)) return STATUS_INVALID_IMAGE_FORMAT;

        u8 *va = base + sv.virtual_address;
        if (copy > 0)
            memcpy(va, mod->file_data + sv.file_offset, (size_t)copy);

        /* Zero-fill tail of virtual size. */
        if (vs > copy) {
            size_t z = (size_t)(vs - copy);
            memset(va + copy, 0, z);
        }
    }

    /* Zero-fill remainder of image. */
    if (mod->image_size > ctx->size_of_image)
        memset(base + ctx->size_of_image, 0, mod->image_size - ctx->size_of_image);

    return STATUS_SUCCESS;
}

/* -------------------------------------------------------------------------- */
/* Module mapping                                                             */
/* -------------------------------------------------------------------------- */

static NTSTATUS ldr_map_and_load(winldr_ctx_t *ctx,
                                 const char *path,
                                 ULONG flags,
                                 ldr_module_t **out)
{
    (void)ctx;
    ldr_module_t *mod = calloc(1, sizeof(*mod));
    if (!mod) return STATUS_NO_MEMORY;

    if (ldr_read_file(path, &mod->file_data, &mod->file_size) < 0) {
        free(mod);
        return STATUS_NO_SUCH_FILE;
    }

    pe_status_t ps = pe_context_init(&mod->pe, mod->file_data, mod->file_size);
    if (ps != PE_OK) goto fail;

    ps = pe_validate(&mod->pe);
    if (ps != PE_OK) goto fail;

    /* Allocate image memory. */
    size_t align = mod->pe.section_alignment;
    if (align < 4096) align = 4096;
    mod->image_size = (mod->pe.size_of_image + align - 1) & ~(align - 1);
    mod->image = ldr_alloc_image(mod->image_size, align);
    if (!mod->image) {
        free(mod);
        return STATUS_NO_MEMORY;
    }

    mod->entry.OriginalBase = (PVOID)mod->pe.image_base;
    mod->entry.DllBase = mod->image;
    mod->entry.SizeOfImage = mod->pe.size_of_image;
    if (mod->pe.entry_point)
        mod->entry.EntryPoint = mod->image + mod->pe.entry_point;
    mod->entry.TimeDateStamp = 0; /* read later from file header */
    mod->entry.Flags = 0;

    /* Read TimeDateStamp from file header. */
    IMAGE_FILE_HEADER fh;
    u32 fh_off = mod->pe.nt_off + 4;
    if (pe_check_read(&mod->pe, fh_off, sizeof(fh)) == PE_OK) {
        mod->entry.TimeDateStamp = pe_file_u32(&mod->pe, fh_off + 4);
    }

    NTSTATUS s = ldr_map_sections(mod);
    if (!NT_SUCCESS(s)) goto fail;

    s = ldr_apply_relocs(mod);
    if (!NT_SUCCESS(s)) goto fail;

    if (flags & LDR_DLL_LOAD_AS_DATAFILE)
        mod->datafile = 1;
    if (flags & LDR_DLL_PIN)
        mod->pinned = 1;

    /* Set names - allocate heap buffers for UNICODE_STRING (was stack - ASAN bug). */
    ldr_set_base_name(mod, path);
    size_t path_len = strlen(path);
    WCHAR *wname = (WCHAR*)malloc((path_len + 1) * sizeof(WCHAR));
    if (!wname) goto fail;
    ldr_ascii_to_wcs(path, wname, path_len + 1);
    ldr_set_unicode_string(&mod->entry.FullDllName, wname,
                           (USHORT)(path_len * sizeof(WCHAR)));

    size_t base_len = strlen(mod->base_name_ansi);
    WCHAR *wbase = (WCHAR*)malloc((base_len + 1) * sizeof(WCHAR));
    if (!wbase) { free(wname); goto fail; }
    ldr_ascii_to_wcs(mod->base_name_ansi, wbase, base_len + 1);
    ldr_set_unicode_string(&mod->entry.BaseDllName, wbase,
                           (USHORT)(base_len * sizeof(WCHAR)));

    /* TLS directory. */
    IMAGE_TLS_DIRECTORY64 tls;
    if (pe_tls_directory(&mod->pe, &tls) == PE_OK) {
        mod->tls = tls;
        mod->has_tls = 1;
    }

    *out = mod;
    return STATUS_SUCCESS;

fail:
    if (mod->entry.FullDllName.Buffer) free(mod->entry.FullDllName.Buffer);
    if (mod->entry.BaseDllName.Buffer) free(mod->entry.BaseDllName.Buffer);
    if (mod->image) ldr_free_image(mod->image, mod->image_size);
    free(mod->file_data);
    free(mod);
    return STATUS_INVALID_IMAGE_FORMAT;
}

/* -------------------------------------------------------------------------- */
/* Module list iteration helpers                                              */
/* -------------------------------------------------------------------------- */

PLDR_DATA_TABLE_ENTRY winldr_first_module(winldr_ctx_t *ctx, int load_order)
{
    LIST_ENTRY *head;
    if (load_order == 0)
        head = &ctx->ldr_data.InLoadOrderModuleList;
    else if (load_order == 1)
        head = &ctx->ldr_data.InMemoryOrderModuleList;
    else
        head = &ctx->ldr_data.InInitializationOrderModuleList;

    if (IsListEmpty(head)) return NULL;
    if (load_order == 0)
        return CONTAINING_RECORD(head->Flink, LDR_DATA_TABLE_ENTRY, InLoadOrderLinks);
    if (load_order == 1)
        return CONTAINING_RECORD(head->Flink, LDR_DATA_TABLE_ENTRY, InMemoryOrderLinks);
    return CONTAINING_RECORD(head->Flink, LDR_DATA_TABLE_ENTRY, InInitializationOrderLinks);
}

PLDR_DATA_TABLE_ENTRY winldr_next_module(winldr_ctx_t *ctx, PLDR_DATA_TABLE_ENTRY cur, int load_order)
{
    LIST_ENTRY *next;
    LIST_ENTRY *head;
    if (load_order == 0) {
        next = cur->InLoadOrderLinks.Flink;
        head = &ctx->ldr_data.InLoadOrderModuleList;
    } else if (load_order == 1) {
        next = cur->InMemoryOrderLinks.Flink;
        head = &ctx->ldr_data.InMemoryOrderModuleList;
    } else {
        next = cur->InInitializationOrderLinks.Flink;
        head = &ctx->ldr_data.InInitializationOrderModuleList;
    }

    if (next == head) return NULL;

    if (load_order == 0)
        return CONTAINING_RECORD(next, LDR_DATA_TABLE_ENTRY, InLoadOrderLinks);
    if (load_order == 1)
        return CONTAINING_RECORD(next, LDR_DATA_TABLE_ENTRY, InMemoryOrderLinks);
    return CONTAINING_RECORD(next, LDR_DATA_TABLE_ENTRY, InInitializationOrderLinks);
}

PLDR_DATA_TABLE_ENTRY winldr_find_module(winldr_ctx_t *ctx, const char *name)
{
    ldr_module_t *m = ldr_lookup_by_name(ctx, name);
    return m ? &m->entry : NULL;
}

/* -------------------------------------------------------------------------- */
/* Export resolution                                                          */
/* -------------------------------------------------------------------------- */

static pe_status_t ldr_export_lookup_cb(void *user, const pe_export_entry_t *ent) __attribute__((unused));
static pe_status_t ldr_export_lookup_cb(void *user, const pe_export_entry_t *ent)
{
    pe_export_entry_t **out = user;
    if (*out) return PE_OK; /* already found */
    *out = (pe_export_entry_t *)ent;
    return PE_OK;
}

static NTSTATUS ldr_resolve_export_in_module(winldr_ctx_t *ctx,
                                             ldr_module_t *mod,
                                             const char *name,
                                             WORD ordinal,
                                             int by_ordinal,
                                             ULONG_PTR *out_addr,
                                             char *forwarder_buf,
                                             size_t forwarder_buf_size);

static NTSTATUS ldr_resolve_export(winldr_ctx_t *ctx,
                                   ldr_module_t *mod,
                                   const char *name,
                                   WORD ordinal,
                                   int by_ordinal,
                                   ULONG_PTR *out_addr,
                                   char *forwarder_buf,
                                   size_t forwarder_buf_size,
                                   int depth)
{
    if (depth > 32) return STATUS_INVALID_IMAGE_FORMAT; /* cycle / too deep */

    char fwd[512];
    NTSTATUS s = ldr_resolve_export_in_module(ctx, mod, name, ordinal, by_ordinal,
                                              out_addr, fwd, sizeof(fwd));
    if (!NT_SUCCESS(s)) return s;

    if (fwd[0] == '\0') {
        /* Direct export. */
        return STATUS_SUCCESS;
    }

    /* Forwarder string: "DLL.Name" or "DLL.#ordinal" or "!Name". */
    const char *spec = fwd;
    if (*spec == '!') ++spec;

    const char *dot = strrchr(spec, '.');
    if (!dot) return STATUS_PROCEDURE_NOT_FOUND;

    char dll_name[256];
    size_t dll_len = (size_t)(dot - spec);
    if (dll_len >= sizeof(dll_name)) return STATUS_PROCEDURE_NOT_FOUND;
    memcpy(dll_name, spec, dll_len);
    dll_name[dll_len] = '\0';
    if (!ldr_ends_with_dll(dll_name)) {
        strncat(dll_name, ".dll", sizeof(dll_name) - strlen(dll_name) - 1);
    }

    /* Resolve the host DLL. */
    HMODULE host;
    WCHAR wdll[260];
    ldr_ascii_to_wcs(dll_name, wdll, sizeof(wdll) / sizeof(wdll[0]));
    s = ldr_load_dll_internal(ctx, NULL, 0, dll_name, &host);
    if (!NT_SUCCESS(s)) return s;

    ldr_module_t *host_mod = (ldr_module_t *)((char *)host - offsetof(ldr_module_t, entry));

    /* Target export name / ordinal. */
    const char *target = dot + 1;
    int target_by_ordinal = 0;
    WORD target_ord = 0;
    char target_name[256];
    if (target[0] == '#') {
        target_by_ordinal = 1;
        target_ord = (WORD)strtoul(target + 1, NULL, 10);
    } else {
        strncpy(target_name, target, sizeof(target_name) - 1);
        target_name[sizeof(target_name) - 1] = '\0';
    }

    return ldr_resolve_export(ctx, host_mod,
                              target_by_ordinal ? NULL : target_name,
                              target_ord, target_by_ordinal,
                              out_addr, forwarder_buf, forwarder_buf_size,
                              depth + 1);
}

static NTSTATUS ldr_resolve_export_in_module(winldr_ctx_t *ctx,
                                             ldr_module_t *mod,
                                             const char *name,
                                             WORD ordinal,
                                             int by_ordinal,
                                             ULONG_PTR *out_addr,
                                             char *forwarder_buf,
                                             size_t forwarder_buf_size)
{
    (void)ctx;
    forwarder_buf[0] = '\0';

    pe_data_directory_t dd;
    pe_status_t ps = pe_directory_get(&mod->pe, IMAGE_DIRECTORY_ENTRY_EXPORT, &dd);
    if (ps != PE_OK || dd.rva == 0 || dd.size == 0)
        return STATUS_PROCEDURE_NOT_FOUND;

    u32 exp_off;
    ps = pe_rva_to_offset(&mod->pe, dd.rva, &exp_off);
    if (ps != PE_OK) return STATUS_INVALID_IMAGE_FORMAT;

    IMAGE_EXPORT_DIRECTORY ed;
    ps = pe_read_export_directory(&mod->pe, exp_off, &ed);
    if (ps != PE_OK) return STATUS_INVALID_IMAGE_FORMAT;

    u32 func_index = (u32)-1;

    if (by_ordinal) {
        if (ordinal < ed.Base) return STATUS_ORDINAL_NOT_FOUND;
        u32 idx = ordinal - ed.Base;
        if (idx >= ed.NumberOfFunctions) return STATUS_ORDINAL_NOT_FOUND;
        func_index = idx;
    } else {
        /* Hint-first binary search-ish linear scan (table is small in tests). */
        u32 names_off, ords_off;
        ps = pe_rva_to_offset(&mod->pe, ed.AddressOfNames, &names_off);
        if (ps != PE_OK) return STATUS_PROCEDURE_NOT_FOUND;
        ps = pe_rva_to_offset(&mod->pe, ed.AddressOfNameOrdinals, &ords_off);
        if (ps != PE_OK) return STATUS_PROCEDURE_NOT_FOUND;

        for (u32 i = 0; i < ed.NumberOfNames; ++i) {
            u32 name_rva_ptr = names_off + i * 4;
            if (pe_check_read(&mod->pe, name_rva_ptr, 4) != PE_OK) continue;
            u32 name_rva = pe_file_u32(&mod->pe, name_rva_ptr);
            const char *sym;
            size_t sym_len;
            u32 name_file_off;
            if (pe_rva_to_offset(&mod->pe, name_rva, &name_file_off) != PE_OK)
                continue;
            if (pe_read_asciiz(&mod->pe, name_file_off, &sym, &sym_len) != PE_OK)
                continue;
            if (ldr_strcasecmp(sym, name) == 0) {
                u16 ord = pe_file_u16(&mod->pe, ords_off + i * 2);
                func_index = ord;
                break;
            }
        }
    }

    if (func_index == (u32)-1) return STATUS_PROCEDURE_NOT_FOUND;
    if (func_index >= ed.NumberOfFunctions) return STATUS_PROCEDURE_NOT_FOUND;

    u32 funcs_off;
    ps = pe_rva_to_offset(&mod->pe, ed.AddressOfFunctions, &funcs_off);
    if (ps != PE_OK) return STATUS_INVALID_IMAGE_FORMAT;

    u32 rva = pe_file_u32(&mod->pe, funcs_off + func_index * 4);
    if (rva == 0) return STATUS_PROCEDURE_NOT_FOUND;

    if (rva >= dd.rva && rva < dd.rva + dd.size) {
        /* Forwarder. */
        u32 fwd_off;
        ps = pe_rva_to_offset(&mod->pe, rva, &fwd_off);
        if (ps != PE_OK) return STATUS_INVALID_IMAGE_FORMAT;
        const char *fs;
        size_t fs_len;
        ps = pe_read_asciiz(&mod->pe, fwd_off, &fs, &fs_len);
        if (ps != PE_OK) return STATUS_INVALID_IMAGE_FORMAT;
        if (fs_len >= forwarder_buf_size) return STATUS_INVALID_IMAGE_FORMAT;
        memcpy(forwarder_buf, fs, fs_len);
        forwarder_buf[fs_len] = '\0';
        return STATUS_SUCCESS;
    }

    *out_addr = (ULONG_PTR)(mod->image + rva);
    return STATUS_SUCCESS;
}

/* -------------------------------------------------------------------------- */
/* Import resolution                                                          */
/* -------------------------------------------------------------------------- */

static NTSTATUS ldr_resolve_thunk(winldr_ctx_t *ctx,
                                  ldr_module_t *dep,
                                  const pe_import_symbol_t *sym,
                                  ULONG_PTR *out_addr)
{
    int by_ord = sym->ordinal_flag != 0;
    WORD ord = sym->ordinal;
    const char *name = sym->name;

    /* Try hint first for named imports. */
    if (!by_ord && sym->hint != 0) {
        /* Hint lookup is a performance optimization; we just do name lookup. */
    }

    return ldr_resolve_export(ctx, dep, name, ord, by_ord, out_addr, NULL, 0, 0);
}

static NTSTATUS ldr_load_dependency(winldr_ctx_t *ctx,
                                    const char *dll_name,
                                    ldr_module_t **out_mod)
{
    /* API set resolution. */
    const char *resolved = winldr_apiset_resolve(ctx, dll_name);
    const char *load_name = resolved ? resolved : dll_name;

    /* Check if already loaded. */
    ldr_module_t *m = ldr_lookup_by_name(ctx, load_name);
    if (m) {
        *out_mod = m;
        return STATUS_SUCCESS;
    }

    HMODULE h;
    NTSTATUS s = ldr_load_dll_internal(ctx, NULL, 0, load_name, &h);
    if (!NT_SUCCESS(s)) return s;

    *out_mod = (ldr_module_t *)((char *)h - offsetof(ldr_module_t, entry));
    return STATUS_SUCCESS;
}

static NTSTATUS ldr_resolve_imports(winldr_ctx_t *ctx, ldr_module_t *mod)
{
    if (mod->datafile) return STATUS_SUCCESS;

    pe_data_directory_t imp_dir;
    pe_status_t ps = pe_directory_get(&mod->pe, IMAGE_DIRECTORY_ENTRY_IMPORT, &imp_dir);
    if (ps != PE_OK || imp_dir.rva == 0 || imp_dir.size == 0)
        return STATUS_SUCCESS;

    u32 imp_off;
    ps = pe_rva_to_offset(&mod->pe, imp_dir.rva, &imp_off);
    if (ps != PE_OK) return STATUS_INVALID_IMAGE_FORMAT;

    u32 max_desc = imp_dir.size / sizeof(IMAGE_IMPORT_DESCRIPTOR);
    for (u32 d = 0; d < max_desc; ++d) {
        u32 desc_off = imp_off + d * sizeof(IMAGE_IMPORT_DESCRIPTOR);
        IMAGE_IMPORT_DESCRIPTOR desc;
        ps = pe_read_import_descriptor(&mod->pe, desc_off, &desc);
        if (ps != PE_OK) return STATUS_INVALID_IMAGE_FORMAT;

        if (desc.DUMMYUNIONNAME.OriginalFirstThunk == 0 && desc.TimeDateStamp == 0 &&
            desc.ForwarderChain == 0 && desc.Name == 0 && desc.FirstThunk == 0)
            break;

        u32 name_off;
        ps = pe_rva_to_offset(&mod->pe, desc.Name, &name_off);
        if (ps != PE_OK) return STATUS_INVALID_IMAGE_FORMAT;
        const char *dll_name;
        size_t dll_len;
        ps = pe_read_asciiz(&mod->pe, name_off, &dll_name, &dll_len);
        if (ps != PE_OK || dll_len == 0) return STATUS_INVALID_IMAGE_FORMAT;

        char dep_name[256];
        if (dll_len >= sizeof(dep_name)) return STATUS_INVALID_IMAGE_FORMAT;
        memcpy(dep_name, dll_name, dll_len);
        dep_name[dll_len] = '\0';

        /* Staleness check for bound imports. */
        if (desc.TimeDateStamp != 0 && desc.TimeDateStamp != 0xFFFFFFFF) {
            /* We would check target DLL timestamp here.  For simplicity and
             * safety we always re-resolve (matches Phase 1 policy). */
        }

        ldr_module_t *dep;
        NTSTATUS s = ldr_load_dependency(ctx, dep_name, &dep);
        if (!NT_SUCCESS(s)) return s;

        /* Resolve IAT.  The ILT is read through its file offset.  The IAT is
         * written at its RVA in mod->image (LDR-2, decision 0030). */
        u32 ilt_rva = desc.DUMMYUNIONNAME.OriginalFirstThunk ? desc.DUMMYUNIONNAME.OriginalFirstThunk : desc.FirstThunk;
        u32 ilt_off;
        ps = pe_rva_to_offset(&mod->pe, ilt_rva, &ilt_off);
        if (ps != PE_OK) return STATUS_INVALID_IMAGE_FORMAT;
        unsigned long long iat_rva = ldr_iat_base_rva(desc.FirstThunk, ilt_rva);
        if (iat_rva == 0) return STATUS_INVALID_IMAGE_FORMAT;

        for (u32 t = 0; ; ++t) {
            u32 thunk_off = ilt_off + t * sizeof(IMAGE_THUNK_DATA64);
            IMAGE_THUNK_DATA64 thunk;
            ps = pe_read_thunk_data64(&mod->pe, thunk_off, &thunk);
            if (ps != PE_OK) return STATUS_INVALID_IMAGE_FORMAT;
            if (thunk.u1.Ordinal == 0) break;

            pe_import_symbol_t sym = {0};
            if (thunk.u1.Ordinal & IMAGE_ORDINAL_FLAG64) {
                sym.ordinal_flag = IMAGE_ORDINAL_FLAG64;
                sym.ordinal = (u16)(thunk.u1.Ordinal & 0xFFFFULL);
            } else {
                u32 ibn_off;
                ps = pe_rva_to_offset(&mod->pe, (u32)thunk.u1.Ordinal, &ibn_off);
                if (ps != PE_OK) return STATUS_INVALID_IMAGE_FORMAT;
                if (pe_check_read(&mod->pe, ibn_off, sizeof(u16) + 1) != PE_OK)
                    return STATUS_INVALID_IMAGE_FORMAT;
                sym.hint = pe_file_u16(&mod->pe, ibn_off);
                ps = pe_read_asciiz(&mod->pe, ibn_off + 2, &sym.name, &sym.name_len);
                if (ps != PE_OK) return STATUS_INVALID_IMAGE_FORMAT;
            }

            ULONG_PTR addr;
            s = ldr_resolve_thunk(ctx, dep, &sym, &addr);
            if (!NT_SUCCESS(s)) return s;

            /* Patch IAT slot at its RVA; the slot must lie in the image. */
            if (!ldr_iat_write(mod->image, mod->image_size, iat_rva, t, (unsigned long long)addr))
                return STATUS_INVALID_IMAGE_FORMAT;
        }
    }

    return STATUS_SUCCESS;
}

/* -------------------------------------------------------------------------- */
/* Delay-load imports                                                         */
/* -------------------------------------------------------------------------- */

NTSTATUS ldr_resolve_delay_imports(winldr_ctx_t *ctx, HMODULE module)
{
    ldr_module_t *mod = (ldr_module_t *)((char *)module - offsetof(ldr_module_t, entry));
    if (mod->datafile) return STATUS_SUCCESS;

    pe_data_directory_t didat;
    pe_status_t ps = pe_directory_get(&mod->pe, IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT, &didat);
    if (ps != PE_OK || didat.rva == 0 || didat.size == 0)
        return STATUS_SUCCESS;

    u32 desc_off;
    ps = pe_rva_to_offset(&mod->pe, didat.rva, &desc_off);
    if (ps != PE_OK) return STATUS_INVALID_IMAGE_FORMAT;

    u32 max_desc = didat.size / sizeof(IMAGE_DELAYLOAD_DESCRIPTOR);
    for (u32 d = 0; d < max_desc; ++d) {
        u32 off = desc_off + d * sizeof(IMAGE_DELAYLOAD_DESCRIPTOR);
        IMAGE_DELAYLOAD_DESCRIPTOR desc;
        ps = pe_check_read(&mod->pe, off, sizeof(desc));
        if (ps != PE_OK) return STATUS_INVALID_IMAGE_FORMAT;
        const u8 *p = mod->pe.data + off;
        desc.Attributes.AllAttributes = pe_load_u32(p + 0);
        desc.DllNameRVA = pe_load_u32(p + 4);
        desc.ModuleHandleRVA = pe_load_u32(p + 8);
        desc.ImportAddressTableRVA = pe_load_u32(p + 12);
        desc.ImportNameTableRVA = pe_load_u32(p + 16);
        desc.BoundImportAddressTableRVA = pe_load_u32(p + 20);
        desc.UnloadImportAddressTableRVA = pe_load_u32(p + 24);
        desc.TimeDateStamp = pe_load_u32(p + 28);

        if (desc.DllNameRVA == 0 && desc.Attributes.AllAttributes == 0)
            break;

        u32 name_off;
        ps = pe_rva_to_offset(&mod->pe, desc.DllNameRVA, &name_off);
        if (ps != PE_OK) return STATUS_INVALID_IMAGE_FORMAT;
        const char *dll_name;
        size_t dll_len;
        ps = pe_read_asciiz(&mod->pe, name_off, &dll_name, &dll_len);
        if (ps != PE_OK) return STATUS_INVALID_IMAGE_FORMAT;

        char dep_name[256];
        if (dll_len >= sizeof(dep_name)) return STATUS_INVALID_IMAGE_FORMAT;
        memcpy(dep_name, dll_name, dll_len);
        dep_name[dll_len] = '\0';

        ldr_module_t *dep;
        NTSTATUS s = ldr_load_dependency(ctx, dep_name, &dep);
        if (!NT_SUCCESS(s)) return s;

        /* Patch module handle slot. */
        if (desc.ModuleHandleRVA) {
            HMODULE *pmh = (HMODULE *)(mod->image + desc.ModuleHandleRVA);
            *pmh = (HMODULE)&dep->entry;
        }

        /* The IAT is written at its RVA in mod->image (LDR-2, decision 0030). */
        u32 ilt_off;
        ps = pe_rva_to_offset(&mod->pe, desc.ImportNameTableRVA, &ilt_off);
        if (ps != PE_OK) return STATUS_INVALID_IMAGE_FORMAT;
        unsigned long long iat_rva = desc.ImportAddressTableRVA;
        if (iat_rva == 0) return STATUS_INVALID_IMAGE_FORMAT;

        for (u32 t = 0; ; ++t) {
            u32 thunk_off = ilt_off + t * sizeof(IMAGE_THUNK_DATA64);
            IMAGE_THUNK_DATA64 thunk;
            ps = pe_read_thunk_data64(&mod->pe, thunk_off, &thunk);
            if (ps != PE_OK) return STATUS_INVALID_IMAGE_FORMAT;
            if (thunk.u1.Ordinal == 0) break;

            pe_import_symbol_t sym = {0};
            if (thunk.u1.Ordinal & IMAGE_ORDINAL_FLAG64) {
                sym.ordinal_flag = IMAGE_ORDINAL_FLAG64;
                sym.ordinal = (u16)(thunk.u1.Ordinal & 0xFFFFULL);
            } else {
                u32 ibn_off;
                ps = pe_rva_to_offset(&mod->pe, (u32)thunk.u1.Ordinal, &ibn_off);
                if (ps != PE_OK) return STATUS_INVALID_IMAGE_FORMAT;
                sym.hint = pe_file_u16(&mod->pe, ibn_off);
                ps = pe_read_asciiz(&mod->pe, ibn_off + 2, &sym.name, &sym.name_len);
                if (ps != PE_OK) return STATUS_INVALID_IMAGE_FORMAT;
            }

            ULONG_PTR addr;
            s = ldr_resolve_thunk(ctx, dep, &sym, &addr);
            if (!NT_SUCCESS(s)) return s;

            if (!ldr_iat_write(mod->image, mod->image_size, iat_rva, t, (unsigned long long)addr))
                return STATUS_INVALID_IMAGE_FORMAT;
        }
    }

    return STATUS_SUCCESS;
}

/* -------------------------------------------------------------------------- */
/* DllMain / TLS callback orchestration                                       */
/* -------------------------------------------------------------------------- */

static void ldr_run_tls_callbacks(winldr_ctx_t *ctx, ldr_module_t *mod, ULONG reason)
{
    if (!mod->has_tls || mod->datafile) return;

    /* AddressOfCallBacks is a VA on the preferred base (the directory is read
     * from the file, before relocation).  The table is read from mod->image at
     * VA - OriginalBase, bounded by the image (LDR-4, decision 0032).  A table
     * outside the image is not read. */
    unsigned int table_rva = 0;
    ULONG_PTR cb_table_va = (ULONG_PTR)mod->tls.AddressOfCallBacks;
    if (ldr_tls_cb_table_rva((unsigned long long)cb_table_va,
                             (unsigned long long)(ULONG_PTR)mod->entry.OriginalBase,
                             mod->image_size, &table_rva) != LDR_TLS_CB_OK)
        return;

    /* Entries are relocated VAs in the image.  The loop stops at the zero
     * terminator or at the image end. */
    for (unsigned long long i = 0; ; ++i) {
        unsigned long long cb_va = 0;
        if (!ldr_tls_cb_entry(mod->image, mod->image_size, table_rva, i, &cb_va))
            break;
        PIMAGE_TLS_CALLBACK cb = (PIMAGE_TLS_CALLBACK)(ULONG_PTR)cb_va;
        ldr_log_call(ctx, mod, reason, 1, (ULONG_PTR)cb);
        if (ctx->tls_callback_hook)
            ctx->tls_callback_hook(ctx, &mod->entry, reason, NULL, cb,
                                   ctx->tls_callback_user);
    }
}

static void ldr_run_dll_main(winldr_ctx_t *ctx, ldr_module_t *mod, ULONG reason)
{
    if (mod->datafile) return;

    ldr_log_call(ctx, mod, reason, 0, 0);
    if (ctx->dll_main_hook)
        ctx->dll_main_hook(ctx, &mod->entry, reason, NULL, ctx->dll_main_user);
}

static NTSTATUS ldr_process_attach(winldr_ctx_t *ctx, ldr_module_t *mod)
{
    if (mod->attached || mod->datafile) return STATUS_SUCCESS;

    /* Detect re-entry on the init stack (cycle). */
    for (size_t i = 0; i < ctx->init_stack_depth; ++i) {
        if (ctx->init_stack[i] == mod) return STATUS_SUCCESS;
    }

    ctx->init_stack[ctx->init_stack_depth++] = mod;

    ldr_run_tls_callbacks(ctx, mod, DLL_PROCESS_ATTACH);
    ldr_run_dll_main(ctx, mod, DLL_PROCESS_ATTACH);
    mod->attached = 1;

    --ctx->init_stack_depth;
    return STATUS_SUCCESS;
}

static NTSTATUS ldr_process_detach(winldr_ctx_t *ctx, ldr_module_t *mod)
{
    if (!mod->attached || mod->datafile) return STATUS_SUCCESS;
    ldr_run_dll_main(ctx, mod, DLL_PROCESS_DETACH);
    mod->attached = 0;
    return STATUS_SUCCESS;
}

/* -------------------------------------------------------------------------- */
/* Search order                                                               */
/* -------------------------------------------------------------------------- */

static int ldr_is_known_dll(winldr_ctx_t *ctx, const char *name)
{
    for (size_t i = 0; i < ctx->known_dll_count; ++i) {
        if (ldr_strcasecmp(ctx->known_dlls[i], name) == 0) return 1;
    }
    return 0;
}

static int ldr_try_path(char *out, size_t outsz, const char *dir, const char *name)
{
#ifdef __MINGW32__
    size_t dlen = strlen(dir);
    size_t nlen = strlen(name);
    if (dlen + 1 + nlen + 1 > outsz) return 0;
    memcpy(out, dir, dlen);
    out[dlen] = '/';
    memcpy(out + dlen + 1, name, nlen);
    out[dlen + 1 + nlen] = '\0';
    return ldr_file_exists(out);
#else
    int n = snprintf(out, outsz, "%s/%s", dir, name);
    return (n > 0 && (size_t)n < outsz && ldr_file_exists(out));
#endif
}

static NTSTATUS ldr_find_dll_path(winldr_ctx_t *ctx,
                                  const char *search_path,
                                  const char *name,
                                  char *out,
                                  size_t outsz)
{
    char lower[256];
    strncpy(lower, name, sizeof(lower) - 1);
    lower[sizeof(lower) - 1] = '\0';
    ldr_str_to_lower(lower);
    if (!ldr_ends_with_dll(lower)) {
        strncat(lower, ".dll", sizeof(lower) - strlen(lower) - 1);
    }

    /* 1. Search path override. */
    if (search_path && search_path[0]) {
        if (ldr_try_path(out, outsz, search_path, lower)) return STATUS_SUCCESS;
    }

    /* 2. Known DLLs -> system32. */
    if (ldr_is_known_dll(ctx, lower) && ctx->system32_dir) {
        if (ldr_try_path(out, outsz, ctx->system32_dir, lower))
            return STATUS_SUCCESS;
    }

    /* 3. Executable directory. */
    if (ctx->exe_dir) {
        if (ldr_try_path(out, outsz, ctx->exe_dir, lower)) return STATUS_SUCCESS;
    }

    /* 4. System32. */
    if (ctx->system32_dir) {
        if (ldr_try_path(out, outsz, ctx->system32_dir, lower))
            return STATUS_SUCCESS;
    }

    /* 5. PATH. */
    for (size_t i = 0; i < ctx->path_count; ++i) {
        if (ldr_try_path(out, outsz, ctx->path_dirs[i], lower))
            return STATUS_SUCCESS;
    }

    /* 6. Current working directory as last resort. */
    if (ldr_try_path(out, outsz, ".", lower)) return STATUS_SUCCESS;

    return STATUS_DLL_NOT_FOUND;
}

/* -------------------------------------------------------------------------- */
/* Core load / unload                                                         */
/* -------------------------------------------------------------------------- */

static NTSTATUS ldr_load_dll_internal(winldr_ctx_t *ctx,
                                      const char *search_path,
                                      ULONG flags,
                                      const char *name,
                                      HMODULE *out)
{
    /* Already loaded? */
    ldr_module_t *existing = ldr_lookup_by_name(ctx, name);
    if (existing) {
        if (!(flags & LDR_DLL_LOAD_AS_DATAFILE) && !existing->datafile)
            existing->entry.LoadCount++;
        if (out) *out = (HMODULE)&existing->entry;
        return STATUS_SUCCESS;
    }

    /* Cycle guard: if this module is currently loading, return a placeholder. */
    for (size_t i = 0; i < ctx->init_stack_depth; ++i) {
        if (ldr_strcasecmp(ctx->init_stack[i]->base_name_ansi, name) == 0) {
            if (out) *out = (HMODULE)&ctx->init_stack[i]->entry;
            return STATUS_SUCCESS;
        }
    }

    char path[1024];
    NTSTATUS s = ldr_find_dll_path(ctx, search_path, name, path, sizeof(path));
    if (!NT_SUCCESS(s)) return s;

    ldr_module_t *mod;
    s = ldr_map_and_load(ctx, path, flags, &mod);
    if (!NT_SUCCESS(s)) return s;

    mod->loading = 1;

    /* Insert into load-order and hash lists immediately so cycles can resolve. */
    InsertTailList(&ctx->ldr_data.InLoadOrderModuleList, &mod->entry.InLoadOrderLinks);
    InsertTailList(&ctx->ldr_data.InMemoryOrderModuleList, &mod->entry.InMemoryOrderLinks);
    ldr_insert_hash(ctx, mod);

    /* Resolve imports (loads dependencies recursively). */
    s = ldr_resolve_imports(ctx, mod);
    if (!NT_SUCCESS(s)) {
        RemoveEntryList(&mod->entry.InLoadOrderLinks);
        RemoveEntryList(&mod->entry.InMemoryOrderLinks);
        RemoveEntryList(&mod->entry.HashLinks);
        ldr_free_image(mod->image, mod->image_size);
        free(mod->file_data);
        free(mod);
        return s;
    }

    /* Delay imports are resolved lazily, but for datafile we skip. */
    if (!mod->datafile) {
        s = ldr_resolve_delay_imports(ctx, (HMODULE)&mod->entry);
        if (!NT_SUCCESS(s)) {
            /* Non-fatal for delay loads: real helper raises SEH on failure.
             * For the loader we leave IAT zeroed and continue. */
        }
    }

    mod->entry.LoadCount = (flags & LDR_DLL_LOAD_AS_DATAFILE) ? 0 : 1;
    mod->loading = 0;

    /* Run attach.  Dependencies were loaded recursively so their attach ran first. */
    if (!mod->datafile)
        ldr_process_attach(ctx, mod);

    /* Insert into initialization-order list *after* successful attach. */
    InsertTailList(&ctx->ldr_data.InInitializationOrderModuleList,
                   &mod->entry.InInitializationOrderLinks);

    if (out) *out = (HMODULE)&mod->entry;
    return STATUS_SUCCESS;
}

NTSTATUS LdrLoadDll(winldr_ctx_t *ctx,
                    const WCHAR *path,
                    ULONG flags,
                    const WCHAR *name,
                    HMODULE *out)
{
    if (!ctx || !name || !out) return STATUS_INVALID_PARAMETER;

    char search[1024] = {0};
    char nm[256] = {0};

    if (path) ldr_wcs_to_ascii(path, search, sizeof(search));
    ldr_wcs_to_ascii(name, nm, sizeof(nm));

    ldr_lock_acquire(ctx);
    NTSTATUS s = ldr_load_dll_internal(ctx, search[0] ? search : NULL, flags, nm, out);
    ldr_lock_release(ctx);
    return s;
}

NTSTATUS LdrGetDllHandle(winldr_ctx_t *ctx, const WCHAR *name, HMODULE *out)
{
    if (!ctx || !name || !out) return STATUS_INVALID_PARAMETER;
    char nm[256];
    ldr_wcs_to_ascii(name, nm, sizeof(nm));

    ldr_lock_acquire(ctx);
    ldr_module_t *m = ldr_lookup_by_name(ctx, nm);
    NTSTATUS s = m ? STATUS_SUCCESS : STATUS_DLL_NOT_FOUND;
    if (m) *out = (HMODULE)&m->entry;
    ldr_lock_release(ctx);
    return s;
}

NTSTATUS LdrUnloadDll(winldr_ctx_t *ctx, HMODULE module)
{
    if (!ctx || !module) return STATUS_INVALID_PARAMETER;

    ldr_lock_acquire(ctx);
    ldr_module_t *mod = (ldr_module_t *)((char *)module - offsetof(ldr_module_t, entry));

    if (mod->pinned) {
        if (mod->entry.LoadCount > 0) mod->entry.LoadCount--;
        ldr_lock_release(ctx);
        return STATUS_SUCCESS;
    }
    if (mod->datafile) {
        RemoveEntryList(&mod->entry.InLoadOrderLinks);
        RemoveEntryList(&mod->entry.InMemoryOrderLinks);
        RemoveEntryList(&mod->entry.InInitializationOrderLinks);
        RemoveEntryList(&mod->entry.HashLinks);
        if (mod->entry.FullDllName.Buffer) free(mod->entry.FullDllName.Buffer);
        if (mod->entry.BaseDllName.Buffer) free(mod->entry.BaseDllName.Buffer);
        ldr_free_image(mod->image, mod->image_size);
        free(mod->file_data);
        free(mod);
        ldr_lock_release(ctx);
        return STATUS_SUCCESS;
    }

    if (mod->entry.LoadCount > 0) mod->entry.LoadCount--;
    if (mod->entry.LoadCount > 0) {
        ldr_lock_release(ctx);
        return STATUS_SUCCESS;
    }

    ldr_process_detach(ctx, mod);

    RemoveEntryList(&mod->entry.InLoadOrderLinks);
    RemoveEntryList(&mod->entry.InMemoryOrderLinks);
    RemoveEntryList(&mod->entry.InInitializationOrderLinks);
    RemoveEntryList(&mod->entry.HashLinks);

    if (mod->entry.FullDllName.Buffer) free(mod->entry.FullDllName.Buffer);
    if (mod->entry.BaseDllName.Buffer) free(mod->entry.BaseDllName.Buffer);
    ldr_free_image(mod->image, mod->image_size);
    free(mod->file_data);
    free(mod);

    ldr_lock_release(ctx);
    return STATUS_SUCCESS;
}

NTSTATUS LdrGetProcedureAddress(winldr_ctx_t *ctx,
                                HMODULE module,
                                const ANSI_STRING *name,
                                WORD ordinal,
                                FARPROC *out)
{
    if (!ctx || !module || !out) return STATUS_INVALID_PARAMETER;

    ldr_module_t *mod = (ldr_module_t *)((char *)module - offsetof(ldr_module_t, entry));
    int by_ord = (name == NULL || name->Buffer == NULL);
    const char *n = by_ord ? NULL : name->Buffer;

    ldr_lock_acquire(ctx);
    ULONG_PTR addr;
    NTSTATUS s = ldr_resolve_export(ctx, mod, n, ordinal, by_ord, &addr, NULL, 0, 0);
    if (NT_SUCCESS(s)) *out = (FARPROC)addr;
    ldr_lock_release(ctx);
    return s;
}
#else
/* Freestanding minimal Ldr for ntdll.dll - no POSIX, no imports */
#include <string.h>
#undef DUMMYUNIONNAME
#undef DUMMYSTRUCTNAME
NTSTATUS LdrLoadDll(winldr_ctx_t *ctx, const WCHAR *path, ULONG flags, const WCHAR *name, HMODULE *out) { (void)ctx;(void)path;(void)flags;(void)name;(void)out; return STATUS_NOT_SUPPORTED; }
NTSTATUS LdrUnloadDll(winldr_ctx_t *ctx, HMODULE mod) { (void)ctx;(void)mod; return STATUS_SUCCESS; }
NTSTATUS LdrGetDllHandle(winldr_ctx_t *ctx, const WCHAR *name, HMODULE *out) { (void)ctx;(void)name;(void)out; return STATUS_DLL_NOT_FOUND; }
NTSTATUS LdrGetProcedureAddress(winldr_ctx_t *ctx, HMODULE mod, const ANSI_STRING *name, WORD ord, FARPROC *out) { (void)ctx;(void)mod;(void)name;(void)ord;(void)out; return STATUS_PROCEDURE_NOT_FOUND; }
NTSTATUS ldr_resolve_delay_imports(winldr_ctx_t *ctx, HMODULE mod) { (void)ctx;(void)mod; return STATUS_SUCCESS; }
winldr_ctx_t *winldr_ctx_create(void) { return NULL; }
void winldr_ctx_destroy(winldr_ctx_t *ctx) { (void)ctx; }
PPEB_LDR_DATA winldr_peb_ldr(winldr_ctx_t *ctx) { (void)ctx; return NULL; }
void winldr_set_dll_main_hook(winldr_ctx_t *ctx, ldr_dll_main_hook_t hook, void *user) { (void)ctx;(void)hook;(void)user; }
void winldr_set_tls_callback_hook(winldr_ctx_t *ctx, ldr_tls_callback_hook_t hook, void *user) { (void)ctx;(void)hook;(void)user; }
void winldr_set_exe_dir(winldr_ctx_t *ctx, const char *path) { (void)ctx;(void)path; }
void winldr_set_system32_dir(winldr_ctx_t *ctx, const char *path) { (void)ctx;(void)path; }
void winldr_register_known_dll(winldr_ctx_t *ctx, const char *name) { (void)ctx;(void)name; }
NTSTATUS winldr_load_apiset_schema(winldr_ctx_t *ctx, const char *path) { (void)ctx;(void)path; return STATUS_SUCCESS; }
const char *winldr_apiset_resolve(winldr_ctx_t *ctx, const char *name) { (void)ctx;(void)name; return NULL; }
PLDR_DATA_TABLE_ENTRY winldr_first_module(winldr_ctx_t *ctx, int order) { (void)ctx;(void)order; return NULL; }
PLDR_DATA_TABLE_ENTRY winldr_next_module(winldr_ctx_t *ctx, PLDR_DATA_TABLE_ENTRY cur, int order) { (void)ctx;(void)cur;(void)order; return NULL; }
PLDR_DATA_TABLE_ENTRY winldr_find_module(winldr_ctx_t *ctx, const char *name) { (void)ctx;(void)name; return NULL; }
const winldr_init_log_entry_t *winldr_get_init_log(winldr_ctx_t *ctx, size_t *count) { if(count) *count=0; return NULL; }
void winldr_clear_init_log(winldr_ctx_t *ctx) { (void)ctx; }
#endif
