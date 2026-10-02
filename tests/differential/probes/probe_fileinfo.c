#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winternl.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

typedef NTSTATUS (NTAPI *PFN_NtCreateFile)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK, PLARGE_INTEGER, ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
typedef NTSTATUS (NTAPI *PFN_NtQueryInformationFile)(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, ULONG);
typedef NTSTATUS (NTAPI *PFN_NtSetInformationFile)(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, ULONG);
typedef NTSTATUS (NTAPI *PFN_NtQueryVolumeInformationFile)(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, ULONG);
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

int main(){
    setvbuf(stdout,NULL,_IONBF,0);
    print_build_header("probe_fileinfo");
    HMODULE ntdll=GetModuleHandleA("ntdll.dll");
    PFN_NtCreateFile pNtCreateFile=(PFN_NtCreateFile)GetProcAddress(ntdll,"NtCreateFile");
    PFN_NtQueryInformationFile pNtQIF=(PFN_NtQueryInformationFile)GetProcAddress(ntdll,"NtQueryInformationFile");
    PFN_NtSetInformationFile pNtSIF=(PFN_NtSetInformationFile)GetProcAddress(ntdll,"NtSetInformationFile");
    PFN_NtQueryVolumeInformationFile pNtQVIF=(PFN_NtQueryVolumeInformationFile)GetProcAddress(ntdll,"NtQueryVolumeInformationFile");
    PFN_NtClose pNtClose=(PFN_NtClose)GetProcAddress(ntdll,"NtClose");
    PFN_RtlNtStatusToDosError pRtl=(PFN_RtlNtStatusToDosError)GetProcAddress(ntdll,"RtlNtStatusToDosError");

    char tmpRootA[MAX_PATH]; GetTempPathA(sizeof(tmpRootA),tmpRootA);
    char fileA[MAX_PATH]; snprintf(fileA,sizeof(fileA),"%sntpers_fileinfo_%lu.tmp", tmpRootA, GetCurrentProcessId());
    HANDLE hWin=CreateFileA(fileA, GENERIC_READ|GENERIC_WRITE, FILE_SHARE_READ|FILE_SHARE_WRITE, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if(hWin!=INVALID_HANDLE_VALUE){ const char *data="hello world fileinfo"; DWORD w; WriteFile(hWin,data,(DWORD)strlen(data),&w,NULL); CloseHandle(hWin); }

    // NT open
    wchar_t wfile[700]; for(int i=0;fileA[i];i++) wfile[i]=(wchar_t)fileA[i]; wfile[strlen(fileA)]=0; for(int i=0;wfile[i];i++) if(wfile[i]=='/') wfile[i]='\\';
    wchar_t ntPath[800]; wcscpy(ntPath,L"\\??\\"); wcscat(ntPath,wfile);
    wchar_t ubuf[800]; UNICODE_STRING us; init_us(&us,ubuf,ntPath);
    OBJECT_ATTRIBUTES oa; InitializeObjectAttributes(&oa,&us,OBJ_CASE_INSENSITIVE,NULL,NULL);
    HANDLE h=NULL; IO_STATUS_BLOCK ios; NTSTATUS stOpen=pNtCreateFile(&h, GENERIC_READ|GENERIC_WRITE|SYNCHRONIZE, &oa, &ios, NULL, FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ|FILE_SHARE_WRITE, FILE_OPEN, FILE_SYNCHRONOUS_IO_NONALERT, NULL,0);
    printf("  \"nt_open\": {\"hex\":\"0x%08lX\",\"dos\":%lu},\n",(unsigned long)(uint32_t)stOpen,(unsigned long)(pRtl?pRtl(stOpen):0));
    if(!NT_SUCCESS(stOpen)){
        printf("  \"query\": [],\n  \"done\": true\n}\n"); DeleteFileA(fileA); return 0;
    }

    // Query classes: combine NT's FILE_INFORMATION_CLASS values
    struct { ULONG cls; const char *name; ULONG expected; ULONG is_fixed; } classes[] = {
        {4, "FileBasicInformation", 40, 1},
        {5, "FileStandardInformation", 24, 1},
        {6, "FileInternalInformation", 8, 1},
        {7, "FileEaInformation", 4, 1},
        {8, "FileAccessInformation", 4, 1},
        {9, "FileNameInformation", 0, 0}, // variable
        {14,"FilePositionInformation", 8, 1},
        {16,"FileModeInformation", 4, 1},
        {17,"FileAlignmentInformation",4,1},
        {18,"FileAllInformation", 0,0}, // variable large
        {22,"FileStreamInformation",0,0},
        {35,"FileAttributeTagInformation",8,1},
        {12,"FileAlternateNameInformation",0,0},
        {13,"FileLinkInformation",0,0},
        {10,"FilePipeInformation",8,1},
        {11,"FilePipeLocalInformation",32,1},
        {19,"FileDispositionInformation",1,1},
        {20,"FileEndOfFileInformation",8,1},
        {28,"FileCompressionInformation",16,1},
        {33,"FileNetworkOpenInformation",56,1},
    };
    int nCls=sizeof(classes)/sizeof(classes[0]);
    printf("  \"query\": [\n");
    for(int i=0;i<nCls;i++){
        char buf[8192]; IO_STATUS_BLOCK iosb;
        ULONG lenFull=sizeof(buf);
        ULONG lenZero=0; ULONG lenSmall=4;
        // Use actual buffer sizes that exercise ReturnLength discipline
        NTSTATUS stFull=pNtQIF(h, &iosb, buf, lenFull, classes[i].cls);
        ULONG retSmall=0xDEADBEEF; char small[4]; IO_STATUS_BLOCK iosSmall; NTSTATUS stSmall=pNtQIF(h, &iosSmall, small, 4, classes[i].cls);
        // Zero len
        IO_STATUS_BLOCK iosZ; NTSTATUS stZero=pNtQIF(h, &iosZ, buf, 0, classes[i].cls);
        // Minimal but non-zero truncated for variable: 8 bytes
        IO_STATUS_BLOCK ios8; char b8[8]; NTSTATUS st8=pNtQIF(h, &ios8, b8, 8, classes[i].cls);
        // Invalid handle probe for first class only later
        printf("    {\"class\":%lu,\"name\":\"%s\",\"full_hex\":\"0x%08lX\",\"full_info\":%llu,\"small_hex\":\"0x%08lX\",\"small_ret\":%llu,\"zero_hex\":\"0x%08lX\",\"trunc8_hex\":\"0x%08lX\"}%s\n",
            (unsigned long)classes[i].cls, classes[i].name,
            (unsigned long)(uint32_t)stFull,(unsigned long long)iosb.Information,
            (unsigned long)(uint32_t)stSmall,(unsigned long long)iosSmall.Information,
            (unsigned long)(uint32_t)stZero,
            (unsigned long)(uint32_t)st8,
            i+1<nCls?",":"");
    }
    printf("  ],\n");

    // Volume information classes
    struct { ULONG cls; const char *name; } vclasses[] = {
        {1, "FileFsVolumeInformation"},
        {2, "FileFsLabelInformation"},
        {3, "FileFsSizeInformation"},
        {4, "FileFsDeviceInformation"},
        {5, "FileFsAttributeInformation"},
        {6, "FileFsControlInformation"},
        {7, "FileFsFullSizeInformation"},
        {8, "FileFsObjectIdInformation"},
    };
    int nV=sizeof(vclasses)/sizeof(vclasses[0]);
    printf("  \"volume_query\": [\n");
    for(int i=0;i<nV;i++){
        char buf[1024]; IO_STATUS_BLOCK iosb; NTSTATUS st=pNtQVIF(h, &iosb, buf, sizeof(buf), vclasses[i].cls);
        char small[4]; IO_STATUS_BLOCK iosS; NTSTATUS stS=pNtQVIF(h, &iosS, small, 4, vclasses[i].cls);
        printf("    {\"class\":%lu,\"name\":\"%s\",\"full_hex\":\"0x%08lX\",\"small_hex\":\"0x%08lX\"}%s\n",(unsigned long)vclasses[i].cls,vclasses[i].name,(unsigned long)(uint32_t)st,(unsigned long)(uint32_t)stS, i+1<nV?",":"");
    }
    printf("  ],\n");

    // SetInformation tests: FileBasicInformation timestamps, FileEndOfFileInformation, FileDispositionInformation, FilePositionInformation
    printf("  \"set_info\": [\n");
    // FilePositionInformation: set file pointer
    { char buf[8]; *(int64_t*)buf=5; IO_STATUS_BLOCK iosb; NTSTATUS st=pNtSIF(h,&iosb,buf,sizeof(buf),14); printf("    {\"name\":\"FilePositionInformation_set5\",\"hex\":\"0x%08lX\"},\n",(unsigned long)(uint32_t)st); }
    // verify
    { char buf[8]; IO_STATUS_BLOCK iosb; NTSTATUS st=pNtQIF(h,&iosb,buf,sizeof(buf),14); int64_t pos=*(int64_t*)buf; printf("    {\"name\":\"FilePositionInformation_verify\",\"hex\":\"0x%08lX\",\"pos\":%lld},\n",(unsigned long)(uint32_t)st,(long long)pos); }
    // FileEndOfFileInformation: extend to 100 bytes
    { char buf[8]; *(int64_t*)buf=100; IO_STATUS_BLOCK iosb; NTSTATUS st=pNtSIF(h,&iosb,buf,sizeof(buf),20); printf("    {\"name\":\"FileEndOfFileInformation_extend100\",\"hex\":\"0x%08lX\"},\n",(unsigned long)(uint32_t)st); }
    // FileBasicInformation: set timestamps to known FILETIME (2020-01-01)
    { struct { int64_t CreationTime,LastAccessTime,LastWriteTime,ChangeTime; ULONG FileAttributes; } bi; bi.CreationTime=132224400000000000LL; bi.LastAccessTime=132224400000000000LL; bi.LastWriteTime=132224400000000000LL; bi.ChangeTime=132224400000000000LL; bi.FileAttributes=FILE_ATTRIBUTE_NORMAL; IO_STATUS_BLOCK iosb; NTSTATUS st=pNtSIF(h,&iosb,&bi,sizeof(bi),4); printf("    {\"name\":\"FileBasicInformation_set\",\"hex\":\"0x%08lX\"},\n",(unsigned long)(uint32_t)st); }
    // Invalid class
    { char buf[8]; IO_STATUS_BLOCK iosb; NTSTATUS st=pNtSIF(h,&iosb,buf,sizeof(buf),9999); printf("    {\"name\":\"BogusClass\",\"hex\":\"0x%08lX\"},\n",(unsigned long)(uint32_t)st); }
    // Small buffer for Set
    { char buf[2]; IO_STATUS_BLOCK iosb; NTSTATUS st=pNtSIF(h,&iosb,buf,2,14); printf("    {\"name\":\"FilePositionInformation_small2\",\"hex\":\"0x%08lX\"}\n",(unsigned long)(uint32_t)st); }
    printf("  ],\n");

    // FILETIME round-trip via Win32
    FILETIME ctime, atime, wtime; GetFileTime(h, &ctime,&atime,&wtime);
    printf("  \"win32_filetime\": {\"creation_low\":%lu,\"creation_high\":%lu,\"write_low\":%lu,\"write_high\":%lu},\n",(unsigned long)ctime.dwLowDateTime,(unsigned long)ctime.dwHighDateTime,(unsigned long)wtime.dwLowDateTime,(unsigned long)wtime.dwHighDateTime);
    BY_HANDLE_FILE_INFORMATION bhfi; GetFileInformationByHandle(h,&bhfi);
    printf("  \"by_handle_info\": {\"size_low\":%lu,\"size_high\":%lu,\"attrs\":%lu,\"nLinks\":%lu},\n",(unsigned long)bhfi.nFileSizeLow,(unsigned long)bhfi.nFileSizeHigh,(unsigned long)bhfi.dwFileAttributes,(unsigned long)bhfi.nNumberOfLinks);
    // Invalid handle test
    { char buf[64]; IO_STATUS_BLOCK iosb; NTSTATUS st=pNtQIF((HANDLE)0x1234,&iosb,buf,sizeof(buf),4); printf("  \"invalid_handle\": {\"hex\":\"0x%08lX\"},\n",(unsigned long)(uint32_t)st); }

    pNtClose(h);
    DeleteFileA(fileA);
    printf("  \"done\": true\n");
    printf("}\n");
    return 0;
}
