/*
 * kshim.c - userspace definitions for nt object/handle/mm tests.
 */

#include "kshim.h"
#ifndef NT_TYPES_H
/* Complete UNICODE_STRING for kshim_ntpath_to_linux field access */
struct _UNICODE_STRING { uint16_t Length; uint16_t MaximumLength; uint16_t *Buffer; };
#endif
#ifdef __KERNEL__
#include <linux/slab.h>
#include <linux/kernel.h>
#endif

#ifdef USERSPACE_KSHIM

#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <stdlib.h>

_Atomic long kshim_alloc_count = 0;

/* Live-allocation table for allocation-site tagging (see kshim.h). Hashed on
 * the pointer; nodes come from plain malloc so they are not self-counted. */
#define KSHIM_TAG_BUCKETS 4096
struct kshim_tag {
    const void *p;
    size_t size;
    const char *file;
    int line;
    struct kshim_tag *next;
};
static struct kshim_tag *kshim_tag_tab[KSHIM_TAG_BUCKETS];
static pthread_mutex_t kshim_tag_lock = PTHREAD_MUTEX_INITIALIZER;

static unsigned kshim_tag_hash(const void *p) {
    return (unsigned)(((uintptr_t)p >> 4) & (KSHIM_TAG_BUCKETS - 1));
}

/* KSHIM_POISON=1 fills every non-zeroed kmalloc with 0xA5 so that a read of
 * uninitialised heap memory produces a stable, visible wrong value instead of
 * whatever the previous user of that block left behind. kzalloc is untouched. */
static int kshim_poison_enabled(void) {
    static int state = -1;   /* -1 unknown, 0 off, 1 on */
    if (state < 0) {
        const char *e = getenv("KSHIM_POISON");
        state = (e && e[0] == '1') ? 1 : 0;
    }
    return state;
}

static _Atomic int kshim_fail_countdown;

void kshim_fail_alloc_nth(int n) {
    atomic_store(&kshim_fail_countdown, n);
}

void *kshim_alloc_tagged(size_t size, int zero, const char *file, int line) {
    if (atomic_load(&kshim_fail_countdown) > 0 && atomic_fetch_sub(&kshim_fail_countdown, 1) == 1)
        return NULL;
    void *p = zero ? calloc(1, size) : malloc(size);
    if (!p) return NULL;
    if (!zero && kshim_poison_enabled())
        memset(p, 0xA5, size);
    atomic_fetch_add(&kshim_alloc_count, 1);
    struct kshim_tag *t = malloc(sizeof *t);
    if (t) {
        t->p = p; t->size = size; t->file = file; t->line = line;
        pthread_mutex_lock(&kshim_tag_lock);
        unsigned b = kshim_tag_hash(p);
        t->next = kshim_tag_tab[b];
        kshim_tag_tab[b] = t;
        pthread_mutex_unlock(&kshim_tag_lock);
    }
    return p;
}

void kshim_free_tagged(const void *p) {
    if (!p) return;
    atomic_fetch_sub(&kshim_alloc_count, 1);
    pthread_mutex_lock(&kshim_tag_lock);
    struct kshim_tag **pp = &kshim_tag_tab[kshim_tag_hash(p)];
    while (*pp && (*pp)->p != p) pp = &(*pp)->next;
    if (*pp) {
        struct kshim_tag *t = *pp;
        *pp = t->next;
        /* KSHIM_POISON=1: fill freed memory with 0xDD so a use after free reads
         * a stable bad value (and faults on a pointer read) instead of stale data. */
        if (kshim_poison_enabled())
            memset((void *)p, 0xDD, t->size);
        free(t);
    }
    pthread_mutex_unlock(&kshim_tag_lock);
    free((void *)p);
}

long kshim_live_site_count(const char *file_suffix) {
    size_t n = strlen(file_suffix);
    long count = 0;
    pthread_mutex_lock(&kshim_tag_lock);
    for (unsigned b = 0; b < KSHIM_TAG_BUCKETS; b++)
        for (struct kshim_tag *t = kshim_tag_tab[b]; t; t = t->next) {
            size_t len = strlen(t->file);
            if (len >= n && strcmp(t->file + len - n, file_suffix) == 0) count++;
        }
    pthread_mutex_unlock(&kshim_tag_lock);
    return count;
}

