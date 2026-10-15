/*
 * msvcrt.c - minimal CRT forwarding to kernel32/ntdll
 * Implements subset required by mingw probes (approx 35 exports)
 * Compiles as freestanding DLL via Makefile.ntdll
 */
#ifdef __MINGW32__
#include "nt_types.h"
#include "ntstatus.h"
#include "msvcrt_policy.h"
#include "msvcrt_format.h"
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>

#ifndef WINAPI
#define WINAPI __stdcall
#endif
#ifndef NTAPI
#define NTAPI __stdcall
#endif
#ifndef HINSTANCE
typedef PVOID HINSTANCE;
#endif
#ifndef LPVOID
typedef PVOID LPVOID;
#endif
#ifndef NTAPI
#define NTAPI WINAPI
#endif

__declspec(dllimport) HANDLE WINAPI GetStdHandle(DWORD);
__declspec(dllimport) BOOL WINAPI WriteFile(HANDLE, const void*, DWORD, DWORD*, void*);
__declspec(dllimport) HANDLE WINAPI GetProcessHeap(void);
__declspec(dllimport) PVOID WINAPI HeapAlloc(HANDLE, DWORD, SIZE_T);
__declspec(dllimport) BOOL WINAPI HeapFree(HANDLE, DWORD, PVOID);
__declspec(dllimport) NTSTATUS NtTerminateProcess(HANDLE, NTSTATUS);
__declspec(dllimport) ULONG RtlNtStatusToDosError(NTSTATUS);

/* DllMain */
BOOL WINAPI DllMain(HINSTANCE h, DWORD r, LPVOID v){ (void)h;(void)r;(void)v; return 1; }
BOOL __stdcall _DllMainCRTStartup(HINSTANCE h, DWORD r, LPVOID v){ return DllMain(h,r,v); }
BOOL __stdcall DllMainCRTStartup(HINSTANCE h, DWORD r, LPVOID v){ return DllMain(h,r,v); }

/* Globals for CRT */
static int g_errno = 0;
static int g_fmode = 0x4000;
static int g_commode = 0;
static unsigned int g_codepage = 65001;
static unsigned int g_mb_cur_max = 1;
static char* g_initenv = NULL;
static int g_app_type = 0;
static void* g_iob[3] = {0};
static int g_lc_codepage = 65001;
static int g_lc_mb_cur_max = 1;

/* FILE-like dummy - msvcrt's FILE is opaque but probe uses it via __iob_func */
void* __iob_func(void){ return g_iob; }
int* _errno(void){ return &g_errno; }
int* _fmode_func(void){ return &g_fmode; }
int _fmode = 0x4000;
int _commode = 0;
int* __p__fmode(void){ return &_fmode; }
int* __p__commode(void){ return &_commode; }
unsigned int* ___lc_codepage_func(void){ return &g_codepage; }
unsigned int* ___mb_cur_max_func(void){ return &g_mb_cur_max; }
char** __initenv = &g_initenv;
int __set_app_type(int t){ g_app_type=t; return t; }
int __setusermatherr(void *f){ (void)f; return 0; }
void _amsg_exit(int n){ (void)n; NtTerminateProcess((HANDLE)(intptr_t)-1, 1); }
int _cexit(void){ return 0; }
void* _lock(int n){ (void)n; return NULL; }
void _unlock(int n){ (void)n; }
int _initterm(void (**start)(void), void (**end)(void)){
    if(!start||!end) return 0;
    for(void (**p)(void)=start; p<end; p++) if(*p) (*p)();
    return 0;
}
void abort(void){ NtTerminateProcess((HANDLE)(intptr_t)-1, 3); while(1){} }
/* CRT-7: atexit handlers are kept in a table and run, last registered first, by exit(). */
#define MS_ATEXIT_MAX 32
static void (*g_atexit_fns[MS_ATEXIT_MAX])(void);
static int g_atexit_count = 0;
int atexit(void (*f)(void)){
    if(!f || g_atexit_count >= MS_ATEXIT_MAX) return -1;
    g_atexit_fns[g_atexit_count++] = f;
    return 0;
}
typedef void (*_onexit_t)(void);
/* _onexit registers f with the atexit list (run in reverse order by exit). */
_onexit_t _onexit(_onexit_t f){
    if(!f || g_atexit_count >= MS_ATEXIT_MAX) return NULL;
    g_atexit_fns[g_atexit_count++] = f;
    return f;
}

