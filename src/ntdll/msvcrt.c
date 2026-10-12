/*
 * msvcrt.c - minimal CRT forwarding to kernel32/ntdll
 * Implements subset required by mingw probes (approx 35 exports)
 * Compiles as freestanding DLL via Makefile.ntdll
 */
#ifdef __MINGW32__
#include "nt_types.h"
#include "ntstatus.h"
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
int atexit(void (*f)(void)){ (void)f; return 0; }
typedef void (*_onexit_t)(void);
_onexit_t _onexit(_onexit_t f){ (void)f; return NULL; }

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
void* calloc(SIZE_T n, SIZE_T s){
    SIZE_T tot=n*s;
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

/* Simple vsnprintf for CRT printf - handles %s %d %u %x %X %p %llu %lu %lld %c %f? minimal */
static int my_vsprintf(char *out, size_t outsz, const char *fmt, va_list ap){
    size_t pos=0;
    for(const char *p=fmt; *p && pos+1<outsz; p++){
        if(*p!='%'){ out[pos++]=*p; continue; }
        p++;
        if(!*p) break;
        // flags/width ignored for simplicity, handle length modifiers
        int is_long=0, is_ll=0, is_z=0;
        if(*p=='l'){ is_long=1; p++; if(*p=='l'){ is_ll=1; p++; } }
        else if(*p=='z'){ is_z=1; p++; }
        else if(*p=='I'){ // I64
            if(p[1]=='6'&&p[2]=='4'){ is_ll=1; p+=3; }
        }
        char spec=*p;
        if(spec=='s'){
            const char *s=va_arg(ap,const char*);
            if(!s) s="(null)";
            size_t sl=strlen(s);
            for(size_t i=0;i<sl && pos+1<outsz;i++) out[pos++]=s[i];
        } else if(spec=='S'){ // wide string
            const WCHAR *ws=va_arg(ap,const WCHAR*);
            if(!ws){ const char *s="(null)"; for(size_t i=0;s[i]&&pos+1<outsz;i++) out[pos++]=s[i]; }
            else for(size_t i=0; ws[i]&&pos+1<outsz;i++) out[pos++]=(char)(ws[i]&0xFF);
        } else if(spec=='c'){
            int c=va_arg(ap,int);
            if(pos+1<outsz) out[pos++]=(char)c;
        } else if(spec=='d' || spec=='i'){
            long long v = is_ll?va_arg(ap,long long): is_long?va_arg(ap,long): is_z? (long long)va_arg(ap,SIZE_T): va_arg(ap,int);
            char buf[32]; int neg=0; unsigned long long uv;
            if(v<0){ neg=1; uv=(unsigned long long)(-v); } else uv=(unsigned long long)v;
            int idx=31; buf[idx--]='\0';
            if(uv==0) buf[idx--]='0';
            else while(uv && idx>=0){ buf[idx--]='0'+(uv%10); uv/=10; }
            if(neg && idx>=0) buf[idx--]='-';
            const char *s=buf+idx+1;
            for(size_t i=0;s[i]&&pos+1<outsz;i++) out[pos++]=s[i];
        } else if(spec=='u'){
            unsigned long long v = is_ll?va_arg(ap,unsigned long long): is_long?va_arg(ap,unsigned long): is_z?va_arg(ap,SIZE_T): va_arg(ap,unsigned int);
            char buf[32]; int idx=31; buf[idx--]='\0';
            if(v==0) buf[idx--]='0';
            else while(v && idx>=0){ buf[idx--]='0'+(v%10); v/=10; }
            const char *s=buf+idx+1;
            for(size_t i=0;s[i]&&pos+1<outsz;i++) out[pos++]=s[i];
        } else if(spec=='x' || spec=='X'){
            unsigned long long v = is_ll?va_arg(ap,unsigned long long): is_long?va_arg(ap,unsigned long): is_z?va_arg(ap,SIZE_T): va_arg(ap,unsigned int);
            char buf[32]; int idx=31; buf[idx--]='\0';
            const char *digits = (spec=='x')?"0123456789abcdef":"0123456789ABCDEF";
            if(v==0) buf[idx--]='0';
            else while(v && idx>=0){ buf[idx--]=digits[v%16]; v/=16; }
            const char *s=buf+idx+1;
            for(size_t i=0;s[i]&&pos+1<outsz;i++) out[pos++]=s[i];
        } else if(spec=='p'){
            void *ptr=va_arg(ap,void*);
            unsigned long long v=(unsigned long long)(uintptr_t)ptr;
            char buf[32]; int idx=31; buf[idx--]='\0';
            if(v==0) buf[idx--]='0';
            else while(v && idx>=0){ buf[idx--]="0123456789abcdef"[v%16]; v/=16; }
            // prefix 0x
            if(pos+2<outsz){ out[pos++]='0'; out[pos++]='x'; }
            const char *s=buf+idx+1;
            for(size_t i=0;s[i]&&pos+1<outsz;i++) out[pos++]=s[i];
        } else if(spec=='f' || spec=='F' || spec=='e' || spec=='E' || spec=='g' || spec=='G'){
            double d=va_arg(ap,double);
            (void)d;
            // simplified: output "0.0"
            const char *s="0.0";
            for(size_t i=0;s[i]&&pos+1<outsz;i++) out[pos++]=s[i];
        } else if(spec=='%'){
            if(pos+1<outsz) out[pos++]='%';
        } else {
            // unknown, copy as is
            if(pos+1<outsz) out[pos++]='%';
            if(pos+1<outsz) out[pos++]=spec;
        }
    }
    /* CRT-2: size 0 means no buffer, so write the NUL only when there is room. */
    if(outsz) out[pos]='\0';
    return (int)pos;
}

static void write_stdout(const char *s, size_t len){
    HANDLE h=GetStdHandle((DWORD)-11);
    if(!h || h==(HANDLE)(intptr_t)-1) h=GetStdHandle((DWORD)-10);
    DWORD w=0;
    WriteFile(h, s, (DWORD)len, &w, NULL);
}
static void write_stderr(const char *s, size_t len){
    HANDLE h=GetStdHandle((DWORD)-12);
    DWORD w=0;
    WriteFile(h, s, (DWORD)len, &w, NULL);
}

/* FILE* based - ignore file, always write to stdout */
int fprintf(void *stream, const char *fmt, ...){
    (void)stream;
    char buf[4096];
    va_list ap; va_start(ap,fmt);
    int n=my_vsprintf(buf,sizeof(buf),fmt,ap);
    va_end(ap);
    if(n>0) write_stdout(buf,(size_t)n);
    return n;
}
int vfprintf(void *stream, const char *fmt, va_list ap){
    (void)stream;
    char buf[4096];
    int n=my_vsprintf(buf,sizeof(buf),fmt,ap);
    if(n>0) write_stdout(buf,(size_t)n);
    return n;
}
int printf(const char *fmt, ...){
    char buf[4096];
    va_list ap; va_start(ap,fmt);
    int n=my_vsprintf(buf,sizeof(buf),fmt,ap);
    va_end(ap);
    if(n>0) write_stdout(buf,(size_t)n);
    return n;
}
int vprintf(const char *fmt, va_list ap){
    char buf[4096];
    int n=my_vsprintf(buf,sizeof(buf),fmt,ap);
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
    (void)stream;
    char ch=(char)c;
    write_stdout(&ch,1);
    return c;
}
int fputs(const char *s, void *stream){ (void)stream; size_t l=strlen(s); write_stdout(s,l); return (int)l; }
int fflush(void *s){ (void)s; return 0; }
void* fopen(const char *a, const char *b){ (void)a;(void)b; return NULL; }
int fclose(void *f){ (void)f; return 0; }
int fgets_dummy(char *s, int n, void *stream){ (void)s;(void)n;(void)stream; return 0; }
char* fgets(char *s, int n, void *stream){ (void)stream; if(n>0) s[0]='\0'; return NULL; }
int sscanf(const char *a, const char *b, ...){ (void)a;(void)b; return 0; }
int sprintf(char *s, const char *fmt, ...){
    va_list ap; va_start(ap,fmt);
    int n=my_vsprintf(s,8192,fmt,ap);
    va_end(ap);
    return n;
}
int vsprintf(char *s, const char *fmt, va_list ap){ return my_vsprintf(s,8192,fmt,ap); }
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

void exit(int code){ NtTerminateProcess((HANDLE)(intptr_t)-1, code); while(1){} }
int _exit(int c){ exit(c); return 0; }
void _Exit(int c){ exit(c); }

/* Exception handler */
int __C_specific_handler(void *a, void *b, void *c, void *d){ (void)a;(void)b;(void)c;(void)d; return 0; }
void* __C_specific_handler_wrapper = __C_specific_handler;

/* Startup helpers */
int __getmainargs(int *argc, char ***argv, char ***envp, int expand, void *si){
    (void)expand;(void)si;
    static char *args[] = {"prog", NULL};
    static char *env[] = {NULL};
    if(argc) *argc=1;
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
void* _acmdln = NULL;
void* _aenvptr = NULL;
int _dowildcard = 0;

#else
/* Linux stub */
#endif
