#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winternl.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

// Registry ground truth: NtCreateKey/NtOpenKey/NtQueryValueKey etc + Win32 Reg* oracle
// Windows build: GetProcAddress on ntdll. VM build: raw stubs via NTPERS_VM (two-target rule).

typedef NTSTATUS (NTAPI *PFN_NtCreateKey)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, ULONG, PUNICODE_STRING, ULONG, PULONG);
typedef NTSTATUS (NTAPI *PFN_NtOpenKey)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES);
typedef NTSTATUS (NTAPI *PFN_NtQueryValueKey)(HANDLE, PUNICODE_STRING, ULONG, PVOID, ULONG, PULONG);
typedef NTSTATUS (NTAPI *PFN_NtSetValueKey)(HANDLE, PUNICODE_STRING, ULONG, ULONG, PVOID, ULONG);
typedef NTSTATUS (NTAPI *PFN_NtDeleteKey)(HANDLE);
typedef NTSTATUS (NTAPI *PFN_NtDeleteValueKey)(HANDLE, PUNICODE_STRING);
typedef NTSTATUS (NTAPI *PFN_NtEnumerateKey)(HANDLE, ULONG, ULONG, PVOID, ULONG, PULONG);
typedef NTSTATUS (NTAPI *PFN_NtEnumerateValueKey)(HANDLE, ULONG, ULONG, PVOID, ULONG, PULONG);
typedef NTSTATUS (NTAPI *PFN_NtQueryKey)(HANDLE, ULONG, PVOID, ULONG, PULONG);
typedef NTSTATUS (NTAPI *PFN_NtClose)(HANDLE);
typedef ULONG (NTAPI *PFN_RtlNtStatusToDosError)(NTSTATUS);

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
    size_t len=wcslen(s)*sizeof(wchar_t);
    memcpy(buf,s,len+sizeof(wchar_t));
    us->Buffer=buf; us->Length=(USHORT)len; us->MaximumLength=(USHORT)(len+sizeof(wchar_t));
}
static void json_escape(const char *s){ while(*s){ if(*s=='\\') printf("\\\\"); else if(*s=='"') printf("\\\""); else if(*s=='\n') printf("\\n"); else if(*s=='\r') printf("\\r"); else if(*s=='\t') printf("\\t"); else putchar(*s); s++; } }
static const char* nt_hex_str(NTSTATUS st){ static char b[16]; snprintf(b,sizeof(b),"0x%08lX",(unsigned long)(uint32_t)st); return b; }

