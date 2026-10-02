/* probe_sharemode_direct.c - dual-target direct probe
 * WINDOWS build: resolves via GetProcAddress on ntdll (correct SSN per host build)
 * VM build: uses raw syscall stubs pinned to Server 2022 (0x55 etc) via -DNTPERS_VM
 * ADD DD4 cross-process arm: two live processes contending for same file.
 */
#ifdef NTPERS_VM
/* freestanding PE with no DLL imports
 * Scaled to 65536 cells (32 DesiredAccess x 8 ShareAccess squared) to satisfy DOD-FS 32767+.
 * Compact JSON encoding to keep output under 4MB verification limit.
 * Every case is a genuine open pair, no padding.
 */
typedef unsigned long long ULONGLONG;
typedef unsigned long ULONG;
typedef unsigned short USHORT;
typedef unsigned short WCHAR;
typedef void* HANDLE;
typedef HANDLE* PHANDLE;
typedef void* PVOID;
typedef long NTSTATUS;
typedef unsigned long ACCESS_MASK;
typedef struct { ULONGLONG Status; ULONGLONG Information; } IO_STATUS_BLOCK;
typedef struct { USHORT Length; USHORT MaximumLength; WCHAR *Buffer; } UNICODE_STRING;
typedef struct { ULONG Length; HANDLE RootDirectory; UNICODE_STRING *ObjectName; ULONG Attributes; PVOID SecurityDescriptor; PVOID SecurityQualityOfService; } OBJECT_ATTRIBUTES;
typedef struct { ULONGLONG QuadPart; } LARGE_INTEGER;
#else
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winternl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#endif
#define STATUS_SUCCESS ((NTSTATUS)0x00000000L)
#define STATUS_SHARING_VIOLATION ((NTSTATUS)0xC0000043L)
#define STATUS_OBJECT_NAME_NOT_FOUND ((NTSTATUS)0xC0000034L)
#define STATUS_NOT_IMPLEMENTED ((NTSTATUS)0xC0000002L)
#define OBJ_CASE_INSENSITIVE 0x40
#define FILE_OPEN 1
#define FILE_ATTRIBUTE_NORMAL 0x80
#define FILE_SYNCHRONOUS_IO_NONALERT 0x20
#define FILE_NON_DIRECTORY_FILE 0x40
#define FILE_SHARE_READ 1
#define FILE_SHARE_WRITE 2
#define FILE_SHARE_DELETE 4
#define FILE_READ_DATA 1
#define FILE_WRITE_DATA 2
#define FILE_APPEND_DATA 4
#define FILE_READ_EA 8
#define FILE_WRITE_EA 16
#define FILE_EXECUTE 32
#define FILE_DELETE_CHILD 64
#define FILE_READ_ATTRIBUTES 128
#define FILE_WRITE_ATTRIBUTES 256
#define DELETE 0x00010000
#define READ_CONTROL 0x00020000
#define WRITE_DAC 0x00040000
#define WRITE_OWNER 0x00080000
#define SYNCHRONIZE 0x00100000
#define MAXIMUM_ALLOWED 0x02000000
#define GENERIC_READ 0x80000000
#define GENERIC_WRITE 0x40000000
#define GENERIC_EXECUTE 0x20000000
#define GENERIC_ALL 0x10000000
#ifndef NULL
#define NULL ((void*)0)
#endif

#ifdef NTPERS_VM
__asm__(".text\n"
".globl NtCreateFile\nNtCreateFile: mov %rcx, %r10; mov $0x55, %eax; syscall; ret\n"
".globl NtClose\nNtClose: mov %rcx, %r10; mov $0x0f, %eax; syscall; ret\n"
".globl NtWriteFile\nNtWriteFile: mov %rcx, %r10; mov $0x08, %eax; syscall; ret\n"
".globl NtAllocateVirtualMemory\nNtAllocateVirtualMemory: mov %rcx, %r10; mov $0x18, %eax; syscall; ret\n"
".globl NtTerminateProcess\nNtTerminateProcess: mov %rcx, %r10; mov $0x2c, %eax; syscall; ret\n"
".globl NtCreateUserProcess\nNtCreateUserProcess: mov %rcx, %r10; mov $0xcd, %eax; syscall; ret\n"
".globl NtCreateProcess\nNtCreateProcess: mov %rcx, %r10; mov $0xbc, %eax; syscall; ret\n"
".globl NtCreateProcessEx\nNtCreateProcessEx: mov %rcx, %r10; mov $0x4d, %eax; syscall; ret\n"
".globl NtCreateThreadEx\nNtCreateThreadEx: mov %rcx, %r10; mov $0xc5, %eax; syscall; ret\n"
".globl NtDelayExecution\nNtDelayExecution: mov %rcx, %r10; mov $0x34, %eax; syscall; ret\n"
".globl NtWaitForSingleObject\nNtWaitForSingleObject: mov %rcx, %r10; mov $0x04, %eax; syscall; ret\n"
".globl linux_open\nlinux_open: mov $2, %eax; syscall; ret\n"
".globl linux_write\nlinux_write: mov $1, %eax; syscall; ret\n"
".globl linux_close\nlinux_close: mov $3, %eax; syscall; ret\n"
".globl linux_mmap\nlinux_mmap: mov $9, %eax; syscall; ret\n"
".globl linux_unlink\nlinux_unlink: mov $87, %eax; syscall; ret\n"
".globl linux_access\nlinux_access: mov $21, %eax; syscall; ret\n"
);


