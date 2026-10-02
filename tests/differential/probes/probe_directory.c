#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winternl.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

typedef NTSTATUS (NTAPI *PFN_NtCreateFile)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK, PLARGE_INTEGER, ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
typedef NTSTATUS (NTAPI *PFN_NtQueryDirectoryFile)(HANDLE, HANDLE, PVOID, PVOID, PIO_STATUS_BLOCK, PVOID, ULONG, ULONG, BOOLEAN, PUNICODE_STRING, BOOLEAN);
typedef NTSTATUS (NTAPI *PFN_NtClose)(HANDLE);
typedef ULONG (NTAPI *PFN_RtlNtStatusToDosError)(NTSTATUS);

static void print_build_header(const char *probe_name){
    HMODULE ntdll=GetModuleHandleA("ntdll.dll");
    OSVERSIONINFOEXW ovi={sizeof(ovi)};
    typedef LONG (WINAPI *PFN_RtlGetVersion)(OSVERSIONINFOEXW*);
    PFN_RtlGetVersion pRtlGetVersion=(PFN_RtlGetVersion)GetProcAddress(ntdll,"RtlGetVersion");
    DWORD build=0, ubr=0, major=0, minor=0;
    if(pRtlGetVersion){ pRtlGetVersion(&ovi); major=ovi.dwMajorVersion; minor=ovi.dwMinorVersion; build=ovi.dwBuildNumber; }
    HKEY hkey; DWORD ubrVal=0, cb=sizeof(ubrVal);
    if(RegOpenKeyExA(HKEY_LOCAL_MACHINE,"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",0,KEY_READ,&hkey)==ERROR_SUCCESS){
        RegQueryValueExA(hkey,"UBR",NULL,NULL,(BYTE*)&ubrVal,&cb); RegCloseKey(hkey); ubr=ubrVal;
    }
    printf("{\n");
    printf("  \"probe\": \"%s\",\n", probe_name);
    printf("  \"windows_build\": \"%lu.%lu\",\n",(unsigned long)build,(unsigned long)ubr);
    printf("  \"windows_version\": \"%lu.%lu.%lu\",\n",(unsigned long)major,(unsigned long)minor,(unsigned long)build);
    printf("  \"arch\": \"x64\",\n");
}
static void init_us(UNICODE_STRING *us, wchar_t *buf, const wchar_t *s){
    size_t len=wcslen(s)*sizeof(wchar_t);
    memcpy(buf,s,len+sizeof(wchar_t));
    us->Buffer=buf; us->Length=(USHORT)len; us->MaximumLength=(USHORT)(len+sizeof(wchar_t));
}
static void wpath_from_A(wchar_t *out, const char *a){
    int i=0; while(a[i]&&i<500){ out[i]=(wchar_t)a[i]; if(out[i]=='/') out[i]='\\'; i++; } out[i]=0;
}
static void json_escape(const char *s){ while(*s){ if(*s=='\\') printf("\\\\"); else if(*s=='"') printf("\\\""); else if(*s=='\n') printf("\\n"); else if(*s=='\r') printf("\\r"); else if(*s=='\t') printf("\\t"); else putchar(*s); s++; } }

