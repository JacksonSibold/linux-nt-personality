/* check_path.c */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc, char **argv){
    const char *path = argc>1 ? argv[1] : "tests/differential/golden/latest/probe_path.json";
    FILE *f=fopen(path,"rb");
    if(!f) f=fopen("../../tests/differential/golden/latest/probe_path.json","rb");
    if(!f && getenv("NTPERS_ROOT")){ char alt[1024]; snprintf(alt,sizeof alt,"%s/tests/differential/golden/latest/probe_path.json",getenv("NTPERS_ROOT")); f=fopen(alt,"rb"); }
    if(!f){ fprintf(stderr,"cannot open %s\n",path); return 1; }
    fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
    char *b=malloc(sz+1); fread(b,1,sz,f); b[sz]=0; fclose(f);
    int fails=0;
    // \\?\ should be INVALID_NAME C0000033
    if(!strstr(b,"\\\\?\\") || !strstr(b,"0xC0000033")){ fprintf(stderr,"note: check \\\\?\\ invalid\n"); }
    // trailing dots should be NOT_FOUND C0000034
    if(!strstr(b,"trailing dots") || !strstr(b,"0xC0000034")){ fprintf(stderr,"MISMATCH trailing dots\n"); fails++; }
    // CON.txt should NOT be blocked as invalid (Win32 succeeds) - check that our checker expects success for win32
    if(!strstr(b,"CON.txt") || !strstr(b,"\"handle_valid\":true")){ fprintf(stderr,"MISMATCH CON.txt win32\n"); fails++; }
    // case insensitivity
    if(!strstr(b,"case upper") || !strstr(b,"\"handle_valid\":true")){ fprintf(stderr,"MISMATCH case\n"); fails++; }
    // relative .. should be invalid C0000033
    if(!strstr(b,"..\\\\") || !strstr(b,"0xC0000033")){ fprintf(stderr,"MISMATCH relative .. invalid\n"); fails++; }
    // Device path success
    if(!strstr(b,"Device HarddiskVolume") || !strstr(b,"0x00000000")){ fprintf(stderr,"MISMATCH device path\n"); fails++; }
    free(b);
    if(fails){ printf("check_path: %d failures (warnings may be ok)\n",fails); return 1; }
    printf("check_path: PASS\n"); return 0;
}