int main(){
    setvbuf(stdout,NULL,_IONBF,0);
    print_build_header("probe_registry");
    HMODULE ntdll=GetModuleHandleA("ntdll.dll");
    PFN_NtCreateKey pNtCreateKey=(PFN_NtCreateKey)GetProcAddress(ntdll,"NtCreateKey");
    PFN_NtOpenKey pNtOpenKey=(PFN_NtOpenKey)GetProcAddress(ntdll,"NtOpenKey");
    PFN_NtQueryValueKey pNtQueryValueKey=(PFN_NtQueryValueKey)GetProcAddress(ntdll,"NtQueryValueKey");
    PFN_NtSetValueKey pNtSetValueKey=(PFN_NtSetValueKey)GetProcAddress(ntdll,"NtSetValueKey");
    PFN_NtDeleteKey pNtDeleteKey=(PFN_NtDeleteKey)GetProcAddress(ntdll,"NtDeleteKey");
    PFN_NtDeleteValueKey pNtDeleteValueKey=(PFN_NtDeleteValueKey)GetProcAddress(ntdll,"NtDeleteValueKey");
    PFN_NtEnumerateKey pNtEnumerateKey=(PFN_NtEnumerateKey)GetProcAddress(ntdll,"NtEnumerateKey");
    PFN_NtEnumerateValueKey pNtEnumerateValueKey=(PFN_NtEnumerateValueKey)GetProcAddress(ntdll,"NtEnumerateValueKey");
    PFN_NtQueryKey pNtQueryKey=(PFN_NtQueryKey)GetProcAddress(ntdll,"NtQueryKey");
    PFN_NtClose pNtClose=(PFN_NtClose)GetProcAddress(ntdll,"NtClose");
    PFN_RtlNtStatusToDosError pRtl=(PFN_RtlNtStatusToDosError)GetProcAddress(ntdll,"RtlNtStatusToDosError");

    // Build unique test key name under HKCU
    char tmpRootA[MAX_PATH]; GetTempPathA(sizeof(tmpRootA),tmpRootA);
    DWORD pid=GetCurrentProcessId();
    DWORD tick=GetTickCount();
    char keyPathA[MAX_PATH];
    snprintf(keyPathA,sizeof(keyPathA),"SOFTWARE\\ntpers_probe_%lu_%lu", (unsigned long)pid,(unsigned long)tick);
    wchar_t keyPathW[600]; for(int i=0;keyPathA[i];i++) keyPathW[i]=(wchar_t)keyPathA[i]; keyPathW[strlen(keyPathA)]=0;

    // Cleanup any stale leftovers (best effort)
    RegDeleteKeyA(HKEY_CURRENT_USER, keyPathA);

    printf("  \"test_key\": \""); json_escape(keyPathA); printf("\",\n");

    // ---- Win32 create path (ground truth) + disposition checks ----
    HKEY hCU=0;
    DWORD disp=0xDEAD;
    LONG lr=RegCreateKeyExA(HKEY_CURRENT_USER, keyPathA, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &hCU, &disp);
    printf("  \"reg_create\": {\"lasterror\":%ld,\"disp\":%lu,\"disp_str\":\"%s\"},\n",(long)lr,(unsigned long)disp, disp==REG_CREATED_NEW_KEY?"NEW":disp==REG_OPENED_EXISTING_KEY?"EXISTING":"?");
    // Second create should give EXISTING
    HKEY hCU2=0; DWORD disp2=0xDEAD;
    LONG lr2=RegCreateKeyExA(HKEY_CURRENT_USER, keyPathA, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &hCU2, &disp2);
    printf("  \"reg_create_second\": {\"lasterror\":%ld,\"disp\":%lu},\n",(long)lr2,(unsigned long)disp2);
    if(hCU2) RegCloseKey(hCU2);

    // ---- Value types matrix via Win32 RegSetValueEx / RegQueryValueEx ----
    struct { const char *name; DWORD type; const BYTE *data; DWORD len; } vals[] = {
        {"Val_SZ", REG_SZ, (BYTE*)"hello", 6},
        {"Val_EXPAND", REG_EXPAND_SZ, (BYTE*)"%TEMP%\\expanded", 17},
        {"Val_BINARY", REG_BINARY, (BYTE*)"\x01\x02\x03\x04", 4},
        {"Val_DWORD", REG_DWORD, (BYTE*)(DWORD[]){0x12345678}, 4},
        {"Val_QWORD", REG_QWORD, (BYTE*)(ULONGLONG[]){0x1122334455667788ULL}, 8},
        {"Val_MULTI", REG_MULTI_SZ, (BYTE*)"a\0b\0c\0\0", 8},
        {"Val_NONE", REG_NONE, (BYTE*)"rawnone", 7},
        {"Val_EMPTY_SZ", REG_SZ, (BYTE*)"", 1},
        {"Val_EMPTY_BINARY", REG_BINARY, (BYTE*)"", 0},
    };
    // Embedded NUL in REG_SZ
    BYTE embedNul[] = {'h','i','\0','t','h','e','r','e','\0'};
    printf("  \"value_sets\": [\n");
    for(int i=0;i<(int)(sizeof(vals)/sizeof(vals[0])); i++){
        LONG s=RegSetValueExA(hCU, vals[i].name, 0, vals[i].type, vals[i].data, vals[i].len);
        printf("    {\"name\":\"%s\",\"type\":%lu,\"len\":%lu,\"lasterror\":%ld}%s\n", vals[i].name,(unsigned long)vals[i].type,(unsigned long)vals[i].len,(long)s, i+1<(int)(sizeof(vals)/sizeof(vals[0]))?",":"");
    }
    printf("  ],\n");
    // embedded NUL
    LONG se=RegSetValueExA(hCU, "Val_EMBED_NUL", 0, REG_SZ, embedNul, sizeof(embedNul));
    printf("  \"embed_nul_set\": {\"lasterror\":%ld},\n",(long)se);
    // 1MB edge
    DWORD bigLen=1024*1024;
    BYTE *big=(BYTE*)HeapAlloc(GetProcessHeap(),0,bigLen);
    if(big){ memset(big, 0xAB, bigLen); LONG sb=RegSetValueExA(hCU,"Val_1MB",0,REG_BINARY,big,bigLen); printf("  \"big_1MB_set\": {\"len\":%lu,\"lasterror\":%ld},\n",(unsigned long)bigLen,(long)sb); HeapFree(GetProcessHeap(),0,big);} else printf("  \"big_1MB_set\": {\"note\":\"alloc fail\"},\n");

    // ---- Query round-trip via Win32 ----
    printf("  \"value_queries_win32\": [\n");
    for(int i=0;i<(int)(sizeof(vals)/sizeof(vals[0])); i++){
        DWORD t=0; BYTE buf[64]; DWORD cb=sizeof(buf);
        LONG q=RegQueryValueExA(hCU, vals[i].name, NULL, &t, buf, &cb);
        printf("    {\"name\":\"%s\",\"lasterror\":%ld,\"type\":%lu,\"cb\":%lu}%s\n", vals[i].name,(long)q,(unsigned long)t,(unsigned long)cb, i+1<(int)(sizeof(vals)/sizeof(vals[0]))?",":"");
    }
    printf("  ],\n");
    // Embedded NUL query
    { DWORD t=0; BYTE buf[32]; DWORD cb=sizeof(buf); LONG q=RegQueryValueExA(hCU,"Val_EMBED_NUL",NULL,&t,buf,&cb); printf("  \"embed_nul_query\": {\"lasterror\":%ld,\"type\":%lu,\"cb\":%lu,\"bytes\":[%u,%u,%u,%u]},\n",(long)q,(unsigned long)t,(unsigned long)cb, buf[0],buf[1],buf[2],buf[3]);}
    // Missing value
    { DWORD t=0; BYTE buf[8]; DWORD cb=sizeof(buf); LONG q=RegQueryValueExA(hCU,"NoSuchValue",NULL,&t,buf,&cb); printf("  \"missing_value_query\": {\"lasterror\":%ld},\n",(long)q);}

    // ---- NT direct tests: NtCreateKey / NtOpenKey / NtQueryValueKey / NtQueryKey / Enumerate ----
    // Open via NtOpenKey using \REGISTRY path is tricky; instead we open via hCU handle's NT handle.
    // Get NT handle via Duplicate? Instead we exercise NT APIs on the already-open hCU by reopening via Win32 handle value cast.
    // Use NtQueryKey on the registry key handle
    HANDLE hNtKey = (HANDLE)hCU; // Win32 HKEY is not directly NT HANDLE but for HKCU it is a handle-like value; we test NtQueryKey on it if possible
    // Better: create via NtCreateKey under \Registry\Machine? But HKLM needs admin. So test NtOpenKey on \REGISTRY\MACHINE\SOFTWARE
    NTSTATUS stOpenMachine=-1, stOpenUser=-1;
    HANDLE hMachine=0, hUser=0;
    if(pNtOpenKey){
        wchar_t machinePath[] = L"\\Registry\\Machine\\Software";
        wchar_t wbufMachine[512]; UNICODE_STRING usM; init_us(&usM,wbufMachine,machinePath);
        OBJECT_ATTRIBUTES oaM; InitializeObjectAttributes(&oaM,&usM,OBJ_CASE_INSENSITIVE,NULL,NULL);
        stOpenMachine = pNtOpenKey(&hMachine, KEY_READ, &oaM);
        // \Registry\User\<sid> - try current user hive via \Registry\User\<sid> query: use HKCU's underlying path via RegGetKey? fallback to \Registry\User
        wchar_t userPath[] = L"\\Registry\\User";
        wchar_t wbufUser[512]; UNICODE_STRING usU; init_us(&usU,wbufUser,userPath);
        OBJECT_ATTRIBUTES oaU; InitializeObjectAttributes(&oaU,&usU,OBJ_CASE_INSENSITIVE,NULL,NULL);
        stOpenUser = pNtOpenKey(&hUser, KEY_READ, &oaU);
    }
    printf("  \"nt_open\": {\"machine_hex\":\"0x%08lX\",\"machine_dos\":%lu,\"user_hex\":\"0x%08lX\"},\n", (unsigned long)(uint32_t)stOpenMachine, (unsigned long)(pRtl?pRtl(stOpenMachine):0), (unsigned long)(uint32_t)stOpenUser);

    // NtQueryKey info classes on hCU (via Win32 handle cast - if fails we note)
    printf("  \"nt_query_key\": [\n");
    if(pNtQueryKey){
        for(int cls=0; cls<4; cls++){
            char buf[512]; ULONG retLen=0xDEADBEEF;
            NTSTATUS st = pNtQueryKey(hNtKey, cls, buf, sizeof(buf), &retLen);
            // also test small buffer
            char small[4]; ULONG retLen2=0xDEADBEEF;
            NTSTATUS stSmall = pNtQueryKey(hNtKey, cls, small, 4, &retLen2);
            printf("    {\"class\":%d,\"ntstatus\":%ld,\"hex\":\"0x%08lX\",\"retLen\":%lu,\"small_hex\":\"0x%08lX\",\"small_retLen\":%lu}%s\n", cls,(long)st,(unsigned long)(uint32_t)st,(unsigned long)retLen,(unsigned long)(uint32_t)stSmall,(unsigned long)retLen2, cls<3?",":"");
        }
    } else printf("    {\"note\":\"no NtQueryKey\"}\n");
    printf("  ],\n");

    // NtQueryValueKey via NT handle (if we got hMachine handle, query a known value like "ProductName" under Machine\Software\Microsoft\Windows NT\CurrentVersion)
    printf("  \"nt_query_value\": [\n");
    if(pNtQueryValueKey && hMachine){
        wchar_t valName[] = L"ProductName";
        wchar_t vbufW[128]; UNICODE_STRING usV; init_us(&usV,vbufW,valName);
        char out[1024]; ULONG retLen=0;
        NTSTATUS st = pNtQueryValueKey(hMachine, &usV, 2 /*KeyValuePartialInformation*/, out, sizeof(out), &retLen);
        char smallOut[4]; ULONG retLenSmall=0;
        NTSTATUS stSmall = pNtQueryValueKey(hMachine, &usV, 2, smallOut, 4, &retLenSmall);
        printf("    {\"value\":\"ProductName\",\"ntstatus\":%ld,\"hex\":\"0x%08lX\",\"retLen\":%lu,\"small_hex\":\"0x%08lX\",\"small_retLen\":%lu},\n",(long)st,(unsigned long)(uint32_t)st,(unsigned long)retLen,(unsigned long)(uint32_t)stSmall,(unsigned long)retLenSmall);
        // missing value
        wchar_t miss[] = L"NoSuchValue123"; wchar_t missW[128]; UNICODE_STRING usMiss; init_us(&usMiss,missW,miss);
        ULONG retLenMiss=0; NTSTATUS stMiss = pNtQueryValueKey(hMachine, &usMiss, 2, out, sizeof(out), &retLenMiss);
        printf("    {\"value\":\"NoSuchValue123\",\"hex\":\"0x%08lX\",\"dos\":%lu},\n", (unsigned long)(uint32_t)stMiss,(unsigned long)(pRtl?pRtl(stMiss):0));
        // null buffer len 0
        ULONG retLenZero=0; NTSTATUS stZero = pNtQueryValueKey(hMachine, &usV, 2, out, 0, &retLenZero);
        printf("    {\"value\":\"ProductName_zero_len\",\"hex\":\"0x%08lX\",\"retLen\":%lu}\n", (unsigned long)(uint32_t)stZero,(unsigned long)retLenZero);
    } else printf("    {\"note\":\"no hMachine or no NtQueryValueKey\"}\n");
    printf("  ],\n");

    // Enumerate
    printf("  \"nt_enumerate\": {\n");
    if(pNtEnumerateKey && hMachine){
        char buf[1024]; ULONG retLen=0;
        NTSTATUS st0 = pNtEnumerateKey(hMachine, 0, 0, buf, sizeof(buf), &retLen);
        printf("    \"key_idx0_hex\":\"0x%08lX\",\"key_idx0_retLen\":%lu,\n", (unsigned long)(uint32_t)st0,(unsigned long)retLen);
        NTSTATUS stBig = pNtEnumerateKey(hMachine, 9999, 0, buf, sizeof(buf), &retLen);
        printf("    \"key_big_idx_hex\":\"0x%08lX\"\n", (unsigned long)(uint32_t)stBig);
    } else printf("    \"note\":\"no enumerate\"\n");
    printf("  },\n");

    // Access denied: try open HKLM Software with KEY_WRITE from non-elevated? We test opening machine key with access 0x2000000 (MAX) vs limited
    LONG lrOpenRO=0, lrOpenRW=0;
    HKEY hRO=0, hRW=0;
    lrOpenRO = RegOpenKeyExA(HKEY_LOCAL_MACHINE,"SOFTWARE",0,KEY_READ,&hRO);
    lrOpenRW = RegOpenKeyExA(HKEY_LOCAL_MACHINE,"SOFTWARE",0,KEY_WRITE,&hRW);
    printf("  \"access_check\": {\"ro_lasterror\":%ld,\"rw_lasterror\":%ld},\n",(long)lrOpenRO,(long)lrOpenRW);
    if(hRO) RegCloseKey(hRO); if(hRW) RegCloseKey(hRW);

    // WOW64 view test
    HKEY h32=0, h64=0;
    LONG lr32 = RegOpenKeyExA(HKEY_LOCAL_MACHINE,"SOFTWARE",0,KEY_READ|KEY_WOW64_32KEY,&h32);
    LONG lr64 = RegOpenKeyExA(HKEY_LOCAL_MACHINE,"SOFTWARE",0,KEY_READ|KEY_WOW64_64KEY,&h64);
    printf("  \"wow64\": {\"32_lasterror\":%ld,\"64_lasterror\":%ld},\n",(long)lr32,(long)lr64);
    if(h32) RegCloseKey(h32); if(h64) RegCloseKey(h64);

    // Cleanup
    if(hCU){
        // delete values
        RegDeleteValueA(hCU,"Val_SZ"); RegDeleteValueA(hCU,"Val_EXPAND"); RegDeleteValueA(hCU,"Val_BINARY");
        RegDeleteValueA(hCU,"Val_DWORD"); RegDeleteValueA(hCU,"Val_QWORD"); RegDeleteValueA(hCU,"Val_MULTI");
        RegDeleteValueA(hCU,"Val_NONE"); RegDeleteValueA(hCU,"Val_EMPTY_SZ"); RegDeleteValueA(hCU,"Val_EMPTY_BINARY");
        RegDeleteValueA(hCU,"Val_EMBED_NUL"); RegDeleteValueA(hCU,"Val_1MB");
        RegCloseKey(hCU);
        RegDeleteKeyA(HKEY_CURRENT_USER, keyPathA);
    }
    if(hMachine && pNtClose) pNtClose(hMachine);
    if(hUser && pNtClose) pNtClose(hUser);
    // Verify delete
    HKEY hCheck=0; LONG lrCheck=RegOpenKeyExA(HKEY_CURRENT_USER, keyPathA,0,KEY_READ,&hCheck);
    printf("  \"delete_verify\": {\"open_after_delete_lasterror\":%ld}\n",(long)lrCheck);
    if(hCheck) RegCloseKey(hCheck);

    printf("}\n");
    return 0;
}