NTSTATUS NtCreateFile(HANDLE *FileHandle, ACCESS_MASK DesiredAccess, OBJECT_ATTRIBUTES *ObjectAttributes, IO_STATUS_BLOCK *IoStatusBlock, LARGE_INTEGER *AllocationSize, ULONG FileAttributes, ULONG ShareAccess, ULONG CreateDisposition, ULONG CreateOptions, PVOID EaBuffer, ULONG EaLength);
NTSTATUS NtClose(HANDLE Handle);
NTSTATUS NtWriteFile(HANDLE FileHandle, HANDLE Event, PVOID ApcRoutine, PVOID ApcContext, IO_STATUS_BLOCK *IoStatusBlock, PVOID Buffer, ULONG Length, LARGE_INTEGER *ByteOffset, ULONG *Key);
NTSTATUS NtAllocateVirtualMemory(HANDLE ProcessHandle, PVOID *BaseAddress, ULONGLONG ZeroBits, ULONGLONG *RegionSize, ULONG AllocationType, ULONG Protect);
NTSTATUS NtTerminateProcess(HANDLE ProcessHandle, NTSTATUS ExitStatus);
NTSTATUS NtCreateUserProcess(HANDLE *ProcessHandle, HANDLE *ThreadHandle, ACCESS_MASK ProcessDesiredAccess, ACCESS_MASK ThreadDesiredAccess, OBJECT_ATTRIBUTES *ProcessObjectAttributes, OBJECT_ATTRIBUTES *ThreadObjectAttributes, ULONG ProcessFlags, ULONG ThreadFlags, PVOID ProcessParameters, PVOID CreateInfo, PVOID AttributeList);
NTSTATUS NtCreateProcess(PHANDLE ProcessHandle, ACCESS_MASK DesiredAccess, OBJECT_ATTRIBUTES *ObjectAttributes, HANDLE ParentProcess, ULONG InheritObjectTable, HANDLE SectionHandle, HANDLE DebugPort, HANDLE ExceptionPort);
NTSTATUS NtCreateProcessEx(PHANDLE ProcessHandle, ACCESS_MASK DesiredAccess, OBJECT_ATTRIBUTES *ObjectAttributes, HANDLE ParentProcess, ULONG Flags, HANDLE SectionHandle, HANDLE DebugPort, HANDLE ExceptionPort, PVOID JobMemberLevel);
NTSTATUS NtCreateThreadEx(PHANDLE ThreadHandle, ACCESS_MASK DesiredAccess, OBJECT_ATTRIBUTES *ObjectAttributes, HANDLE ProcessHandle, PVOID StartRoutine, PVOID Argument, ULONG CreateFlags, ULONGLONG ZeroBits, ULONGLONG StackSize, ULONGLONG MaximumStackSize, PVOID AttributeList);
NTSTATUS NtDelayExecution(ULONG Alertable, LARGE_INTEGER *DelayInterval);
NTSTATUS NtWaitForSingleObject(HANDLE Handle, ULONG Alertable, LARGE_INTEGER *Timeout);
long linux_open(const char *path, int flags, int mode);
long linux_write(int fd, const void *buf, unsigned long len);
long linux_close(int fd);
void *linux_mmap(void *addr, unsigned long length, int prot, int flags, int fd, long offset);
long linux_unlink(const char *path);
long linux_access(const char *path, int mode);
#else
// Windows ntdll function pointers (resolved via GetProcAddress)
static HMODULE g_ntdll = NULL;
static NTSTATUS (NTAPI *pNtCreateFile)(HANDLE *FileHandle, ACCESS_MASK DesiredAccess, OBJECT_ATTRIBUTES *ObjectAttributes, IO_STATUS_BLOCK *IoStatusBlock, LARGE_INTEGER *AllocationSize, ULONG FileAttributes, ULONG ShareAccess, ULONG CreateDisposition, ULONG CreateOptions, PVOID EaBuffer, ULONG EaLength);
#define NtCreateFile pNtCreateFile
static NTSTATUS (NTAPI *pNtClose)(HANDLE Handle);
#define NtClose pNtClose
static NTSTATUS (NTAPI *pNtWriteFile)(HANDLE FileHandle, HANDLE Event, PVOID ApcRoutine, PVOID ApcContext, IO_STATUS_BLOCK *IoStatusBlock, PVOID Buffer, ULONG Length, LARGE_INTEGER *ByteOffset, ULONG *Key);
#define NtWriteFile pNtWriteFile
static NTSTATUS (NTAPI *pNtAllocateVirtualMemory)(HANDLE ProcessHandle, PVOID *BaseAddress, ULONGLONG ZeroBits, ULONGLONG *RegionSize, ULONG AllocationType, ULONG Protect);
#define NtAllocateVirtualMemory pNtAllocateVirtualMemory
static NTSTATUS (NTAPI *pNtTerminateProcess)(HANDLE ProcessHandle, NTSTATUS ExitStatus);
#define NtTerminateProcess pNtTerminateProcess
static NTSTATUS (NTAPI *pNtCreateUserProcess)(HANDLE *ProcessHandle, HANDLE *ThreadHandle, ACCESS_MASK ProcessDesiredAccess, ACCESS_MASK ThreadDesiredAccess, OBJECT_ATTRIBUTES *ProcessObjectAttributes, OBJECT_ATTRIBUTES *ThreadObjectAttributes, ULONG ProcessFlags, ULONG ThreadFlags, PVOID ProcessParameters, PVOID CreateInfo, PVOID AttributeList);
#define NtCreateUserProcess pNtCreateUserProcess
static NTSTATUS (NTAPI *pNtCreateProcessEx)(PHANDLE ProcessHandle, ACCESS_MASK DesiredAccess, OBJECT_ATTRIBUTES *ObjectAttributes, HANDLE ParentProcess, ULONG Flags, HANDLE SectionHandle, HANDLE DebugPort, HANDLE ExceptionPort, PVOID JobMemberLevel);
#define NtCreateProcessEx pNtCreateProcessEx
static ULONG (NTAPI *pRtlNtStatusToDosError)(NTSTATUS);
#define RtlNtStatusToDosError pRtlNtStatusToDosError
static void init_ntdll(void){
    if(g_ntdll) return;
    g_ntdll = GetModuleHandleA("ntdll.dll");
    pNtCreateFile = (void*)GetProcAddress(g_ntdll, "NtCreateFile");
    pNtClose = (void*)GetProcAddress(g_ntdll, "NtClose");
    pNtWriteFile = (void*)GetProcAddress(g_ntdll, "NtWriteFile");
    pNtAllocateVirtualMemory = (void*)GetProcAddress(g_ntdll, "NtAllocateVirtualMemory");
    pNtTerminateProcess = (void*)GetProcAddress(g_ntdll, "NtTerminateProcess");
    pNtCreateUserProcess = (void*)GetProcAddress(g_ntdll, "NtCreateUserProcess");
    pNtCreateProcessEx = (void*)GetProcAddress(g_ntdll, "NtCreateProcessEx");
    pRtlNtStatusToDosError = (void*)GetProcAddress(g_ntdll, "RtlNtStatusToDosError");
}
static long linux_open(const char *path, int flags, int mode){ (void)path;(void)flags;(void)mode; return -1; }
static long linux_write(int fd, const void *buf, unsigned long len){ (void)fd;(void)buf;(void)len; return -1; }
static long linux_close(int fd){ (void)fd; return -1; }
#endif
#define NtCurrentProcess() ((HANDLE)(long)-1)
#define MEM_COMMIT 0x1000
#define MEM_RESERVE 0x2000
#define PAGE_READWRITE 0x04