/* Heap wrappers - use kernel32 Heap */
/* Each block has a 16-byte header holding the requested size, so realloc
 * copies only the valid part of the old block (CRT-4). HeapSize is a stub in
 * the kernel32 shim, so the size cannot come from the heap. */
#define MS_BLOCK_HDR 16
void* malloc(SIZE_T s){
    HANDLE h=GetProcessHeap();
    if(!h || s > ((SIZE_T)-1) - MS_BLOCK_HDR) return NULL;
    unsigned char *base=HeapAlloc(h,0,s+MS_BLOCK_HDR);
    if(!base) return NULL;
    *(SIZE_T*)base=s;
    return base+MS_BLOCK_HDR;
}
void free(void *p){ if(p){ HANDLE h=GetProcessHeap(); HeapFree(h,0,(unsigned char*)p-MS_BLOCK_HDR);} }
/* CRT-3: a product that wraps must not become a short block. */
void* calloc(SIZE_T n, SIZE_T s){
    SIZE_T tot;
    if(!ms_mul_size(n,s,&tot)) return NULL;
    void *p=malloc(tot);
    if(p) memset(p,0,tot);
    return p;
}
void* realloc(void *p, SIZE_T s){
    if(!p) return malloc(s);
    if(s==0){ free(p); return NULL; }
    SIZE_T old=*(SIZE_T*)((unsigned char*)p-MS_BLOCK_HDR);
    void *np=malloc(s);
    /* On failure the original block is left valid and is not freed. */
    if(!np) return NULL;
    memcpy(np,p,old<s?old:s);
    free(p);
    return np;
}
void* HeapReAlloc_wrapper(HANDLE h, DWORD f, PVOID m, SIZE_T s){ (void)h;(void)f;(void)m;(void)s; return NULL; }

void __chkstk_ms(void){}
void ___chkstk_ms(void){}
void* memset(void *s, int c, SIZE_T n){ unsigned char *p=s; for(SIZE_T i=0;i<n;i++) p[i]=(unsigned char)c; return s; }
/* String/memory */
void* memcpy(void *d, const void *s, SIZE_T n){ return memmove(d,s,n); }
void* memmove(void *d, const void *s, SIZE_T n){
    unsigned char *dd=d; const unsigned char *ss=s;
    if(dd<ss) for(SIZE_T i=0;i<n;i++) dd[i]=ss[i];
    else for(SIZE_T i=n;i>0;i--) dd[i-1]=ss[i-1];
    return d;
}
SIZE_T strlen(const char *s){ SIZE_T n=0; while(s[n]) n++; return n; }
int strcmp(const char *a, const char *b){ while(*a&&*b&&*a==*b){a++;b++;} return (unsigned char)*a-(unsigned char)*b; }
int strncmp(const char *a, const char *b, SIZE_T n){ for(SIZE_T i=0;i<n;i++){ if(a[i]!=b[i]) return (unsigned char)a[i]-(unsigned char)b[i]; if(a[i]==0) return 0; } return 0; }
char* strerror(int e){ (void)e; return "unknown"; }
int wcscmp(const WCHAR *a, const WCHAR *b){ while(*a&&*b&&*a==*b){a++;b++;} return *a-*b; }
SIZE_T wcslen(const WCHAR *s){ SIZE_T n=0; while(s[n]) n++; return n; }
WCHAR* wcscpy(WCHAR *d, const WCHAR *s){ WCHAR *r=d; while((*d++=*s++)); return r; }
WCHAR* wcscat(WCHAR *d, const WCHAR *s){ WCHAR *p=d; while(*p) p++; while((*p++=*s++)); return d; }

/* localeconv dummy */
struct lconv { char *decimal_point; char *thousands_sep; char *grouping; char *int_curr_symbol; char *currency_symbol; char *mon_decimal_point; char *mon_thousands_sep; char *mon_grouping; char *positive_sign; char *negative_sign; char int_frac_digits; char frac_digits; char p_cs_precedes; char p_sep_by_space; char n_cs_precedes; char n_sep_by_space; char p_sign_posn; char n_sign_posn; };
static struct lconv g_lconv = { ".", "", "", "", "", "", "", "", "", "", 127,127,127,127,127,127,127,127 };
struct lconv* localeconv(void){ return &g_lconv; }

