/* ucrtbase_heap.inc - malloc and friends on the process heap (included by ucrtbase.c). */

void *w_malloc(size_t n)
{
    void *p = HeapAlloc(GetProcessHeap(), 0, n ? n : 1);
    if (!p)
        ucrt_set_errno(UCRT_ENOMEM);
    return p;
}

void w_free(void *p)
{
    if (p)
        HeapFree(GetProcessHeap(), 0, p);
}

void *w_calloc(size_t count, size_t size)
{
    void *p;
    if (size && count > (size_t)-1 / size) {
        ucrt_set_errno(UCRT_ENOMEM);
        return NULL;
    }
    {
        size_t bytes = count * size;
        p = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, bytes ? bytes : 1);
    }
    if (!p)
        ucrt_set_errno(UCRT_ENOMEM);
    return p;
}

void *w_realloc(void *p, size_t n)
{
    void *q;
    if (!p)
        return w_malloc(n);
    if (n == 0) { /* the UCRT frees the block and returns NULL */
        HeapFree(GetProcessHeap(), 0, p);
        return NULL;
    }
    q = HeapReAlloc(GetProcessHeap(), 0, p, n);
    if (!q)
        ucrt_set_errno(UCRT_ENOMEM);
    return q;
}

size_t w__msize(void *p)
{
    if (!p) {
        ucrt_invalid(UCRT_EINVAL);
        return (size_t)-1;
    }
    return HeapSize(GetProcessHeap(), 0, p);
}

void *w__expand(void *p, size_t n)
{
    void *q;
    if (!p) {
        ucrt_invalid(UCRT_EINVAL);
        return NULL;
    }
    q = HeapReAlloc(GetProcessHeap(), HEAP_REALLOC_IN_PLACE_ONLY, p, n ? n : 1);
    if (!q)
        ucrt_set_errno(UCRT_ENOMEM);
    return q;
}

void *w__recalloc(void *p, size_t count, size_t size)
{
    size_t old = 0, want;
    void *q;
    if (size && count > (size_t)-1 / size) {
        ucrt_set_errno(UCRT_ENOMEM);
        return NULL;
    }
    want = count * size;
    if (p)
        old = HeapSize(GetProcessHeap(), 0, p);
    q = w_realloc(p, want);
    if (q && want > old)
        ucrt_memset((char *)q + old, 0, want - old);
    return q;
}

/* Aligned blocks keep the address of the heap block just below the aligned address. */
static int uct_pow2(size_t a) { return a && !(a & (a - 1)); }

void *w__aligned_offset_malloc(size_t size, size_t align, size_t offset)
{
    uintptr_t p, a;
    void *orig;
    if (!uct_pow2(align) || (offset && offset >= size)) {
        ucrt_invalid(UCRT_EINVAL);
        return NULL;
    }
    if (align < sizeof(void *))
        align = sizeof(void *);
    orig = HeapAlloc(GetProcessHeap(), 0, size + align + sizeof(void *));
    if (!orig) {
        ucrt_set_errno(UCRT_ENOMEM);
        return NULL;
    }
    p = (uintptr_t)orig;
    a = ((p + sizeof(void *) + offset + (align - 1)) & ~(uintptr_t)(align - 1)) - offset;
    ((void **)a)[-1] = orig;
    return (void *)a;
}

void *w__aligned_malloc(size_t size, size_t align) { return w__aligned_offset_malloc(size, align, 0); }

void w__aligned_free(void *p)
{
    if (p)
        HeapFree(GetProcessHeap(), 0, ((void **)p)[-1]);
}

static size_t uct_aligned_size(void *p)
{
    void *orig = ((void **)p)[-1];
    return HeapSize(GetProcessHeap(), 0, orig) - (size_t)((char *)p - (char *)orig);
}

size_t w__aligned_msize(void *p, size_t align, size_t offset)
{
    (void)align;
    (void)offset;
    if (!p) {
        ucrt_invalid(UCRT_EINVAL);
        return (size_t)-1;
    }
    return uct_aligned_size(p);
}

void *w__aligned_offset_realloc(void *p, size_t size, size_t align, size_t offset)
{
    void *q;
    size_t old, keep;
    if (!p)
        return w__aligned_offset_malloc(size, align, offset);
    if (size == 0) {
        w__aligned_free(p);
        return NULL;
    }
    q = w__aligned_offset_malloc(size, align, offset);
    if (!q)
        return NULL;
    old = uct_aligned_size(p);
    keep = old < size ? old : size;
    ucrt_memcpy(q, p, keep);
    w__aligned_free(p);
    return q;
}

void *w__aligned_realloc(void *p, size_t size, size_t align) { return w__aligned_offset_realloc(p, size, align, 0); }

void *w__aligned_offset_recalloc(void *p, size_t count, size_t size, size_t align, size_t offset)
{
    size_t old = 0, want;
    void *q;
    if (size && count > (size_t)-1 / size) {
        ucrt_set_errno(UCRT_ENOMEM);
        return NULL;
    }
    want = count * size;
    if (p)
        old = uct_aligned_size(p);
    q = w__aligned_offset_realloc(p, want, align, offset);
    if (q && want > old)
        ucrt_memset((char *)q + old, 0, want - old);
    return q;
}

void *w__aligned_recalloc(void *p, size_t count, size_t size, size_t align)
{
    return w__aligned_offset_recalloc(p, count, size, align, 0);
}

/* The "_base" forms the C++ runtime calls. */
void *w__malloc_base(size_t n) { return w_malloc(n); }
void w__free_base(void *p) { w_free(p); }
void *w__calloc_base(size_t c, size_t s) { return w_calloc(c, s); }
void *w__realloc_base(void *p, size_t n) { return w_realloc(p, n); }
size_t w__msize_base(void *p) { return w__msize(p); }

typedef int (*uct_new_handler)(size_t);
static uct_new_handler g_uct_new_handler;
static int g_uct_new_mode;

int w__callnewh(size_t n)
{
    if (!g_uct_new_handler)
        return 0;
    return g_uct_new_handler(n) ? 1 : 0;
}

uct_new_handler w__set_new_handler(uct_new_handler h)
{
    uct_new_handler old = g_uct_new_handler;
    g_uct_new_handler = h;
    return old;
}

int w__set_new_mode(int m)
{
    int old = g_uct_new_mode;
    if (m != 0 && m != 1) {
        ucrt_invalid(UCRT_EINVAL);
        return -1;
    }
    g_uct_new_mode = m;
    return old;
}

int w__query_new_mode(void) { return g_uct_new_mode; }
intptr_t w__get_heap_handle(void) { return (intptr_t)GetProcessHeap(); }
int w__heapchk(void) { return -2; /* _HEAPOK */ }
int w__heapmin(void) { return 0; }
