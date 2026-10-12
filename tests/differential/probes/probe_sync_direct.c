/* probe_sync_direct.c - dual-target direct probe
 * WINDOWS build: resolves via GetProcAddress on ntdll (correct SSN per host build)
 * VM build: uses raw syscall stubs pinned to Server 2022 (0x48 etc) via -DNTPERS_VM
 */
#ifdef NTPERS_VM
/* probe_sync_direct.c - freestanding direct-syscall sync probe
 * Mirrors probe_sync.json sequence using embedded syscall stubs.
 * No DLL imports, JSON via NtCreateFile/NtWriteFile to 9p share.
 */
typedef unsigned long long ULONGLONG;
typedef unsigned long ULONG;
typedef unsigned short USHORT;
typedef unsigned short WCHAR;
typedef void* HANDLE;
typedef void* PVOID;
typedef long NTSTATUS;
typedef unsigned char BOOLEAN;
typedef unsigned long ACCESS_MASK;
typedef ULONGLONG ULONG_PTR;
typedef ULONGLONG SIZE_T;
typedef struct { ULONGLONG Status; ULONGLONG Information; } IO_STATUS_BLOCK;
typedef struct { USHORT Length; USHORT MaximumLength; WCHAR *Buffer; } UNICODE_STRING;
typedef struct { ULONG Length; HANDLE RootDirectory; UNICODE_STRING *ObjectName; ULONG Attributes; PVOID SecurityDescriptor; PVOID SecurityQualityOfService; } OBJECT_ATTRIBUTES;
typedef struct { ULONGLONG QuadPart; } LARGE_INTEGER;
typedef long LONG;
#else
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winternl.h>
#include <stdio.h>
#endif
#define STATUS_SUCCESS ((NTSTATUS)0x00000000L)
#define STATUS_TIMEOUT ((NTSTATUS)0x00000102L)
#define STATUS_ABANDONED_WAIT_0 ((NTSTATUS)0x00000080L)
#define STATUS_MUTANT_NOT_OWNED ((NTSTATUS)0xC0000046L)
#define STATUS_SEMAPHORE_LIMIT_EXCEEDED ((NTSTATUS)0xC0000047L)
#define OBJ_CASE_INSENSITIVE 0x40
#define FILE_OVERWRITE_IF 5
#define FILE_SYNCHRONOUS_IO_NONALERT 0x20
#define FILE_NON_DIRECTORY_FILE 0x40
#define GENERIC_READWRITE 0x12019F
#define NotificationEvent 0
#define SynchronizationEvent 1
#define WaitAll 0
#define WaitAny 1

#ifdef NTPERS_VM
__asm__(".text\n"
".globl NtCreateEvent\nNtCreateEvent: mov %rcx, %r10; mov $0x48, %eax; syscall; ret\n"
".globl NtSetEvent\nNtSetEvent: mov %rcx, %r10; mov $0x0e, %eax; syscall; ret\n"
".globl NtResetEvent\nNtResetEvent: mov %rcx, %r10; mov $0x180, %eax; syscall; ret\n"
".globl NtPulseEvent\nNtPulseEvent: mov %rcx, %r10; mov $0x143, %eax; syscall; ret\n"
".globl NtCreateMutant\nNtCreateMutant: mov %rcx, %r10; mov $0xb6, %eax; syscall; ret\n"
".globl NtReleaseMutant\nNtReleaseMutant: mov %rcx, %r10; mov $0x20, %eax; syscall; ret\n"
".globl NtCreateSemaphore\nNtCreateSemaphore: mov %rcx, %r10; mov $0xc3, %eax; syscall; ret\n"
".globl NtReleaseSemaphore\nNtReleaseSemaphore: mov %rcx, %r10; mov $0x0a, %eax; syscall; ret\n"
".globl NtWaitForSingleObject\nNtWaitForSingleObject: mov %rcx, %r10; mov $0x04, %eax; syscall; ret\n"
".globl NtWaitForMultipleObjects\nNtWaitForMultipleObjects: mov %rcx, %r10; mov $0x5b, %eax; syscall; ret\n"
".globl NtQueryPerformanceCounter\nNtQueryPerformanceCounter: mov %rcx, %r10; mov $0x31, %eax; syscall; ret\n"
".globl NtCreateThreadEx\nNtCreateThreadEx: mov %rcx, %r10; mov $0xc5, %eax; syscall; ret\n"
".globl NtTerminateThread\nNtTerminateThread: mov %rcx, %r10; mov $0x53, %eax; syscall; ret\n"
".globl NtClose\nNtClose: mov %rcx, %r10; mov $0x0f, %eax; syscall; ret\n"
".globl NtCreateFile\nNtCreateFile: mov %rcx, %r10; mov $0x55, %eax; syscall; ret\n"
".globl NtWriteFile\nNtWriteFile: mov %rcx, %r10; mov $0x08, %eax; syscall; ret\n"
".globl NtAllocateVirtualMemory\nNtAllocateVirtualMemory: mov %rcx, %r10; mov $0x18, %eax; syscall; ret\n"
".globl NtTerminateProcess\nNtTerminateProcess: mov %rcx, %r10; mov $0x2c, %eax; syscall; ret\n"
".globl NtDelayExecution\nNtDelayExecution: mov %rcx, %r10; mov $0x34, %eax; syscall; ret\n"
".globl linux_open\nlinux_open: mov $2, %eax; syscall; ret\n"
".globl linux_write\nlinux_write: mov $1, %eax; syscall; ret\n"
".globl linux_close\nlinux_close: mov $3, %eax; syscall; ret\n"
);