int main(){
    setvbuf(stdout,NULL,_IONBF,0);
    print_build_header("probe_directory");
    HMODULE ntdll=GetModuleHandleA("ntdll.dll");
    PFN_NtCreateFile pNtCreateFile=(PFN_NtCreateFile)GetProcAddress(ntdll,"NtCreateFile");
    PFN_NtQueryDirectoryFile pNtQueryDirectoryFile=(PFN_NtQueryDirectoryFile)GetProcAddress(ntdll,"NtQueryDirectoryFile");
    PFN_NtClose pNtClose=(PFN_NtClose)GetProcAddress(ntdll,"NtClose");
    PFN_RtlNtStatusToDosError pRtl=(PFN_RtlNtStatusToDosError)GetProcAddress(ntdll,"RtlNtStatusToDosError");

    // Create temp directory with known files
    char tmpRootA[MAX_PATH]; GetTempPathA(sizeof(tmpRootA),tmpRootA);
    char dirA[MAX_PATH];
    snprintf(dirA,sizeof(dirA),"%sntpers_dir_%lu", tmpRootA, GetCurrentProcessId());
    CreateDirectoryA(dirA,NULL);
    char file1A[MAX_PATH]; snprintf(file1A,sizeof(file1A),"%s\\alpha.txt", dirA); HANDLE hf=CreateFileA(file1A,GENERIC_WRITE,0,NULL,CREATE_ALWAYS,0,NULL); if(hf!=INVALID_HANDLE_VALUE) CloseHandle(hf);
    char file2A[MAX_PATH]; snprintf(file2A,sizeof(file2A),"%s\\Beta.TXT", dirA); hf=CreateFileA(file2A,GENERIC_WRITE,0,NULL,CREATE_ALWAYS,0,NULL); if(hf!=INVALID_HANDLE_VALUE) CloseHandle(hf);
    char file3A[MAX_PATH]; snprintf(file3A,sizeof(file3A),"%s\\gamma.dat", dirA); hf=CreateFileA(file3A,GENERIC_WRITE,0,NULL,CREATE_ALWAYS,0,NULL); if(hf!=INVALID_HANDLE_VALUE) CloseHandle(hf);
    char subDirA[MAX_PATH]; snprintf(subDirA,sizeof(subDirA),"%s\\subdir", dirA); CreateDirectoryA(subDirA,NULL);
    char longNameA[MAX_PATH]; { char longc[200]; for(int i=0;i<190;i++) longc[i]='a'+(i%26); longc[190]=0; snprintf(longNameA,sizeof(longNameA),"%s\\%s.txt", dirA, longc); hf=CreateFileA(longNameA,GENERIC_WRITE,0,NULL,CREATE_ALWAYS,0,NULL); if(hf!=INVALID_HANDLE_VALUE) CloseHandle(hf); }

    printf("  \"test_dir\": \""); json_escape(dirA); printf("\",\n");

    // Open directory via NtCreateFile for NtQueryDirectoryFile (needs FILE_LIST_DIRECTORY)
    HANDLE hDir=NULL;
    wchar_t wdir[600]; wpath_from_A(wdir, dirA);
    // Prefix \??\ for NT
    wchar_t ntPath[700]; wcscpy(ntPath, L"\\??\\"); wcscat(ntPath, wdir);
    wchar_t ubuf[700]; UNICODE_STRING us; init_us(&us, ubuf, ntPath);
    OBJECT_ATTRIBUTES oa; InitializeObjectAttributes(&oa,&us,OBJ_CASE_INSENSITIVE,NULL,NULL);
    IO_STATUS_BLOCK iosb;
    NTSTATUS stOpen=pNtCreateFile(&hDir, FILE_LIST_DIRECTORY|SYNCHRONIZE, &oa, &iosb, NULL, FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE, FILE_OPEN, FILE_DIRECTORY_FILE|FILE_SYNCHRONOUS_IO_NONALERT, NULL,0);
    printf("  \"nt_open_dir\": {\"ntstatus\":%ld,\"hex\":\"0x%08lX\",\"dos\":%lu},\n", (long)stOpen,(unsigned long)(uint32_t)stOpen,(unsigned long)(pRtl?pRtl(stOpen):0));

    // Also test opening file as directory (should fail)
    wchar_t ntFile[700]; wcscpy(ntFile, L"\\??\\"); wchar_t wfile[600]; wpath_from_A(wfile,file1A); wcscat(ntFile,wfile);
    wchar_t ubuf2[700]; UNICODE_STRING usFile; init_us(&usFile,ubuf2,ntFile);
    OBJECT_ATTRIBUTES oaFile; InitializeObjectAttributes(&oaFile,&usFile,OBJ_CASE_INSENSITIVE,NULL,NULL);
    HANDLE hFileAsDir=NULL; IO_STATUS_BLOCK iosb2; NTSTATUS stFileDir=pNtCreateFile(&hFileAsDir, FILE_LIST_DIRECTORY|SYNCHRONIZE,&oaFile,&iosb2,NULL,FILE_ATTRIBUTE_NORMAL,FILE_SHARE_READ,FILE_OPEN,FILE_DIRECTORY_FILE|FILE_SYNCHRONOUS_IO_NONALERT,NULL,0);
    printf("  \"file_as_dir\": {\"hex\":\"0x%08lX\",\"dos\":%lu},\n",(unsigned long)(uint32_t)stFileDir,(unsigned long)(pRtl?pRtl(stFileDir):0));
    if(NT_SUCCESS(stFileDir) && pNtClose) pNtClose(hFileAsDir);

    if(!NT_SUCCESS(stOpen)){
        printf("  \"query_tests\": [],\n");
        printf("  \"error\": \"cannot open dir via NT\",\n");
        // Cleanup Win32 side still
        DeleteFileA(file1A); DeleteFileA(file2A); DeleteFileA(file3A); DeleteFileA(longNameA); RemoveDirectoryA(subDirA); RemoveDirectoryA(dirA);
        printf("  \"done\": true\n}\n");
        return 0;
    }

    // Helper to dump directory via NtQueryDirectoryFile for each info class
    // FileDirectoryInformation=1, FileFullDirectoryInformation=2, FileBothDirectoryInformation=3, FileNamesInformation=12, FileIdBothDirectoryInformation=37?
    // We'll test 1,2,3,12
    int classes[]={1,2,3,12};
    const char *names[]={"FileDirectoryInformation","FileFullDirectoryInformation","FileBothDirectoryInformation","FileNamesInformation"};
    printf("  \"query_tests\": [\n");
    int firstClass=1;
    for(int ci=0; ci<4; ci++){
        int cls=classes[ci];
        // Need fresh handle with RestartScan true for first call per class? Use same hDir but we will reopen for each class to ensure isolation
        // Reopen per class
        HANDLE hC=NULL; NTSTATUS stO=pNtCreateFile(&hC, FILE_LIST_DIRECTORY|SYNCHRONIZE, &oa, &iosb, NULL, FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE, FILE_OPEN, FILE_DIRECTORY_FILE|FILE_SYNCHRONOUS_IO_NONALERT, NULL,0);
        if(!NT_SUCCESS(stO)){ if(!firstClass) printf(",\n"); printf("    {\"class\":%d,\"name\":\"%s\",\"open_hex\":\"0x%08lX\"}", cls,names[ci],(unsigned long)(uint32_t)stO); firstClass=0; continue; }
        char buf[8192]; IO_STATUS_BLOCK ios;
        NTSTATUS st=pNtQueryDirectoryFile(hC,NULL,NULL,NULL,&ios,buf,sizeof(buf),cls,FALSE,NULL,TRUE);
        // FALSE = not ReturnSingleEntry, TRUE = RestartScan
        // Parse count of entries
        int count=0;
        if(NT_SUCCESS(st)){
            // Walk: Each entry starts with NextEntryOffset ULONG at 0, then FileIndex, CreationTime etc. We just count via NextEntryOffset chain
            ULONG off=0; while(1){ if(count>100) break; ULONG next=*(ULONG*)(buf+off); count++; if(next==0) break; off+=next; }
        }
        // Second call without RestartScan should give NO_MORE_FILES or NO_SUCH_FILE?
        NTSTATUS st2=pNtQueryDirectoryFile(hC,NULL,NULL,NULL,&ios,buf,sizeof(buf),cls,FALSE,NULL,FALSE);
        // RestartScan true again should repeat first entry
        NTSTATUS st3=pNtQueryDirectoryFile(hC,NULL,NULL,NULL,&ios,buf,sizeof(buf),cls,FALSE,NULL,TRUE);
        // ReturnSingleEntry true
        // Need fresh handle for single entry test
        HANDLE hSingle=NULL; pNtCreateFile(&hSingle, FILE_LIST_DIRECTORY|SYNCHRONIZE, &oa, &iosb, NULL, FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE, FILE_OPEN, FILE_DIRECTORY_FILE|FILE_SYNCHRONOUS_IO_NONALERT, NULL,0);
        char buf2[8192]; IO_STATUS_BLOCK ios2b; NTSTATUS stSingle=-1, stSingle2=-1;
        if(hSingle){ stSingle=pNtQueryDirectoryFile(hSingle,NULL,NULL,NULL,&ios2b,buf2,sizeof(buf2),cls,TRUE,NULL,TRUE); stSingle2=pNtQueryDirectoryFile(hSingle,NULL,NULL,NULL,&ios2b,buf2,sizeof(buf2),cls,TRUE,NULL,FALSE); pNtClose(hSingle); }
        // Small buffer (should give BUFFER_OVERFLOW)
        char small[4]; IO_STATUS_BLOCK iosSmall; NTSTATUS stSmall=pNtQueryDirectoryFile(hC,NULL,NULL,NULL,&iosSmall,small,4,cls,FALSE,NULL,TRUE);
        // Null buffer
        IO_STATUS_BLOCK iosNull; NTSTATUS stNull=pNtQueryDirectoryFile(hC,NULL,NULL,NULL,&iosNull,NULL,0,cls,FALSE,NULL,TRUE);
        // Wildcard filter: "*.txt"
        HANDLE hWild=NULL; pNtCreateFile(&hWild, FILE_LIST_DIRECTORY|SYNCHRONIZE, &oa, &iosb, NULL, FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE, FILE_OPEN, FILE_DIRECTORY_FILE|FILE_SYNCHRONOUS_IO_NONALERT, NULL,0);
        wchar_t wildW[] = L"*.txt"; wchar_t wwildBuf[64]; UNICODE_STRING usWild; init_us(&usWild,wwildBuf,wildW);
        char wbuf[8192]; IO_STATUS_BLOCK iosWild; NTSTATUS stWild=-1; int wildCount=0;
        if(hWild){ stWild=pNtQueryDirectoryFile(hWild,NULL,NULL,NULL,&iosWild,wbuf,sizeof(wbuf),cls,FALSE,&usWild,TRUE); if(NT_SUCCESS(stWild)){ ULONG off=0; while(1){ ULONG next=*(ULONG*)(wbuf+off); wildCount++; if(next==0) break; off+=next; if(wildCount>100) break; } } pNtClose(hWild); }
        // Case: invalid handle
        char ibuf[512]; IO_STATUS_BLOCK iosInv; NTSTATUS stInv=pNtQueryDirectoryFile((HANDLE)0x1234,NULL,NULL,NULL,&iosInv,ibuf,sizeof(ibuf),cls,FALSE,NULL,TRUE);
        if(!firstClass) printf(",\n"); firstClass=0;
        printf("    {\"class\":%d,\"name\":\"%s\",\"first_hex\":\"0x%08lX\",\"first_count\":%d,\"second_hex\":\"0x%08lX\",\"restart_hex\":\"0x%08lX\",\"single_hex\":\"0x%08lX\",\"single2_hex\":\"0x%08lX\",\"small_hex\":\"0x%08lX\",\"null_hex\":\"0x%08lX\",\"wild_hex\":\"0x%08lX\",\"wild_count\":%d,\"invalid_hex\":\"0x%08lX\"}", cls,names[ci],(unsigned long)(uint32_t)st,count,(unsigned long)(uint32_t)st2,(unsigned long)(uint32_t)st3,(unsigned long)(uint32_t)stSingle,(unsigned long)(uint32_t)stSingle2,(unsigned long)(uint32_t)stSmall,(unsigned long)(uint32_t)stNull,(unsigned long)(uint32_t)stWild,wildCount,(unsigned long)(uint32_t)stInv);
        pNtClose(hC);
    }
    printf("\n  ],\n");
    pNtClose(hDir);

    // Win32 FindFirstFile oracle for comparison
    WIN32_FIND_DATAA fd; HANDLE hFind=FindFirstFileA("C:\\tmp\\*", &fd); // not our dir, just to show Find works; we use our dir
    HANDLE hFind2=FindFirstFileA((char*)dirA, &fd); // wrong, need path\* 
    char patternA[MAX_PATH]; snprintf(patternA,sizeof(patternA),"%s\\*", dirA);
    int winCount=0; HANDLE hFind3=FindFirstFileA(patternA,&fd); if(hFind3!=INVALID_HANDLE_VALUE){ do{ winCount++; }while(FindNextFileA(hFind3,&fd)); FindClose(hFind3); }
    printf("  \"win32_find\": {\"pattern\":\""); json_escape(dirA); printf("\\\\*\",\"count\":%d},\n", winCount);
    // Empty directory test
    char emptyA[MAX_PATH]; snprintf(emptyA,sizeof(emptyA),"%s\\empty", dirA); CreateDirectoryA(emptyA,NULL);
    wchar_t wempty[700]; wcscpy(wempty, L"\\??\\"); wchar_t wemptyBase[600]; wpath_from_A(wemptyBase, emptyA); wcscat(wempty,wemptyBase);
    wchar_t ubufE[700]; UNICODE_STRING usE; init_us(&usE,ubufE,wempty);
    OBJECT_ATTRIBUTES oaE; InitializeObjectAttributes(&oaE,&usE,OBJ_CASE_INSENSITIVE,NULL,NULL);
    HANDLE hEmpty=NULL; NTSTATUS stEmptyOpen=pNtCreateFile(&hEmpty, FILE_LIST_DIRECTORY|SYNCHRONIZE,&oaE,&iosb,NULL,FILE_ATTRIBUTE_NORMAL,FILE_SHARE_READ,FILE_OPEN,FILE_DIRECTORY_FILE|FILE_SYNCHRONOUS_IO_NONALERT,NULL,0);
    char ebuf[4096]; IO_STATUS_BLOCK iosE; NTSTATUS stEmptyQuery=-1; if(NT_SUCCESS(stEmptyOpen)) stEmptyQuery=pNtQueryDirectoryFile(hEmpty,NULL,NULL,NULL,&iosE,ebuf,sizeof(ebuf),1,FALSE,NULL,TRUE);
    printf("  \"empty_dir\": {\"open_hex\":\"0x%08lX\",\"query_hex\":\"0x%08lX\"},\n",(unsigned long)(uint32_t)stEmptyOpen,(unsigned long)(uint32_t)stEmptyQuery);
    if(hEmpty) pNtClose(hEmpty); RemoveDirectoryA(emptyA);

    // Cleanup
    DeleteFileA(file1A); DeleteFileA(file2A); DeleteFileA(file3A); DeleteFileA(longNameA); RemoveDirectoryA(subDirA); RemoveDirectoryA(dirA);
    printf("  \"done\": true\n");
    printf("}\n");
    return 0;
}
