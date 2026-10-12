/* check_exception.c */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc, char **argv){
    const char *path = argc>1 ? argv[1] : "tests/differential/golden/latest/probe_exception.json";
    FILE *f=fopen(path,"rb");
    if(!f) f=fopen("../../tests/differential/golden/latest/probe_exception.json","rb");
    if(!f && getenv("NTPERS_ROOT")){ char alt[1024]; snprintf(alt,sizeof alt,"%s/tests/differential/golden/latest/probe_exception.json",getenv("NTPERS_ROOT")); f=fopen(alt,"rb"); }
    if(!f){ fprintf(stderr,"cannot open %s\n",path); return 1; }
    fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
    char *b=malloc(sz+1); fread(b,1,sz,f); b[sz]=0; fclose(f);
    int fails=0;
    if(!strstr(b,"\"name\":\"av_read\"") || !strstr(b,"\"info0\":0")){ fprintf(stderr,"MISMATCH av_read 0\n"); fails++; }
    if(!strstr(b,"\"name\":\"av_write\"") || !strstr(b,"\"info0\":1")){ fprintf(stderr,"MISMATCH av_write 1\n"); fails++; }
    if(!strstr(b,"\"name\":\"av_execute\"") || !strstr(b,"\"info0\":8")){ fprintf(stderr,"MISMATCH av_execute 8\n"); fails++; }
    if(!strstr(b,"\"hex\":\"0xC0000094\"")){ fprintf(stderr,"MISMATCH div0 C0000094\n"); fails++; }
    if(!strstr(b,"\"hex\":\"0x80000003\"")){ fprintf(stderr,"MISMATCH breakpoint 80000003\n"); fails++; }
    if(!strstr(b,"\"hex\":\"0x80000001\"")){ fprintf(stderr,"MISMATCH guard 80000001\n"); fails++; }
    // veh order
    if(!strstr(b,"\"veh\"")){ fprintf(stderr,"MISMATCH veh order\n"); fails++; }
    free(b);
    if(fails){ printf("check_exception: %d failures\n",fails); return 1; }
    printf("check_exception: PASS\n"); return 0;
}