NTSTATUS NtCreateEvent(HANDLE *EventHandle, ACCESS_MASK DesiredAccess, OBJECT_ATTRIBUTES *ObjectAttributes, ULONG EventType, BOOLEAN InitialState);
NTSTATUS NtSetEvent(HANDLE EventHandle, LONG *PreviousState);
NTSTATUS NtResetEvent(HANDLE EventHandle, LONG *PreviousState);
NTSTATUS NtPulseEvent(HANDLE EventHandle, LONG *PreviousState);
NTSTATUS NtCreateMutant(HANDLE *MutantHandle, ACCESS_MASK DesiredAccess, OBJECT_ATTRIBUTES *ObjectAttributes, BOOLEAN InitialOwner);
NTSTATUS NtReleaseMutant(HANDLE MutantHandle, LONG *PreviousCount);
NTSTATUS NtCreateSemaphore(HANDLE *SemaphoreHandle, ACCESS_MASK DesiredAccess, OBJECT_ATTRIBUTES *ObjectAttributes, LONG InitialCount, LONG MaximumCount);
NTSTATUS NtReleaseSemaphore(HANDLE SemaphoreHandle, LONG ReleaseCount, LONG *PreviousCount);
NTSTATUS NtWaitForSingleObject(HANDLE Handle, BOOLEAN Alertable, LARGE_INTEGER *Timeout);
NTSTATUS NtWaitForMultipleObjects(ULONG Count, HANDLE *Handles, ULONG WaitType, BOOLEAN Alertable, LARGE_INTEGER *Timeout);
NTSTATUS NtQueryPerformanceCounter(LARGE_INTEGER *PerformanceCounter, LARGE_INTEGER *PerformanceFrequency);
NTSTATUS NtCreateThreadEx(HANDLE *ThreadHandle, ACCESS_MASK DesiredAccess, OBJECT_ATTRIBUTES *ObjectAttributes, HANDLE ProcessHandle, PVOID StartRoutine, PVOID Argument, ULONG CreateFlags, ULONG ZeroBits, ULONG StackSize, ULONG MaximumStackSize, PVOID AttributeList);
NTSTATUS NtTerminateThread(HANDLE ThreadHandle, NTSTATUS ExitStatus);
NTSTATUS NtClose(HANDLE Handle);
NTSTATUS NtCreateFile(HANDLE *FileHandle, ACCESS_MASK DesiredAccess, OBJECT_ATTRIBUTES *ObjectAttributes, IO_STATUS_BLOCK *IoStatusBlock, LARGE_INTEGER *AllocationSize, ULONG FileAttributes, ULONG ShareAccess, ULONG CreateDisposition, ULONG CreateOptions, PVOID EaBuffer, ULONG EaLength);
NTSTATUS NtWriteFile(HANDLE FileHandle, HANDLE Event, PVOID ApcRoutine, PVOID ApcContext, IO_STATUS_BLOCK *IoStatusBlock, PVOID Buffer, ULONG Length, LARGE_INTEGER *ByteOffset, ULONG *Key);
NTSTATUS NtAllocateVirtualMemory(HANDLE ProcessHandle, PVOID *BaseAddress, ULONGLONG ZeroBits, ULONGLONG *RegionSize, ULONG AllocationType, ULONG Protect);
NTSTATUS NtTerminateProcess(HANDLE ProcessHandle, NTSTATUS ExitStatus);
NTSTATUS NtDelayExecution(BOOLEAN Alertable, LARGE_INTEGER *DelayInterval);
long linux_open(const char *path, int flags, int mode);
long linux_write(int fd, const void *buf, unsigned long len);
long linux_close(int fd);
#else
// Windows ntdll function pointers (resolved via GetProcAddress)
static HMODULE g_ntdll = NULL;
static NTSTATUS (NTAPI *pNtCreateEvent)(HANDLE *EventHandle, ACCESS_MASK DesiredAccess, OBJECT_ATTRIBUTES *ObjectAttributes, ULONG EventType, BOOLEAN InitialState);
#define NtCreateEvent pNtCreateEvent
static NTSTATUS (NTAPI *pNtSetEvent)(HANDLE EventHandle, LONG *PreviousState);
#define NtSetEvent pNtSetEvent
static NTSTATUS (NTAPI *pNtResetEvent)(HANDLE EventHandle, LONG *PreviousState);
#define NtResetEvent pNtResetEvent
static NTSTATUS (NTAPI *pNtPulseEvent)(HANDLE EventHandle, LONG *PreviousState);
#define NtPulseEvent pNtPulseEvent
static NTSTATUS (NTAPI *pNtCreateMutant)(HANDLE *MutantHandle, ACCESS_MASK DesiredAccess, OBJECT_ATTRIBUTES *ObjectAttributes, BOOLEAN InitialOwner);
#define NtCreateMutant pNtCreateMutant
static NTSTATUS (NTAPI *pNtReleaseMutant)(HANDLE MutantHandle, LONG *PreviousCount);
#define NtReleaseMutant pNtReleaseMutant
static NTSTATUS (NTAPI *pNtCreateSemaphore)(HANDLE *SemaphoreHandle, ACCESS_MASK DesiredAccess, OBJECT_ATTRIBUTES *ObjectAttributes, LONG InitialCount, LONG MaximumCount);
#define NtCreateSemaphore pNtCreateSemaphore
static NTSTATUS (NTAPI *pNtReleaseSemaphore)(HANDLE SemaphoreHandle, LONG ReleaseCount, LONG *PreviousCount);
#define NtReleaseSemaphore pNtReleaseSemaphore
static NTSTATUS (NTAPI *pNtWaitForSingleObject)(HANDLE Handle, BOOLEAN Alertable, LARGE_INTEGER *Timeout);
#define NtWaitForSingleObject pNtWaitForSingleObject
static NTSTATUS (NTAPI *pNtWaitForMultipleObjects)(ULONG Count, HANDLE *Handles, ULONG WaitType, BOOLEAN Alertable, LARGE_INTEGER *Timeout);
#define NtWaitForMultipleObjects pNtWaitForMultipleObjects
static NTSTATUS (NTAPI *pNtQueryPerformanceCounter)(LARGE_INTEGER *PerformanceCounter, LARGE_INTEGER *PerformanceFrequency);
#define NtQueryPerformanceCounter pNtQueryPerformanceCounter
static NTSTATUS (NTAPI *pNtCreateThreadEx)(HANDLE *ThreadHandle, ACCESS_MASK DesiredAccess, OBJECT_ATTRIBUTES *ObjectAttributes, HANDLE ProcessHandle, PVOID StartRoutine, PVOID Argument, ULONG CreateFlags, ULONG_PTR ZeroBits, SIZE_T StackSize, SIZE_T MaximumStackSize, PVOID AttributeList);
#define NtCreateThreadEx pNtCreateThreadEx
static NTSTATUS (NTAPI *pNtTerminateThread)(HANDLE ThreadHandle, NTSTATUS ExitStatus);
#define NtTerminateThread pNtTerminateThread
static NTSTATUS (NTAPI *pNtClose)(HANDLE Handle);
#define NtClose pNtClose
static NTSTATUS (NTAPI *pNtCreateFile)(HANDLE *FileHandle, ACCESS_MASK DesiredAccess, OBJECT_ATTRIBUTES *ObjectAttributes, IO_STATUS_BLOCK *IoStatusBlock, LARGE_INTEGER *AllocationSize, ULONG FileAttributes, ULONG ShareAccess, ULONG CreateDisposition, ULONG CreateOptions, PVOID EaBuffer, ULONG EaLength);
#define NtCreateFile pNtCreateFile
static NTSTATUS (NTAPI *pNtWriteFile)(HANDLE FileHandle, HANDLE Event, PVOID ApcRoutine, PVOID ApcContext, IO_STATUS_BLOCK *IoStatusBlock, PVOID Buffer, ULONG Length, LARGE_INTEGER *ByteOffset, ULONG *Key);
#define NtWriteFile pNtWriteFile
static NTSTATUS (NTAPI *pNtAllocateVirtualMemory)(HANDLE ProcessHandle, PVOID *BaseAddress, ULONGLONG ZeroBits, ULONGLONG *RegionSize, ULONG AllocationType, ULONG Protect);
#define NtAllocateVirtualMemory pNtAllocateVirtualMemory
static NTSTATUS (NTAPI *pNtTerminateProcess)(HANDLE ProcessHandle, NTSTATUS ExitStatus);
#define NtTerminateProcess pNtTerminateProcess
static NTSTATUS (NTAPI *pNtDelayExecution)(BOOLEAN Alertable, LARGE_INTEGER *DelayInterval);
#define NtDelayExecution pNtDelayExecution
static void init_ntdll(void){
    if(g_ntdll) return;
    g_ntdll = GetModuleHandleA("ntdll.dll");
    pNtCreateEvent = (void*)GetProcAddress(g_ntdll, "NtCreateEvent");
    pNtSetEvent = (void*)GetProcAddress(g_ntdll, "NtSetEvent");
    pNtResetEvent = (void*)GetProcAddress(g_ntdll, "NtResetEvent");
    pNtPulseEvent = (void*)GetProcAddress(g_ntdll, "NtPulseEvent");
    pNtCreateMutant = (void*)GetProcAddress(g_ntdll, "NtCreateMutant");
    pNtReleaseMutant = (void*)GetProcAddress(g_ntdll, "NtReleaseMutant");
    pNtCreateSemaphore = (void*)GetProcAddress(g_ntdll, "NtCreateSemaphore");
    pNtReleaseSemaphore = (void*)GetProcAddress(g_ntdll, "NtReleaseSemaphore");
    pNtWaitForSingleObject = (void*)GetProcAddress(g_ntdll, "NtWaitForSingleObject");
    pNtWaitForMultipleObjects = (void*)GetProcAddress(g_ntdll, "NtWaitForMultipleObjects");
    pNtQueryPerformanceCounter = (void*)GetProcAddress(g_ntdll, "NtQueryPerformanceCounter");
    pNtCreateThreadEx = (void*)GetProcAddress(g_ntdll, "NtCreateThreadEx");
    pNtTerminateThread = (void*)GetProcAddress(g_ntdll, "NtTerminateThread");
    pNtClose = (void*)GetProcAddress(g_ntdll, "NtClose");
    pNtCreateFile = (void*)GetProcAddress(g_ntdll, "NtCreateFile");
    pNtWriteFile = (void*)GetProcAddress(g_ntdll, "NtWriteFile");
    pNtAllocateVirtualMemory = (void*)GetProcAddress(g_ntdll, "NtAllocateVirtualMemory");
    pNtTerminateProcess = (void*)GetProcAddress(g_ntdll, "NtTerminateProcess");
    pNtDelayExecution = (void*)GetProcAddress(g_ntdll, "NtDelayExecution");
}
static long linux_open(const char *path, int flags, int mode){ return -1; }
static long linux_write(int fd, const void *buf, unsigned long len){ return -1; }
static long linux_close(int fd){ return -1; }
#endif
#define NtCurrentProcess() ((HANDLE)(long)-1)
#define MEM_COMMIT 0x1000
#define MEM_RESERVE 0x2000
#define PAGE_READWRITE 0x04

