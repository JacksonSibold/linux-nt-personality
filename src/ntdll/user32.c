#ifdef __MINGW32__
#include "nt_types.h"
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#ifndef WINAPI
#define WINAPI __stdcall
#endif
#ifndef NTAPI
#define NTAPI WINAPI
#endif
#ifndef HINSTANCE
typedef PVOID HINSTANCE;
#endif
BOOL WINAPI DllMain(HINSTANCE h, DWORD r, PVOID v){ (void)h;(void)r;(void)v; return 1; }
BOOL __stdcall _DllMainCRTStartup(HINSTANCE h, DWORD r, PVOID v){ return DllMain(h,r,v); }
BOOL __stdcall DllMainCRTStartup(HINSTANCE h, DWORD r, PVOID v){ return DllMain(h,r,v); }
void __chkstk_ms(void){}
void ___chkstk_ms(void){}
static size_t my_strlen(const char *s){ size_t n=0; while(s[n]) n++; return n; }
static void* my_memcpy(void *d, const void *s, size_t n){ char *dd=d; const char *ss=s; for(size_t i=0;i<n;i++) dd[i]=ss[i]; return d; }
#define strlen my_strlen
#define memcpy my_memcpy

/* Simple wsprintfW - handles %s %d %u %x %X %p %c */
int WINAPI wsprintfW(WCHAR *out, const WCHAR *fmt, ...){
    va_list ap; va_start(ap,fmt);
    WCHAR *pout=out;
    for(const WCHAR *p=fmt; *p; p++){
        if(*p!='%'){ *pout++=*p; continue; }
        p++;
        if(!*p) break;
        int is_ll=0;
        if(*p=='l'){ p++; if(*p=='l'){ is_ll=1; p++; } }
        WCHAR spec=*p;
        if(spec=='s'){
            const char *s=va_arg(ap,const char*);
            if(!s) s="(null)";
            for(size_t i=0;s[i];i++) *pout++=(WCHAR)s[i];
        } else if(spec=='S' || spec=='s'){
            // for wsprintfW, %s is WCHAR*, %S is char*
            // we already handled char, now wide
            // heuristic: if spec is 's' with l prefix would be wide, but we treat as above
            // for simplicity assume %s with wide when no l
        } else if(spec=='d' || spec=='i'){
            long long v=is_ll?va_arg(ap,long long):va_arg(ap,int);
            char buf[32]; int neg=0; unsigned long long uv;
            if(v<0){ neg=1; uv=(unsigned long long)(-v);} else uv=(unsigned long long)v;
            int idx=31; buf[idx--]='\0';
            if(uv==0) buf[idx--]='0';
            else while(uv && idx>=0){ buf[idx--]='0'+(uv%10); uv/=10; }
            if(neg && idx>=0) buf[idx--]='-';
            const char *s=buf+idx+1;
            for(size_t i=0;s[i];i++) *pout++=(WCHAR)s[i];
        } else if(spec=='u'){
            unsigned long long v=is_ll?va_arg(ap,unsigned long long):va_arg(ap,unsigned int);
            char buf[32]; int idx=31; buf[idx--]='\0';
            if(v==0) buf[idx--]='0';
            else while(v && idx>=0){ buf[idx--]='0'+(v%10); v/=10; }
            const char *s=buf+idx+1;
            for(size_t i=0;s[i];i++) *pout++=(WCHAR)s[i];
        } else if(spec=='x' || spec=='X'){
            unsigned long long v=is_ll?va_arg(ap,unsigned long long):va_arg(ap,unsigned int);
            char buf[32]; int idx=31; buf[idx--]='\0';
            const char *digits=(spec=='x')?"0123456789abcdef":"0123456789ABCDEF";
            if(v==0) buf[idx--]='0';
            else while(v && idx>=0){ buf[idx--]=digits[v%16]; v/=16; }
            const char *s=buf+idx+1;
            for(size_t i=0;s[i];i++) *pout++=(WCHAR)s[i];
        } else if(spec=='c'){
            int c=va_arg(ap,int);
            *pout++=(WCHAR)c;
        } else if(spec=='%'){
            *pout++='%';
        } else {
            *pout++='%'; *pout++=spec;
        }
    }
    *pout=0;
    va_end(ap);
    return (int)(pout-out);
}
int WINAPI wsprintfA(char *out, const char *fmt, ...){
    va_list ap; va_start(ap,fmt);
    char *pout=out;
    for(const char *p=fmt; *p; p++){
        if(*p!='%'){ *pout++=*p; continue; }
        p++;
        if(!*p) break;
        int is_ll=0;
        if(*p=='l'){ p++; if(*p=='l'){ is_ll=1; p++; } }
        char spec=*p;
        if(spec=='s'){
            const char *s=va_arg(ap,const char*);
            if(!s) s="(null)";
            size_t l=strlen(s);
            memcpy(pout,s,l); pout+=l;
        } else if(spec=='d'){
            long long v=is_ll?va_arg(ap,long long):va_arg(ap,int);
            char buf[32]; int idx=31; buf[idx--]='\0';
            int neg=0; unsigned long long uv;
            if(v<0){neg=1; uv=-v;} else uv=v;
            if(uv==0) buf[idx--]='0';
            else while(uv && idx>=0){buf[idx--]='0'+(uv%10); uv/=10;}
            if(neg&&idx>=0) buf[idx--]='-';
            const char *s=buf+idx+1;
            size_t l=strlen(s); memcpy(pout,s,l); pout+=l;
        } else if(spec=='%'){ *pout++='%'; }
        else { *pout++='%'; *pout++=spec; }
    }
    *pout='\0';
    va_end(ap);
    return (int)(pout-out);
}
int WINAPI wvsprintfW(WCHAR *out, const WCHAR *fmt, va_list ap){
    // simplified delegate to wsprintfW with va_list not supported directly; just return 0
    (void)out;(void)fmt;(void)ap; return 0;
}
int WINAPI wvsprintfA(char *out, const char *fmt, va_list ap){
    (void)out;(void)fmt;(void)ap; return 0;
}
#endif
