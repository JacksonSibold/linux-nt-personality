/*
 * msvcrt_format.h - the printf formatter behind the msvcrt snprintf family.
 *
 * Header-only so tests/ntdll-host can run it on the host (test_msvcrt_policy.c).
 * Handles %s %S %c %d %i %u %x %X %p %f-family (prints 0.0) and %%, with the
 * l, ll, z and I64 length modifiers. Flags and widths are not supported.
 */
#ifndef NTDLL_MSVCRT_FORMAT_H
#define NTDLL_MSVCRT_FORMAT_H

#include "nt_types.h"
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* CRT-2: count every character, store it only while it leaves room for the NUL. */
static inline void ms_put(char *out, size_t outsz, size_t *pos, char c){
    if(*pos+1<outsz) out[*pos]=c;
    (*pos)++;
}
static inline void ms_puts(char *out, size_t outsz, size_t *pos, const char *s){
    for(size_t i=0;s[i];i++) ms_put(out,outsz,pos,s[i]);
}

/* CRT-2: characters actually stored (without the NUL) when the formatter
 * returned needed and the buffer holds outsz bytes. */
static inline size_t ms_stored(int needed, size_t outsz){
    size_t n=(size_t)needed;
    if(outsz==0) return 0;
    return n>=outsz ? outsz-1 : n;
}

/* Formats into out[outsz] and always NUL-terminates when outsz is not 0.
 * Returns the number of characters the full output needs, excluding the NUL,
 * as C99 snprintf does. outsz 0 means no buffer: nothing is written. */
static int my_vsprintf(char *out, size_t outsz, const char *fmt, va_list ap){
    size_t pos=0;
    for(const char *p=fmt; *p; p++){
        if(*p!='%'){ ms_put(out,outsz,&pos,*p); continue; }
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
        if(spec=='s' && !is_long){
            const char *s=va_arg(ap,const char*);
            if(!s) s="(null)";
            ms_puts(out,outsz,&pos,s);
        } else if(spec=='s' || spec=='S'){ // %ls and %S: wide string, narrowed to one byte per character
            const WCHAR *ws=va_arg(ap,const WCHAR*);
            if(!ws) ms_puts(out,outsz,&pos,"(null)");
            else for(size_t i=0; ws[i]; i++) ms_put(out,outsz,&pos,(char)(ws[i]&0xFF));
        } else if(spec=='c'){
            int c=va_arg(ap,int);
            ms_put(out,outsz,&pos,(char)c);
        } else if(spec=='d' || spec=='i'){
            long long v = is_ll?va_arg(ap,long long): is_long?va_arg(ap,long): is_z? (long long)va_arg(ap,SIZE_T): va_arg(ap,int);
            char buf[32]; int neg=0; unsigned long long uv;
            if(v<0){ neg=1; uv=(unsigned long long)(-v); } else uv=(unsigned long long)v;
            int idx=31; buf[idx--]='\0';
            if(uv==0) buf[idx--]='0';
            else while(uv && idx>=0){ buf[idx--]='0'+(uv%10); uv/=10; }
            if(neg && idx>=0) buf[idx--]='-';
            ms_puts(out,outsz,&pos,buf+idx+1);
        } else if(spec=='u'){
            unsigned long long v = is_ll?va_arg(ap,unsigned long long): is_long?va_arg(ap,unsigned long): is_z?va_arg(ap,SIZE_T): va_arg(ap,unsigned int);
            char buf[32]; int idx=31; buf[idx--]='\0';
            if(v==0) buf[idx--]='0';
            else while(v && idx>=0){ buf[idx--]='0'+(v%10); v/=10; }
            ms_puts(out,outsz,&pos,buf+idx+1);
        } else if(spec=='x' || spec=='X'){
            unsigned long long v = is_ll?va_arg(ap,unsigned long long): is_long?va_arg(ap,unsigned long): is_z?va_arg(ap,SIZE_T): va_arg(ap,unsigned int);
            char buf[32]; int idx=31; buf[idx--]='\0';
            const char *digits = (spec=='x')?"0123456789abcdef":"0123456789ABCDEF";
            if(v==0) buf[idx--]='0';
            else while(v && idx>=0){ buf[idx--]=digits[v%16]; v/=16; }
            ms_puts(out,outsz,&pos,buf+idx+1);
        } else if(spec=='p'){
            void *ptr=va_arg(ap,void*);
            unsigned long long v=(unsigned long long)(uintptr_t)ptr;
            char buf[32]; int idx=31; buf[idx--]='\0';
            if(v==0) buf[idx--]='0';
            else while(v && idx>=0){ buf[idx--]="0123456789abcdef"[v%16]; v/=16; }
            // prefix 0x
            ms_puts(out,outsz,&pos,"0x");
            ms_puts(out,outsz,&pos,buf+idx+1);
        } else if(spec=='f' || spec=='F' || spec=='e' || spec=='E' || spec=='g' || spec=='G'){
            double d=va_arg(ap,double);
            (void)d;
            // simplified: output "0.0"
            ms_puts(out,outsz,&pos,"0.0");
        } else if(spec=='%'){
            ms_put(out,outsz,&pos,'%');
        } else {
            // unknown, copy as is
            ms_put(out,outsz,&pos,'%');
            ms_put(out,outsz,&pos,spec);
        }
    }
    if(outsz) out[ms_stored((int)pos,outsz)]='\0';
    return (int)pos;
}

#endif /* NTDLL_MSVCRT_FORMAT_H */