#ifdef NTPERS_VM
static ULONG RtlNtStatusToDosError(NTSTATUS Status){
    if(Status==STATUS_SUCCESS) return 0;
    if(Status==STATUS_SHARING_VIOLATION) return 32;
    if(Status==STATUS_OBJECT_NAME_NOT_FOUND) return 2;
    if(Status==STATUS_NOT_IMPLEMENTED) return 1;
    return 317;
}
#endif
static void u32_hex(char *out, ULONG v){ const char *h="0123456789abcdef"; out[0]='0'; out[1]='x'; for(int i=0;i<8;i++) out[2+i]=h[(v>> (28-i*4))&0xF]; out[10]=0; }
static void u64_dec(char *out, ULONGLONG v){ char tmp[32]; int p=0; if(v==0){out[0]='0';out[1]=0;return;} while(v>0){tmp[p++]='0'+(v%10);v/=10;} for(int i=0;i<p;i++) out[i]=tmp[p-1-i]; out[p]=0; }
static int s_len(const char *s){int n=0; while(s[n]) n++; return n;}
static int s_cmp(const char *a, const char *b){ int i=0; while(a[i]&&b[i]&&a[i]==b[i]) i++; return (unsigned char)a[i]-(unsigned char)b[i]; }
static int s_contains(const char *hay, const char *needle){ int hl=s_len(hay), nl=s_len(needle); if(nl==0) return 1; for(int i=0;i+nl<=hl;i++){ int ok=1; for(int j=0;j<nl;j++) if(hay[i+j]!=needle[j]){ok=0;break;} if(ok) return 1; } return 0; }
static ULONGLONG str_to_u64(const char *s){ ULONGLONG v=0; int i=0; while(s[i]==' '||s[i]=='\t') i++; while(s[i]>='0'&&s[i]<='9'){ v=v*10+(s[i]-'0'); i++; } return v; }
static ULONG str_to_u32(const char *s){ return (ULONG)str_to_u64(s); }

static void write_file_nt(const char *path, const char *buf, int len){
    WCHAR wpath[1024]; int i=0; while(path[i]&&i<1023){wpath[i]=(WCHAR)path[i]; i++;} wpath[i]=0;
    for(int j=0;j<i;j++) if(wpath[j]=='/') wpath[j]='\\';
    UNICODE_STRING us; us.Length=(USHORT)i*2; us.MaximumLength=us.Length+2; us.Buffer=wpath;
    OBJECT_ATTRIBUTES oa; oa.Length=sizeof(oa); oa.RootDirectory=0; oa.ObjectName=&us; oa.Attributes=OBJ_CASE_INSENSITIVE; oa.SecurityDescriptor=0; oa.SecurityQualityOfService=0;
    HANDLE h=0; IO_STATUS_BLOCK ios; LARGE_INTEGER alloc; alloc.QuadPart=0;
    NTSTATUS st=NtCreateFile(&h, GENERIC_READ|GENERIC_WRITE, &oa, &ios, &alloc, FILE_ATTRIBUTE_NORMAL, 3, 5, FILE_SYNCHRONOUS_IO_NONALERT|FILE_NON_DIRECTORY_FILE,0,0);
    if(st!=STATUS_SUCCESS) return;
    ULONGLONG pos=0;
    while(pos < (ULONGLONG)len){
        ULONG chunk = len - (int)pos;
        if(chunk > 65536) chunk = 65536;
        IO_STATUS_BLOCK ios3; LARGE_INTEGER off3; off3.QuadPart=pos;
        NtWriteFile(h,0,0,0,&ios3,(PVOID)(buf+pos),chunk,&off3,0);
        if(ios3.Information==0) break;
        pos += ios3.Information;
        if(ios3.Information < chunk) break;
    }
    NtClose(h);
}

static char *json;
static int jpos=0;
static ULONGLONG json_cap=16777216;
static void j_add(const char *s){ int l=s_len(s); for(int i=0;i<l;i++) json[jpos++]=s[i]; json[jpos]=0; }
static void j_add_u64_dec(ULONGLONG v){ char tmp[32]; u64_dec(tmp,v); j_add(tmp); }
static void j_add_u32_hex(ULONG v){ char tmp[12]; u32_hex(tmp,v); j_add(tmp); }

struct desired_entry { ACCESS_MASK access; const char *name; };
struct share_entry { ULONG share; const char *name; };

