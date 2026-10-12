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

/* Wide formatter shared by wsprintfW and wvsprintfW (U32-2). Handles %s %d %u %x %X %c. */
static int fmt_wide(WCHAR *out, const WCHAR *fmt, va_list ap){
    WCHAR *pout=out;
    for(const WCHAR *p=fmt; *p; p++){
        if(*p!='%'){ *pout++=*p; continue; }
        p++;
        if(!*p) break;
        int is_ll=0;
        int narrow_str=0;
        if(*p=='h'){ narrow_str=1; p++; }
        else if(*p=='l'){ p++; if(*p=='l'){ is_ll=1; p++; } }
        WCHAR spec=*p;
        /* U32-1: in wsprintfW %s and %ls are wide strings; %S and %hs are narrow. */
        if(spec=='s' && !narrow_str){
            const WCHAR *s=va_arg(ap,const WCHAR*);
            if(!s){ const char *nul="(null)"; for(size_t i=0;nul[i];i++) *pout++=(WCHAR)nul[i]; }
            else for(;*s;s++) *pout++=*s;
        } else if(spec=='s' || spec=='S'){
            const char *s=va_arg(ap,const char*);
            if(!s) s="(null)";
            for(size_t i=0;s[i];i++) *pout++=(WCHAR)(unsigned char)s[i];
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
    return (int)(pout-out);
}
int WINAPI wsprintfW(WCHAR *out, const WCHAR *fmt, ...){
    va_list ap; va_start(ap,fmt);
    int n=fmt_wide(out,fmt,ap);
    va_end(ap);
    return n;
}
int WINAPI wvsprintfW(WCHAR *out, const WCHAR *fmt, va_list ap){
    return fmt_wide(out,fmt,ap);
}

/* Narrow formatter shared by wsprintfA and wvsprintfA (U32-2). */
static int fmt_narrow(char *out, const char *fmt, va_list ap){
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
        } else if(spec=='c'){
            /* U32-3: %c prints one character, as in wsprintfW. */
            int c=va_arg(ap,int);
            *pout++=(char)c;
        } else if(spec=='%'){ *pout++='%'; }
        else { *pout++='%'; *pout++=spec; }
    }
    *pout='\0';
    return (int)(pout-out);
}
int WINAPI wsprintfA(char *out, const char *fmt, ...){
    va_list ap; va_start(ap,fmt);
    int n=fmt_narrow(out,fmt,ap);
    va_end(ap);
    return n;
}
int WINAPI wvsprintfA(char *out, const char *fmt, va_list ap){
    return fmt_narrow(out,fmt,ap);
}
#endif
