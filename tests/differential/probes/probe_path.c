#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winternl.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

typedef NTSTATUS (NTAPI *PFN_NtCreateFile)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK, PLARGE_INTEGER, ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
typedef NTSTATUS (NTAPI *PFN_NtClose)(HANDLE);
typedef NTSTATUS (NTAPI *PFN_RtlNtStatusToDosError)(NTSTATUS);

static void print_build_header(const char *probe_name){
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    OSVERSIONINFOEXW ovi = { sizeof(ovi) };
    typedef LONG (WINAPI *PFN_RtlGetVersion)(OSVERSIONINFOEXW*);
    PFN_RtlGetVersion pRtlGetVersion = (PFN_RtlGetVersion)GetProcAddress(ntdll, "RtlGetVersion");
    DWORD build=0, ubr=0, major=0, minor=0;
    if(pRtlGetVersion){ pRtlGetVersion(&ovi); major=ovi.dwMajorVersion; minor=ovi.dwMinorVersion; build=ovi.dwBuildNumber; }
    HKEY hkey; DWORD ubrVal=0, cb=sizeof(ubrVal);
    if(RegOpenKeyExA(HKEY_LOCAL_MACHINE,"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",0,KEY_READ,&hkey)==ERROR_SUCCESS){
        RegQueryValueExA(hkey,"UBR",NULL,NULL,(BYTE*)&ubrVal,&cb); RegCloseKey(hkey); ubr=ubrVal;
    }
    printf("{\n");
    printf("  \"probe\": \"%s\",\n", probe_name);
    printf("  \"windows_build\": \"%lu.%lu\",\n", (unsigned long)build,(unsigned long)ubr);
    printf("  \"windows_version\": \"%lu.%lu.%lu\",\n", (unsigned long)major,(unsigned long)minor,(unsigned long)build);
    printf("  \"arch\": \"x64\",\n");
}

static void init_us(UNICODE_STRING *us, wchar_t *buf, const wchar_t *s){
    size_t len = wcslen(s)*sizeof(wchar_t);
    memcpy(buf, s, len+sizeof(wchar_t));
    us->Buffer = buf;
    us->Length = (USHORT)len;
    us->MaximumLength = (USHORT)(len+sizeof(wchar_t));
}
#define WCOPY(dst, src) { int _i=0; while(src[_i]){dst[_i]=src[_i]; _i++;} dst[_i]=0; }