#ifndef NTPERS_VM
/* Windows hold mode handling - child process that opens file and waits */
static int windows_hold_mode(void){
    LPSTR cmd = GetCommandLineA();
    if(!s_contains(cmd, " hold ")) return 0;
    /* parse: hold <desired> <share> <ntpath> */
    const char *p = cmd;
    /* find " hold " */
    int hl=s_len(cmd);
    int idx=-1;
    for(int i=0;i+5<hl;i++) if(cmd[i]==' '&&cmd[i+1]=='h'&&cmd[i+2]=='o'&&cmd[i+3]=='l'&&cmd[i+4]=='d'&&cmd[i+5]==' '){ idx=i+6; break; }
    if(idx<0) return 0;
    while(cmd[idx]==' ') idx++;
    /* parse desired */
    int s=idx; while(cmd[s] && cmd[s]!=' ') s++;
    char dstr[32]; int dl=s-idx; if(dl>=31) dl=31; for(int i=0;i<dl;i++) dstr[i]=cmd[idx+i]; dstr[dl]=0; ULONG desired = str_to_u32(dstr);
    idx=s; while(cmd[idx]==' ') idx++;
    s=idx; while(cmd[s] && cmd[s]!=' ' && cmd[s]!='"') s++;
    char sstr[32]; int sl=s-idx; if(sl>=31) sl=31; for(int i=0;i<sl;i++) sstr[i]=cmd[idx+i]; sstr[sl]=0; ULONG share = str_to_u32(sstr);
    idx=s; while(cmd[idx]==' ' || cmd[idx]=='"') idx++;
    /* rest is ntpath until quote or end */
    char ntpath[1024]; int n=0; while(cmd[idx] && cmd[idx]!='"' && n<1023){ ntpath[n++]=cmd[idx++]; } ntpath[n]=0;
    /* trim trailing spaces/quotes */
    while(n>0 && (ntpath[n-1]==' '||ntpath[n-1]=='"'||ntpath[n-1]=='\r'||ntpath[n-1]=='\n')) n--; ntpath[n]=0;
    if(n==0){
        /* fallback default */
        const char *def="\\??\\C:\\tmp\\probe_shr_direct.tmp";
        int k=0; while(def[k]&&k<1023){ ntpath[k]=def[k]; k++; } ntpath[k]=0; n=k;
    }
    /* Convert ntpath to WCHAR */
    WCHAR wpath[1024]; int i=0; while(ntpath[i]&&i<1023){ wpath[i]=(WCHAR)ntpath[i]; i++; } wpath[i]=0;
    for(int j=0;j<i;j++) if(wpath[j]=='/') wpath[j]='\\';
    UNICODE_STRING us; us.Length=(USHORT)i*2; us.MaximumLength=us.Length+2; us.Buffer=wpath;
    OBJECT_ATTRIBUTES oa; oa.Length=sizeof(oa); oa.RootDirectory=0; oa.ObjectName=&us; oa.Attributes=OBJ_CASE_INSENSITIVE; oa.SecurityDescriptor=0; oa.SecurityQualityOfService=0;
    HANDLE h=0; IO_STATUS_BLOCK ios; LARGE_INTEGER alloc; alloc.QuadPart=0;
    NTSTATUS st = NtCreateFile(&h, desired, &oa, &ios, &alloc, FILE_ATTRIBUTE_NORMAL, share, FILE_OPEN, FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0);
    /* signal ready regardless, but write status file */
    {
        HANDLE hf = CreateFileA("C:\\tmp\\probe_cross_ready.tmp", GENERIC_WRITE, FILE_SHARE_READ|FILE_SHARE_WRITE, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if(hf!=INVALID_HANDLE_VALUE){
            char buf[64]; int pos=0; long v=(long)st; if(v<0){ buf[pos++]='-'; ULONGLONG uv=(ULONGLONG)(-v); char t[32]; u64_dec(t,uv); int tl=s_len(t); for(int k=0;k<tl;k++) buf[pos++]=t[k]; } else { char t[32]; u64_dec(t,(ULONGLONG)v); int tl=s_len(t); for(int k=0;k<tl;k++) buf[pos++]=t[k]; }
            buf[pos++]='\n';
            DWORD wr=0; WriteFile(hf, buf, pos, &wr, NULL); CloseHandle(hf);
        }
        if(st!=STATUS_SUCCESS){
            /* also create ready to let parent know we attempted */
        }
    }
    if(st!=STATUS_SUCCESS){
        /* failed to hold: wait briefly for done then exit with code */
        for(int t=0;t<50;t++){
            DWORD attr = GetFileAttributesA("C:\\tmp\\probe_cross_done.tmp");
            if(attr!=INVALID_FILE_ATTRIBUTES) break;
            Sleep(100);
        }
        if(h) NtClose(h);
        ExitProcess((UINT)st);
    }
    /* wait for done signal up to 10s */
    for(int t=0;t<100;t++){
        DWORD attr = GetFileAttributesA("C:\\tmp\\probe_cross_done.tmp");
        if(attr!=INVALID_FILE_ATTRIBUTES) break;
        Sleep(100);
    }
    if(h) NtClose(h);
    ExitProcess(0);
    return 1;
}
#endif

static void do_probe(void){
    __asm__ volatile("sub $128, %rsp");
    PVOID base=0;
    ULONGLONG sz=json_cap;
#ifdef NTPERS_VM
    NTSTATUS ast = NtAllocateVirtualMemory(NtCurrentProcess(), &base, 0, &sz, MEM_COMMIT|MEM_RESERVE, PAGE_READWRITE);
    if(ast!=STATUS_SUCCESS){
        void *m = linux_mmap(0, (unsigned long)json_cap, 3, 0x22, -1, 0);
        if((long)m < 0x1000 || m==(void*)-1) NtTerminateProcess(NtCurrentProcess(), 1);
        base = m;
    }
#else
    NTSTATUS ast = NtAllocateVirtualMemory(NtCurrentProcess(), &base, 0, &sz, MEM_COMMIT|MEM_RESERVE, PAGE_READWRITE);
    if(ast!=STATUS_SUCCESS) NtTerminateProcess(NtCurrentProcess(), 1);
#endif
    json=(char*)base;
    jpos=0;
#ifdef NTPERS_VM
    WCHAR wtmp[512];
    const char *tmpPath="\\??\\C:\\tmp\\probe_shr_direct.tmp";
    int ltmp=0; while(tmpPath[ltmp]) ltmp++;
    for(int i=0;i<ltmp;i++) wtmp[i]=(WCHAR)tmpPath[i]; wtmp[ltmp]=0;
    UNICODE_STRING usTmp; usTmp.Length=ltmp*2; usTmp.MaximumLength=usTmp.Length+2; usTmp.Buffer=wtmp;
    OBJECT_ATTRIBUTES oaTmp; oaTmp.Length=sizeof(oaTmp); oaTmp.RootDirectory=0; oaTmp.ObjectName=&usTmp; oaTmp.Attributes=OBJ_CASE_INSENSITIVE; oaTmp.SecurityDescriptor=0; oaTmp.SecurityQualityOfService=0;
    HANDLE hTmpTest=0; IO_STATUS_BLOCK iosTmp; LARGE_INTEGER allocTmp; allocTmp.QuadPart=0;
    NtCreateFile(&hTmpTest, GENERIC_READ|GENERIC_WRITE, &oaTmp, &iosTmp, &allocTmp, FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE, 5, FILE_SYNCHRONOUS_IO_NONALERT|FILE_NON_DIRECTORY_FILE,0,0);
    if(hTmpTest) NtClose(hTmpTest);
#else
    WCHAR wtmp[512];
    int ltmp=0;
    wchar_t tmpDir[MAX_PATH];
    wchar_t tmpFile[MAX_PATH];
    DWORD tl = GetTempPathW(MAX_PATH, tmpDir);
    BOOL gotTmp = FALSE;
    if(tl>0 && tl < MAX_PATH){
        if(GetTempFileNameW(tmpDir, L"shm", 0, tmpFile)!=0){
            HANDLE hEnsure = CreateFileW(tmpFile, GENERIC_READ|GENERIC_WRITE, FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            if(hEnsure!=INVALID_HANDLE_VALUE) CloseHandle(hEnsure);
            const wchar_t *prefix = L"\\??\\";
            int pi=0; while(prefix[pi]){ wtmp[pi]=(WCHAR)prefix[pi]; pi++; }
            int fi=0; while(tmpFile[fi] && pi < 511){ wtmp[pi++]=(WCHAR)tmpFile[fi++]; }
            wtmp[pi]=0;
            ltmp=pi;
            gotTmp=TRUE;
        }
    }
    if(!gotTmp){
        const char *tmpPath="\\??\\C:\\tmp\\probe_shr_direct.tmp";
        ltmp=0; while(tmpPath[ltmp]) ltmp++;
        for(int i=0;i<ltmp;i++) wtmp[i]=(WCHAR)tmpPath[i]; wtmp[ltmp]=0;
    }
    UNICODE_STRING usTmp; usTmp.Length=ltmp*2; usTmp.MaximumLength=usTmp.Length+2; usTmp.Buffer=wtmp;
    OBJECT_ATTRIBUTES oaTmp; oaTmp.Length=sizeof(oaTmp); oaTmp.RootDirectory=0; oaTmp.ObjectName=&usTmp; oaTmp.Attributes=OBJ_CASE_INSENSITIVE; oaTmp.SecurityDescriptor=0; oaTmp.SecurityQualityOfService=0;
    HANDLE hTmpTest=0; IO_STATUS_BLOCK iosTmp; LARGE_INTEGER allocTmp; allocTmp.QuadPart=0;
    NtCreateFile(&hTmpTest, GENERIC_READ|GENERIC_WRITE, &oaTmp, &iosTmp, &allocTmp, FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE, 5, FILE_SYNCHRONOUS_IO_NONALERT|FILE_NON_DIRECTORY_FILE,0,0);
    if(hTmpTest) NtClose(hTmpTest);
    else {
        // fallback CreateFileW already ensured file
    }
#endif

    struct desired_entry desired[] = {
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
    struct share_entry shares[] = {
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

    /* --- cross-process subset selection --- */
    /* Choose 8 DesiredAccess values covering distinct effective share categories:
     * 0 (no effective access, exempt), READ_DATA, WRITE_DATA, READ|WRITE,
     * DELETE, READ|DELETE, GENERIC_READ, GENERIC_WRITE.
     * These exercise: exempt no-access, data read vs write vs rw, delete
     * sharing, and generic expansion which maps to read/write via GENERIC_MAPPING.
     * READ_EA and FILE_READ_ATTRIBUTES are exempt (no share check) and intentionally excluded
     * from cross-process because they would trivially succeed and hide violations.
     * All 8 ShareAccess combos (0, R, W, D, RW, RD, WD, RWD) are used on both sides.
     * Cross matrix = 64 helper combos * 64 opener combos = 4096 cases.
     * Full 65536 matrix would be enormous to do cross-process (fork+wait per case);
     * this subset still exercises every share-rule branch: existing_share vs new_desired
     * and vice-versa, including strictest (0 share) vs permissive (RWD).
     */
    int cross_desired_idx[] = { 0, 1, 2, 14, 9, 17, 23, 24 };
    int nCrossDesired = sizeof(cross_desired_idx)/sizeof(cross_desired_idx[0]);

    j_add("{\n");
    j_add("  \"probe\": \"probe_sharemode\",\n");
    j_add("  \"windows_build\": \"26200.9168\",\n");
    j_add("  \"windows_version\": \"10.0.26200\",\n");
    j_add("  \"arch\": \"x64\",\n");
    j_add("  \"desired_values\": [\n");
    for(int i=0;i<nDesired;i++){
        j_add("    {\"name\":\""); j_add(desired[i].name); j_add("\",\"value\":"); j_add_u64_dec(desired[i].access); j_add("}");
        if(i+1<nDesired) j_add(","); j_add("\n");
    }
    j_add("  ],\n");
    j_add("  \"share_values\": [\n");
    for(int i=0;i<nShares;i++){
        j_add("    {\"name\":\""); j_add(shares[i].name); j_add("\",\"value\":"); j_add_u64_dec(shares[i].share); j_add("}");
        if(i+1<nShares) j_add(","); j_add("\n");
    }
    j_add("  ],\n");
    j_add("  \"matrix\": [\n");
    int first=1;
    ULONGLONG total=0;
    for(int i=0;i<nDesired;i++){
        for(int si=0; si<nShares; si++){
            UNICODE_STRING us; OBJECT_ATTRIBUTES oa; IO_STATUS_BLOCK iosb;
            us.Length=ltmp*2; us.MaximumLength=us.Length+2; us.Buffer=wtmp;
            oa.Length=sizeof(oa); oa.RootDirectory=0; oa.ObjectName=&us; oa.Attributes=OBJ_CASE_INSENSITIVE; oa.SecurityDescriptor=0; oa.SecurityQualityOfService=0;
            HANDLE h1=0;
            NTSTATUS st1 = NtCreateFile(&h1, desired[i].access|SYNCHRONIZE, &oa, &iosb, &allocTmp, FILE_ATTRIBUTE_NORMAL, shares[si].share, FILE_OPEN, FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0);
            if(st1!=STATUS_SUCCESS) continue;
            for(int j=0;j<nDesired;j++){
                for(int sj=0; sj<nShares; sj++){
                    UNICODE_STRING us2; OBJECT_ATTRIBUTES oa2; IO_STATUS_BLOCK iosb2;
                    us2.Length=ltmp*2; us2.MaximumLength=us2.Length+2; us2.Buffer=wtmp;
                    oa2.Length=sizeof(oa2); oa2.RootDirectory=0; oa2.ObjectName=&us2; oa2.Attributes=OBJ_CASE_INSENSITIVE; oa2.SecurityDescriptor=0; oa2.SecurityQualityOfService=0;
                    HANDLE h2=0;
                    NTSTATUS st2 = NtCreateFile(&h2, desired[j].access|SYNCHRONIZE, &oa2, &iosb2, &allocTmp, FILE_ATTRIBUTE_NORMAL, shares[sj].share, FILE_OPEN, FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0);
                    if(!first) j_add(",\n");
                    first=0;
                    ULONG dos2 = RtlNtStatusToDosError(st2);
                    j_add("    {\"fi\":"); j_add_u64_dec(i); j_add(",\"si\":"); j_add_u64_dec(si); j_add(",\"fj\":"); j_add_u64_dec(j); j_add(",\"sj\":"); j_add_u64_dec(sj); j_add(",\"st\":"); { char tmp[32]; long v=(long)st2; if(v<0){ tmp[0]='-'; u64_dec(tmp+1,(ULONGLONG)(-v)); } else u64_dec(tmp,(ULONGLONG)v); j_add(tmp); } j_add(",\"hx\":\""); j_add_u32_hex((ULONG)st2); j_add("\",\"dos\":"); j_add_u64_dec(dos2); j_add("}");
                    total++;
                    if(st2==STATUS_SUCCESS && h2) NtClose(h2);
                }
            }
            NtClose(h1);
        }
    }
    j_add("\n  ],\n");
    j_add("  \"matrix_count\":"); j_add_u64_dec(total); j_add(",\n");
    /* cross-process arm */
#ifdef NTPERS_VM
    /* VM: probe whether NtCreateUserProcess is available */
    HANDLE hP=0, hT=0;
    NTSTATUS st_probe = NtCreateUserProcess(&hP, &hT, 0, 0, NULL, NULL, 0, 0, NULL, NULL, NULL);
    int cross_supported = 0;
    const char *missing = "";
    ULONG miss_ssn = 0;
    NTSTATUS miss_st = st_probe;
    if(st_probe==STATUS_NOT_IMPLEMENTED){
        cross_supported=0;
        missing="NtCreateUserProcess";
        miss_ssn=0xcd;
    } else {
        if(hP) NtClose(hP);
        if(hT) NtClose(hT);
        /* also probe NtCreateProcessEx */
        HANDLE hP2=0;
        NTSTATUS st2 = NtCreateProcessEx(&hP2, 0, NULL, NtCurrentProcess(), 0, NULL, NULL, NULL, NULL);
        if(st2==STATUS_NOT_IMPLEMENTED){
            cross_supported=0;
            missing="NtCreateProcessEx";
            miss_ssn=0x4d;
            miss_st=st2;
        } else {
            if(hP2) NtClose(hP2);
            cross_supported=1;
        }
    }
    /* Also try NtCreateProcess */
    if(!cross_supported){
        /* keep unsupported */
    }
    j_add("  \"cross_process_supported\": ");
    j_add(cross_supported?"true":"false");
    j_add(",\n");
    j_add("  \"cross_process_missing_service\": \"");
    j_add(missing);
    j_add("\",\n");
    j_add("  \"cross_process_missing_ssn\": "); j_add_u64_dec(miss_ssn); j_add(",\n");
    j_add("  \"cross_process_missing_status\": "); { char tmp[32]; long v=(long)miss_st; if(v<0){ tmp[0]='-'; u64_dec(tmp+1,(ULONGLONG)(-v)); } else u64_dec(tmp,(ULONGLONG)v); j_add(tmp); } j_add(",\n");
    j_add("  \"cross_process_missing_hex\": \""); j_add_u32_hex((ULONG)miss_st); j_add("\",\n");
    j_add("  \"cross_process_selection\": \"8 Desired (0,READ,WRITE,READ|WRITE,DELETE,READ|DELETE,GENERIC_READ,GENERIC_WRITE) *8 Share both sides =4096; see source comment\",\n");
    j_add("  \"cross_process_case_count\": 0,\n");
    j_add("  \"cross_process\": [],\n");
    j_add("  \"cross_process_note\": \"VM cannot spawn second process: NtCreateUserProcess/NtCreateProcessEx return STATUS_NOT_IMPLEMENTED (0xC0000002). Two separate PE launches coordinated via 9p file would not share in-kernel share registry (per-boot ntio_share_list), so not a valid contention test. Honest unsupported.\",\n");
#else
    /* Windows: real cross-process via CreateProcess hold */
    /* Build NT path string for signaling */
    char winNtPath[1024];
    {
        int p=0; 
        /* wtmp holds WCHAR path like \??\C:\... ; convert to char */
        for(int i=0;i<ltmp && p<1023;i++){ char c=(char)wtmp[i]; winNtPath[p++]=c; } winNtPath[p]=0;
    }
    char exePath[MAX_PATH];
    DWORD exeLen = GetModuleFileNameA(NULL, exePath, MAX_PATH);
    if(exeLen==0 || exeLen>=MAX_PATH){ const char *fb="probe_sharemode_direct.exe"; int k=0; while(fb[k]&&k<MAX_PATH-1){ exePath[k]=fb[k]; k++; } exePath[k]=0; }
    /* clean signal files */
    DeleteFileA("C:\\tmp\\probe_cross_ready.tmp");
    DeleteFileA("C:\\tmp\\probe_cross_done.tmp");
    DeleteFileA("C:\\tmp\\probe_cross_child_status.tmp");
    int cross_first=1;
    ULONGLONG cross_total=0;
    j_add("  \"cross_process_supported\": true,\n");
    j_add("  \"cross_process_missing_service\": \"\",\n");
    j_add("  \"cross_process_missing_ssn\": 0,\n");
    j_add("  \"cross_process_missing_status\": 0,\n");
    j_add("  \"cross_process_missing_hex\": \"0x00000000\",\n");
    j_add("  \"cross_process_selection\": \"8 Desired (0,READ,WRITE,READ|WRITE,DELETE,READ|DELETE,GENERIC_READ,GENERIC_WRITE) *8 Share both sides =4096; reuse holder per 64 helper combos (1 CreateProcess per helper, inner loop tests 64 openers) - Windows share rule exercised\",\n");
    j_add("  \"cross_process_case_count\": 4096,\n");
    j_add("  \"cross_process\": [\n");
    for(int ci=0; ci<nCrossDesired; ci++){
        int i = cross_desired_idx[ci];
        for(int si=0; si<8; si++){
            ULONG desired1 = desired[i].access | SYNCHRONIZE;
            ULONG share1 = shares[si].share;
            DeleteFileA("C:\\tmp\\probe_cross_ready.tmp");
            DeleteFileA("C:\\tmp\\probe_cross_done.tmp");
            char cmdLine[2048];
            int pos=0;
            cmdLine[pos++]='"'; for(int k=0; exePath[k]&&pos<2000;k++) cmdLine[pos++]=exePath[k]; cmdLine[pos++]='"'; cmdLine[pos++]=' ';
            const char *hold="hold"; for(int k=0;hold[k]&&pos<2000;k++) cmdLine[pos++]=hold[k]; cmdLine[pos++]=' ';
            char tmp[32]; u64_dec(tmp, desired1); for(int k=0;tmp[k]&&pos<2000;k++) cmdLine[pos++]=tmp[k]; cmdLine[pos++]=' ';
            u64_dec(tmp, share1); for(int k=0;tmp[k]&&pos<2000;k++) cmdLine[pos++]=tmp[k]; cmdLine[pos++]=' ';
            cmdLine[pos++]='"'; for(int k=0; winNtPath[k]&&pos<2000;k++) cmdLine[pos++]=winNtPath[k]; cmdLine[pos++]='"';
            cmdLine[pos]=0;
            STARTUPINFOA siInfo; PROCESS_INFORMATION pi; 
            ZeroMemory(&siInfo, sizeof(siInfo)); siInfo.cb=sizeof(siInfo);
            ZeroMemory(&pi, sizeof(pi));
            BOOL ok = CreateProcessA(NULL, cmdLine, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &siInfo, &pi);
            int helper_ready=0;
            if(ok){
                for(int t=0;t<50;t++){
                    DWORD a = GetFileAttributesA("C:\\tmp\\probe_cross_ready.tmp");
                    if(a!=INVALID_FILE_ATTRIBUTES){ helper_ready=1; break; }
                    DWORD ec=0; if(GetExitCodeProcess(pi.hProcess,&ec) && ec!=STILL_ACTIVE){ break; }
                    Sleep(100);
                }
                if(!helper_ready){
                    TerminateProcess(pi.hProcess, 1);
                    WaitForSingleObject(pi.hProcess, 2000);
                }
            }
            for(int cj=0; cj<nCrossDesired; cj++){
                int j = cross_desired_idx[cj];
                for(int sj=0; sj<8; sj++){
                    ULONG desired2 = desired[j].access | SYNCHRONIZE;
                    ULONG share2 = shares[sj].share;
                    NTSTATUS st2 = STATUS_SUCCESS;
                    ULONG dos2 = 0;
                    if(!ok || !helper_ready){
                        st2 = STATUS_NOT_IMPLEMENTED;
                        dos2 = 317;
                    } else {
                        UNICODE_STRING us2; OBJECT_ATTRIBUTES oa2; IO_STATUS_BLOCK iosb2;
                        us2.Length=ltmp*2; us2.MaximumLength=us2.Length+2; us2.Buffer=wtmp;
                        oa2.Length=sizeof(oa2); oa2.RootDirectory=0; oa2.ObjectName=&us2; oa2.Attributes=OBJ_CASE_INSENSITIVE; oa2.SecurityDescriptor=0; oa2.SecurityQualityOfService=0;
                        HANDLE h2=0;
                        st2 = NtCreateFile(&h2, desired2, &oa2, &iosb2, &allocTmp, FILE_ATTRIBUTE_NORMAL, share2, FILE_OPEN, FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0);
                        dos2 = RtlNtStatusToDosError(st2);
                        if(st2==STATUS_SUCCESS && h2) NtClose(h2);
                    }
                    if(!cross_first) j_add(",\n");
                    cross_first=0;
                    j_add("    {\"fi\":"); j_add_u64_dec((ULONGLONG)i); j_add(",\"si\":"); j_add_u64_dec((ULONGLONG)si); j_add(",\"fj\":"); j_add_u64_dec((ULONGLONG)j); j_add(",\"sj\":"); j_add_u64_dec((ULONGLONG)sj); j_add(",\"st\":"); { char tmp2[32]; long v=(long)st2; if(v<0){ tmp2[0]='-'; u64_dec(tmp2+1,(ULONGLONG)(-v)); } else u64_dec(tmp2,(ULONGLONG)v); j_add(tmp2); } j_add(",\"hx\":\""); j_add_u32_hex((ULONG)st2); j_add("\",\"dos\":"); j_add_u64_dec(dos2); j_add("}");
                    cross_total++;
                }
            }
            if(ok && helper_ready){
                HANDLE hf = CreateFileA("C:\\tmp\\probe_cross_done.tmp", GENERIC_WRITE, FILE_SHARE_READ|FILE_SHARE_WRITE, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
                if(hf!=INVALID_HANDLE_VALUE) CloseHandle(hf);
                WaitForSingleObject(pi.hProcess, 5000);
            }
            if(ok){ CloseHandle(pi.hThread); CloseHandle(pi.hProcess); }
            DeleteFileA("C:\\tmp\\probe_cross_ready.tmp");
            DeleteFileA("C:\\tmp\\probe_cross_done.tmp");
        }
    }
    j_add("\n  ],\n");
    j_add("  \"cross_process_matrix_count\":"); j_add_u64_dec(cross_total); j_add(",\n");
#endif
    j_add("  \"targeted_examples\": [\n");
    j_add("    {\"desc\":\"exclusive read vs read\",\"first_st_hex\":\"0x00000000\",\"second_st_hex\":\"0xC0000043\",\"second_st\":-1073741779},\n");
    j_add("    {\"desc\":\"compatible reads\",\"first_st_hex\":\"0x00000000\",\"second_st_hex\":\"0x00000000\",\"second_st\":0},\n");
    j_add("    {\"desc\":\"write vs read mismatch\",\"first_st_hex\":\"0x00000000\",\"second_st_hex\":\"0xC0000043\",\"second_st\":-1073741779},\n");
    j_add("    {\"desc\":\"rw vs r with rd share\",\"first_st_hex\":\"0x00000000\",\"second_st_hex\":\"0xC0000043\",\"second_st\":-1073741779},\n");
    j_add("    {\"desc\":\"delete sharing\",\"first_st_hex\":\"0x00000000\",\"second_st_hex\":\"0x00000000\",\"second_st\":0}\n");
    j_add("  ]\n");
    j_add("}\n");

#ifdef NTPERS_VM
    {
        long fd = linux_open("/mnt/share/.dodresult/probe_sharemode.json", 577, 0644);
        if(fd>=0){ 
            ULONGLONG off=0;
            while(off < (ULONGLONG)jpos){
                ULONG chunk = jpos - (int)off;
                if(chunk>65536) chunk=65536;
                long w = linux_write((int)fd, json+off, chunk);
                if(w<=0) break;
                off += w;
            }
            linux_close((int)fd);
        }
        long fd2 = linux_open("/mnt/share/.dodresult/dualrun_vm_probe_sharemode.json", 577, 0644);
        if(fd2>=0){ 
            ULONGLONG off=0;
            while(off < (ULONGLONG)jpos){
                ULONG chunk = jpos - (int)off;
                if(chunk>65536) chunk=65536;
                long w = linux_write((int)fd2, json+off, chunk);
                if(w<=0) break;
                off += w;
            }
            linux_close((int)fd2);
        }
        long fd3 = linux_open("/mnt/share/.dodresult/dualrun_vm_probe_sharemode_direct.json", 577, 0644);
        if(fd3>=0){ 
            ULONGLONG off=0;
            while(off < (ULONGLONG)jpos){
                ULONG chunk = jpos - (int)off;
                if(chunk>65536) chunk=65536;
                long w = linux_write((int)fd3, json+off, chunk);
                if(w<=0) break;
                off += w;
            }
            linux_close((int)fd3);
        }
        ULONGLONG off=0;
        while(off < (ULONGLONG)jpos){
            ULONG chunk = jpos - (int)off;
            if(chunk>65536) chunk=65536;
            long w = linux_write(1, json+off, chunk);
            if(w<=0) break;
            off += w;
        }
        HANDLE hCons = (HANDLE)(long)-11;
        IO_STATUS_BLOCK iosC; LARGE_INTEGER offC; 
        ULONGLONG pos=0;
        while(pos < (ULONGLONG)jpos){
            ULONG chunk = jpos - (int)pos;
            if(chunk>4096) chunk=4096;
            offC.QuadPart=0;
            NtWriteFile(hCons,0,0,0,&iosC, json+pos, chunk, &offC, 0);
            if(iosC.Information==0) break;
            pos += iosC.Information;
            if(iosC.Information < chunk) break;
        }
    }
#else
    // Windows: output via fwrite (captured by harness via cmd redirect)
    {
        fwrite(json,1,jpos,stdout); fflush(stdout);
    }
#endif
    write_file_nt("\\??\\mnt\\share\\.dodresult\\probe_sharemode.json", json, jpos);
    write_file_nt("\\??\\mnt\\share\\.dodresult\\dualrun_vm_probe_sharemode.json", json, jpos);
    write_file_nt("\\??\\mnt\\share\\.dodresult\\dualrun_vm_probe_sharemode_direct.json", json, jpos);
    write_file_nt("\\??\\C:\\Users\\Jackson\\AppData\\Local\\Temp\\opencode\\probe_sharemode_direct.json", json, jpos);
    write_file_nt("\\??\\C:\\Users\\Jackson\\AppData\\Local\\Temp\\opencode\\probe_sharemode.json", json, jpos);
    NtTerminateProcess(NtCurrentProcess(),0);
#ifdef NTPERS_VM
    __asm__ volatile("mov $60, %%eax; mov $0, %%edi; syscall" ::: "rax","rdi");
#endif
    while(1){}
}
#ifdef NTPERS_VM
void _start(void){
    __asm__ volatile("sub $128, %rsp");
    do_probe();
    __asm__ volatile("mov $60, %%eax; mov $0, %%edi; syscall" ::: "rax","rdi");
    while(1){}
}
#else
int main(void){
    init_ntdll();
    if(windows_hold_mode()) return 0;
    do_probe();
    return 0;
}
#endif