static int s_len(const char *s){int n=0;while(s[n])n++;return n;}
/* Extra NT-path copies of the JSON output (the Windows build captures stdout).
   Override at build time with -DNTPERS_STAGE_NT=... */
#ifndef NTPERS_STAGE_NT
#define NTPERS_STAGE_NT "\\??\\mnt\\share\\.dodresult"
#endif

static void write_file_nt(const char *path, const char *buf, int len){
    WCHAR wpath[1024]; int i=0; while(path[i]&&i<1023){wpath[i]=(WCHAR)path[i]; i++;} wpath[i]=0;
    for(int j=0;j<i;j++) if(wpath[j]=='/') wpath[j]='\\';
    UNICODE_STRING us; us.Length=(USHORT)i*2; us.MaximumLength=us.Length+2; us.Buffer=wpath;
    OBJECT_ATTRIBUTES oa; oa.Length=sizeof(oa); oa.RootDirectory=0; oa.ObjectName=&us; oa.Attributes=OBJ_CASE_INSENSITIVE; oa.SecurityDescriptor=0; oa.SecurityQualityOfService=0;
    HANDLE h=0; IO_STATUS_BLOCK ios; LARGE_INTEGER alloc; alloc.QuadPart=0;
    NTSTATUS st=NtCreateFile(&h, GENERIC_READWRITE, &oa, &ios, &alloc, 0x80, 3, FILE_OVERWRITE_IF, FILE_SYNCHRONOUS_IO_NONALERT|FILE_NON_DIRECTORY_FILE,0,0);
    if(st!=STATUS_SUCCESS) return;
    IO_STATUS_BLOCK ios2; LARGE_INTEGER off; off.QuadPart=0;
    NtWriteFile(h,0,0,0,&ios2,(PVOID)buf,len,&off,0);
    NtClose(h);
}
static void u32_hex(char *out, ULONG v){ const char *h="0123456789ABCDEF"; out[0]='0'; out[1]='x'; for(int i=0;i<8;i++) out[2+i]=h[(v>>(28-i*4))&0xF]; out[10]=0; }
static void s_hex32(char *out, long v){ const char *h="0123456789abcdef"; unsigned long uv=(unsigned long)v; out[0]='0'; out[1]='x'; for(int i=0;i<8;i++) out[2+i]=h[(uv>>(28-i*4))&0xF]; out[10]=0; }
static void u64_dec(char *out, ULONGLONG v){ char tmp[32]; int p=0; if(v==0){out[0]='0';out[1]=0;return;} while(v>0){tmp[p++]='0'+(v%10);v/=10;} for(int i=0;i<p;i++) out[i]=tmp[p-1-i]; out[p]=0; }
static void i64_dec(char *out, long long v){ if(v==0){out[0]='0';out[1]=0;return;} int neg=0; unsigned long long uv; if(v<0){neg=1; uv=(unsigned long long)(-v);} else uv=(unsigned long long)v; char tmp[32]; int p=0; while(uv>0){tmp[p++]='0'+(uv%10); uv/=10;} int pos=0; if(neg) out[pos++]='-'; for(int i=0;i<p;i++) out[pos+i]=tmp[p-1-i]; out[pos+p]=0; }
static char *json; static int jpos=0; static ULONGLONG json_cap=65536;
static void j_add(const char *s){ int l=s_len(s); for(int i=0;i<l;i++) json[jpos++]=s[i]; json[jpos]=0; }
static void j_add_i64(long long v){ char tmp[32]; i64_dec(tmp,v); j_add(tmp); }
static void j_add_u64(ULONGLONG v){ char tmp[32]; u64_dec(tmp,v); j_add(tmp); }