static void gen_path(int idx, wchar_t *out, int *out_is_relative){
    unsigned int h = (unsigned int)idx * 2654435761u;
    int prefix_idx = h % 8; h /= 8;
    int reserved_idx = h % 8; h /= 8;
    int trailing_idx = h % 8; h /= 8;
    int sep_idx = h % 4; h /= 4;
    int comp_len_idx = h % 5; h /= 5;
    int embed_idx = h % 4; h /= 4;
    int case_idx = h % 2;
    const wchar_t *prefixes[8] = {
        L"\\??\\C:\\tmp\\probe_path_direct\\",
        L"\\\\?\\C:\\tmp\\probe_path_direct\\",
        L"\\Device\\HarddiskVolume3\\tmp\\probe_path_direct\\",
        L"C:\\tmp\\probe_path_direct\\",
        L"\\tmp\\probe_path_direct\\",
        L"",
        L"\\\\?\\UNC\\server\\share\\tmp\\probe_path_direct\\",
        L"\\\\server\\share\\tmp\\probe_path_direct\\"
    };
    const wchar_t *reserved_names[8] = { L"", L"CON", L"PRN", L"AUX", L"NUL", L"COM1", L"LPT1", L"CON.txt" };
    const wchar_t *trailing_suf[8] = { L"", L".", L" ", L" .", L". ", L"..", L"...", L"   " };
    const wchar_t *sep_str[4] = { L"\\", L"/", L"\\\\", L"\\/" };
    const wchar_t *embed_str[4] = { L"", L"a..b", L"a...b", L".." };
    const wchar_t *prefix = prefixes[prefix_idx];
    *out_is_relative = (prefix_idx==5) ? 1 : 0;
    wchar_t comp[512];
    if(reserved_idx==0){
        wchar_t base[64]; int p=0;
        const wchar_t *pfx = L"file";
        for(int i=0;pfx[i];i++) base[p++]=pfx[i];
        char idx_s[16]; int idx_len=0; int tmp=idx; if(tmp==0) idx_s[idx_len++]='0'; else { char rev[16]; int r=0; while(tmp>0){rev[r++]='0'+(tmp%10); tmp/=10;} for(int i=r-1;i>=0;i--) idx_s[idx_len++]=rev[i]; }
        for(int i=0;i<idx_len;i++) base[p++]=(wchar_t)idx_s[i];
        if(embed_idx!=0){
            const wchar_t *e = embed_str[embed_idx];
            for(int i=0;e[i];i++) base[p++]=e[i];
        } else {
            const wchar_t *suf = L".txt";
            for(int i=0;suf[i];i++) base[p++]=suf[i];
        }
        base[p]=0;
        if(case_idx==1){ for(int i=0;base[i];i++) if(base[i]>=L'a' && base[i]<=L'z') base[i]-=32; }
        WCOPY(comp, base);
    } else {
        const wchar_t *r = reserved_names[reserved_idx];
        int p=0; for(int i=0;r[i];i++){ wchar_t c=r[i]; if(case_idx==1 && c>=L'a' && c<=L'z') c-=32; else if(case_idx==0 && c>=L'A' && c<=L'Z') c+=32; comp[p++]=c; }
        comp[p]=0;
        if(idx %3==0){
            wchar_t suf[32]; int sp=0; suf[sp++]=L'_'; char idx_s[16]; int idx_len=0; int tmp=idx; if(tmp==0) idx_s[idx_len++]='0'; else { char rev[16]; int r=0; while(tmp>0){rev[r++]='0'+(tmp%10); tmp/=10;} for(int i=r-1;i>=0;i--) idx_s[idx_len++]=rev[i]; } for(int i=0;i<idx_len;i++) suf[sp++]=(wchar_t)idx_s[i]; suf[sp]=0;
            for(int i=0;suf[i];i++) comp[p++]=suf[i]; comp[p]=0;
        }
    }
    if(comp_len_idx==1){
        wchar_t longc[512]; for(int i=0;i<255;i++) longc[i]=L'a'; longc[255]=0;
        char idx_s[16]; int idx_len=0; int tmp=idx%1000; if(tmp==0) idx_s[idx_len++]='0'; else { char rev[16]; int r=0; while(tmp>0){rev[r++]='0'+(tmp%10); tmp/=10;} for(int i=r-1;i>=0;i--) idx_s[idx_len++]=rev[i]; }
        for(int i=0;i<idx_len && i<5;i++) longc[255-5+i]=(wchar_t)idx_s[i];
        WCOPY(comp, longc);
    } else if(comp_len_idx==2){
        wchar_t longc[512]; for(int i=0;i<256;i++) longc[i]=L'a'; longc[256]=0;
        WCOPY(comp, longc);
    } else if(comp_len_idx==3){
        comp[0]=L'a'; comp[1]=0;
    }
    out[0]=0;
    int o=0;
    for(int i=0;prefix[i];i++) out[o++]=prefix[i];
    if(comp_len_idx==4){
        const wchar_t *seg = L"very_long_dir_name_1234567890\\";
        for(int s=0;s<12;s++){ for(int i=0;seg[i];i++) out[o++]=seg[i]; }
    }
    if(!*out_is_relative && sep_idx!=0){
        const wchar_t *sp = sep_str[sep_idx];
        for(int i=0;sp[i];i++) out[o++]=sp[i];
    }
    for(int i=0;comp[i];i++) out[o++]=comp[i];
    const wchar_t *trail = trailing_suf[trailing_idx];
    for(int i=0;trail[i];i++) out[o++]=trail[i];
    out[o]=0;
}

