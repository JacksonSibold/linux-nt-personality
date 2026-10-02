/* check_sync.c - enforce probe_sync golden */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc, char **argv){
    const char *path = argc>1 ? argv[1] : "tests/differential/golden/latest/probe_sync.json";
    FILE *f=fopen(path,"rb");
    if(!f){ f=fopen("../../tests/differential/golden/latest/probe_sync.json","rb"); }
    if(!f){ f=fopen("C:/Users/Jackson/Desktop/02 - Personal/winapi-native-os/tests/differential/golden/latest/probe_sync.json","rb"); }
    if(!f){ fprintf(stderr,"cannot open %s\n",path); return 1; }
    fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
    char *b=malloc(sz+1); fread(b,1,sz,f); b[sz]=0; fclose(f);
    int fails=0;
    // Check auto event after_set_wait1 ==0
    if(!strstr(b,"\"after_set_wait1\":0")){ fprintf(stderr,"MISMATCH auto after_set_wait1\n"); fails++; }
    if(!strstr(b,"\"after_set_wait2\":258")){ fprintf(stderr,"MISMATCH auto after_set_wait2\n"); fails++; }
    if(!strstr(b,"\"initial_wait\":258")){ fprintf(stderr,"MISMATCH initial_wait\n"); fails++; }
    if(!strstr(b,"\"wait_result\":128") && !strstr(b,"\"wait_hex\":\"0x80\"")){ fprintf(stderr,"MISMATCH abandon 128\n"); fails++; }
    if(!strstr(b,"\"non_owner_win32_err\": 288") && !strstr(b,"\"non_owner_win32_err\":288")){ fprintf(stderr,"MISMATCH non_owner 288\n"); fails++; }
    if(!strstr(b,"\"err\":298") && !strstr(b,"\"err\": 298")){ fprintf(stderr,"MISMATCH sem over 298\n"); fails++; }
    if(!strstr(b,"\"wait_any_index\": 1")){ fprintf(stderr,"MISMATCH wait_any_index 1\n"); fails++; }
    if(!strstr(b,"\"wait_all_abandoned\": 128")){ fprintf(stderr,"MISMATCH wait_all_abandoned\n"); fails++; }
    free(b);
    if(fails){ printf("check_sync: %d failures\n",fails); return 1; }
    printf("check_sync: PASS\n"); return 0;
}