/* signal stub */
typedef void (*sighandler_t)(int);
sighandler_t signal(int sig, sighandler_t h){ (void)sig;(void)h; return NULL; }

/* The standard output handle (StandardOutput). An unset handle (NULL) makes the
 * WriteFile fail and the text is dropped, as in the Windows CRT. It never falls
 * back to the input handle. */
static void write_stdout(const char *s, size_t len){
    HANDLE h=GetStdHandle((DWORD)-11);
    DWORD w=0;
    WriteFile(h, s, (DWORD)len, &w, NULL);
}
static void write_stderr(const char *s, size_t len){
    HANDLE h=GetStdHandle((DWORD)-12);
    DWORD w=0;
    WriteFile(h, s, (DWORD)len, &w, NULL);
}
/* &g_iob[2] (stderr) goes to the standard error handle; any other stream goes to
 * the standard output handle (the probes use stdout and stderr only). */
static void write_stream(void *stream, const char *s, size_t len){
    if(stream == (void*)&g_iob[2]) write_stderr(s,len);
    else write_stdout(s,len);
}

/* FILE* based output: the stream picks the handle (write_stream). */
int fprintf(void *stream, const char *fmt, ...){
    char buf[4096];
    va_list ap; va_start(ap,fmt);
    int n=(int)ms_stored(my_vsprintf(buf,sizeof(buf),fmt,ap),sizeof(buf));
    va_end(ap);
    if(n>0) write_stream(stream,buf,(size_t)n);
    return n;
}
int vfprintf(void *stream, const char *fmt, va_list ap){
    char buf[4096];
    int n=(int)ms_stored(my_vsprintf(buf,sizeof(buf),fmt,ap),sizeof(buf));
    if(n>0) write_stream(stream,buf,(size_t)n);
    return n;
}
int printf(const char *fmt, ...){
    char buf[4096];
    va_list ap; va_start(ap,fmt);
    int n=(int)ms_stored(my_vsprintf(buf,sizeof(buf),fmt,ap),sizeof(buf));
    va_end(ap);
    if(n>0) write_stdout(buf,(size_t)n);
    return n;
}
int vprintf(const char *fmt, va_list ap){
    char buf[4096];
    int n=(int)ms_stored(my_vsprintf(buf,sizeof(buf),fmt,ap),sizeof(buf));
    if(n>0) write_stdout(buf,(size_t)n);
    return n;
}
int puts(const char *s){
    size_t l=strlen(s);
    write_stdout(s,l);
    write_stdout("\n",1);
    return (int)l+1;
}
int putchar(int c){ char ch=(char)c; write_stdout(&ch,1); return c; }
int fputc(int c, void *stream){
    char ch=(char)c;
    write_stream(stream,&ch,1);
    return c;
}
int fputs(const char *s, void *stream){ size_t l=strlen(s); write_stream(stream,s,l); return (int)l; }
int fflush(void *s){ (void)s; return 0; }
void* fopen(const char *a, const char *b){ (void)a;(void)b; return NULL; }
int fclose(void *f){ (void)f; return 0; }
int fgets_dummy(char *s, int n, void *stream){ (void)s;(void)n;(void)stream; return 0; }
/* fgets on stdin reads the standard input handle one byte at a time until a
 * newline (kept) or n-1 bytes, then NUL-terminates. Any other stream returns NULL,
 * as before. End of input or a read error with nothing read returns NULL. */
__declspec(dllimport) BOOL WINAPI ReadFile(HANDLE, void*, DWORD, DWORD*, void*);
char* fgets(char *s, int n, void *stream){
    if(n<=0 || !s) return NULL;
    s[0]='\0';
    if(stream != (void*)&g_iob[0]) return NULL;
    HANDLE h=GetStdHandle((DWORD)-10);
    if(!h || h==(HANDLE)(intptr_t)-1) return NULL;
    int i=0;
    while(i < n-1){
        char c=0; DWORD r=0;
        if(!ReadFile(h,&c,1,&r,NULL) || r==0) break;
        s[i++]=c;
        if(c=='\n') break;
    }
    if(i==0) return NULL;
    s[i]='\0';
    return s;
}
/* fread on stdin reads the standard input handle until count items of size bytes
 * are read or the input ends. Returns the number of whole items read. Any other
 * stream reads nothing. */
