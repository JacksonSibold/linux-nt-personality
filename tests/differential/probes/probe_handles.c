#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winternl.h>
#include <stdio.h>
#include <stdint.h>

typedef NTSTATUS (NTAPI *PFN_NtQueryObject)(HANDLE, OBJECT_INFORMATION_CLASS, PVOID, ULONG, PULONG);
typedef NTSTATUS (NTAPI *PFN_NtDuplicateObject)(HANDLE, HANDLE, HANDLE, PHANDLE, ACCESS_MASK, ULONG, ULONG);
typedef NTSTATUS (NTAPI *PFN_NtClose)(HANDLE);
static void json_escape(const char *s){ while(*s){ if(*s=='\\') printf("\\\\"); else if(*s=='"') printf("\\\""); else if(*s=='\n') printf("\\n"); else if(*s=='\r') printf("\\r"); else if(*s=='\t') printf("\\t"); else putchar(*s); s++; } }

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
    printf("  \"probe\": \"probe_handles\",\n");
    printf("  \"windows_build\": \"%lu.%lu\",\n", (unsigned long)build,(unsigned long)ubr);
    printf("  \"windows_version\": \"%lu.%lu.%lu\",\n", (unsigned long)major,(unsigned long)minor,(unsigned long)build);
    printf("  \"arch\": \"x64\",\n");

    // Handle values test
    char tmpPath[MAX_PATH], tmpFile[MAX_PATH];
    GetTempPathA(sizeof(tmpPath), tmpPath);
    GetTempFileNameA(tmpPath, "hdl", 0, tmpFile);
    HANDLE h1 = CreateFileA(tmpFile, GENERIC_READ|GENERIC_WRITE, FILE_SHARE_READ|FILE_SHARE_WRITE, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    HANDLE h2 = CreateFileA(tmpFile, GENERIC_READ, FILE_SHARE_READ|FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    // Also create event
    HANDLE hEvent = CreateEventA(NULL, FALSE, FALSE, NULL);
    HANDLE hMutex = CreateMutexA(NULL, FALSE, NULL);
    HANDLE hFile2 = CreateFileA(tmpFile, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

    printf("  \"handles\": [\n");
    HANDLE arr[] = {h1,h2,hEvent,hMutex,hFile2};
    const char *names[] = {"CreateFile_RW","CreateFile_R","Event","Mutex","CreateFile_R2"};
    for(int i=0;i<5;i++){
        HANDLE h = arr[i];
        uintptr_t v = (uintptr_t)h;
        printf("    {\"name\":\"%s\",\"handle\":\"0x%llX\",\"value\":%llu,\"mod4\":%llu,\"is_pseudo\":false}%s\n",
            names[i], (unsigned long long)v, (unsigned long long)v, (unsigned long long)(v%4), i<4?",":"");
    }
    printf("  ],\n");
    // Pseudo handles
    HANDLE pseudoProc = GetCurrentProcess();
    HANDLE pseudoThread = GetCurrentThread();
    printf("  \"pseudo_handles\": [\n");
    printf("    {\"name\":\"GetCurrentProcess\",\"handle\":\"0x%llX\",\"value\":\"%lld\"},\n", (unsigned long long)(uintptr_t)pseudoProc, (long long)(intptr_t)pseudoProc);
    printf("    {\"name\":\"GetCurrentThread\",\"handle\":\"0x%llX\",\"value\":\"%lld\"}\n", (unsigned long long)(uintptr_t)pseudoThread, (long long)(intptr_t)pseudoThread);
    printf("  ],\n");

    // GetProcessHandleCount
    DWORD handleCount=0;
    BOOL gc = GetProcessHandleCount(GetCurrentProcess(), &handleCount);
    printf("  \"GetProcessHandleCount\": {\"success\":%s,\"count\":%lu},\n", gc?"true":"false", (unsigned long)handleCount);

    // NtQueryObject tests
    PFN_NtQueryObject pNtQueryObject = (PFN_NtQueryObject)GetProcAddress(ntdll, "NtQueryObject");
    printf("  \"nt_query_object\": [\n");
    if(pNtQueryObject && h1 != INVALID_HANDLE_VALUE){
        // ObjectBasicInformation = 0
        struct { ULONG GrantedAccess; ULONG HandleCount; ULONG PointerCount; ULONG Reserved[10]; } basic = {0};
        // Actually OBJECT_BASIC_INFORMATION per winternl:  but we query generic
        // Use struct with known layout: NtQueryObject Basic returns OBJECT_BASIC_INFORMATION
        typedef struct _OBJECT_BASIC_INFORMATION {
            ULONG Attributes;
            ACCESS_MASK GrantedAccess;
            ULONG HandleCount;
            ULONG PointerCount;
            ULONG Reserved[10];
        } OBJECT_BASIC_INFORMATION;
        OBJECT_BASIC_INFORMATION obi;
        ULONG retLen=0;
        NTSTATUS st = pNtQueryObject(h1, 0, &obi, sizeof(obi), &retLen);
        printf("    {\"handle\":\"0x%llX\",\"info_class\":0,\"class_name\":\"ObjectBasicInformation\",\"ntstatus\":%ld,\"ntstatus_hex\":\"0x%08lX\",\"GrantedAccess\":\"0x%lX\",\"HandleCount\":%lu,\"PointerCount\":%lu,\"retLen\":%lu},\n",
            (unsigned long long)(uintptr_t)h1, (long)st, (unsigned long)(uint32_t)st, (unsigned long)obi.GrantedAccess, (unsigned long)obi.HandleCount, (unsigned long)obi.PointerCount, (unsigned long)retLen);
        // ObjectNameInformation = 1
        char buf[512];
        st = pNtQueryObject(h1, 1, buf, sizeof(buf), &retLen);
        // For file, name should be device path like \Device\HarddiskVolume...
        // Decode UNICODE_STRING at start of buf
        UNICODE_STRING *us = (UNICODE_STRING*)buf;
        char nameA[256]={0};
        if(NT_SUCCESS(st) && us->Length>0 && us->Buffer){
            int len = us->Length/2;
            if(len>255) len=255;
            for(int i=0;i<len;i++) nameA[i]=(char)us->Buffer[i];
        }
        printf("    {\"handle\":\"0x%llX\",\"info_class\":1,\"class_name\":\"ObjectNameInformation\",\"ntstatus\":%ld,\"ntstatus_hex\":\"0x%08lX\",\"retLen\":%lu,\"name\":\"", (unsigned long long)(uintptr_t)h1, (long)st, (unsigned long)(uint32_t)st, (unsigned long)retLen); json_escape(nameA); printf("\"},\n");
        // ObjectTypeInformation = 2
        st = pNtQueryObject(h1, 2, buf, sizeof(buf), &retLen);
        // OBJECT_TYPE_INFORMATION layout: UNICODE_STRING TypeName at offset 0, then fields. We'll just extract first UNICODE_STRING
        UNICODE_STRING *typeName = (UNICODE_STRING*)buf;
        char typeA[64]={0};
        if(NT_SUCCESS(st) && typeName->Length>0 && typeName->Buffer){
            int len = typeName->Length/2;
            if(len>63) len=63;
            for(int i=0;i<len;i++) typeA[i]=(char)typeName->Buffer[i];
        }
        printf("    {\"handle\":\"0x%llX\",\"info_class\":2,\"class_name\":\"ObjectTypeInformation\",\"ntstatus\":%ld,\"ntstatus_hex\":\"0x%08lX\",\"retLen\":%lu,\"type\":\"%s\"},\n",
            (unsigned long long)(uintptr_t)h1, (long)st, (unsigned long)(uint32_t)st, (unsigned long)retLen, typeA);
        // Also query event type
        st = pNtQueryObject(hEvent, 2, buf, sizeof(buf), &retLen);
        typeName = (UNICODE_STRING*)buf;
        memset(typeA,0,sizeof(typeA));
        if(NT_SUCCESS(st) && typeName->Length>0 && typeName->Buffer){
            int len = typeName->Length/2;
            if(len>63) len=63;
            for(int i=0;i<len;i++) typeA[i]=(char)typeName->Buffer[i];
        }
        printf("    {\"handle\":\"0x%llX\",\"info_class\":2,\"class_name\":\"ObjectTypeInformation\",\"ntstatus\":%ld,\"ntstatus_hex\":\"0x%08lX\",\"retLen\":%lu,\"type\":\"%s\"},\n",
            (unsigned long long)(uintptr_t)hEvent, (long)st, (unsigned long)(uint32_t)st, (unsigned long)retLen, typeA);
        // ObjectBasicInformation with small buffer to test BUFFER_OVERFLOW vs BUFFER_TOO_SMALL
        st = pNtQueryObject(h1, 0, buf, 4, &retLen);
        printf("    {\"handle\":\"0x%llX\",\"info_class\":0,\"buffer_len\":4,\"ntstatus\":%ld,\"ntstatus_hex\":\"0x%08lX\",\"retLen\":%lu},\n",
            (unsigned long long)(uintptr_t)h1, (long)st, (unsigned long)(uint32_t)st, (unsigned long)retLen);
    }
    printf("    {\"sentinel\": true}\n");
    printf("  ],\n");

    // DuplicateHandle tests
    printf("  \"duplicate_handle\": [\n");
    if(h1 != INVALID_HANDLE_VALUE){
        HANDLE hDup = NULL;
        BOOL ok = DuplicateHandle(GetCurrentProcess(), h1, GetCurrentProcess(), &hDup, 0, FALSE, DUPLICATE_SAME_ACCESS);
        printf("    {\"op\":\"DuplicateHandle_SAME_ACCESS\",\"src\":\"0x%llX\",\"dst\":\"0x%llX\",\"success\":%s,\"lasterror\":%lu},\n",
            (unsigned long long)(uintptr_t)h1, (unsigned long long)(uintptr_t)hDup, ok?"true":"false", (unsigned long)GetLastError());
        if(ok && pNtQueryObject){
            typedef struct _OBJECT_BASIC_INFORMATION { ULONG Attributes; ACCESS_MASK GrantedAccess; ULONG HandleCount; ULONG PointerCount; ULONG Reserved[10]; } OBI;
            OBI obiSrc, obiDup;
            ULONG rl;
            NTSTATUS st1 = pNtQueryObject(h1, 0, &obiSrc, sizeof(obiSrc), &rl);
            NTSTATUS st2 = pNtQueryObject(hDup, 0, &obiDup, sizeof(obiDup), &rl);
            printf("    {\"compare\":\"GrantedAccess\",\"src_access\":\"0x%lX\",\"dup_access\":\"0x%lX\",\"match\":%s},\n",
                (unsigned long)obiSrc.GrantedAccess, (unsigned long)obiDup.GrantedAccess, (obiSrc.GrantedAccess==obiDup.GrantedAccess)?"true":"false");
            printf("    {\"HandleCount_after_dup_src\":%lu,\"HandleCount_dup\":%lu},\n", (unsigned long)obiSrc.HandleCount, (unsigned long)obiDup.HandleCount);
            CloseHandle(hDup);
        }
        // Duplicate with reduced access
        HANDLE hDup2 = NULL;
        ok = DuplicateHandle(GetCurrentProcess(), h1, GetCurrentProcess(), &hDup2, GENERIC_READ, FALSE, 0);
        printf("    {\"op\":\"DuplicateHandle_GENERIC_READ\",\"src\":\"0x%llX\",\"dst\":\"0x%llX\",\"success\":%s,\"lasterror\":%lu},\n",
            (unsigned long long)(uintptr_t)h1, (unsigned long long)(uintptr_t)hDup2, ok?"true":"false", (unsigned long)GetLastError());
        if(ok){
            if(pNtQueryObject){
                typedef struct _OBJECT_BASIC_INFORMATION { ULONG Attributes; ACCESS_MASK GrantedAccess; ULONG HandleCount; ULONG PointerCount; ULONG Reserved[10]; } OBI;
                OBI obi; ULONG rl; NTSTATUS st = pNtQueryObject(hDup2, 0, &obi, sizeof(obi), &rl);
                printf("    {\"dup2_granted\":\"0x%lX\",\"ntstatus\":%ld},\n", (unsigned long)obi.GrantedAccess, (long)st);
            }
            CloseHandle(hDup2);
        }
        // Duplicate with DUPLICATE_CLOSE_SOURCE
        HANDLE hTmp = CreateFileA(tmpFile, GENERIC_READ, FILE_SHARE_READ|FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        HANDLE hDup3 = NULL;
        ok = DuplicateHandle(GetCurrentProcess(), hTmp, GetCurrentProcess(), &hDup3, 0, FALSE, DUPLICATE_CLOSE_SOURCE|DUPLICATE_SAME_ACCESS);
        // After close_source, hTmp should be invalid
        DWORD errDup = GetLastError();
        BOOL validAfter = (hTmp != NULL) ? (WaitForSingleObject(hTmp,0)!=WAIT_FAILED || GetLastError()!=ERROR_INVALID_HANDLE) : FALSE;
        // Check via GetHandleInformation
        DWORD flags;
        BOOL gi = GetHandleInformation(hTmp, &flags);
        printf("    {\"op\":\"DuplicateHandle_CLOSE_SOURCE\",\"src\":\"0x%llX\",\"dst\":\"0x%llX\",\"success\":%s,\"lasterror\":%lu,\"src_still_valid\":%s},\n",
            (unsigned long long)(uintptr_t)hTmp, (unsigned long long)(uintptr_t)hDup3, ok?"true":"false", (unsigned long)errDup, gi?"true":"false");
        if(ok) CloseHandle(hDup3);
        else CloseHandle(hTmp);
    }
    printf("    {\"sentinel\": true}\n");
    printf("  ],\n");

    // CloseHandle invalid handle tests
    printf("  \"close_tests\": [\n");
    {
        HANDLE invalid = (HANDLE)0x1234;
        BOOL ok = CloseHandle(invalid);
        printf("    {\"handle\":\"0x1234\",\"success\":%s,\"lasterror\":%lu},\n", ok?"true":"false", (unsigned long)GetLastError());
        ok = CloseHandle(NULL);
        printf("    {\"handle\":\"NULL\",\"success\":%s,\"lasterror\":%lu},\n", ok?"true":"false", (unsigned long)GetLastError());
        ok = CloseHandle(pseudoProc);
        printf("    {\"handle\":\"pseudoProc(-1)\",\"success\":%s,\"lasterror\":%lu},\n", ok?"true":"false", (unsigned long)GetLastError());
        ok = CloseHandle((HANDLE)(uintptr_t)0xFFFFFFFF);
        printf("    {\"handle\":\"0xFFFFFFFF\",\"success\":%s,\"lasterror\":%lu}\n", ok?"true":"false", (unsigned long)GetLastError());
    }
    printf("  ]\n");

    if(h1!=INVALID_HANDLE_VALUE) CloseHandle(h1);
    if(h2!=INVALID_HANDLE_VALUE) CloseHandle(h2);
    if(hEvent) CloseHandle(hEvent);
    if(hMutex) CloseHandle(hMutex);
    if(hFile2!=INVALID_HANDLE_VALUE) CloseHandle(hFile2);
    DeleteFileA(tmpFile);
    printf("}\n");
    return 0;
}
