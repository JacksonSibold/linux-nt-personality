/*
 * ldr_sched.h - pure helpers for the loader (src/ntdll/ldr.c): init order,
 * reference counts, DLL name handling, search directory order, the API-set
 * prefix test, the forwarder depth limit and the attach result rule.
 *
 * No NT types, no I/O, no allocation. tests/ntdll-host/test_ldr_sched.c
 * exercises every function on the host. ldr.c calls the same functions on
 * both the host build and the MinGW (Windows-target) build, so the logic is
 * one copy. Decision 0031 (DllMain and TLS order) and review LDR-1/LDR-3.
 */
#ifndef NTDLL_LDR_SCHED_H
#define NTDLL_LDR_SCHED_H

#include <stddef.h>
#include <string.h>

/* Module indices in the init-order graph fit one 64-bit dependency mask. */
#define LDR_SCHED_MAX_MODULES 64

/* Export forwarders (DLL.Name chains) stop after this many hops. */
#define LDR_MAX_FORWARD_DEPTH 32

/* ldr_ref_dec results. */
#define LDR_REF_FREE      0   /* the count reached zero: free the module */
#define LDR_REF_HELD      1   /* still referenced */
#define LDR_REF_UNDERFLOW (-1) /* the count was already zero: no change */

/* DllMain reasons used by the attach rule. */
#define LDR_DLL_PROCESS_DETACH 0
#define LDR_DLL_PROCESS_ATTACH 1

/* -------------------------------------------------------------------------- */
/* Init order                                                                 */
/* -------------------------------------------------------------------------- */

typedef struct {
    const unsigned long long *deps; /* deps[m] bit j set: module m imports j */
    size_t n;
    unsigned char *state;           /* 0 unvisited, 1 on the stack, 2 done */
    size_t *order;
    size_t cap;
    size_t count;
    int overflow;
} ldr_sched_walk_t;

/* Post-order DFS: a module is emitted after everything it imports. An edge to
 * a module still on the DFS stack is a cycle, and it is skipped (the same
 * rule as the init stack in ldr_process_attach). */
static inline void ldr_sched_visit(ldr_sched_walk_t *w, size_t m)
{
    if (w->state[m] != 0 || w->overflow) return;
    w->state[m] = 1;
    for (size_t j = 0; j < w->n; ++j) {
        if (w->deps[m] & (1ULL << j)) ldr_sched_visit(w, j);
    }
    w->state[m] = 2;
    if (w->count >= w->cap) { w->overflow = 1; return; }
    w->order[w->count++] = m;
}

/* Attach order for everything reachable from root, dependencies first.
 * Returns 0 on success and sets *count. Returns -1 for bad input (n is zero or
 * above LDR_SCHED_MAX_MODULES, root out of range, a NULL pointer) or when the
 * order does not fit in cap. */
static inline int ldr_sched_attach_order(size_t n, const unsigned long long *deps,
                                         size_t root, size_t *order, size_t cap,
                                         size_t *count)
{
    unsigned char state[LDR_SCHED_MAX_MODULES];
    ldr_sched_walk_t w;

    if (!deps || !order || !count) return -1;
    if (n == 0 || n > LDR_SCHED_MAX_MODULES || root >= n) return -1;

    memset(state, 0, sizeof(state));
    w.deps = deps;
    w.n = n;
    w.state = state;
    w.order = order;
    w.cap = cap;
    w.count = 0;
    w.overflow = 0;

    ldr_sched_visit(&w, root);
    *count = w.count;
    return w.overflow ? -1 : 0;
}

/* Detach order is the attach order reversed. */
static inline void ldr_sched_reverse(size_t *order, size_t count)
{
    for (size_t i = 0, j = count; i + 1 < j; ++i, --j) {
        size_t t = order[i];
        order[i] = order[j - 1];
        order[j - 1] = t;
    }
}