SIZE_T fread(void *p, SIZE_T size, SIZE_T count, void *stream){
    SIZE_T total=0, got=0;
    if(!p || !ms_mul_size(size, count, &total) || total==0) return 0;
    if(stream != (void*)&g_iob[0]) return 0;
    HANDLE h=GetStdHandle((DWORD)-10);
    if(!h || h==(HANDLE)(intptr_t)-1) return 0;
    while(got < total){
        SIZE_T left = total - got;
        DWORD want = left > 0x10000 ? 0x10000u : (DWORD)left;
        DWORD r=0;
        if(!ReadFile(h, (char*)p + got, want, &r, NULL) || r==0) break;
        got += r;
    }
    return got / size;
}
int sscanf(const char *a, const char *b, ...){ (void)a;(void)b; return 0; }
/* CRT-1: format into a local buffer of MS_SPRINTF_LIMIT bytes, then copy only
 * the bytes the output needs. The caller's buffer is still unsized, so this
 * bounds the write to MS_SPRINTF_LIMIT but cannot protect a smaller buffer. */
static int ms_bounded_vsprintf(char *s, const char *fmt, va_list ap){
    char tmp[MS_SPRINTF_LIMIT];
    int n=(int)ms_stored(my_vsprintf(tmp,sizeof(tmp),fmt,ap),sizeof(tmp));
    memcpy(s,tmp,(size_t)ms_sprintf_write_size((SIZE_T)n));
    return n;
}
int sprintf(char *s, const char *fmt, ...){
    va_list ap; va_start(ap,fmt);
    int n=ms_bounded_vsprintf(s,fmt,ap);
    va_end(ap);
    return n;
}
int vsprintf(char *s, const char *fmt, va_list ap){ return ms_bounded_vsprintf(s,fmt,ap); }
int snprintf(char *s, SIZE_T n, const char *fmt, ...){
    va_list ap; va_start(ap,fmt);
    int r=my_vsprintf(s,n,fmt,ap);
    va_end(ap);
    return r;
}
int vsnprintf(char *s, SIZE_T n, const char *fmt, va_list ap){ return my_vsprintf(s,n,fmt,ap); }
int _snprintf(char *s, SIZE_T n, const char *fmt, ...){
    va_list ap; va_start(ap,fmt);
    int r=my_vsprintf(s,n,fmt,ap);
    va_end(ap);
    return r;
}
int _vsnprintf(char *s, SIZE_T n, const char *fmt, va_list ap){ return my_vsprintf(s,n,fmt,ap); }

void exit(int code){
    while(g_atexit_count > 0){
        void (*f)(void) = g_atexit_fns[--g_atexit_count];
        f();
    }
    NtTerminateProcess((HANDLE)(intptr_t)-1, code);
    while(1){}
}
/* _exit and _Exit do not run atexit handlers (CRT-7). */
int _exit(int c){ NtTerminateProcess((HANDLE)(intptr_t)-1, c); while(1){} return 0; }
void _Exit(int c){ NtTerminateProcess((HANDLE)(intptr_t)-1, c); while(1){} }

/* Exception handler */
/* No scope table is searched here, so no handler matches. Return
 * ExceptionContinueSearch (1); 0 is ExceptionContinueExecution and would
 * resume at the faulting instruction (CRT-6). */
int __C_specific_handler(void *a, void *b, void *c, void *d){ (void)a;(void)b;(void)c;(void)d; return 1; }
void* __C_specific_handler_wrapper = __C_specific_handler;

/* Startup helpers */
void ms_load_cmdline(void);
/* argv is split from the process command line by the Windows rules (see
 * ms_startup_argv, defined with the command line below). An empty line or a
 * line that does not fit gives argv {"prog"}. */