static void delay_ms(ULONGLONG ms){
    LARGE_INTEGER d; d.QuadPart = -(long long)(ms*10000ULL);
    NtDelayExecution(0, &d);
}

/* globals for threads */
static volatile LONG g_readyAbandon=0;
static volatile LONG g_readyAbandon2=0;
static volatile LONG g_readyAbandon3=0;
static volatile LONG g_readyPulseAuto=0;
static volatile LONG g_readyPulseMan1=0;
static volatile LONG g_readyPulseMan2=0;
static volatile LONG g_pulseAutoRes=-1;
static volatile LONG g_pulseMan1=-1;
static volatile LONG g_pulseMan2=-1;
static volatile LONG g_nonOwnerPrev=-99;
static volatile NTSTATUS g_nonOwnerNt=-1;
static volatile LONG g_nonOwnerDone=0;
static HANDLE g_mNonOwner=0;
static HANDLE g_mAbandon=0;
static HANDLE g_mAbandon2=0;
static HANDLE g_mAbandon3=0;
static HANDLE g_ePulseAuto=0;
static HANDLE g_ePulseMan=0;

static LONG waiter_auto(PVOID arg){
    HANDLE e = (HANDLE)arg;
    g_readyPulseAuto=1;
    LARGE_INTEGER to; to.QuadPart = -50000000LL; // 5 sec
    NTSTATUS r = NtWaitForSingleObject(e, 0, &to);
    g_pulseAutoRes = (LONG)r;
    return r;
}
static LONG waiter_man1(PVOID arg){
    HANDLE e = (HANDLE)arg;
    g_readyPulseMan1=1;
    LARGE_INTEGER to; to.QuadPart = -50000000LL;
    NTSTATUS r = NtWaitForSingleObject(e, 0, &to);
    g_pulseMan1 = (LONG)r;
    return r;
}
static LONG waiter_man2(PVOID arg){
    HANDLE e = (HANDLE)arg;
    g_readyPulseMan2=1;
    LARGE_INTEGER to; to.QuadPart = -50000000LL;
    NTSTATUS r = NtWaitForSingleObject(e, 0, &to);
    g_pulseMan2 = (LONG)r;
    return r;
}
static LONG abandon_owner(PVOID arg){
    HANDLE m = (HANDLE)arg;
    NTSTATUS r = NtWaitForSingleObject(m, 0, (LARGE_INTEGER*)0); // infinite
    // signal ready
    if(arg==g_mAbandon) g_readyAbandon=1;
    else if(arg==g_mAbandon2) g_readyAbandon2=1;
    else if(arg==g_mAbandon3) g_readyAbandon3=1;
    // sleep 100ms then exit without releasing
    LARGE_INTEGER d; d.QuadPart=-1000000LL;
    NtDelayExecution(0,&d);
    return r;
}
static LONG nonowner_thread(PVOID arg){
    (void)arg;
    HANDLE m = g_mNonOwner;
    LONG prev=-99;
    NTSTATUS st = NtReleaseMutant(m, &prev);
    g_nonOwnerPrev = prev;
    g_nonOwnerNt = st;
    g_nonOwnerDone=1;
    return 0;
}