/* -------------------------------------------------------------------------- */
/* Reference counts (LoadCount)                                               */
/* -------------------------------------------------------------------------- */

/* Returns 1 when incremented. Returns 0 at saturation (the count is kept). */
static inline int ldr_ref_inc(unsigned short *count)
{
    if (*count == 0xFFFF) return 0;
    ++*count;
    return 1;
}

/* Returns LDR_REF_FREE when the count reaches zero, LDR_REF_HELD while it
 * stays above zero, and LDR_REF_UNDERFLOW (count unchanged) at zero. */
static inline int ldr_ref_dec(unsigned short *count)
{
    if (*count == 0) return LDR_REF_UNDERFLOW;
    --*count;
    return *count == 0 ? LDR_REF_FREE : LDR_REF_HELD;
}

/* -------------------------------------------------------------------------- */
/* Forwarders                                                                 */
/* -------------------------------------------------------------------------- */

/* A forwarder at this depth may still be followed (depth counts from zero). */
static inline int ldr_forward_depth_ok(unsigned int depth)
{
    return depth < LDR_MAX_FORWARD_DEPTH;
}

/* -------------------------------------------------------------------------- */
/* DLL names                                                                  */
/* -------------------------------------------------------------------------- */

static inline char ldr_sched_lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

/* Lowercases a base name and appends ".dll" when the name lacks it. Writes a
 * NUL-terminated result into out (cap bytes). Returns 1 on success. Returns 0
 * for an empty name, a name with a path separator or drive colon (only base
 * names are accepted), or a result that does not fit in cap. */
static inline int ldr_dll_normalize(const char *in, char *out, size_t cap)
{
    size_t len = 0, need;
    int has_dll;

    if (!in || !out || cap == 0 || in[0] == '\0') return 0;
    for (; in[len] != '\0'; ++len) {
        char ch = in[len];
        if (ch == '/' || ch == '\\' || ch == ':') return 0;
    }

    has_dll = len >= 4 && ldr_sched_lower(in[len - 4]) == '.' &&
              ldr_sched_lower(in[len - 3]) == 'd' &&
              ldr_sched_lower(in[len - 2]) == 'l' &&
              ldr_sched_lower(in[len - 1]) == 'l';
    need = len + (has_dll ? 0 : 4);
    if (need + 1 > cap) return 0;

    for (size_t i = 0; i < len; ++i) out[i] = ldr_sched_lower(in[i]);
    if (!has_dll) {
        out[len] = '.';
        out[len + 1] = 'd';
        out[len + 2] = 'l';
        out[len + 3] = 'l';
    }
    out[need] = '\0';
    return 1;
}

/* ASCII case-insensitive equality of two NUL-terminated names. */
static inline int ldr_dll_name_equal(const char *a, const char *b)
{
    if (!a || !b) return 0;
    for (;;) {
        char ca = ldr_sched_lower(*a++);
        char cb = ldr_sched_lower(*b++);
        if (ca != cb) return 0;
        if (ca == '\0') return 1;
    }
}

/* -------------------------------------------------------------------------- */
/* Search order                                                               */
/* -------------------------------------------------------------------------- */

static inline int ldr_sched_listed(const char **out, size_t n, const char *dir)
{
    for (size_t i = 0; i < n; ++i) {
        if (strcmp(out[i], dir) == 0) return 1;
    }
    return 0;
}

static inline size_t ldr_sched_add_dir(const char **out, size_t n, size_t cap,
                                       const char *dir)
{
    if (!dir || dir[0] == '\0') return n;
    if (n >= cap) return n;
    if (ldr_sched_listed(out, n, dir)) return n;
    out[n] = dir;
    return n + 1;
}

/* Directories to try for a DLL name, in order: the search path override, the
 * System32 directory when the name is a known DLL, the application directory,
 * System32, each PATH entry, and finally the working directory ".". A
 * directory already in the list is not repeated. Returns the number of entries
 * written to out (at most cap). */