int ms_startup_argv(char ***out);
int __getmainargs(int *argc, char ***argv, char ***envp, int expand, void *si){
    ms_load_cmdline();
    (void)expand;(void)si;
    static char *env[] = {NULL};
    static char *fallback[] = {"prog", NULL};
    char **args = NULL;
    int n = ms_startup_argv(&args);
    if(n < 1){ n = 1; args = fallback; }
    if(argc) *argc=n;
    if(argv) *argv=args;
    if(envp) *envp=env;
    return 0;
}
int _initterm_e(void (**a)(void), void (**b)(void)){ return _initterm(a,b); }

/* errno helpers */
int* _errno_func(void){ return &g_errno; }

/* Additional exports expected */
int _fileno(void *f){ (void)f; return 1; }
int _isatty(int f){ (void)f; return 0; }
void* _aenvptr = NULL;
int _dowildcard = 0;

/* ---- K32-M msvcrt additions (MS-1) ----------------------------------------
 * setjmp/longjmp, fwrite, getenv, setvbuf, strncpy and _snwprintf. The stdio
 * streams are unbuffered, so setvbuf only checks its mode. getenv returns a
 * pointer into a snapshot taken at the first call (the CRT copies the
 * environment too); the snapshot holds 16 KB and drops what does not fit. */

/* jmp_buf layout (x64, used only by setjmp and longjmp here):
 *   0 Frame, 8 Rbx, 16 Rsp (the caller's, after the return), 24 Rbp,
 *   32 Rsi, 40 Rdi, 48 R12, 56 R13, 64 R14, 72 R15, 80 Rip.
 * The XMM registers are not saved. */
__asm__(
    ".text\n"
    ".globl _setjmp\n"
    "_setjmp:\n"
    "    leaq 8(%rsp), %rax\n"
    "    movq %rax, 16(%rcx)\n"
    "    movq (%rsp), %rax\n"
    "    movq %rax, 80(%rcx)\n"
    "    movq %rbx, 8(%rcx)\n"
    "    movq %rbp, 24(%rcx)\n"
    "    movq %rsi, 32(%rcx)\n"
    "    movq %rdi, 40(%rcx)\n"
    "    movq %r12, 48(%rcx)\n"
    "    movq %r13, 56(%rcx)\n"
    "    movq %r14, 64(%rcx)\n"
    "    movq %r15, 72(%rcx)\n"
    "    xorl %eax, %eax\n"
    "    ret\n"
    ".globl longjmp\n"
    "longjmp:\n"
    "    movl %edx, %eax\n"
    "    testl %eax, %eax\n"
    "    jnz 1f\n"
    "    movl $1, %eax\n"
    "1:\n"
    "    movq 8(%rcx), %rbx\n"
    "    movq 24(%rcx), %rbp\n"
    "    movq 32(%rcx), %rsi\n"
    "    movq 40(%rcx), %rdi\n"
    "    movq 48(%rcx), %r12\n"
    "    movq 56(%rcx), %r13\n"
    "    movq 64(%rcx), %r14\n"
    "    movq 72(%rcx), %r15\n"
    "    movq 16(%rcx), %rsp\n"
    "    jmp *80(%rcx)\n"
);

char *strncpy(char *d, const char *s, SIZE_T n){
    SIZE_T i = 0;
    for(; i < n && s[i]; ++i) d[i] = s[i];
    for(; i < n; ++i) d[i] = '\0';
    return d;
}

/* Mode check only: _IOFBF 0, _IOLBF 0x40, _IONBF 4. Every stream is unbuffered. */
int setvbuf(void *stream, char *buf, int mode, SIZE_T size){
    (void)stream;(void)buf;(void)size;
    if(mode != 0 && mode != 0x40 && mode != 4) return -1;
    return 0;
}

/* stderr is &g_iob[2]; any other stream writes to stdout (the probes use
 * stdout and stderr only). Returns count when every item was written. */
SIZE_T fwrite(const void *p, SIZE_T size, SIZE_T count, void *stream){
    SIZE_T total = 0;
    if(!ms_mul_size(size, count, &total) || total == 0) return 0;
    if(stream == (void*)&g_iob[2]) write_stderr((const char*)p, total);
    else write_stdout((const char*)p, total);
    return count;
}