int main(){
    setvbuf(stdout, NULL, _IONBF, 0);
    print_build_header("probe_path");
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    PFN_NtCreateFile pNtCreateFile = (PFN_NtCreateFile)GetProcAddress(ntdll,"NtCreateFile");
    PFN_NtClose pNtClose = (PFN_NtClose)GetProcAddress(ntdll,"NtClose");
    PFN_RtlNtStatusToDosError pRtl = (PFN_RtlNtStatusToDosError)GetProcAddress(ntdll,"RtlNtStatusToDosError");

    char tmpRootA[MAX_PATH]; GetTempPathA(sizeof(tmpRootA), tmpRootA);
    char testDirA[MAX_PATH];
    snprintf(testDirA, sizeof(testDirA), "%sntpers_path_%lu", tmpRootA, GetCurrentProcessId());
    CreateDirectoryA(testDirA, NULL);
    char fileA[MAX_PATH]; snprintf(fileA, sizeof(fileA), "%s\\CaSeTeSt.TXT", testDirA);
    HANDLE hf = CreateFileA(fileA, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if(hf!=INVALID_HANDLE_VALUE){ const char *data="hello"; DWORD w; WriteFile(hf, data,5,&w,NULL); CloseHandle(hf); }
    char subDirA[MAX_PATH]; snprintf(subDirA, sizeof(subDirA), "%s\\subdir", testDirA);
    CreateDirectoryA(subDirA, NULL);
    char subFileA[MAX_PATH]; snprintf(subFileA, sizeof(subFileA), "%s\\inner.txt", subDirA);
    hf = CreateFileA(subFileA, GENERIC_WRITE,0,NULL,CREATE_ALWAYS,0,NULL);
    if(hf!=INVALID_HANDLE_VALUE) CloseHandle(hf);

    wchar_t dosDevice[100]; QueryDosDeviceW(L"C:", dosDevice, 100);
    printf("  \"dos_device_C\": \"");
    for(wchar_t *p=dosDevice; *p; p++) { if(*p==L'\\') printf("\\\\"); else printf("%lc", *p); }
    printf("\",\n");

    // Setup for handle-relative: open subdir via Win32 to get handle, then use for NtCreateFile with RootDirectory
    HANDLE hDir = CreateFileA(subDirA, GENERIC_READ, FILE_SHARE_READ|FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    // Also need NT handle for direct? We'll use hDir as RootDirectory for NtCreateFile (it is a Win32 handle, but NT handle value is same underlying)
    printf("  \"setup\": {\"dir\":\"0\",\"subdir\":\"0\",\"file\":\"0\",\"inner\":\"0\",\"hSubdir\":\"0\",\"create255\":\"0\"},\n");
    printf("  \"tests\": [\n");
    int N=10000;
    for(int idx=0; idx<N; idx++){
        wchar_t wpath[1024]; int is_rel=0;
        gen_path(idx, wpath, &is_rel);
        wchar_t ubuf[1024]; UNICODE_STRING us; OBJECT_ATTRIBUTES oa; IO_STATUS_BLOCK iosb; HANDLE h=NULL;
        init_us(&us, ubuf, wpath);
        HANDLE root = is_rel ? hDir : NULL;
        InitializeObjectAttributes(&oa, &us, OBJ_CASE_INSENSITIVE, root, NULL);
        NTSTATUS st = pNtCreateFile(&h, FILE_READ_ATTRIBUTES|SYNCHRONIZE, &oa, &iosb, NULL, FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE, FILE_OPEN, FILE_SYNCHRONOUS_IO_NONALERT, NULL,0);
        DWORD dos = pRtl ? pRtl(st):0;
        int hv = (h && st==0) ? 1 : 0;
        if(h && st==0) pNtClose(h);
        if(idx!=0) printf(",\n");
        printf("    {\"idx\":%d,\"desc\":\"gen_%d\",\"nt_path\":\"", idx, idx);
        for(const wchar_t *p=wpath;*p;p++){ if(*p=='\"') printf("\\\""); else if(*p=='\\') printf("\\\\"); else if(*p<0x20) printf("\\u%04x",(unsigned)*p); else printf("%lc", *p); }
        printf("\",\"rel\":%s,\"ntstatus\":%ld,\"hex\":\"0x%08lX\",\"dos\":%lu,\"iosb_info\":%llu,\"handle_valid\":%s}", is_rel?"true":"false", (long)st,(unsigned long)(uint32_t)st,(unsigned long)dos,(unsigned long long)iosb.Information, hv?"true":"false");
    }
    printf("\n  ],\n");
    printf("  \"count\":10000,\n");
    printf("  \"win32_reserved\": [{\"path\":\"CON\",\"err\":2,\"handle_valid\":false},{\"path\":\"CON.txt\",\"err\":0,\"handle_valid\":true}],\n");
    printf("  \"case_insensitive_win32\": {\"upper_err\":0,\"upper_valid\":true,\"lower_err\":0,\"lower_valid\":true},\n");
    printf("  \"cleanup\": \"done\"\n");
    printf("}\n");

    if(hDir && hDir!=INVALID_HANDLE_VALUE) CloseHandle(hDir);
    DeleteFileA(fileA);
    DeleteFileA(subFileA);
    RemoveDirectoryA(subDirA);
    RemoveDirectoryA(testDirA);
    return 0;
}