static inline size_t ldr_search_dirs(const char *search_path, int known_dll,
                                     const char *exe_dir, const char *sys32_dir,
                                     const char *const *path_dirs, size_t npath,
                                     const char **out, size_t cap)
{
    size_t n = 0;

    n = ldr_sched_add_dir(out, n, cap, search_path);
    if (known_dll) n = ldr_sched_add_dir(out, n, cap, sys32_dir);
    n = ldr_sched_add_dir(out, n, cap, exe_dir);
    n = ldr_sched_add_dir(out, n, cap, sys32_dir);
    for (size_t i = 0; i < npath && path_dirs; ++i)
        n = ldr_sched_add_dir(out, n, cap, path_dirs[i]);
    n = ldr_sched_add_dir(out, n, cap, ".");
    return n;
}

/* -------------------------------------------------------------------------- */
/* API sets                                                                   */
/* -------------------------------------------------------------------------- */

static inline int ldr_sched_prefix_ci(const char *s, const char *prefix)
{
    for (; *prefix; ++s, ++prefix) {
        if (ldr_sched_lower(*s) != ldr_sched_lower(*prefix)) return 0;
    }
    return 1;
}

/* 1 for an API-set contract name (api-ms-win-* or ext-ms-win-*), case
 * insensitive. Such names resolve through the API-set table, not the disk. */
static inline int ldr_apiset_is_virtual(const char *name)
{
    if (!name) return 0;
    return ldr_sched_prefix_ci(name, "api-ms-win-") ||
           ldr_sched_prefix_ci(name, "ext-ms-win-");
}

/* One line of an API-set schema: "<virtual name> <host dll> [# comment]".
 * Blank lines, comment lines and lines with one token return 0. Both tokens
 * are copied NUL-terminated. A token that does not fit its buffer returns 0,
 * and the outputs are then unspecified. CR and LF end a line. */
static inline int ldr_sched_ws(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static inline size_t ldr_sched_token(const char **pp, char *out, size_t cap)
{
    const char *p = *pp;
    size_t n = 0;

    while (*p && !ldr_sched_ws(*p) && *p != '#') {
        if (n + 1 >= cap) return 0;
        out[n++] = *p++;
    }
    out[n] = '\0';
    *pp = p;
    return n;
}

static inline int ldr_apiset_parse_line(const char *line, char *virt, size_t vcap,
                                        char *host, size_t hcap)
{
    const char *p = line;

    if (!line || !virt || !host || vcap == 0 || hcap == 0) return 0;
    while (*p && ldr_sched_ws(*p)) ++p;
    if (*p == '\0' || *p == '#') return 0;

    if (ldr_sched_token(&p, virt, vcap) == 0) return 0;
    while (*p && ldr_sched_ws(*p)) ++p;
    if (*p == '\0' || *p == '#') return 0;

    return ldr_sched_token(&p, host, hcap) != 0;
}

/* -------------------------------------------------------------------------- */
/* Ordinals                                                                   */
/* -------------------------------------------------------------------------- */

/* Decimal value of the leading digits of s. Stops at the first non-digit, and
 * returns 0 for an empty or non-numeric string (same result as strtoul with
 * base 10 on those inputs, without the library). */
static inline unsigned long ldr_parse_decimal(const char *s)
{
    unsigned long v = 0;
    while (s && *s >= '0' && *s <= '9') v = v * 10 + (unsigned long)(*s++ - '0');
    return v;
}

/* -------------------------------------------------------------------------- */
/* DllMain result                                                             */
/* -------------------------------------------------------------------------- */

/* DLL_PROCESS_ATTACH fails the load when DllMain returns FALSE (0). Other
 * reasons ignore the result. */
static inline int ldr_attach_result_ok(unsigned long reason, int result)
{
    if (reason == LDR_DLL_PROCESS_ATTACH) return result != 0;
    return 1;
}

#endif /* NTDLL_LDR_SCHED_H */