void kshim_leak_report(void) {
    pthread_mutex_lock(&kshim_tag_lock);
    for (unsigned b = 0; b < KSHIM_TAG_BUCKETS; b++)
        for (struct kshim_tag *t = kshim_tag_tab[b]; t; t = t->next)
            fprintf(stderr, "LEAK SITE %s:%d size=%zu ptr=%p\n",
                    t->file, t->line, t->size, t->p);
    pthread_mutex_unlock(&kshim_tag_lock);
}

__thread struct nt_eprocess *kshim_current_proc_ptr = NULL;
__thread struct nt_ethread *kshim_current_thread_ptr = NULL;

struct kshim_guard_state kshim_guard = {0};

/* mmap helpers: reserve = PROT_NONE anonymous mapping to claim VA */
void *kshim_mmap_reserve(void *addr, size_t size) {
    void *p = mmap(addr, size, PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (p == MAP_FAILED) {
        /* If MAP_FIXED_NOREPLACE not available or addr==NULL fallback */
        if (addr == NULL) {
            p = mmap(NULL, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (p == MAP_FAILED) return NULL;
            /* If caller wanted specific addr but we couldn't get it, unmap and fail */
            if (addr != NULL && p != addr) {
                munmap(p, size);
                return NULL;
            }
            return p;
        }
        /* Try with MAP_FIXED (may destroy existing mapping, but VAD says free) */
        p = mmap(addr, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        if (p == MAP_FAILED) return NULL;
        return p;
    }
    return p;
}

int kshim_mmap_commit(void *addr, size_t size, int prot) {
    /* For committing, we use mprotect if already reserved, otherwise mmap */
    /* Try mprotect first; if it fails (no mapping), mmap fixed */
    if (mprotect(addr, size, prot) == 0) return 0;
    void *p = mmap(addr, size, prot, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (p == MAP_FAILED) return -1;
    return 0;
}

int kshim_munmap(void *addr, size_t size) {
    if (!addr || size == 0) return 0;
    /* munmap returns 0 on success, -1 on fail. We ignore failures for already-unmapped */
    munmap(addr, size);
    return 0;
}

int kshim_mprotect(void *addr, size_t size, int prot) {
    return mprotect(addr, size, prot);
}

/* Lock and unlock pages in physical memory. 0 on success, -1 with errno set
 * when the host refuses (for example the working-set quota). */
int kshim_mlock(void *addr, size_t size) {
    return mlock(addr, size);
}

int kshim_munlock(void *addr, size_t size) {
    return munlock(addr, size);
}

void kshim_madvise_free(void *addr, size_t size) {
    /* MEM_RESET: contents become undefined. Use MADV_DONTNEED to zero next touch */
#ifdef MADV_FREE
    madvise(addr, size, MADV_FREE);
#else
    madvise(addr, size, MADV_DONTNEED);
#endif
    /* Try DONTNEED as well for guarantee */
    madvise(addr, size, MADV_DONTNEED);
}

int kshim_section_create(struct kshim_section_backing *b, size_t size) {
    b->size = size;
    b->fd = -1;
    b->mem = NULL;
    if (size == 0) return 0;
    /* Use memfd_create if available */
#ifdef MFD_CLOEXEC
    int fd = memfd_create("ntsection", MFD_CLOEXEC);
    if (fd >= 0) {
        if (ftruncate(fd, (off_t)size) != 0) { close(fd); return -1; }
        b->fd = fd;
        return 0;
    }
#endif
    /* Fallback: anonymous memory */
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return -1;
    b->mem = p;
    b->fd = -1;
    return 0;
}

int kshim_section_create_file(struct kshim_section_backing *b, const char *path, size_t *size) {
    (void)path;
    /* For tests, treat file sections same as pagefile */
    return kshim_section_create(b, *size);
}

void *kshim_section_map(struct kshim_section_backing *b, void *addr, size_t offset, size_t size, int prot, int cow) {
    (void)cow;
    (void)offset;
    if (b->fd >= 0) {
        void *p = mmap(addr, size, prot, MAP_SHARED | MAP_FIXED, b->fd, (off_t)offset);
        if (p == MAP_FAILED) {
            /* Try private */
            p = mmap(addr, size, prot, MAP_PRIVATE | MAP_FIXED, b->fd, (off_t)offset);
            if (p == MAP_FAILED) return NULL;
        }
        return p;
    }
    if (b->mem) {
        /* Copy-on-write simulation: mmap anonymous then copy from backing if not cow */
        void *p = mmap(addr, size, prot, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        if (p == MAP_FAILED) return NULL;
        if (!cow && prot & PROT_WRITE) {
            /* Copy initial data */
            size_t copy = size < b->size ? size : b->size;
            memcpy(p, b->mem, copy);
        }
        return p;
    }
    /* Anonymous private */
    void *p = mmap(addr, size, prot, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (p == MAP_FAILED) return NULL;
    return p;
}

/* File-backed view on the host. The shim cannot map a host file (the Windows
 * mmap shim rejects fd-backed mappings), so the view is anonymous memory filled
 * from the section's fd at map time. Bytes past the end of the file read as zero.
 * mm.c writes shared writable views back with pwrite on flush and unmap. Copy-on-
 * write views are never written back. */
void *kshim_section_map_file(struct kshim_section_backing *b, void *addr, size_t offset, size_t size, int prot, int cow) {
    (void)cow;
    void *p = mmap(addr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (p == MAP_FAILED) return NULL;
    size_t done = 0;
    while (done < size) {
        ssize_t n = pread(b->fd, (char *)p + done, size - done, (off_t)(offset + done));
        if (n < 0) { munmap(p, size); return NULL; }
        if (n == 0) break;
        done += (size_t)n;
    }
    if (prot != (PROT_READ | PROT_WRITE) && mprotect(p, size, prot) != 0) {
        munmap(p, size);
        return NULL;
    }
    return p;
}

void kshim_install_guard_handler(void) {
    /* No-op for shim: guard is handled logically via NtMmHandleGuardFault */
}

int kshim_handle_guard_fault(void *addr) {
    (void)addr;
    return 0;
}

/* -------------------------------------------------------------------------- */
/* NT I/O shim (VFS over POSIX)                                                 */
/* -------------------------------------------------------------------------- */

#ifdef __KERNEL__
extern char g_kshim_root[PATH_MAX];
#else
char g_kshim_root[PATH_MAX] = "/tmp";
#endif

void kshim_set_root(const char *root) {
    if (!root) return;
    snprintf(g_kshim_root, sizeof(g_kshim_root), "%s", root);
    /* strip trailing slash */
    size_t n = strlen(g_kshim_root);
    while (n > 1 && g_kshim_root[n-1] == '/') { g_kshim_root[n-1] = '\0'; n--; }
}

static bool is_reserved_name(const char *comp) {
    /* Ground truth (probe_path 26200.9168): CON.txt is NOT blocked (Win32 succeeds, NT returns NOT_FOUND not INVALID).
       Only exact reserved names are blocked. Stripping of trailing dots is no longer done for NT paths. */
    static const char *reserved[] = {"CON","PRN","AUX","NUL","COM1","COM2","COM3","COM4","COM5","COM6","COM7","COM8","COM9","LPT1","LPT2","LPT3","LPT4","LPT5","LPT6","LPT7","LPT8","LPT9"};
    for (size_t i=0;i<sizeof(reserved)/sizeof(reserved[0]);i++) {
        if (strcasecmp(comp, reserved[i])==0) return true;
    }
    return false;
}

/* NTSTATUS values needed for path conversion */
#ifndef STATUS_SUCCESS
#define STATUS_SUCCESS ((NTSTATUS)0x00000000L)
#define STATUS_OBJECT_NAME_INVALID ((NTSTATUS)0xC0000033L)
#define STATUS_OBJECT_NAME_NOT_FOUND ((NTSTATUS)0xC0000034L)
#define STATUS_NAME_TOO_LONG ((NTSTATUS)0xC0000106L)
#endif

#ifndef KSHIM_NT_EMPTY_COMPONENT_LIMIT
#define KSHIM_NT_EMPTY_COMPONENT_LIMIT 384
#endif
NTSTATUS kshim_ntpath_to_linux(const struct _UNICODE_STRING *us, char *out, size_t outlen) {
    if (!us || !us->Buffer || !out || outlen==0) return STATUS_OBJECT_NAME_INVALID;
    /* Heap-allocate large buffers to avoid kernel stack overflow (was 12KB stack). */
#ifdef __KERNEL__
    char *wpath = kmalloc(PATH_MAX, GFP_KERNEL);
    char *rel = kmalloc(PATH_MAX, GFP_KERNEL);
    char *combined = kmalloc(PATH_MAX, GFP_KERNEL);
    if (!wpath || !rel || !combined) {
        kfree(wpath); kfree(rel); kfree(combined);
        return STATUS_NO_MEMORY;
    }
#else
    char *wpath = malloc(PATH_MAX);
    char *rel = malloc(PATH_MAX);
    char *combined = malloc(PATH_MAX);
    if (!wpath || !rel || !combined) {
        free(wpath); free(rel); free(combined);
        return STATUS_NO_MEMORY;
    }
#endif
    memset(rel, 0, PATH_MAX);
    /* Convert WCHAR buffer to char */
    size_t chars = us->Length / sizeof(uint16_t);
    if (chars >= PATH_MAX) {
#ifdef __KERNEL__
        kfree(wpath); kfree(rel); kfree(combined);
#else
        free(wpath); free(rel); free(combined);
#endif
        return STATUS_NAME_TOO_LONG;
    }
    for (size_t i=0;i<chars;i++) {
        uint16_t wc = ((uint16_t*)us->Buffer)[i];
        if (wc >= 0x80) wpath[i] = '?'; /* non-ascii -> ? for test simplicity */
        else wpath[i] = (char)wc;
    }
    wpath[chars]='\0';

    /* Golden 26200.9457 (probe_path): a slash is an ordinary character of an NT name, not a
     * separator. The directory before it is missing, so the lookup is PATH_NOT_FOUND. */
    if (strchr(wpath, '/')) {
#ifdef __KERNEL__
        kfree(wpath); kfree(rel); kfree(combined);
#else
        free(wpath); free(rel); free(combined);
#endif
        return STATUS_OBJECT_PATH_NOT_FOUND;
    }
    /* An empty component is 0xC0000033 inside a DOS-device or device name shorter than
     * KSHIM_NT_EMPTY_COMPONENT_LIMIT characters, and 0xC000003A otherwise (the same rule as
     * io.c kshim_ntpath_to_linux; golden 26200.9457, probe_path). A \?\ prefix is always
     * 0xC0000033. */
    if (strstr(wpath, "\\\\")) {
        int dos_or_dev = strncmp(wpath, "\\??\\", 4) == 0 || strncmp(wpath, "\\Device\\", 8) == 0;
        NTSTATUS e = STATUS_OBJECT_PATH_NOT_FOUND;
        if (strncmp(wpath, "\\\\?\\", 4) == 0) e = STATUS_OBJECT_NAME_INVALID;
        else if (dos_or_dev && chars < KSHIM_NT_EMPTY_COMPONENT_LIMIT) e = STATUS_OBJECT_NAME_INVALID;
#ifdef __KERNEL__
        kfree(wpath); kfree(rel); kfree(combined);
#else
        free(wpath); free(rel); free(combined);
#endif
        return e;
    }

    /* Normalize backslashes to forward slash for internal processing, but keep original for prefix detection */
    /* Detect prefixes on raw wpath with backslashes */
    const char *rem = NULL;
    size_t rem_len = 0;
    if (strncmp(wpath, "\\??\\", 4)==0) {
        rem = wpath+4;
        if (rem[0] && rem[1]==':') {
            /* Skip drive letter like C: or C:\ */
            rem += 2;
            if (rem[0]=='\\' || rem[0]=='/') rem++;
        } else if (rem[0]=='\\' || rem[0]=='/') {
            rem++;
        }
    } else if (strncmp(wpath, "\\\\?\\", 4)==0) {
        // Windows NT layer rejects \\?\ prefix for NtCreateFile (probe_path returns C0000033).
        // Win32 handles \\?\ at higher layer, but NT path via NtCreateFile should be INVALID_NAME.
#ifdef __KERNEL__
        kfree(wpath); kfree(rel); kfree(combined);
#else
        free(wpath); free(rel); free(combined);
#endif
        return STATUS_OBJECT_NAME_INVALID;
    } else if (strncmp(wpath, "\\Device\\HarddiskVolume", 22)==0) {
        const char *p = wpath+22;
        while (*p && *p>='0' && *p<='9') p++;
        if (*p=='\\' || *p=='/') p++;
        else if (*p=='\0') p = "";
        rem = p;
    } else if (wpath[0]=='\\' || wpath[0]=='/') {
        rem = wpath+1;
        /* Also handle \C:\ style? Rare */
        if (rem[0] && rem[1]==':' ) { rem+=2; if (*rem=='\\' || *rem=='/') rem++; }
    } else {
        rem = wpath;
        /* Strip drive prefix if present like C:\ */
        if (rem[0] && rem[1]==':' ) { rem+=2; if (*rem=='\\' || *rem=='/') rem++; }
        /* Also handle C:foo relative */
    }
    if (!rem) rem = "";
    /* Now rem contains NT path remainder with backslashes */
    /* Convert to linux relative path, splitting components */
    size_t rel_len = 0;
    const char *p = rem;
    while (*p) {
        /* skip consecutive separators */
        while (*p=='\\' || *p=='/') p++;
        if (!*p) break;
        const char *start = p;
        while (*p && *p!='\\' && *p!='/') p++;
        size_t clen = p - start;
        if (clen==0) continue;
        if (clen > NAME_MAX) {
#ifdef __KERNEL__
            kfree(wpath); kfree(rel); kfree(combined);
#else
            free(wpath); free(rel); free(combined);
#endif
            return STATUS_NAME_TOO_LONG;
        }
        char comp[NAME_MAX+1];
        memcpy(comp, start, clen);
        comp[clen]='\0';
        // Ground truth: NT layer preserves trailing dots/spaces (probe_path: CaSeTeSt.TXT... -> NOT_FOUND, not stripped success).
        // Do NOT strip trailing dots/spaces for NtCreateFile path. Reserved check is after exact comp, no stripping.
        if (is_reserved_name(comp)) {
#ifdef __KERNEL__
            kfree(wpath); kfree(rel); kfree(combined);
#else
            free(wpath); free(rel); free(combined);
#endif
            return STATUS_OBJECT_NAME_INVALID;
        }
        if (clen==1 && comp[0]=='.') continue;
        if (clen==2 && comp[0]=='.' && comp[1]=='.') {
            // Ground truth: relative "..\\CaSeTeSt.TXT" with RootDirectory returns C0000033 INVALID_NAME.
            // NT layer does not support .. traversal for relative opens; reject.
#ifdef __KERNEL__
            kfree(wpath); kfree(rel); kfree(combined);
#else
            free(wpath); free(rel); free(combined);
#endif
            return STATUS_OBJECT_NAME_INVALID;
        }
        if (rel_len>0) { rel[rel_len++]='/'; }
        if (rel_len + clen >= PATH_MAX) {
#ifdef __KERNEL__
            kfree(wpath); kfree(rel); kfree(combined);
#else
            free(wpath); free(rel); free(combined);
#endif
            return STATUS_NAME_TOO_LONG;
        }
        memcpy(rel+rel_len, comp, clen);
        rel_len += clen;
        rel[rel_len]='\0';
    }

    /* Combine with root */
    if (rel_len==0) {
        snprintf(combined, PATH_MAX, "%s", g_kshim_root);
    } else {
        int n = snprintf(combined, PATH_MAX, "%s/%s", g_kshim_root, rel);
        if (n<0 || (size_t)n >= PATH_MAX) {
#ifdef __KERNEL__
            kfree(wpath); kfree(rel); kfree(combined);
#else
            free(wpath); free(rel); free(combined);
#endif
            return STATUS_NAME_TOO_LONG;
        }
    }
    /* For absolute vs relative distinction inside test, we also need to handle empty rel = root */
    /* Check overall reserved? Already per component */

    /* Copy out */
    size_t need = strlen(combined)+1;
    if (need > outlen) {
#ifdef __KERNEL__
        kfree(wpath); kfree(rel); kfree(combined);
#else
        free(wpath); free(rel); free(combined);
#endif
        return STATUS_NAME_TOO_LONG;
    }
    memcpy(out, combined, need);
    /* Also need to handle case where original path was absolute device like \??\C:\ but rel empty => out = root */
    /* Ensure output uses forward slashes */
    for (char *c=out; *c; ++c) if (*c=='\\') *c='/';
    (void)rem_len;
#ifdef __KERNEL__
    kfree(wpath); kfree(rel); kfree(combined);
#else
    free(wpath); free(rel); free(combined);
#endif
    return STATUS_SUCCESS;
}

struct file *kshim_filp_open(const char *path, int flags, mode_t mode) {
    struct file *f = (struct file*)calloc(1, sizeof(*f));
    if (!f) return NULL;
    int fd = open(path, flags, mode);
    if (fd < 0) { free(f); return NULL; }
    struct stat st;
    if (fstat(fd, &st)==0) f->is_dir = S_ISDIR(st.st_mode);
    else f->is_dir = false;
    f->fd = fd;
    f->path_copy = strdup(path);
    return f;
}
void kshim_filp_close(struct file *f) {
    if (!f) return;
    if (f->fd >= 0) close(f->fd);
    free(f->path_copy);
    free(f);
}
/* Stat timestamps. glibc (Linux) has st_atim/st_mtim/st_ctim as struct
 * timespec with nanoseconds. MinGW only has st_atime/st_mtime/st_ctime
 * (time_t, whole seconds), so the MinGW branch builds a timespec with tv_nsec 0. */
/* The host build (tests/kernel-host) puts its Linux-layout sys/stat.h on the
 * include path under USERSPACE_KSHIM, so it takes the st_atim branch too. */
#if defined(__linux__) || defined(USERSPACE_KSHIM)
#define KSHIM_STAT_ATIM(s) ((s).st_atim)
#define KSHIM_STAT_MTIM(s) ((s).st_mtim)
#define KSHIM_STAT_CTIM(s) ((s).st_ctim)
#else
static inline struct timespec kshim_stat_ts(time_t t) {
    struct timespec ts;
    ts.tv_sec = t;
    ts.tv_nsec = 0;
    return ts;
}
#define KSHIM_STAT_ATIM(s) kshim_stat_ts((s).st_atime)
#define KSHIM_STAT_MTIM(s) kshim_stat_ts((s).st_mtime)
#define KSHIM_STAT_CTIM(s) kshim_stat_ts((s).st_ctime)
#endif

int kshim_vfs_getattr(struct file *f, struct kstat *st) {
    if (!f || !st) return -1;
    struct stat s;
    if (fstat(f->fd, &s)!=0) return -1;
    st->dev = s.st_dev; st->ino = s.st_ino; st->mode = s.st_mode; st->size = s.st_size;
    st->atime = KSHIM_STAT_ATIM(s); st->mtime = KSHIM_STAT_MTIM(s); st->ctime = KSHIM_STAT_CTIM(s);
    st->nlink = s.st_nlink;
    return 0;
}
int kshim_vfs_getattr_path(const char *path, struct kstat *st) {
    struct stat s;
    if (stat(path, &s)!=0) return -1;
    st->dev = s.st_dev; st->ino = s.st_ino; st->mode = s.st_mode; st->size = s.st_size;
    st->atime = KSHIM_STAT_ATIM(s); st->mtime = KSHIM_STAT_MTIM(s); st->ctime = KSHIM_STAT_CTIM(s);
    st->nlink = s.st_nlink;
    return 0;
}
ssize_t kshim_kernel_read(struct file *f, void *buf, size_t len, loff_t *pos) {
    if (!f || f->fd<0) return -1;
    ssize_t r = pread(f->fd, buf, len, (off_t)*pos);
    if (r>=0) *pos += r;
    return r;
}
ssize_t kshim_kernel_write(struct file *f, const void *buf, size_t len, loff_t *pos) {
    if (!f || f->fd<0) return -1;
    ssize_t r = pwrite(f->fd, buf, len, (off_t)*pos);
    if (r>=0) *pos += r;
    return r;
}
int kshim_vfs_truncate(struct file *f, loff_t size) {
    if (!f) return -1;
    return ftruncate(f->fd, (off_t)size);
}
int kshim_vfs_rename(const char *oldpath, const char *newpath) {
    return rename(oldpath, newpath);
}
int kshim_vfs_unlink(const char *path) {
    struct stat s;
    if (stat(path,&s)==0 && S_ISDIR(s.st_mode)) return rmdir(path);
    return unlink(path);
}

#endif /* USERSPACE_KSHIM */
#ifdef __KERNEL__
#include <linux/sched.h>
struct nt_eprocess *kshim_current_process(void) {
    extern struct nt_eprocess *nt_current_eprocess(void);
    return nt_current_eprocess();
}
struct nt_ethread *kshim_current_thread(void) {
#ifdef CONFIG_NT_PERSONALITY
    extern struct nt_ethread *nt_current_ethread(void);
    return nt_current_ethread();
#else
    extern struct nt_thread *nt_current_thread_ptr;
    return (struct nt_ethread *)nt_current_thread_ptr;
#endif
}
#endif
