/*
 * ntdll_runtime.c - freestanding runtime for ntdll.dll
 *
 * Provides memcpy/memset/strlen etc and a simple bump allocator so that
 * ldr.c / unwind.c / except.c can link with -nostdlib and produce zero imports.
 *
 * Only compiled for __MINGW32__ (Windows PE). For Linux test builds the system
 * libc provides these.
 */
#ifdef __MINGW32__
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include "nt_types.h"

/* ------------------------------------------------------------------ */
/* Compiler builtins / freestanding string / mem                       */
/* ------------------------------------------------------------------ */
/* With the Rtl library of src/ntdll/rtl (Wine's string.c and wcstring.c) the first group comes from there:
 * the ntdll Makefile defines NTDLL_RTL_PORT for ntdll_runtime.o. The other DLLs still build these. */
#ifdef NTDLL_RTL_PORT
void *memcpy(void *dst, const void *src, size_t n);
size_t strlen(const char *s);
#else
void *memcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t*)dst;
    const uint8_t *s = (const uint8_t*)src;
    for (size_t i=0;i<n;i++) d[i]=s[i];
    return dst;
}
void *memmove(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t*)dst;
    const uint8_t *s = (const uint8_t*)src;
    if (d < s) {
        for (size_t i=0;i<n;i++) d[i]=s[i];
    } else if (d > s) {
        for (size_t i=n;i>0;i--) d[i-1]=s[i-1];
    }
    return dst;
}
void *memset(void *s, int c, size_t n) {
    uint8_t *p = (uint8_t*)s;
    for (size_t i=0;i<n;i++) p[i]=(uint8_t)c;
    return s;
}
int memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *pa=a, *pb=b;
    for (size_t i=0;i<n;i++) if(pa[i]!=pb[i]) return (int)pa[i]-(int)pb[i];
    return 0;
}
size_t strlen(const char *s) {
    size_t n=0;
    while (s && s[n]) n++;
    return n;
}
size_t wcslen(const WCHAR *s) {
    size_t n=0;
    while (s && s[n]) n++;
    return n;
}
int strcmp(const char *a, const char *b) {
    while (*a && *b && *a==*b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}
int strncmp(const char *a, const char *b, size_t n) {
    for (size_t i=0;i<n;i++) {
        if (a[i]!=b[i]) return (unsigned char)a[i]-(unsigned char)b[i];
        if (a[i]==0) return 0;
    }
    return 0;
}
char *strcpy(char *dst, const char *src) {
    char *d=dst;
    while ((*d++=*src++));
    return dst;
}
char *strncpy(char *dst, const char *src, size_t n) {
    size_t i;
    for(i=0;i<n && src[i];i++) dst[i]=src[i];
    for(;i<n;i++) dst[i]=0;
    return dst;
}
char *strcat(char *dst, const char *src) {
    size_t dl=strlen(dst);
    size_t i;
    for(i=0;src[i];i++) dst[dl+i]=src[i];
    dst[dl+i]=0;
    return dst;
}
char *strncat(char *dst, const char *src, size_t n) {
    size_t dl=strlen(dst);
    size_t i;
    for(i=0;i<n && src[i];i++) dst[dl+i]=src[i];
    dst[dl+i]=0;
    return dst;
}
#endif /* NTDLL_RTL_PORT */
int strcasecmp(const char *a, const char *b) {
    while (*a && *b) {
        char ca=*a++, cb=*b++;
        if (ca>='A'&&ca<='Z') ca=ca-'A'+'a';
        if (cb>='A'&&cb<='Z') cb=cb-'A'+'a';
        if (ca!=cb) return (unsigned char)ca-(unsigned char)cb;
    }
    return (unsigned char)*a - (unsigned char)*b;
}
int strncasecmp(const char *a, const char *b, size_t n) {
    for(size_t i=0;i<n;i++) {
        char ca=a[i], cb=b[i];
        if (ca>='A'&&ca<='Z') ca=ca-'A'+'a';
        if (cb>='A'&&cb<='Z') cb=cb-'A'+'a';
        if (ca!=cb) return (unsigned char)ca-(unsigned char)cb;
        if (ca==0) return 0;
    }
    return 0;
}
#ifndef NTDLL_RTL_PORT
void *memchr(const void *s, int c, size_t n) {
    const uint8_t *p=s;
    for(size_t i=0;i<n;i++) if(p[i]==(uint8_t)c) return (void*)(p+i);
    return NULL;
}
char *strchr(const char *s, int c) {
    while (*s) { if (*s==(char)c) return (char*)s; s++; }
    return c==0 ? (char*)s : NULL;
}
char *strrchr(const char *s, int c) {
    const char *last=NULL;
    while (*s) { if (*s==(char)c) last=s; s++; }
    if (c==0) return (char*)s;
    return (char*)last;
}
int sscanf(const char *str, const char *fmt, ...) { (void)str; (void)fmt; return 0; }
#endif /* NTDLL_RTL_PORT */
int snprintf(char *s, size_t n, const char *fmt, ...) { (void)fmt; if(n) s[0]=0; return 0; }
int vsnprintf(char *s, size_t n, const char *fmt, va_list ap) { (void)fmt;(void)ap; if(n) s[0]=0; return 0; }

/* ------------------------------------------------------------------ */
/* malloc and friends, on the process heap (ntdll_heap.c)              */
/* ------------------------------------------------------------------ */
/* A NULL heap handle names the process heap. ntdll calls RtlAllocateHeap in its own module; the
 * other DLLs that link this file import it from ntdll.dll. */
#ifdef NTDLL_BUILD
PVOID __stdcall RtlAllocateHeap(PVOID heap, ULONG flags, SIZE_T size);
BOOLEAN __stdcall RtlFreeHeap(PVOID heap, ULONG flags, PVOID ptr);
PVOID __stdcall RtlReAllocateHeap(PVOID heap, ULONG flags, PVOID ptr, SIZE_T size);
#else
__declspec(dllimport) PVOID __stdcall RtlAllocateHeap(PVOID heap, ULONG flags, SIZE_T size);
__declspec(dllimport) BOOLEAN __stdcall RtlFreeHeap(PVOID heap, ULONG flags, PVOID ptr);
__declspec(dllimport) PVOID __stdcall RtlReAllocateHeap(PVOID heap, ULONG flags, PVOID ptr, SIZE_T size);
#endif
#define RT_HEAP_ZERO_MEMORY 0x00000008u

void *malloc(size_t sz) {
    return RtlAllocateHeap(NULL, 0, sz ? sz : 1);
}
void free(void *p) {
    if (p) RtlFreeHeap(NULL, 0, p);
}
void *calloc(size_t n, size_t sz) {
    /* n * sz must fit in size_t; otherwise return NULL instead of a short block. */
    if (sz != 0 && n > (size_t)-1 / sz) return NULL;
    size_t tot = n*sz;
    return RtlAllocateHeap(NULL, RT_HEAP_ZERO_MEMORY, tot ? tot : 1);
}
void *realloc(void *ptr, size_t sz) {
    if (!ptr) return malloc(sz);
    if (sz==0) { free(ptr); return NULL; }
    /* On failure the old block stays valid and NULL is returned. */
    return RtlReAllocateHeap(NULL, 0, ptr, sz);
}
char *strdup(const char *s) {
    size_t n=strlen(s)+1;
    char *p=(char*)malloc(n);
    if(p) memcpy(p,s,n);
    return p;
}

/* ------------------------------------------------------------------ */
/* Minimal stubs for other libc functions used by ldr.c               */
/* ------------------------------------------------------------------ */
#ifdef errno
#undef errno
#endif
int errno;
int * _errno(void) { return &errno; }

/* Provide dummy FILE etc for stdio - not actually used in freestanding path */
typedef struct { int dummy; } FILE;
FILE *fopen(const char *a, const char *b){ (void)a;(void)b; return NULL; }
int fclose(FILE *f){ (void)f; return 0; }
char *fgets(char *s, int n, FILE *f){ (void)s;(void)n;(void)f; return NULL; }
int fprintf(FILE *f, const char *fmt, ...){ (void)f;(void)fmt; return 0; }
int printf(const char *fmt, ...){ (void)fmt; return 0; }
void __chkstk(void) {}
void _chkstk(void) {}
void ___chkstk_ms(void) {}
void _alloca(void) {}

/* Provide __mingw_vsnprintf etc */
int __mingw_vsnprintf(char *s, size_t n, const char *fmt, va_list ap){ (void)s;(void)n;(void)fmt;(void)ap; if(n) s[0]=0; return 0; }
/* MinGW's inline fprintf in <stdio.h> (used by pe_parse.c diagnostics) calls this. */
int __mingw_vfprintf(FILE *f, const char *fmt, va_list ap){ (void)f;(void)fmt;(void)ap; return 0; }

/* Provide _exit for except.c */
void _exit(int c){ (void)c; while(1) { __asm__ volatile("hlt"); } }

/* Provide msync stub used in except.c via our earlier freestanding path - but for mingw we already stubbed via ifndef so not needed */

#endif
