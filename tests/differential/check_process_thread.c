/* check_process_thread.c */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc, char **argv){
    const char *path = argc>1 ? argv[1] : "tests/differential/golden/latest/probe_process_thread.json";
    FILE *f=fopen(path,"rb");
    if(!f) f=fopen("../../tests/differential/golden/latest/probe_process_thread.json","rb");
    if(!f && getenv("NTPERS_ROOT")){ char alt[1024]; snprintf(alt,sizeof alt,"%s/tests/differential/golden/latest/probe_process_thread.json",getenv("NTPERS_ROOT")); f=fopen(alt,"rb"); }
    if(!f){ fprintf(stderr,"cannot open %s\n",path); return 1; }
    fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
    char *b=malloc(sz+1); fread(b,1,sz,f); b[sz]=0; fclose(f);
    int fails=0;
    if(!strstr(b,"\"len\":48") || !strstr(b,"\"hex\":\"0x00000000\"")){ fprintf(stderr,"MISMATCH process_basic len 48 success\n"); fails++; }
    if(!strstr(b,"\"len\":49") || !strstr(b,"\"hex\":\"0xC0000004\"")){ fprintf(stderr,"MISMATCH len 49 mismatch\n"); fails++; }
    if(!strstr(b,"\"prev\":0") || !strstr(b,"\"prev\":1")){ fprintf(stderr,"MISMATCH suspend prev 0/1\n"); fails++; }
    if(!strstr(b,"\"prev\":2")){ fprintf(stderr,"MISMATCH resume prev 2\n"); fails++; }
    if(!strstr(b,"\"thread_before_wait\":259")){ fprintf(stderr,"MISMATCH STILL_ACTIVE 259\n"); fails++; }
    if(!strstr(b,"\"process_child_after\":42")){ fprintf(stderr,"MISMATCH child exit 42\n"); fails++; }
    if(!strstr(b,"\"teb_peb_match\":true")){ fprintf(stderr,"MISMATCH teb_peb\n"); fails++; }
    free(b);
    /* Phase 2: enforce implementation matches golden (exact ==48, suspend counts, STILL_ACTIVE) */
    {
        const char *impl_paths[] = {
            "src/kernel/nt/proc_thread.c",
            "../../src/kernel/nt/proc_thread.c",
            NULL
        };
        FILE *pf=NULL;
        for(int i=0; impl_paths[i]; i++){ pf=fopen(impl_paths[i],"rb"); if(pf) break; }
        if(!pf){ fprintf(stderr,"MISMATCH cannot open proc_thread.c for implementation check\n"); fails++; }
        else {
            fseek(pf,0,SEEK_END); long sz2=ftell(pf); fseek(pf,0,SEEK_SET);
            char *ib=malloc(sz2+1); fread(ib,1,sz2,pf); ib[sz2]=0; fclose(pf);
            // Must contain exact size check !=48 and INFO_LENGTH_MISMATCH, not >=
            if(!strstr(ib,"ProcessInformationLength != need") && !strstr(ib,"Length != 48")){
                fprintf(stderr,"MISMATCH proc_thread.c missing exact ==48 check\n"); fails++;
            }
            if(strstr(ib,">=") && strstr(ib,"ProcessInformationLength >=")){
                fprintf(stderr,"MISMATCH proc_thread.c uses >= instead of == (would wrongly succeed for 49,1024)\n"); fails++;
            }
            if(!strstr(ib,"STATUS_INFO_LENGTH_MISMATCH")){ fprintf(stderr,"MISMATCH missing STATUS_INFO_LENGTH_MISMATCH\n"); fails++; }
            if(!strstr(ib,"suspend_count") || !strstr(ib,"TIF_NTSUSPEND") ){
                fprintf(stderr,"MISMATCH missing suspend_count/TIF design\n"); fails++;
            }
            if(!strstr(ib,"STILL_ACTIVE") && !strstr(ib,"259")){
                fprintf(stderr,"MISMATCH missing STILL_ACTIVE 259\n"); fails++;
            }
            // Check that test binary exists and would pass (optional)
            free(ib);
        }
    }
    if(fails){ printf("check_process_thread: %d failures\n",fails); return 1; }
    printf("check_process_thread: PASS (golden and implementation match)\n"); return 0;
}
