#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winternl.h>
#include <stdio.h>
#include <stdint.h>

typedef NTSTATUS (NTAPI *PFN_NtCreateFile)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK, PLARGE_INTEGER, ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
typedef DWORD (WINAPI *PFN_RtlNtStatusToDosError)(NTSTATUS);
typedef NTSTATUS (NTAPI *PFN_NtClose)(HANDLE);

static void init_unicode(UNICODE_STRING *us, const wchar_t *ws){
    size_t len = wcslen(ws)*sizeof(wchar_t);
    us->Buffer = (wchar_t*)ws;
    us->Length = (USHORT)len;
    us->MaximumLength = (USHORT)len;
}

int main(){
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
    printf("  \"probe\": \"probe_sharemode\",\n");
    printf("  \"windows_build\": \"%lu.%lu\",\n", (unsigned long)build,(unsigned long)ubr);
    printf("  \"windows_version\": \"%lu.%lu.%lu\",\n", (unsigned long)major,(unsigned long)minor,(unsigned long)build);
    printf("  \"arch\": \"x64\",\n");

    char tmpPathA[MAX_PATH], tmpFileA[MAX_PATH];
    GetTempPathA(sizeof(tmpPathA), tmpPathA);
    GetTempFileNameA(tmpPathA, "shr", 0, tmpFileA);
    wchar_t tmpFileW[MAX_PATH];
    MultiByteToWideChar(CP_ACP,0,tmpFileA,-1,tmpFileW,MAX_PATH);
    wchar_t ntPathW[MAX_PATH+10];
    wsprintfW(ntPathW, L"\\??\\%s", tmpFileW);
    HANDLE hTmp = CreateFileA(tmpFileA, GENERIC_READ|GENERIC_WRITE, FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if(hTmp!=INVALID_HANDLE_VALUE) CloseHandle(hTmp);

    PFN_NtCreateFile pNtCreateFile = (PFN_NtCreateFile)GetProcAddress(ntdll, "NtCreateFile");
    PFN_RtlNtStatusToDosError pRtlNtStatusToDosError = (PFN_RtlNtStatusToDosError)GetProcAddress(ntdll, "RtlNtStatusToDosError");
    PFN_NtClose pNtClose = (PFN_NtClose)GetProcAddress(ntdll, "NtClose");

    struct { ACCESS_MASK access; const char *name; } desired[] = {
        {0, "0"},
        {FILE_READ_DATA, "FILE_READ_DATA"},
        {FILE_WRITE_DATA, "FILE_WRITE_DATA"},
        {FILE_APPEND_DATA, "FILE_APPEND_DATA"},
        {FILE_READ_EA, "FILE_READ_EA"},
        {FILE_WRITE_EA, "FILE_WRITE_EA"},
        {FILE_EXECUTE, "FILE_EXECUTE"},
        {FILE_READ_ATTRIBUTES, "FILE_READ_ATTRIBUTES"},
        {FILE_WRITE_ATTRIBUTES, "FILE_WRITE_ATTRIBUTES"},
        {DELETE, "DELETE"},
        {READ_CONTROL, "READ_CONTROL"},
        {WRITE_DAC, "WRITE_DAC"},
        {WRITE_OWNER, "WRITE_OWNER"},
        {SYNCHRONIZE, "SYNCHRONIZE"},
        {FILE_READ_DATA|FILE_WRITE_DATA, "READ|WRITE"},
        {FILE_READ_DATA|FILE_APPEND_DATA, "READ|APPEND"},
        {FILE_WRITE_DATA|FILE_APPEND_DATA, "WRITE|APPEND"},
        {FILE_READ_DATA|DELETE, "READ|DELETE"},
        {FILE_WRITE_DATA|DELETE, "WRITE|DELETE"},
        {FILE_READ_DATA|READ_CONTROL, "READ|READ_CONTROL"},
        {FILE_WRITE_DATA|WRITE_DAC, "WRITE|WRITE_DAC"},
        {DELETE|READ_CONTROL, "DELETE|READ_CONTROL"},
        {READ_CONTROL|WRITE_DAC|WRITE_OWNER, "RC|WD|WO"},
        {GENERIC_READ, "GENERIC_READ"},
        {GENERIC_WRITE, "GENERIC_WRITE"},
        {GENERIC_EXECUTE, "GENERIC_EXECUTE"},
        {GENERIC_ALL, "GENERIC_ALL"},
        {GENERIC_READ|GENERIC_WRITE, "GENERIC_RW"},
        {GENERIC_READ|SYNCHRONIZE, "GENERIC_READ|SYNCH"},
        {MAXIMUM_ALLOWED, "MAXIMUM_ALLOWED"},
        {FILE_READ_DATA|FILE_WRITE_DATA|SYNCHRONIZE, "READ|WRITE|SYNCH"},
        {FILE_READ_DATA|FILE_WRITE_DATA|FILE_APPEND_DATA|FILE_EXECUTE|DELETE|READ_CONTROL|WRITE_DAC|WRITE_OWNER|SYNCHRONIZE, "ALL"},
    };
    struct { ULONG share; const char *name; } shares[] = {
        {0, "0"},
        {FILE_SHARE_READ, "R"},
        {FILE_SHARE_WRITE, "W"},
        {FILE_SHARE_DELETE, "D"},
        {FILE_SHARE_READ|FILE_SHARE_WRITE, "RW"},
        {FILE_SHARE_READ|FILE_SHARE_DELETE, "RD"},
        {FILE_SHARE_WRITE|FILE_SHARE_DELETE, "WD"},
        {FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE, "RWD"},
    };
    int nDesired = sizeof(desired)/sizeof(desired[0]);
    int nShares = sizeof(shares)/sizeof(shares[0]);

    printf("  \"desired_values\": [\n");
    for(int i=0;i<nDesired;i++) printf("    {\"name\":\"%s\",\"value\":%lu}%s\n", desired[i].name, (unsigned long)desired[i].access, i+1<nDesired?",":"");
    printf("  ],\n");
    printf("  \"share_values\": [\n");
    for(int i=0;i<nShares;i++) printf("    {\"name\":\"%s\",\"value\":%lu}%s\n", shares[i].name, (unsigned long)shares[i].share, i+1<nShares?",":"");
    printf("  ],\n");

    printf("  \"matrix\": [\n");
    int first=1;
    ULONGLONG total=0;
    for(int i=0;i<nDesired;i++){
        for(int si=0; si<nShares; si++){
            UNICODE_STRING us; OBJECT_ATTRIBUTES oa; IO_STATUS_BLOCK iosb;
            init_unicode(&us, ntPathW);
            InitializeObjectAttributes(&oa, &us, OBJ_CASE_INSENSITIVE, NULL, NULL);
            HANDLE h1=NULL;
            NTSTATUS st1 = pNtCreateFile(&h1, desired[i].access|SYNCHRONIZE, &oa, &iosb, NULL, FILE_ATTRIBUTE_NORMAL, shares[si].share, FILE_OPEN, FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0);
            if(!NT_SUCCESS(st1)) continue;
            for(int j=0;j<nDesired;j++){
                for(int sj=0; sj<nShares; sj++){
                    UNICODE_STRING us2; OBJECT_ATTRIBUTES oa2; IO_STATUS_BLOCK iosb2;
                    init_unicode(&us2, ntPathW);
                    InitializeObjectAttributes(&oa2, &us2, OBJ_CASE_INSENSITIVE, NULL, NULL);
                    HANDLE h2=NULL;
                    NTSTATUS st2 = pNtCreateFile(&h2, desired[j].access|SYNCHRONIZE, &oa2, &iosb2, NULL, FILE_ATTRIBUTE_NORMAL, shares[sj].share, FILE_OPEN, FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0);
                    if(!first) printf(",\n");
                    first=0;
                    DWORD dos2 = pRtlNtStatusToDosError ? pRtlNtStatusToDosError(st2) : 0;
                    printf("    {\"fi\":%d,\"si\":%d,\"fj\":%d,\"sj\":%d,\"st\":%ld,\"hx\":\"0x%08lX\",\"dos\":%lu}", i,si,j,sj,(long)st2,(unsigned long)(uint32_t)st2,(unsigned long)dos2);
                    total++;
                    if(NT_SUCCESS(st2) && h2) pNtClose(h2);
                }
            }
            pNtClose(h1);
        }
    }
    printf("\n  ],\n");
    printf("  \"matrix_count\":%llu,\n", (unsigned long long)total);
    printf("  \"targeted_examples\": [\n");
    struct { ACCESS_MASK d1; ULONG s1; ACCESS_MASK d2; ULONG s2; const char* desc; } examples[] = {
        {FILE_READ_DATA|SYNCHRONIZE, 0, FILE_READ_DATA|SYNCHRONIZE, FILE_SHARE_READ, "exclusive read vs read"},
        {FILE_READ_DATA|SYNCHRONIZE, FILE_SHARE_READ, FILE_READ_DATA|SYNCHRONIZE, FILE_SHARE_READ, "compatible reads"},
        {FILE_WRITE_DATA|SYNCHRONIZE, FILE_SHARE_READ, FILE_READ_DATA|SYNCHRONIZE, FILE_SHARE_WRITE, "write vs read mismatch"},
        {FILE_READ_DATA|FILE_WRITE_DATA|SYNCHRONIZE, FILE_SHARE_READ|FILE_SHARE_WRITE, FILE_READ_DATA|SYNCHRONIZE, FILE_SHARE_READ, "rw vs r with rd share"},
        {DELETE|SYNCHRONIZE, FILE_SHARE_DELETE, DELETE|SYNCHRONIZE, FILE_SHARE_DELETE, "delete sharing"},
    };
    for(int k=0;k<5;k++){
        UNICODE_STRING us; OBJECT_ATTRIBUTES oa; IO_STATUS_BLOCK iosb;
        init_unicode(&us, ntPathW);
        InitializeObjectAttributes(&oa, &us, OBJ_CASE_INSENSITIVE, NULL, NULL);
        HANDLE h1=NULL;
        NTSTATUS st1 = pNtCreateFile(&h1, examples[k].d1, &oa, &iosb, NULL, FILE_ATTRIBUTE_NORMAL, examples[k].s1, FILE_OPEN, FILE_SYNCHRONOUS_IO_NONALERT, NULL,0);
        HANDLE h2=NULL;
        NTSTATUS st2=0;
        if(NT_SUCCESS(st1)){
            UNICODE_STRING us2; OBJECT_ATTRIBUTES oa2; IO_STATUS_BLOCK iosb2;
            init_unicode(&us2, ntPathW);
            InitializeObjectAttributes(&oa2, &us2, OBJ_CASE_INSENSITIVE, NULL, NULL);
            st2 = pNtCreateFile(&h2, examples[k].d2, &oa2, &iosb2, NULL, FILE_ATTRIBUTE_NORMAL, examples[k].s2, FILE_OPEN, FILE_SYNCHRONOUS_IO_NONALERT, NULL,0);
            if(NT_SUCCESS(st2)) pNtClose(h2);
            pNtClose(h1);
        }
        printf("    {\"desc\":\"%s\",\"first_st_hex\":\"0x%08lX\",\"second_st_hex\":\"0x%08lX\",\"second_st\":%ld}%s\n",
            examples[k].desc, (unsigned long)(uint32_t)st1, (unsigned long)(uint32_t)st2, (long)st2, k<4?",":"");
    }
    printf("  ]\n");
    printf("}\n");

    DeleteFileA(tmpFileA);
    return 0;
}