/* Process command line, narrowed (non-ASCII as '?'), for _acmdln. */
static char g_ms_cmdline[2048];
static void ms_narrow(const WCHAR *in, SIZE_T n, char *out, SIZE_T cap){
    SIZE_T i = 0;
    for(; i < n && i + 1 < cap; ++i) out[i] = (in[i] <= 0x7F) ? (char)in[i] : '?';
    out[i] = '\0';
}
char *_acmdln = NULL;
void ms_load_cmdline(void){
    PTEB teb = NULL;
    __asm__ volatile("movq %%gs:0x30, %0" : "=r"(teb) :: "memory");
    PEB *peb = teb ? teb->ProcessEnvironmentBlock : NULL;
    RTL_USER_PROCESS_PARAMETERS *pp = peb ? peb->ProcessParameters : NULL;
    if(!pp || !pp->CommandLine.Buffer) g_ms_cmdline[0] = '\0';
    else ms_narrow(pp->CommandLine.Buffer, pp->CommandLine.Length / sizeof(WCHAR), g_ms_cmdline, sizeof(g_ms_cmdline));
    _acmdln = g_ms_cmdline;
}

/* The startup argv: g_ms_cmdline split by ms_split_cmdline. Returns argc or -1. */
static char g_ms_args[4096];
static char *g_ms_argv[129];
int ms_startup_argv(char ***out){
    int n = ms_split_cmdline(g_ms_cmdline, g_ms_args, sizeof(g_ms_args), g_ms_argv, 128);
    *out = g_ms_argv;
    return n;
}

/* Environment snapshot for getenv: NAME=value entries, each NUL terminated,
 * the list ended by an empty entry. */
static char g_ms_env[16384];
static int g_ms_env_loaded = 0;
static void ms_load_env(void){
    if(g_ms_env_loaded) return;
    g_ms_env_loaded = 1;
    PTEB teb = NULL;
    __asm__ volatile("movq %%gs:0x30, %0" : "=r"(teb) :: "memory");
    PEB *peb = teb ? teb->ProcessEnvironmentBlock : NULL;
    RTL_USER_PROCESS_PARAMETERS *pp = peb ? peb->ProcessParameters : NULL;
    const WCHAR *env = pp ? (const WCHAR *)pp->Environment : NULL;
    SIZE_T limit = (pp && pp->EnvironmentSize) ? pp->EnvironmentSize / sizeof(WCHAR) : 32768;
    SIZE_T out = 0, pos = 0;
    if(!env){ g_ms_env[0] = '\0'; return; }
    while(pos < limit && env[pos]){
        SIZE_T len = 0;
        while(pos + len < limit && env[pos + len]) ++len;
        if(out + len + 2 > sizeof(g_ms_env)) break;
        ms_narrow(env + pos, len, g_ms_env + out, len + 1);
        out += len;
        g_ms_env[out++] = '\0';
        pos += len + 1;
    }
    g_ms_env[out] = '\0';
}
static int ms_ci_eq(char a, char b){
    if(a >= 'a' && a <= 'z') a = (char)(a - 'a' + 'A');
    if(b >= 'a' && b <= 'z') b = (char)(b - 'a' + 'A');
    return a == b;
}
/* getenv: the value of name, or NULL. The name compare is ASCII case-insensitive. */
char *getenv(const char *name){
    if(!name || !name[0]) return NULL;
    ms_load_env();
    SIZE_T nl = strlen(name);
    const char *p = g_ms_env;
    while(*p){
        SIZE_T l = strlen(p);
        if(l > nl && p[nl] == '='){
            SIZE_T i = 0;
            while(i < nl && ms_ci_eq(p[i], name[i])) ++i;
            if(i == nl) return (char*)(p + nl + 1);
        }
        p += l + 1;
    }
    return NULL;
}

/* The wide formatter (ms_wformat) is in msvcrt_wformat.h, so the host suite can test it. */
#include "msvcrt_wformat.h"

int _snwprintf(WCHAR *buf, SIZE_T n, const WCHAR *fmt, ...){
    va_list ap;
    va_start(ap, fmt);
    int r = ms_wformat(buf, n, fmt, ap);
    va_end(ap);
    return r;
}
#else
/* Linux stub */
#endif
