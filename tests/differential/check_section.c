/* check_section.c */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc, char **argv){
    const char *path = argc>1 ? argv[1] : "tests/differential/golden/latest/probe_section.json";
    FILE *f=fopen(path,"rb");
    if(!f) f=fopen("../../tests/differential/golden/latest/probe_section.json","rb");
    if(!f && getenv("NTPERS_ROOT")){ char alt[1024]; snprintf(alt,sizeof alt,"%s/tests/differential/golden/latest/probe_section.json",getenv("NTPERS_ROOT")); f=fopen(alt,"rb"); }
    if(!f){ fprintf(stderr,"cannot open %s\n",path); return 1; }
    fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
    char *b=malloc(sz+1); fread(b,1,sz,f); b[sz]=0; fclose(f);
    int fails=0;
    if(!strstr(b,"\"hex\":\"0xC0000220\"")){ fprintf(stderr,"MISMATCH MAPPED_ALIGNMENT not found\n"); fails++; }
    if(!strstr(b,"\"hex\":\"0xC000001F\"")){ fprintf(stderr,"MISMATCH INVALID_VIEW_SIZE\n"); fails++; }
    if(!strstr(b,"\"State\":4096") && !strstr(b,"\"StateStr\":\"COMMIT\"")){ fprintf(stderr,"MISMATCH commit state\n"); fails++; }
    if(!strstr(b,"\"Type\":262144")){ fprintf(stderr,"MISMATCH MAPPED type\n"); fails++; }
    // Check that reserve reports Reserve
    if(!strstr(b,"\"State\":8192")){ fprintf(stderr,"MISMATCH reserve state\n"); fails++; }
    free(b);
    if(fails){ printf("check_section: %d failures\n",fails); return 1; }
    printf("check_section: PASS\n"); return 0;
}