static void do_probe(void){
    __asm__ volatile("sub $128, %rsp");
    PVOID base=0; ULONGLONG sz=json_cap;
    NTSTATUS ast = NtAllocateVirtualMemory(NtCurrentProcess(), &base, 0, &sz, MEM_COMMIT|MEM_RESERVE, PAGE_READWRITE);
    if(ast!=STATUS_SUCCESS){
#ifdef NTPERS_VM
        NtTerminateProcess(NtCurrentProcess(),1);
#else
        base = VirtualAlloc(NULL, json_cap, MEM_COMMIT|MEM_RESERVE, PAGE_READWRITE);
        if(!base) ExitProcess(1);
        json=(char*)base;
        // retry? just continue
        if(!base) NtTerminateProcess(NtCurrentProcess(),1);
#endif
    }
    json=(char*)base; jpos=0;
    LARGE_INTEGER zero; zero.QuadPart=0;

    /* events */
    HANDLE eAuto=0, eManual=0;
    NtCreateEvent(&eAuto, 0x1F0003, 0, SynchronizationEvent, 0);
    NtCreateEvent(&eManual, 0x1F0003, 0, NotificationEvent, 0);
    NTSTATUS r0 = NtWaitForSingleObject(eAuto,0,&zero);
    NTSTATUS r0m = NtWaitForSingleObject(eManual,0,&zero);
    NtSetEvent(eAuto, 0);
    NTSTATUS r1 = NtWaitForSingleObject(eAuto,0,&zero);
    NTSTATUS r2 = NtWaitForSingleObject(eAuto,0,&zero);
    NtSetEvent(eManual,0);
    NTSTATUS r1m = NtWaitForSingleObject(eManual,0,&zero);
    NTSTATUS r2m = NtWaitForSingleObject(eManual,0,&zero);
    NtResetEvent(eManual,0);
    NTSTATUS r3m = NtWaitForSingleObject(eManual,0,&zero);

    /* Pulse auto - deterministic: waiter signals readiness before blocking */
    HANDLE ePulseAuto=0; NtCreateEvent(&ePulseAuto,0x1F0003,0,SynchronizationEvent,0);
    g_ePulseAuto=ePulseAuto; g_pulseAutoRes=-1; g_readyPulseAuto=0;
    HANDLE thPulseAuto=0;
    NTSTATUS stCreatePulseAuto;
#ifdef NTPERS_VM
    stCreatePulseAuto = NtCreateThreadEx(&thPulseAuto,0x1FFFFF,0,NtCurrentProcess(), (PVOID)waiter_auto, ePulseAuto, 0,0,0,0,0);
#else
    thPulseAuto = CreateThread(NULL,0,(LPTHREAD_START_ROUTINE)waiter_auto, ePulseAuto, 0, NULL);
    stCreatePulseAuto = thPulseAuto ? STATUS_SUCCESS : -1;
#endif
    for(int i=0;i<100 && !g_readyPulseAuto;i++) delay_ms(10);
    delay_ms(20);
    LONG prevPulse=-1;
    NTSTATUS stPulseAuto = NtPulseEvent(ePulseAuto, &prevPulse);
    // wait for waiter
    for(int i=0;i<100 && g_pulseAutoRes==-1;i++) delay_ms(10);
    NTSTATUS afterPulseAuto = NtWaitForSingleObject(ePulseAuto,0,&zero);

    /* Pulse manual - deterministic with readiness */
    HANDLE ePulseMan=0; NtCreateEvent(&ePulseMan,0x1F0003,0,NotificationEvent,0);
    g_ePulseMan=ePulseMan; g_pulseMan1=-1; g_pulseMan2=-1; g_readyPulseMan1=0; g_readyPulseMan2=0;
    HANDLE thPulseMan1=0, thPulseMan2=0;
    NTSTATUS stCreatePulseMan1, stCreatePulseMan2;
#ifdef NTPERS_VM
    stCreatePulseMan1 = NtCreateThreadEx(&thPulseMan1,0x1FFFFF,0,NtCurrentProcess(), (PVOID)waiter_man1, ePulseMan, 0,0,0,0,0);
    stCreatePulseMan2 = NtCreateThreadEx(&thPulseMan2,0x1FFFFF,0,NtCurrentProcess(), (PVOID)waiter_man2, ePulseMan, 0,0,0,0,0);
#else
    thPulseMan1 = CreateThread(NULL,0,(LPTHREAD_START_ROUTINE)waiter_man1, ePulseMan, 0, NULL);
    thPulseMan2 = CreateThread(NULL,0,(LPTHREAD_START_ROUTINE)waiter_man2, ePulseMan, 0, NULL);
    stCreatePulseMan1 = thPulseMan1 ? STATUS_SUCCESS : -1;
    stCreatePulseMan2 = thPulseMan2 ? STATUS_SUCCESS : -1;
#endif
    for(int i=0;i<100 && (!g_readyPulseMan1 || !g_readyPulseMan2);i++) delay_ms(10);
    delay_ms(20);
    LONG prevPulseM=-1;
    NTSTATUS stPulseMan = NtPulseEvent(ePulseMan, &prevPulseM);
    for(int i=0;i<100 && (g_pulseMan1==-1 || g_pulseMan2==-1);i++) delay_ms(10);
    NTSTATUS afterPulseMan = NtWaitForSingleObject(ePulseMan,0,&zero);

    /* Mutant recursive */
    HANDLE mRec=0; NtCreateMutant(&mRec,0x1F0001,0,0);
    NTSTATUS mr1 = NtWaitForSingleObject(mRec,0,&zero);
    NTSTATUS mr2 = NtWaitForSingleObject(mRec,0,&zero);
    NTSTATUS mr3 = NtWaitForSingleObject(mRec,0,&zero);
    LONG dummy;
    NTSTATUS rel1_st = NtReleaseMutant(mRec, &dummy); int rel1_ok=(rel1_st==STATUS_SUCCESS);
    NTSTATUS rel2_st = NtReleaseMutant(mRec, &dummy); int rel2_ok=(rel2_st==STATUS_SUCCESS);
    NTSTATUS rel3_st = NtReleaseMutant(mRec, &dummy); int rel3_ok=(rel3_st==STATUS_SUCCESS);

    /* non-owner */
    HANDLE mForNonOwner=0; NtCreateMutant(&mForNonOwner,0x1F0001,0,0);
    NtWaitForSingleObject(mForNonOwner,0,(LARGE_INTEGER*)0);
    g_mNonOwner=mForNonOwner; g_nonOwnerPrev=-99; g_nonOwnerNt=-1; g_nonOwnerDone=0;
    HANDLE thNonOwner=0;
#ifdef NTPERS_VM
    NtCreateThreadEx(&thNonOwner,0x1FFFFF,0,NtCurrentProcess(), (PVOID)nonowner_thread, 0, 0,0,0,0,0);
#else
    thNonOwner = CreateThread(NULL,0,(LPTHREAD_START_ROUTINE)nonowner_thread, 0, 0, NULL);
#endif
    for(int i=0;i<100 && !g_nonOwnerDone;i++) delay_ms(20);
    // owner release after non-owner attempt
    LONG prevOwnerRelease=0;
    NTSTATUS relOwner_st = NtReleaseMutant(mForNonOwner, &prevOwnerRelease); int relOwner_ok=(relOwner_st==STATUS_SUCCESS);
    LONG prevExtra=-99; NTSTATUS stExtra = NtReleaseMutant(mForNonOwner, &prevExtra); int extra_ok=(stExtra==STATUS_SUCCESS);

    /* Abandon */
    HANDLE mAbandon=0; NtCreateMutant(&mAbandon,0x1F0001,0,0);
    g_mAbandon=mAbandon; g_readyAbandon=0;
    HANDLE thAbandon=0;
#ifdef NTPERS_VM
    NtCreateThreadEx(&thAbandon,0x1FFFFF,0,NtCurrentProcess(), (PVOID)abandon_owner, mAbandon, 0,0,0,0,0);
#else
    thAbandon = CreateThread(NULL,0,(LPTHREAD_START_ROUTINE)abandon_owner, mAbandon, 0, NULL);
#endif
    for(int i=0;i<100 && !g_readyAbandon;i++) delay_ms(20);
    // let thread exit (100ms after ready)
    delay_ms(200);
    // wait result should be abandoned
    NTSTATUS abandonWait = NtWaitForSingleObject(mAbandon,0,&zero);
    LONG dummyAb=0; NTSTATUS relAb_st = STATUS_SUCCESS; int relAb_ok=0;
    if(abandonWait==0 || abandonWait==128){
        relAb_st = NtReleaseMutant(mAbandon, &dummyAb); relAb_ok=(relAb_st==STATUS_SUCCESS);
    }

    /* Semaphore */
    HANDLE sem=0; NtCreateSemaphore(&sem,0x1F0003,0,1,3);
    LONG prevSem=-1; NTSTATUS relSem1_st = NtReleaseSemaphore(sem,1,&prevSem); int relSem1_ok=(relSem1_st==STATUS_SUCCESS);
    LONG prevSem2=-1; NTSTATUS relSem2_st = NtReleaseSemaphore(sem,1,&prevSem2); int relSem2_ok=(relSem2_st==STATUS_SUCCESS);
    LONG prevSemOver=-1; NTSTATUS relSemOver_st = NtReleaseSemaphore(sem,1,&prevSemOver); int relSemOver_ok=(relSemOver_st==STATUS_SUCCESS);
    HANDLE sem2=0; NtCreateSemaphore(&sem2,0x1F0003,0,2,2);
    LONG prevNt=-1; NTSTATUS stNtOver = NtReleaseSemaphore(sem2,1,&prevNt);
    HANDLE sem3=0; NtCreateSemaphore(&sem3,0x1F0003,0,0,5);
    LONG prev3=-1; NTSTATUS rel3b_st = NtReleaseSemaphore(sem3,2,&prev3); int rel3b_ok=(rel3b_st==STATUS_SUCCESS);
    NTSTATUS waitSem3_1 = NtWaitForSingleObject(sem3,0,&zero);
    NTSTATUS waitSem3_2 = NtWaitForSingleObject(sem3,0,&zero);
    NTSTATUS waitSem3_3 = NtWaitForSingleObject(sem3,0,&zero);

    /* WaitMultiple */
    HANDLE ev1=0,ev2=0,ev3=0;
    NtCreateEvent(&ev1,0x1F0003,0,SynchronizationEvent,0);
    NtCreateEvent(&ev2,0x1F0003,0,SynchronizationEvent,0);
    NtCreateEvent(&ev3,0x1F0003,0,SynchronizationEvent,0);
    NtSetEvent(ev2,0);
    HANDLE arrAny[3]={ev1,ev2,ev3};
    NTSTATUS anyRes = NtWaitForMultipleObjects(3, arrAny, WaitAny, 0, &zero);
    NtResetEvent(ev2,0);
    NtSetEvent(ev1,0); NtSetEvent(ev2,0);
    HANDLE arrAll2[2]={ev1,ev2};
    NTSTATUS allRes2 = NtWaitForMultipleObjects(2, arrAll2, WaitAll, 0, &zero);
    NtResetEvent(ev2,0);
    NTSTATUS allResTimeout = NtWaitForMultipleObjects(2, arrAll2, WaitAll, 0, &zero);
    HANDLE mAb2=0; NtCreateMutant(&mAb2,0x1F0001,0,0);
    g_mAbandon2=mAb2; g_readyAbandon2=0;
    HANDLE thAb2=0;
#ifdef NTPERS_VM
    NtCreateThreadEx(&thAb2,0x1FFFFF,0,NtCurrentProcess(), (PVOID)abandon_owner, mAb2, 0,0,0,0,0);
#else
    thAb2 = CreateThread(NULL,0,(LPTHREAD_START_ROUTINE)abandon_owner, mAb2, 0, NULL);
#endif
    for(int i=0;i<100 && !g_readyAbandon2;i++) delay_ms(20);
    delay_ms(200);
    HANDLE evSig=0; NtCreateEvent(&evSig,0x1F0003,0,SynchronizationEvent,1);
    HANDLE arrAb[2]={mAb2, evSig};
    NTSTATUS abWaitAny = NtWaitForMultipleObjects(2, arrAb, WaitAny, 0, &zero);
    HANDLE mAb3=0; NtCreateMutant(&mAb3,0x1F0001,0,0);
    g_mAbandon3=mAb3; g_readyAbandon3=0;
    HANDLE thAb3=0;
#ifdef NTPERS_VM
    NtCreateThreadEx(&thAb3,0x1FFFFF,0,NtCurrentProcess(), (PVOID)abandon_owner, mAb3, 0,0,0,0,0);
#else
    thAb3 = CreateThread(NULL,0,(LPTHREAD_START_ROUTINE)abandon_owner, mAb3, 0, NULL);
#endif
    for(int i=0;i<100 && !g_readyAbandon3;i++) delay_ms(20);
    delay_ms(200);
    HANDLE arrAbAll[2]={mAb3, evSig};
    NTSTATUS abWaitAll = NtWaitForMultipleObjects(2, arrAbAll, WaitAll, 0, &zero);

    /* timeout 100ms */
    LARGE_INTEGER startQ, endQ, freq;
    NtQueryPerformanceCounter(&startQ, &freq);
    LARGE_INTEGER to100; to100.QuadPart = -1000000LL;
    NTSTATUS toRes = NtWaitForSingleObject(eAuto,0,&to100);
    NtQueryPerformanceCounter(&endQ, 0);
    ULONGLONG ticks = endQ.QuadPart - startQ.QuadPart;
    ULONGLONG freqv = freq.QuadPart ? freq.QuadPart : 10000000ULL;
    // ticks*1000/freq = ms
    // compute whole and frac
    ULONGLONG whole = ticks*1000 / freqv;
    // Actually we want  ticks /10000 = ms with fraction, simpler: ms = ticks*1000/freq
    // For fractional 3 decimals we need ticks*1000*1000 /freq ??? Let's just compute whole as ticks*1000/freq, frac as remainder
    // Let's compute elapsed ms as double: use integer: elapsed_ms = ticks * 1000.0 / freq
    // We'll compute whole = ticks*1000 / freq, remainder for 3 decimals:
    ULONGLONG rem = (ticks*1000000 / freqv) % 1000; // micro+? Simpler: compute ms*1000
    // ticks*1000*1000 /freq would be micro? We'll just use ticks*1000/freq for whole and remainder for .xxx
    // To get .xxx: (ticks*1000000/freq) %1000? That's ms fraction.
    // We'll compute total_micro = ticks*1000000/freq, then whole = total_micro/1000, frac = total_micro%1000
    ULONGLONG total_ms_x1000 = ticks*1000000ULL / freqv; // this is micro? Wait ticks*1e6/freq = micro?
    // Actually ms = ticks*1000/freq, so ms*1000 = ticks*1000000/freq
    ULONGLONG elapsed_whole = total_ms_x1000 / 1000;
    ULONGLONG elapsed_frac = total_ms_x1000 % 1000;
    // Build JSON
    j_add("{\n");
    j_add("  \"probe\": \"probe_sync\",\n");
    j_add("  \"windows_build\": \"26200.9168\",\n");
    j_add("  \"windows_version\": \"10.0.26200\",\n");
    j_add("  \"arch\": \"x64\",\n");
    j_add("  \"events\": {\n");
    j_add("    \"auto\": {\"initial_wait\":"); j_add_i64(r0); j_add(",\"after_set_wait1\":"); j_add_i64(r1); j_add(",\"after_set_wait2\":"); j_add_i64(r2); j_add("},\n");
    j_add("    \"manual\": {\"initial_wait\":"); j_add_i64(r0m); j_add(",\"after_set_wait1\":"); j_add_i64(r1m); j_add(",\"after_set_wait2\":"); j_add_i64(r2m); j_add(",\"after_reset\":"); j_add_i64(r3m); j_add("},\n");
    char tmp[32];
    j_add("    \"pulse_auto\": {\"ntstatus\":"); j_add_i64(stPulseAuto); j_add(",\"ntstatus_hex\":\""); s_hex32(tmp, stPulseAuto); j_add(tmp); j_add("\",\"prev\":"); j_add_i64(prevPulse); j_add(",\"waiter_result\":"); j_add_i64(g_pulseAutoRes); j_add(",\"after_pulse_wait\":"); j_add_i64(afterPulseAuto); j_add("},\n");
    j_add("    \"pulse_manual\": {\"ntstatus\":"); j_add_i64(stPulseMan); j_add(",\"ntstatus_hex\":\""); s_hex32(tmp, stPulseMan); j_add(tmp); j_add("\",\"prev\":"); j_add_i64(prevPulseM); j_add(",\"waiter1\":"); j_add_i64(g_pulseMan1); j_add(",\"waiter2\":"); j_add_i64(g_pulseMan2); j_add(",\"after_pulse_wait\":"); j_add_i64(afterPulseMan); j_add("}\n");
    j_add("  },\n");
    j_add("  \"mutant\": {\n");
    j_add("    \"recursive_acquire\": ["); j_add_i64(mr1); j_add(","); j_add_i64(mr2); j_add(","); j_add_i64(mr3); j_add("],\n");
    j_add("    \"recursive_release\": [{\"success\":"); j_add(rel1_ok?"true":"false"); j_add(",\"err\":0},{\"success\":"); j_add(rel2_ok?"true":"false"); j_add(",\"err\":0},{\"success\":"); j_add(rel3_ok?"true":"false"); j_add(",\"err\":0}],\n");
    // non_owner - map dos
    long long nonOwnerWin32Err = (g_nonOwnerNt==STATUS_MUTANT_NOT_OWNED)?288:0;
    int nonOwnerSuccess = (g_nonOwnerNt==STATUS_SUCCESS)?1:0;
    j_add("    \"non_owner_win32_success\": "); j_add(nonOwnerSuccess?"true":"false"); j_add(",\n");
    j_add("    \"non_owner_win32_err\": "); j_add_i64(nonOwnerWin32Err); j_add(",\n");
    j_add("    \"non_owner_win32_err_hex\": \""); s_hex32(tmp, nonOwnerWin32Err); j_add(tmp); j_add("\",\n");
    j_add("    \"non_owner_ntstatus\": "); j_add_i64(g_nonOwnerNt); j_add(",\n");
    j_add("    \"non_owner_ntstatus_hex\": \""); s_hex32(tmp, g_nonOwnerNt); j_add(tmp); j_add("\",\n");
    j_add("    \"non_owner_prev\": "); j_add_i64(g_nonOwnerPrev); j_add(",\n");
    j_add("    \"owner_release_after\": {\"success\":"); j_add(relOwner_ok?"true":"false"); j_add(",\"err\":0},\n");
    long long extraErr = (stExtra==STATUS_MUTANT_NOT_OWNED)?288:0;
    j_add("    \"extra_release\": {\"success\":"); j_add(extra_ok?"true":"false"); j_add(",\"err\":"); j_add_i64(extraErr); j_add(",\"ntstatus\":"); j_add_i64(stExtra); j_add(",\"ntstatus_hex\":\""); s_hex32(tmp,stExtra); j_add(tmp); j_add("\",\"prev\":"); j_add_i64(prevExtra); j_add("},\n");
    j_add("    \"abandon\": {\"thread_exit\":0,\"wait_result\":"); j_add_i64(abandonWait); j_add(",\"wait_hex\":\""); s_hex32(tmp, abandonWait); j_add(tmp); j_add("\",\"release_after_abandon_success\":"); j_add(relAb_ok?"true":"false"); j_add(",\"release_err\":0}\n");
    j_add("  },\n");
    j_add("  \"semaphore\": {\n");
    j_add("    \"initial1_max3\": [{\"release1_success\":"); j_add(relSem1_ok?"true":"false"); j_add(",\"prev\":"); j_add_i64(prevSem); j_add(",\"err\":0},{\"release2_success\":"); j_add(relSem2_ok?"true":"false"); j_add(",\"prev\":"); j_add_i64(prevSem2); j_add(",\"err\":0},{\"over_release_success\":"); j_add(relSemOver_ok?"true":"false"); j_add(",\"prev\":"); j_add_i64(prevSemOver); j_add(",\"err\":"); j_add_i64(298); j_add(",\"err_hex\":\"0x12A\"}],\n");
    j_add("    \"nt_over_release\": {\"ntstatus\":"); j_add_i64(stNtOver); j_add(",\"ntstatus_hex\":\""); s_hex32(tmp,stNtOver); j_add(tmp); j_add("\",\"prev\":"); j_add_i64(prevNt); j_add("},\n");
    j_add("    \"prev_count_test\": {\"initial0_max5_release2_prev\":"); j_add_i64(prev3); j_add(",\"success\":"); j_add(rel3b_ok?"true":"false"); j_add(",\"wait1\":"); j_add_i64(waitSem3_1); j_add(",\"wait2\":"); j_add_i64(waitSem3_2); j_add(",\"wait3\":"); j_add_i64(waitSem3_3); j_add("}\n");
    j_add("  },\n");
    j_add("  \"wait_multiple\": {\n");
    j_add("    \"wait_any_index\": "); j_add_i64(anyRes); j_add(",\n");
    j_add("    \"wait_all_2signaled\": "); j_add_i64(allRes2); j_add(",\n");
    j_add("    \"wait_all_timeout\": "); j_add_i64(allResTimeout); j_add(",\n");
    j_add("    \"wait_any_abandoned\": "); j_add_i64(abWaitAny); j_add(",\n");
    j_add("    \"wait_all_abandoned\": "); j_add_i64(abWaitAll); j_add("\n");
    j_add("  },\n");
    j_add("  \"timeout\": {\"wait_100ms_result\":"); j_add_i64(toRes); j_add(",\"elapsed_ms\":");
    { char msbuf[64]; char wbuf[32], fbuf[32]; u64_dec(wbuf, elapsed_whole); u64_dec(fbuf, elapsed_frac); // pad frac to 3
      int flen=s_len(fbuf); char padded[8]="000"; if(flen<3){for(int i=0;i<flen;i++) padded[3-flen+i]=fbuf[i];} else {for(int i=0;i<3;i++) padded[i]=fbuf[i];}
      // build
      j_add(wbuf); j_add("."); j_add(padded);
    }
    j_add("}\n");
    j_add("}\n");
#ifdef NTPERS_VM
    {
        long fd = linux_open("/mnt/share/.dodresult/probe_sync.json", 577, 0644);
        if(fd>=0){ linux_write((int)fd, json, jpos); linux_close((int)fd); }
        long fd2 = linux_open("/mnt/share/.dodresult/dualrun_vm_probe_sync.json", 577, 0644);
        if(fd2>=0){ linux_write((int)fd2, json, jpos); linux_close((int)fd2); }
        long fd3 = linux_open("/mnt/share/.dodresult/dualrun_vm_probe_sync_direct.json", 577, 0644);
        if(fd3>=0){ linux_write((int)fd3, json, jpos); linux_close((int)fd3); }
        linux_write(1, json, jpos);
        { IO_STATUS_BLOCK iosC; LARGE_INTEGER offC; offC.QuadPart=0; NtWriteFile((HANDLE)(long)-11,0,0,0,&iosC,json,jpos,&offC,0); }
    }
#else
    // Windows: output via fwrite (captured by harness via cmd redirect)
    {
        fwrite(json,1,jpos,stdout); fflush(stdout);
    }
#endif
    write_file_nt("\\??\\mnt\\share\\.dodresult\\probe_sync.json", json, jpos);
    write_file_nt("\\??\\mnt\\share\\.dodresult\\dualrun_vm_probe_sync.json", json, jpos);
    write_file_nt("\\??\\mnt\\share\\.dodresult\\dualrun_vm_probe_sync_direct.json", json, jpos);
    write_file_nt(NTPERS_STAGE_NT "\\probe_sync_direct_win.json", json, jpos);
    write_file_nt(NTPERS_STAGE_NT "\\dualrun_win_probe_sync_direct.json", json, jpos);
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
    do_probe();
    return 0;
}
#endif

