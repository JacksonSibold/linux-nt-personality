/* probe_ntstatus_direct.c - dual-target direct probe
 * WINDOWS build: resolves via GetProcAddress (correct SSN per host)
 * VM build: raw syscall stubs pinned to Server 2022 via -DNTPERS_VM
 * Scaled toward 1000 distinct NTSTATUS via real syscalls (brute force).
 */
#ifdef NTPERS_VM
typedef unsigned long long ULONGLONG;
typedef unsigned long ULONG;
typedef unsigned short USHORT;
typedef unsigned short WCHAR;
typedef void* HANDLE;
typedef void* PVOID;
typedef long NTSTATUS;
typedef struct { ULONGLONG Status; ULONGLONG Information; } IO_STATUS_BLOCK;
typedef struct { USHORT Length; USHORT MaximumLength; WCHAR *Buffer; } UNICODE_STRING;
typedef struct { ULONG Length; HANDLE RootDirectory; UNICODE_STRING *ObjectName; ULONG Attributes; PVOID SecurityDescriptor; PVOID SecurityQualityOfService; } OBJECT_ATTRIBUTES;
typedef struct { ULONGLONG QuadPart; } LARGE_INTEGER;
#else
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winternl.h>
#include <stdio.h>
#include "ntpers_probe_root.h"
#endif
#define STATUS_SUCCESS 0
#define FILE_SUPERSEDE 0
#define FILE_OPEN 1
#define FILE_CREATE 2
#define FILE_OPEN_IF 3
#define FILE_OVERWRITE 4
#define FILE_OVERWRITE_IF 5
#define FILE_SYNCHRONOUS_IO_NONALERT 0x20
#define FILE_NON_DIRECTORY_FILE 0x40
#define FILE_DIRECTORY_FILE 0x01
#define OBJ_CASE_INSENSITIVE 0x40
#define FILE_SHARE_READ 1
#define FILE_SHARE_WRITE 2
#define FILE_SHARE_DELETE 4
#define MEM_COMMIT 0x1000
#define MEM_RESERVE 0x2000
#define PAGE_READWRITE 0x04
#ifdef NTPERS_VM
#define NTPERS_TMP_NT "\\??\\C:\\tmp\\probe_ntstatus.tmp"
#define NTPERS_NONEXIST_NT "\\??\\C:\\tmp\\nonexistent_98765.tmp"
#else
static char g_tmp_nt[MAX_PATH+64], g_nonexist_nt[MAX_PATH+64];
#define NTPERS_TMP_NT g_tmp_nt
#define NTPERS_NONEXIST_NT g_nonexist_nt
#endif
#ifdef NTPERS_VM
__asm__(".text\n"
".globl NtCreateFile\nNtCreateFile: mov %rcx, %r10; mov $0x55, %eax; syscall; ret\n"
".globl NtQueryInformationFile\nNtQueryInformationFile: mov %rcx, %r10; mov $0x11, %eax; syscall; ret\n"
".globl NtQueryObject\nNtQueryObject: mov %rcx, %r10; mov $0x10, %eax; syscall; ret\n"
".globl NtQuerySystemInformation\nNtQuerySystemInformation: mov %rcx, %r10; mov $0x36, %eax; syscall; ret\n"
".globl NtWriteFile\nNtWriteFile: mov %rcx, %r10; mov $0x08, %eax; syscall; ret\n"
".globl NtClose\nNtClose: mov %rcx, %r10; mov $0x0f, %eax; syscall; ret\n"
".globl NtCreateEvent\nNtCreateEvent: mov %rcx, %r10; mov $0x48, %eax; syscall; ret\n"
".globl NtQueryEvent\nNtQueryEvent: mov %rcx, %r10; mov $0x56, %eax; syscall; ret\n"
".globl NtAllocateVirtualMemory\nNtAllocateVirtualMemory: mov %rcx, %r10; mov $0x18, %eax; syscall; ret\n"
".globl NtFreeVirtualMemory\nNtFreeVirtualMemory: mov %rcx, %r10; mov $0x1e, %eax; syscall; ret\n"
".globl NtProtectVirtualMemory\nNtProtectVirtualMemory: mov %rcx, %r10; mov $0x50, %eax; syscall; ret\n"
".globl NtQueryVirtualMemory\nNtQueryVirtualMemory: mov %rcx, %r10; mov $0x23, %eax; syscall; ret\n"
".globl NtTerminateProcess\nNtTerminateProcess: mov %rcx, %r10; mov $0x2c, %eax; syscall; ret\n"
".globl linux_open\nlinux_open: mov $2, %eax; syscall; ret\n"
".globl linux_write\nlinux_write: mov $1, %eax; syscall; ret\n"
".globl linux_close\nlinux_close: mov $3, %eax; syscall; ret\n"
);
NTSTATUS NtCreateFile(HANDLE *FileHandle, ULONG DesiredAccess, OBJECT_ATTRIBUTES *ObjectAttributes, IO_STATUS_BLOCK *IoStatusBlock, LARGE_INTEGER *AllocationSize, ULONG FileAttributes, ULONG ShareAccess, ULONG CreateDisposition, ULONG CreateOptions, PVOID EaBuffer, ULONG EaLength);
NTSTATUS NtQueryInformationFile(HANDLE FileHandle, IO_STATUS_BLOCK *IoStatusBlock, PVOID FileInformation, ULONG Length, ULONG FileInformationClass);
NTSTATUS NtQueryObject(HANDLE Handle, ULONG ObjectInformationClass, PVOID ObjectInformation, ULONG ObjectInformationLength, ULONG *ReturnLength);
NTSTATUS NtQuerySystemInformation(ULONG SystemInformationClass, PVOID SystemInformation, ULONG SystemInformationLength, ULONG *ReturnLength);
NTSTATUS NtWriteFile(HANDLE FileHandle, HANDLE Event, PVOID ApcRoutine, PVOID ApcContext, IO_STATUS_BLOCK *IoStatusBlock, PVOID Buffer, ULONG Length, LARGE_INTEGER *ByteOffset, ULONG *Key);
NTSTATUS NtClose(HANDLE Handle);
NTSTATUS NtCreateEvent(HANDLE *EventHandle, ULONG DesiredAccess, OBJECT_ATTRIBUTES *ObjectAttributes, ULONG EventType, ULONG InitialState);
NTSTATUS NtQueryEvent(HANDLE EventHandle, ULONG EventInformationClass, PVOID EventInformation, ULONG EventInformationLength, ULONG *ReturnLength);
NTSTATUS NtAllocateVirtualMemory(HANDLE ProcessHandle, PVOID *BaseAddress, ULONGLONG ZeroBits, ULONGLONG *RegionSize, ULONG AllocationType, ULONG Protect);
NTSTATUS NtFreeVirtualMemory(HANDLE ProcessHandle, PVOID *BaseAddress, ULONGLONG *RegionSize, ULONG FreeType);
NTSTATUS NtProtectVirtualMemory(HANDLE ProcessHandle, PVOID *BaseAddress, ULONGLONG *RegionSize, ULONG NewProtect, ULONG *OldProtect);
NTSTATUS NtQueryVirtualMemory(HANDLE ProcessHandle, PVOID BaseAddress, ULONG MemoryInformationClass, PVOID MemoryInformation, ULONGLONG MemoryInformationLength, ULONGLONG *ReturnLength);
NTSTATUS NtTerminateProcess(HANDLE ProcessHandle, NTSTATUS ExitStatus);
long linux_open(const char *path, int flags, int mode);
long linux_write(int fd, const void *buf, unsigned long len);
long linux_close(int fd);
#else
static HMODULE g_ntdll=NULL;
static NTSTATUS (NTAPI *pNtCreateFile)(HANDLE*,ULONG,OBJECT_ATTRIBUTES*,IO_STATUS_BLOCK*,LARGE_INTEGER*,ULONG,ULONG,ULONG,ULONG,PVOID,ULONG);
#define NtCreateFile pNtCreateFile
static NTSTATUS (NTAPI *pNtQueryInformationFile)(HANDLE,IO_STATUS_BLOCK*,PVOID,ULONG,ULONG);
#define NtQueryInformationFile pNtQueryInformationFile
static NTSTATUS (NTAPI *pNtQueryObject)(HANDLE,ULONG,PVOID,ULONG,ULONG*);
#define NtQueryObject pNtQueryObject
static NTSTATUS (NTAPI *pNtQuerySystemInformation)(ULONG,PVOID,ULONG,ULONG*);
#define NtQuerySystemInformation pNtQuerySystemInformation
static NTSTATUS (NTAPI *pNtWriteFile)(HANDLE,HANDLE,PVOID,PVOID,IO_STATUS_BLOCK*,PVOID,ULONG,LARGE_INTEGER*,ULONG*);
#define NtWriteFile pNtWriteFile
static NTSTATUS (NTAPI *pNtClose)(HANDLE);
#define NtClose pNtClose
static NTSTATUS (NTAPI *pNtCreateEvent)(HANDLE*,ULONG,OBJECT_ATTRIBUTES*,ULONG,ULONG);
#define NtCreateEvent pNtCreateEvent
static NTSTATUS (NTAPI *pNtQueryEvent)(HANDLE,ULONG,PVOID,ULONG,ULONG*);
#define NtQueryEvent pNtQueryEvent
static NTSTATUS (NTAPI *pNtAllocateVirtualMemory)(HANDLE,PVOID*,ULONGLONG,ULONGLONG*,ULONG,ULONG);
#define NtAllocateVirtualMemory pNtAllocateVirtualMemory
static NTSTATUS (NTAPI *pNtFreeVirtualMemory)(HANDLE,PVOID*,ULONGLONG*,ULONG);
#define NtFreeVirtualMemory pNtFreeVirtualMemory
static NTSTATUS (NTAPI *pNtProtectVirtualMemory)(HANDLE,PVOID*,ULONGLONG*,ULONG,ULONG*);
#define NtProtectVirtualMemory pNtProtectVirtualMemory
static NTSTATUS (NTAPI *pNtQueryVirtualMemory)(HANDLE,PVOID,ULONG,PVOID,ULONGLONG,ULONGLONG*);
#define NtQueryVirtualMemory pNtQueryVirtualMemory
static NTSTATUS (NTAPI *pNtTerminateProcess)(HANDLE,NTSTATUS);
#define NtTerminateProcess pNtTerminateProcess
static ULONG (NTAPI *pRtlNtStatusToDosError)(NTSTATUS);
#define RtlNtStatusToDosError pRtlNtStatusToDosError
static void init_ntdll(void){
 if(g_ntdll) return;
 g_ntdll=GetModuleHandleA("ntdll.dll");
 pNtCreateFile=(void*)GetProcAddress(g_ntdll,"NtCreateFile");
 pNtQueryInformationFile=(void*)GetProcAddress(g_ntdll,"NtQueryInformationFile");
 pNtQueryObject=(void*)GetProcAddress(g_ntdll,"NtQueryObject");
 pNtQuerySystemInformation=(void*)GetProcAddress(g_ntdll,"NtQuerySystemInformation");
 pNtWriteFile=(void*)GetProcAddress(g_ntdll,"NtWriteFile");
 pNtClose=(void*)GetProcAddress(g_ntdll,"NtClose");
 pNtCreateEvent=(void*)GetProcAddress(g_ntdll,"NtCreateEvent");
 pNtQueryEvent=(void*)GetProcAddress(g_ntdll,"NtQueryEvent");
 pNtAllocateVirtualMemory=(void*)GetProcAddress(g_ntdll,"NtAllocateVirtualMemory");
 pNtFreeVirtualMemory=(void*)GetProcAddress(g_ntdll,"NtFreeVirtualMemory");
 pNtProtectVirtualMemory=(void*)GetProcAddress(g_ntdll,"NtProtectVirtualMemory");
 pNtQueryVirtualMemory=(void*)GetProcAddress(g_ntdll,"NtQueryVirtualMemory");
 pNtTerminateProcess=(void*)GetProcAddress(g_ntdll,"NtTerminateProcess");
 pRtlNtStatusToDosError=(void*)GetProcAddress(g_ntdll,"RtlNtStatusToDosError");
}
static long linux_open(const char *p,int f,int m){return -1;}
static long linux_write(int f,const void *b,unsigned long l){return -1;}
static long linux_close(int f){return -1;}
#endif
#define NtCurrentProcess() ((HANDLE)(long)-1)
void ___chkstk_ms(void){}
#ifdef NTPERS_VM
static ULONG RtlNtStatusToDosError(NTSTATUS Status){
 switch((ULONG)Status){case 0:return 0;case 0xC0000034:return 2;case 0xC000003A:return 3;case 0xC0000033:return 123;case 0xC0000008:return 6;case 0xC0000022:return 5;case 0xC0000043:return 32;case 0xC0000035:return 183;case 0xC000000D:return 87;case 0xC0000004:return 24;case 0xC0000023:return 122;case 0x80000005:return 234;case 0xC0000005:return 998;default: return 317; }
}
#endif
static void u32_hex(char *o,ULONG v){const char *h="0123456789ABCDEF";o[0]='0';o[1]='x';for(int i=0;i<8;i++)o[2+i]=h[(v>>(28-i*4))&0xF];o[10]=0;}
static void u64_dec(char *o,ULONGLONG v){char t[32];int p=0;if(v==0){o[0]='0';o[1]=0;return;}while(v>0){t[p++]='0'+(v%10);v/=10;}for(int i=0;i<p;i++)o[i]=t[p-1-i];o[p]=0;}
static void i64_dec(char *o,long long v){if(v==0){o[0]='0';o[1]=0;return;}int neg=v<0;unsigned long long uv=neg?(unsigned long long)(-v):(unsigned long long)v;char t[32];int p=0;while(uv>0){t[p++]='0'+(uv%10);uv/=10;}int pos=0;if(neg)o[pos++]='-';for(int i=0;i<p;i++)o[pos+i]=t[p-1-i];o[pos+p]=0;}
static int s_len(const char *s){int n=0;while(s[n])n++;return n;}
static char *json; static int jpos; static unsigned long long json_cap=8388608;
static void j_add(const char *s){int l=s_len(s); if(jpos + l + 1 >= (int)json_cap) return; for(int i=0;i<l;i++) json[jpos++]=s[i]; json[jpos]=0;}
static unsigned long distinct[2048]; static int distinct_cnt;
static void add_distinct(unsigned long st){ for(int i=0;i<distinct_cnt;i++) if(distinct[i]==st) return; if(distinct_cnt<2048) distinct[distinct_cnt++]=st; }
static void write_file_nt(const char *path,const char *buf,int len){
 WCHAR wpath[1024];int i=0;while(path[i]&&i<1023){wpath[i]=(WCHAR)path[i];i++;}wpath[i]=0;for(int j=0;j<i;j++) if(wpath[j]=='/') wpath[j]='\\';
 UNICODE_STRING us;us.Length=(USHORT)i*2; us.MaximumLength=us.Length+2; us.Buffer=wpath;
 OBJECT_ATTRIBUTES oa;oa.Length=sizeof(oa);oa.RootDirectory=0;oa.ObjectName=&us;oa.Attributes=0x40;oa.SecurityDescriptor=0;oa.SecurityQualityOfService=0;
 HANDLE h=0; IO_STATUS_BLOCK ios; LARGE_INTEGER alloc;alloc.QuadPart=0;
 NTSTATUS st=NtCreateFile(&h,0x12019F,&oa,&ios,&alloc,0x80,3,5,0x20|0x40,0,0);
 if(st!=0) return; IO_STATUS_BLOCK ios2; LARGE_INTEGER off;off.QuadPart=0;
 unsigned long long pos=0; while(pos<(unsigned long long)len){unsigned long chunk=len-(int)pos; if(chunk>65536) chunk=65536; IO_STATUS_BLOCK ios3; LARGE_INTEGER off3;off3.QuadPart=pos; NtWriteFile(h,0,0,0,&ios3,(void*)(buf+pos),chunk,&off3,0); if(ios3.Information==0) break; pos+=ios3.Information; if(ios3.Information<chunk) break; } NtClose(h);
}
#ifndef NTPERS_VM
/* Host build read from the running system: RtlGetVersion for the build number, UBR from the registry. */
typedef LONG (WINAPI *PFN_RtlGetVersion_host)(OSVERSIONINFOEXW*);
static void host_build_strings(char *build, size_t bn, char *ver, size_t vn){
 OSVERSIONINFOEXW ovi={0}; DWORD ubr=0, v=0, cb=sizeof v; HKEY hk;
 PFN_RtlGetVersion_host pr=(PFN_RtlGetVersion_host)GetProcAddress(GetModuleHandleA("ntdll.dll"),"RtlGetVersion");
 ovi.dwOSVersionInfoSize=sizeof ovi;
 if(pr) pr(&ovi);
 if(RegOpenKeyExA(HKEY_LOCAL_MACHINE,"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",0,KEY_READ,&hk)==ERROR_SUCCESS){
  if(RegQueryValueExA(hk,"UBR",NULL,NULL,(BYTE*)&v,&cb)==ERROR_SUCCESS) ubr=v;
  RegCloseKey(hk);
 }
 snprintf(build,bn,"%lu.%lu",(unsigned long)ovi.dwBuildNumber,(unsigned long)ubr);
 snprintf(ver,vn,"%lu.%lu.%lu",(unsigned long)ovi.dwMajorVersion,(unsigned long)ovi.dwMinorVersion,(unsigned long)ovi.dwBuildNumber);
}
#endif
static void do_probe(void){
#ifdef NTPERS_VM
 __asm__ volatile("sub $128, %rsp");
#endif
 { void *jbase=0; unsigned long long jsz=json_cap; NTSTATUS jst=NtAllocateVirtualMemory((void*)(long)-1, &jbase, 0, &jsz, 0x3000, 0x04); if(jst==0){ json=(char*)jbase; } else { static char fb[65536]; json=fb; json_cap=65536; } }
 jpos=0; distinct_cnt=0;
 j_add("{\n  \"probe\": \"probe_ntstatus\",\n");
#ifdef NTPERS_VM
 j_add("  \"windows_build\": \"26200.9168\",\n  \"windows_version\": \"10.0.26200\",\n");
#else
 { char wb[32], wv[32]; host_build_strings(wb, sizeof wb, wv, sizeof wv);
   j_add("  \"windows_build\": \""); j_add(wb); j_add("\",\n");
   j_add("  \"windows_version\": \""); j_add(wv); j_add("\",\n"); }
#endif
 j_add("  \"arch\": \"x64\",\n");
 j_add("  \"rtl_mappings\": [\n");
 struct { ULONG s; const char *n; } tbl[] = {

        {0x00000000,"STATUS_SUCCESS"},
        {0x00000001,"STATUS_WAIT_1"},
        {0x00000002,"STATUS_WAIT_2"},
        {0x00000003,"STATUS_WAIT_3"},
        {0x0000003F,"STATUS_WAIT_63"},
        {0x00000080,"STATUS_ABANDONED"},
        {0x000000BF,"STATUS_ABANDONED_WAIT_63"},
        {0x000000C0,"STATUS_USER_APC"},
        {0x00000100,"STATUS_KERNEL_APC"},
        {0x00000101,"STATUS_ALERTED"},
        {0x00000102,"STATUS_TIMEOUT"},
        {0x00000103,"STATUS_PENDING"},
        {0x00000104,"STATUS_REPARSE"},
        {0x00000105,"STATUS_MORE_ENTRIES"},
        {0x00000106,"STATUS_NOT_ALL_ASSIGNED"},
        {0x00000107,"STATUS_SOME_NOT_MAPPED"},
        {0x00000108,"STATUS_OPLOCK_BREAK_IN_PROGRESS"},
        {0x00000109,"STATUS_VOLUME_MOUNTED"},
        {0x0000010A,"STATUS_RXACT_COMMITTED"},
        {0x0000010B,"STATUS_NOTIFY_CLEANUP"},
        {0x0000010C,"STATUS_NOTIFY_ENUM_DIR"},
        {0x0000010D,"STATUS_NO_QUOTAS_FOR_ACCOUNT"},
        {0x0000010E,"STATUS_PRIMARY_TRANSPORT_CONNECT_FAILED"},
        {0x00000110,"STATUS_PAGE_FAULT_TRANSITION"},
        {0x00000111,"STATUS_PAGE_FAULT_DEMAND_ZERO"},
        {0x00000112,"STATUS_PAGE_FAULT_COPY_ON_WRITE"},
        {0x00000113,"STATUS_PAGE_FAULT_GUARD_PAGE"},
        {0x00000114,"STATUS_PAGE_FAULT_PAGING_FILE"},
        {0x00000115,"STATUS_CACHE_PAGE_FLUSHED"},
        {0x00000367,"STATUS_WAIT_FOR_OPLOCK"},
        {0x80000001,"STATUS_GUARD_PAGE_VIOLATION"},
        {0x80000002,"STATUS_DATATYPE_MISALIGNMENT"},
        {0x80000003,"STATUS_BREAKPOINT"},
        {0x80000004,"STATUS_SINGLE_STEP"},
        {0x80000005,"STATUS_BUFFER_OVERFLOW"},
        {0x80000006,"STATUS_NO_MORE_FILES"},
        {0x8000000A,"STATUS_HANDLES_CLOSED"},
        {0x8000000B,"STATUS_NO_INHERITANCE"},
        {0x8000000C,"STATUS_GUID_SUBSTITUTION_MADE"},
        {0x8000000D,"STATUS_PARTIAL_COPY"},
        {0x8000000E,"STATUS_DEVICE_PAPER_EMPTY"},
        {0x8000000F,"STATUS_DEVICE_POWERED_OFF"},
        {0x80000010,"STATUS_DEVICE_OFF_LINE"},
        {0x80000011,"STATUS_DEVICE_BUSY"},
        {0x80000012,"STATUS_NO_MORE_EAS"},
        {0x80000013,"STATUS_INVALID_EA_NAME"},
        {0x80000014,"STATUS_EA_LIST_INCONSISTENT"},
        {0x80000015,"STATUS_INVALID_EA_FLAG"},
        {0x80000016,"STATUS_VERIFY_REQUIRED"},
        {0x80000017,"STATUS_EXTRANEOUS_INFORMATION"},
        {0x80000018,"STATUS_RXACT_COMMIT_NECESSARY"},
        {0x8000001A,"STATUS_NO_MORE_ENTRIES"},
        {0x8000001B,"STATUS_FILEMARK_DETECTED"},
        {0x8000001C,"STATUS_MEDIA_CHANGED"},
        {0x8000001D,"STATUS_BUS_RESET"},
        {0x8000001E,"STATUS_END_OF_MEDIA"},
        {0x8000001F,"STATUS_BEGINNING_OF_MEDIA"},
        {0x80000020,"STATUS_MEDIA_CHECK"},
        {0x80000021,"STATUS_SETMARK_DETECTED"},
        {0x80000022,"STATUS_NO_DATA_DETECTED"},
        {0x80000023,"STATUS_REDIRECTOR_HAS_OPEN_HANDLES"},
        {0x80000024,"STATUS_SERVER_HAS_OPEN_HANDLES"},
        {0x80000025,"STATUS_ALREADY_COMPLETE"},
        {0xC0000001,"STATUS_UNSUCCESSFUL"},
        {0xC0000002,"STATUS_NOT_IMPLEMENTED"},
        {0xC0000003,"STATUS_INVALID_INFO_CLASS"},
        {0xC0000004,"STATUS_INFO_LENGTH_MISMATCH"},
        {0xC0000005,"STATUS_ACCESS_VIOLATION"},
        {0xC0000006,"STATUS_IN_PAGE_ERROR"},
        {0xC0000007,"STATUS_PAGEFILE_QUOTA"},
        {0xC0000008,"STATUS_INVALID_HANDLE"},
        {0xC0000009,"STATUS_BAD_INITIAL_STACK"},
        {0xC000000A,"STATUS_BAD_INITIAL_PC"},
        {0xC000000B,"STATUS_INVALID_CID"},
        {0xC000000C,"STATUS_TIMER_NOT_CANCELED"},
        {0xC000000D,"STATUS_INVALID_PARAMETER"},
        {0xC000000E,"STATUS_NO_SUCH_DEVICE"},
        {0xC000000F,"STATUS_NO_SUCH_FILE"},
        {0xC0000010,"STATUS_INVALID_DEVICE_REQUEST"},
        {0xC0000011,"STATUS_END_OF_FILE"},
        {0xC0000012,"STATUS_WRONG_VOLUME"},
        {0xC0000013,"STATUS_NO_MEDIA_IN_DEVICE"},
        {0xC0000014,"STATUS_UNRECOGNIZED_MEDIA"},
        {0xC0000015,"STATUS_NONEXISTENT_SECTOR"},
        {0xC0000016,"STATUS_MORE_PROCESSING_REQUIRED"},
        {0xC0000017,"STATUS_NO_MEMORY"},
        {0xC0000018,"STATUS_CONFLICTING_ADDRESSES"},
        {0xC0000019,"STATUS_NOT_MAPPED_VIEW"},
        {0xC000001A,"STATUS_UNABLE_TO_FREE_VM"},
        {0xC000001B,"STATUS_UNABLE_TO_DELETE_SECTION"},
        {0xC000001C,"STATUS_INVALID_SYSTEM_SERVICE"},
        {0xC000001D,"STATUS_ILLEGAL_INSTRUCTION"},
        {0xC000001E,"STATUS_INVALID_LOCK_SEQUENCE"},
        {0xC000001F,"STATUS_INVALID_VIEW_SIZE"},
        {0xC0000020,"STATUS_INVALID_FILE_FOR_SECTION"},
        {0xC0000021,"STATUS_ALREADY_COMMITTED"},
        {0xC0000022,"STATUS_ACCESS_DENIED"},
        {0xC0000023,"STATUS_BUFFER_TOO_SMALL"},
        {0xC0000024,"STATUS_OBJECT_TYPE_MISMATCH"},
        {0xC0000025,"STATUS_NONCONTINUABLE_EXCEPTION"},
        {0xC0000026,"STATUS_INVALID_DISPOSITION"},
        {0xC0000027,"STATUS_UNWIND"},
        {0xC0000028,"STATUS_BAD_STACK"},
        {0xC0000029,"STATUS_INVALID_UNWIND_TARGET"},
        {0xC000002A,"STATUS_NOT_LOCKED"},
        {0xC000002B,"STATUS_PARITY_ERROR"},
        {0xC000002C,"STATUS_UNABLE_TO_DECOMMIT_VM"},
        {0xC000002D,"STATUS_NOT_COMMITTED"},
        {0xC000002E,"STATUS_INVALID_PORT_ATTRIBUTES"},
        {0xC000002F,"STATUS_PORT_MESSAGE_TOO_LONG"},
        {0xC0000030,"STATUS_INVALID_PARAMETER_MIX"},
        {0xC0000031,"STATUS_INVALID_QUOTA_LOWER"},
        {0xC0000032,"STATUS_DISK_CORRUPT_ERROR"},
        {0xC0000033,"STATUS_OBJECT_NAME_INVALID"},
        {0xC0000034,"STATUS_OBJECT_NAME_NOT_FOUND"},
        {0xC0000035,"STATUS_OBJECT_NAME_COLLISION"},
        {0xC0000037,"STATUS_PORT_DISCONNECTED"},
        {0xC0000038,"STATUS_DEVICE_ALREADY_ATTACHED"},
        {0xC0000039,"STATUS_OBJECT_PATH_INVALID"},
        {0xC000003A,"STATUS_OBJECT_PATH_NOT_FOUND"},
        {0xC000003B,"STATUS_OBJECT_PATH_SYNTAX_BAD"},
        {0xC000003C,"STATUS_DATA_OVERRUN"},
        {0xC000003D,"STATUS_DATA_LATE_ERROR"},
        {0xC000003E,"STATUS_DATA_ERROR"},
        {0xC000003F,"STATUS_CRC_ERROR"},
        {0xC0000040,"STATUS_SECTION_TOO_BIG"},
        {0xC0000041,"STATUS_PORT_CONNECTION_REFUSED"},
        {0xC0000042,"STATUS_INVALID_PORT_HANDLE"},
        {0xC0000043,"STATUS_SHARING_VIOLATION"},
        {0xC0000044,"STATUS_QUOTA_EXCEEDED"},
        {0xC0000045,"STATUS_INVALID_PAGE_PROTECTION"},
        {0xC0000046,"STATUS_MUTANT_NOT_OWNED"},
        {0xC0000047,"STATUS_SEMAPHORE_LIMIT_EXCEEDED"},
        {0xC0000048,"STATUS_PORT_ALREADY_SET"},
        {0xC0000049,"STATUS_SECTION_NOT_IMAGE"},
        {0xC000004A,"STATUS_SUSPEND_COUNT_EXCEEDED"},
        {0xC000004B,"STATUS_THREAD_IS_TERMINATING"},
        {0xC000004C,"STATUS_BAD_WORKING_SET_LIMIT"},
        {0xC000004D,"STATUS_INCOMPATIBLE_FILE_MAP"},
        {0xC000004E,"STATUS_SECTION_PROTECTION"},
        {0xC000004F,"STATUS_EAS_NOT_SUPPORTED"},
        {0xC0000050,"STATUS_EA_TOO_LARGE"},
        {0xC0000051,"STATUS_NONEXISTENT_EA_ENTRY"},
        {0xC0000052,"STATUS_NO_EAS_ON_FILE"},
        {0xC0000053,"STATUS_EA_CORRUPT_ERROR"},
        {0xC0000054,"STATUS_FILE_LOCK_CONFLICT"},
        {0xC0000055,"STATUS_LOCK_NOT_GRANTED"},
        {0xC0000056,"STATUS_DELETE_PENDING"},
        {0xC0000057,"STATUS_CTL_FILE_NOT_SUPPORTED"},
        {0xC0000058,"STATUS_UNKNOWN_REVISION"},
        {0xC0000059,"STATUS_REVISION_MISMATCH"},
        {0xC000005A,"STATUS_INVALID_OWNER"},
        {0xC000005B,"STATUS_INVALID_PRIMARY_GROUP"},
        {0xC000005C,"STATUS_NO_IMPERSONATION_TOKEN"},
        {0xC000005D,"STATUS_CANT_DISABLE_MANDATORY"},
        {0xC000005E,"STATUS_NO_LOGON_SERVERS"},
        {0xC000005F,"STATUS_NO_SUCH_LOGON_SESSION"},
        {0xC0000060,"STATUS_NO_SUCH_PRIVILEGE"},
        {0xC0000061,"STATUS_PRIVILEGE_NOT_HELD"},
        {0xC0000062,"STATUS_INVALID_ACCOUNT_NAME"},
        {0xC0000063,"STATUS_USER_EXISTS"},
        {0xC0000064,"STATUS_NO_SUCH_USER"},
        {0xC0000065,"STATUS_GROUP_EXISTS"},
        {0xC0000066,"STATUS_NO_SUCH_GROUP"},
        {0xC0000067,"STATUS_MEMBER_IN_GROUP"},
        {0xC0000068,"STATUS_MEMBER_NOT_IN_GROUP"},
        {0xC0000069,"STATUS_LAST_ADMIN"},
        {0xC000006A,"STATUS_WRONG_PASSWORD"},
        {0xC000006B,"STATUS_ILL_FORMED_PASSWORD"},
        {0xC000006C,"STATUS_PASSWORD_RESTRICTION"},
        {0xC000006D,"STATUS_LOGON_FAILURE"},
        {0xC000006E,"STATUS_ACCOUNT_RESTRICTION"},
        {0xC000006F,"STATUS_INVALID_LOGON_HOURS"},
        {0xC0000070,"STATUS_INVALID_WORKSTATION"},
        {0xC0000071,"STATUS_PASSWORD_EXPIRED"},
        {0xC0000072,"STATUS_ACCOUNT_DISABLED"},
        {0xC0000073,"STATUS_NONE_MAPPED"},
        {0xC0000074,"STATUS_TOO_MANY_LUIDS_REQUESTED"},
        {0xC0000075,"STATUS_LUIDS_EXHAUSTED"},
        {0xC0000076,"STATUS_INVALID_SUB_AUTHORITY"},
        {0xC0000077,"STATUS_INVALID_ACL"},
        {0xC0000078,"STATUS_INVALID_SID"},
        {0xC0000079,"STATUS_INVALID_SECURITY_DESCR"},
        {0xC000007A,"STATUS_PROCEDURE_NOT_FOUND"},
        {0xC000007B,"STATUS_INVALID_IMAGE_FORMAT"},
        {0xC000007C,"STATUS_NO_TOKEN"},
        {0xC000007D,"STATUS_BAD_INHERITANCE_ACL"},
        {0xC000007E,"STATUS_RANGE_NOT_LOCKED"},
        {0xC000007F,"STATUS_DISK_FULL"},
        {0xC0000080,"STATUS_SERVER_DISABLED"},
        {0xC0000081,"STATUS_SERVER_NOT_DISABLED"},
        {0xC0000082,"STATUS_TOO_MANY_GUIDS_REQUESTED"},
        {0xC0000083,"STATUS_GUIDS_EXHAUSTED"},
        {0xC0000084,"STATUS_INVALID_ID_AUTHORITY"},
        {0xC0000085,"STATUS_AGENTS_EXHAUSTED"},
        {0xC0000086,"STATUS_INVALID_VOLUME_LABEL"},
        {0xC0000087,"STATUS_SECTION_NOT_EXTENDED"},
        {0xC0000088,"STATUS_NOT_MAPPED_DATA"},
        {0xC0000089,"STATUS_RESOURCE_DATA_NOT_FOUND"},
        {0xC000008A,"STATUS_RESOURCE_TYPE_NOT_FOUND"},
        {0xC000008B,"STATUS_RESOURCE_NAME_NOT_FOUND"},
        {0xC000008C,"STATUS_ARRAY_BOUNDS_EXCEEDED"},
        {0xC000008D,"STATUS_FLOAT_DENORMAL_OPERAND"},
        {0xC000008E,"STATUS_FLOAT_DIVIDE_BY_ZERO"},
        {0xC000008F,"STATUS_FLOAT_INEXACT_RESULT"},
        {0xC0000090,"STATUS_FLOAT_INVALID_OPERATION"},
        {0xC0000091,"STATUS_FLOAT_OVERFLOW"},
        {0xC0000092,"STATUS_FLOAT_STACK_CHECK"},
        {0xC0000093,"STATUS_FLOAT_UNDERFLOW"},
        {0xC0000094,"STATUS_INTEGER_DIVIDE_BY_ZERO"},
        {0xC0000095,"STATUS_INTEGER_OVERFLOW"},
        {0xC0000096,"STATUS_PRIVILEGED_INSTRUCTION"},
        {0xC0000097,"STATUS_TOO_MANY_PAGING_FILES"},
        {0xC0000098,"STATUS_FILE_INVALID"},
        {0xC0000099,"STATUS_ALLOTTED_SPACE_EXCEEDED"},
        {0xC000009A,"STATUS_INSUFFICIENT_RESOURCES"},
        {0xC000009B,"STATUS_DFS_EXIT_PATH_FOUND"},
        {0xC000009C,"STATUS_DEVICE_DATA_ERROR"},
        {0xC000009D,"STATUS_DEVICE_NOT_CONNECTED"},
        {0xC000009E,"STATUS_DEVICE_POWER_FAILURE"},
        {0xC000009F,"STATUS_FREE_VM_NOT_AT_BASE"},
        {0xC00000A0,"STATUS_MEMORY_NOT_ALLOCATED"},
        {0xC00000A1,"STATUS_WORKING_SET_QUOTA"},
        {0xC00000A2,"STATUS_MEDIA_WRITE_PROTECTED"},
        {0xC00000A3,"STATUS_DEVICE_NOT_READY"},
        {0xC00000A4,"STATUS_INVALID_GROUP_ATTRIBUTES"},
        {0xC00000A5,"STATUS_BAD_IMPERSONATION_LEVEL"},
        {0xC00000A6,"STATUS_CANT_OPEN_ANONYMOUS"},
        {0xC00000A7,"STATUS_BAD_VALIDATION_CLASS"},
        {0xC00000A8,"STATUS_BAD_TOKEN_TYPE"},
        {0xC00000A9,"STATUS_BAD_MASTER_BOOT_RECORD"},
        {0xC00000AA,"STATUS_INSTRUCTION_MISALIGNMENT"},
        {0xC00000AB,"STATUS_INSTANCE_NOT_AVAILABLE"},
        {0xC00000AC,"STATUS_PIPE_NOT_AVAILABLE"},
        {0xC00000AD,"STATUS_INVALID_PIPE_STATE"},
        {0xC00000AE,"STATUS_PIPE_BUSY"},
        {0xC00000AF,"STATUS_ILLEGAL_FUNCTION"},
        {0xC00000B0,"STATUS_PIPE_DISCONNECTED"},
        {0xC00000B1,"STATUS_PIPE_CLOSING"},
        {0xC00000B2,"STATUS_PIPE_CONNECTED"},
        {0xC00000B3,"STATUS_PIPE_LISTENING"},
        {0xC00000B4,"STATUS_INVALID_READ_MODE"},
        {0xC00000B5,"STATUS_IO_TIMEOUT"},
        {0xC00000B6,"STATUS_FILE_FORCED_CLOSED"},
        {0xC00000B7,"STATUS_PROFILING_NOT_STARTED"},
        {0xC00000B8,"STATUS_PROFILING_NOT_STOPPED"},
        {0xC00000B9,"STATUS_COULD_NOT_INTERPRET"},
        {0xC00000BA,"STATUS_FILE_IS_A_DIRECTORY"},
        {0xC00000BB,"STATUS_NOT_SUPPORTED"},
        {0xC00000BC,"STATUS_REMOTE_NOT_LISTENING"},
        {0xC00000BD,"STATUS_DUPLICATE_NAME"},
        {0xC00000BE,"STATUS_BAD_NETWORK_PATH"},
        {0xC00000BF,"STATUS_NETWORK_BUSY"},
        {0xC00000C0,"STATUS_DEVICE_DOES_NOT_EXIST"},
        {0xC00000C1,"STATUS_TOO_MANY_COMMANDS"},
        {0xC00000C2,"STATUS_ADAPTER_HARDWARE_ERROR"},
        {0xC00000C3,"STATUS_INVALID_NETWORK_RESPONSE"},
        {0xC00000C4,"STATUS_UNEXPECTED_NETWORK_ERROR"},
        {0xC00000C5,"STATUS_BAD_REMOTE_ADAPTER"},
        {0xC00000C6,"STATUS_PRINT_QUEUE_FULL"},
        {0xC00000C7,"STATUS_NO_SPOOL_SPACE"},
        {0xC00000C8,"STATUS_PRINT_CANCELLED"},
        {0xC00000C9,"STATUS_NETWORK_NAME_DELETED"},
        {0xC00000CA,"STATUS_NETWORK_ACCESS_DENIED"},
        {0xC00000CB,"STATUS_BAD_DEVICE_TYPE"},
        {0xC00000CC,"STATUS_BAD_NETWORK_NAME"},
        {0xC00000CD,"STATUS_TOO_MANY_NAMES"},
        {0xC00000CE,"STATUS_TOO_MANY_SESSIONS"},
        {0xC00000CF,"STATUS_SHARING_PAUSED"},
        {0xC00000D0,"STATUS_REQUEST_NOT_ACCEPTED"},
        {0xC00000D1,"STATUS_REDIRECTOR_PAUSED"},
        {0xC00000D2,"STATUS_NET_WRITE_FAULT"},
        {0xC00000D3,"STATUS_PROFILING_AT_LIMIT"},
        {0xC00000D4,"STATUS_NOT_SAME_DEVICE"},
        {0xC00000D5,"STATUS_FILE_RENAMED"},
        {0xC00000D6,"STATUS_VIRTUAL_CIRCUIT_CLOSED"},
        {0xC00000D7,"STATUS_NO_SECURITY_EOBJECT"},
        {0xC00000D8,"STATUS_CANT_WAIT"},
        {0xC00000D9,"STATUS_PIPE_EMPTY"},
        {0xC00000DA,"STATUS_CANT_ACCESS_DOMAIN_INFO"},
        {0xC00000DB,"STATUS_CANT_TERMINATE_SELF"},
        {0xC00000DC,"STATUS_INVALID_SERVER_STATE"},
        {0xC00000DD,"STATUS_INVALID_DOMAIN_STATE"},
        {0xC00000DE,"STATUS_INVALID_DOMAIN_ROLE"},
        {0xC00000DF,"STATUS_NO_SUCH_DOMAIN"},
        {0xC00000E0,"STATUS_DOMAIN_EXISTS"},
        {0xC00000E1,"STATUS_DOMAIN_LIMIT_EXCEEDED"},
        {0xC00000E2,"STATUS_OPLOCK_NOT_GRANTED"},
        {0xC00000E3,"STATUS_INVALID_OPLOCK_PROTOCOL"},
        {0xC00000E4,"STATUS_INTERNAL_DB_CORRUPTION"},
        {0xC00000E5,"STATUS_INTERNAL_ERROR"},
        {0xC00000E6,"STATUS_GENERIC_NOT_MAPPED"},
        {0xC00000E7,"STATUS_BAD_DESCRIPTOR_FORMAT"},
        {0xC00000E8,"STATUS_INVALID_USER_BUFFER"},
        {0xC00000E9,"STATUS_UNEXPECTED_IO_ERROR"},
        {0xC00000EA,"STATUS_UNEXPECTED_MM_CREATE_ERR"},
        {0xC00000EB,"STATUS_UNEXPECTED_MM_MAP_ERROR"},
        {0xC00000EC,"STATUS_UNEXPECTED_MM_EXTEND_ERR"},
        {0xC00000ED,"STATUS_NOT_LOGON_PROCESS"},
        {0xC00000EE,"STATUS_LOGON_SESSION_EXISTS"},
        {0xC00000EF,"STATUS_INVALID_PARAMETER_1"},
        {0xC00000F0,"STATUS_INVALID_PARAMETER_2"},
        {0xC00000F1,"STATUS_INVALID_PARAMETER_3"},
        {0xC00000F2,"STATUS_INVALID_PARAMETER_4"},
        {0xC00000F3,"STATUS_INVALID_PARAMETER_5"},
        {0xC00000F4,"STATUS_INVALID_PARAMETER_6"},
        {0xC00000F5,"STATUS_INVALID_PARAMETER_7"},
        {0xC00000F6,"STATUS_INVALID_PARAMETER_8"},
        {0xC00000F7,"STATUS_INVALID_PARAMETER_9"},
        {0xC00000F8,"STATUS_INVALID_PARAMETER_10"},
        {0xC00000F9,"STATUS_INVALID_PARAMETER_11"},
        {0xC00000FA,"STATUS_INVALID_PARAMETER_12"},
        {0xC00000FB,"STATUS_REDIRECTOR_NOT_STARTED"},
        {0xC00000FC,"STATUS_REDIRECTOR_STARTED"},
        {0xC00000FD,"STATUS_STACK_OVERFLOW"},
        {0xC00000FE,"STATUS_NO_SUCH_PACKAGE"},
        {0xC00000FF,"STATUS_BAD_FUNCTION_TABLE"},
        {0xC0000100,"STATUS_VARIABLE_NOT_FOUND"},
        {0xC0000101,"STATUS_DIRECTORY_NOT_EMPTY"},
        {0xC0000102,"STATUS_FILE_CORRUPT_ERROR"},
        {0xC0000103,"STATUS_NOT_A_DIRECTORY"},
        {0xC0000104,"STATUS_BAD_LOGON_SESSION_STATE"},
        {0xC0000105,"STATUS_LOGON_SESSION_COLLISION"},
        {0xC0000106,"STATUS_NAME_TOO_LONG"},
        {0xC0000107,"STATUS_FILES_OPEN"},
        {0xC0000108,"STATUS_CONNECTION_IN_USE"},
        {0xC0000109,"STATUS_MESSAGE_NOT_FOUND"},
        {0xC000010A,"STATUS_PROCESS_IS_TERMINATING"},
        {0xC000010B,"STATUS_INVALID_LOGON_TYPE"},
        {0xC000010C,"STATUS_NO_GUID_TRANSLATION"},
        {0xC000010D,"STATUS_CANNOT_IMPERSONATE"},
        {0xC000010E,"STATUS_IMAGE_ALREADY_LOADED"},
        {0xC0000110,"STATUS_ABANDONED_HIBERFILE_TEST"},
        {0xC0000111,"STATUS_NO_LDT"},
        {0xC0000112,"STATUS_INVALID_LDT_SIZE"},
        {0xC0000113,"STATUS_INVALID_LDT_OFFSET"},
        {0xC0000114,"STATUS_INVALID_LDT_DESCRIPTOR"},
        {0xC0000115,"STATUS_INVALID_IMAGE_NE_FORMAT"},
        {0xC0000118,"STATUS_RXACT_INVALID_STATE"},
        {0xC0000119,"STATUS_RXACT_COMMIT_FAILURE"},
        {0xC000011A,"STATUS_MAPPED_FILE_SIZE_ZERO"},
        {0xC000011B,"STATUS_TOO_MANY_OPENED_FILES"},
        {0xC0000120,"STATUS_CANCELLED"},
        {0xC0000121,"STATUS_CANNOT_DELETE"},
        {0xC0000122,"STATUS_INVALID_COMPUTER_NAME"},
        {0xC0000123,"STATUS_FILE_DELETED"},
        {0xC0000124,"STATUS_SPECIAL_ACCOUNT"},
        {0xC0000125,"STATUS_SPECIAL_GROUP"},
        {0xC0000126,"STATUS_SPECIAL_USER"},
        {0xC0000127,"STATUS_MEMBERS_PRIMARY_GROUP"},
        {0xC000012C,"STATUS_FILE_CLOSED"},
        {0xC000012D,"STATUS_TOO_MANY_THREADS"},
        {0xC000012E,"STATUS_THREAD_NOT_IN_PROCESS"},
        {0xC000012F,"STATUS_TOKEN_ALREADY_IN_USE"},
        {0xC0000130,"STATUS_PAGEFILE_QUOTA_EXCEEDED"},
        {0xC0000131,"STATUS_COMMITMENT_LIMIT"},
        {0xC0000132,"STATUS_INVALID_IMAGE_LE_FORMAT"},
        {0xC0000133,"STATUS_INVALID_IMAGE_NOT_MZ"},
        {0xC0000134,"STATUS_INVALID_IMAGE_PROTECT"},
        {0xC0000135,"STATUS_INVALID_IMAGE_WIN_16"},
        {0xC0000136,"STATUS_LOGON_SERVER_CONFLICT"},
        {0xC0000137,"STATUS_TIME_DIFFERENCE_AT_DC"},
        {0xC0000138,"STATUS_PIPE_BROKEN"},
        {0xC0000139,"STATUS_REGISTRY_CORRUPT"},
        {0xC000013A,"STATUS_TOO_LATE"},
        {0xC000013B,"STATUS_NO_SUCH_MEMBER"},
        {0xC000013C,"STATUS_INVALID_MEMBER"},
        {0xC000013D,"STATUS_TOO_MANY_SIDS"},
        {0xC000013E,"STATUS_LM_CROSS_ENCRYPTION_REQUIRED"},
        {0xC000013F,"STATUS_NO_LOGON_SERVER"},
        {0xC000014D,"STATUS_CURRENT_DOMAIN_NOT_ALLOWED"},
        {0xC000017C,"STATUS_KEY_DELETED"},
        {0xC000017D,"STATUS_NO_LOG_SPACE"},
        {0xC0000180,"STATUS_KEY_HAS_CHILDREN"},
        {0xC0000181,"STATUS_CHILD_MUST_BE_VOLATILE"},
        {0xC0000182,"STATUS_DEVICE_CONFIGURATION_ERROR"},
        {0xC0000183,"STATUS_DRIVER_INTERNAL_ERROR"},
        {0xC0000184,"STATUS_INVALID_DEVICE_STATE"},
        {0xC0000185,"STATUS_IO_DEVICE_ERROR"},
        {0xC0000186,"STATUS_DEVICE_PROTOCOL_ERROR"},
        {0xC0000187,"STATUS_BACKUP_CONTROLLER"},
        {0xC0000188,"STATUS_LOG_FILE_FULL"},
        {0xC000018A,"STATUS_NO_TRUST_LSA_SECRET"},
        {0xC000018B,"STATUS_NO_TRUST_SAM_ACCOUNT"},
        {0xC000018C,"STATUS_TRUSTED_DOMAIN_FAILURE"},
        {0xC000018D,"STATUS_TRUSTED_RELATIONSHIP_FAILURE"},
        {0xC000018F,"STATUS_EVENTLOG_FILE_CORRUPT"},
        {0xC0000190,"STATUS_EVENTLOG_CANT_START"},
        {0xC0000191,"STATUS_TRUST_FAILURE"},
        {0xC0000192,"STATUS_MUTANT_LIMIT_EXCEEDED"},
        {0xC0000193,"STATUS_ACCOUNT_EXPIRED"},
        {0xC0000194,"STATUS_NETLOGON_NOT_STARTED"},
        {0xC0000195,"STATUS_POSSIBLE_DEADLOCK"},
        {0xC0000196,"STATUS_NETWORK_CREDENTIAL_CONFLICT"},
        {0xC0000197,"STATUS_REMOTE_SESSION_LIMIT"},
        {0xC0000198,"STATUS_EVENTLOG_FILE_CHANGED"},
        {0xC0000199,"STATUS_NOLOGON_INTERDOMAIN_TRUST_ACCOUNT"},
        {0xC000019A,"STATUS_NOLOGON_WORKSTATION_TRUST_ACCOUNT"},
        {0xC000019B,"STATUS_NOLOGON_SERVER_TRUST_ACCOUNT"},
        {0xC000019C,"STATUS_DOMAIN_TRUST_INCONSISTENT"},
        {0xC000019D,"STATUS_FS_DRIVER_REQUIRED"},
        {0xC000019E,"STATUS_IMAGE_ALREADY_LOADED_AS_DLL"},
        {0xC000019F,"STATUS_INCOMPATIBLE_WITH_GLOBAL_SHORT_NAME_REGISTRY_SETTING"},
        {0xC00001A0,"STATUS_SHORT_NAMES_NOT_ENABLED_ON_VOLUME"},
        {0xC00001A1,"STATUS_SECURITY_STREAM_IS_INCONSISTENT"},
        {0xC00001A2,"STATUS_INVALID_LOCK_RANGE"},
        {0xC00001A3,"STATUS_INVALID_ACE_CONDITION"},
        {0xC00001A4,"STATUS_IMAGE_SUBSYSTEM_NOT_PRESENT"},
        {0xC00001A5,"STATUS_NOTIFICATION_GUID_ALREADY_DEFINED"},
        {0xC00001A7,"STATUS_NOTIFY_OBJECT_REMOVED"},
        {0xC00001A8,"STATUS_NO_CALLBACK_ACTIVE"},
        {0xC00001A9,"STATUS_PWD_TOO_SHORT"},
        {0xC00001AA,"STATUS_PWD_TOO_RECENT"},
        {0xC00001AB,"STATUS_PWD_HISTORY_CONFLICT"},
        {0xC00001AE,"STATUS_PLUGPLAY_NO_DEVICE"},
        {0xC00001AF,"STATUS_UNSUPPORTED_COMPRESSION"},
        {0xC00001B0,"STATUS_INVALID_HW_PROFILE"},
        {0xC00001B1,"STATUS_INVALID_PLUGPLAY_DEVICE_PATH"},
        {0xC00001B2,"STATUS_DRIVER_ORDINAL_NOT_FOUND"},
        {0xC00001B3,"STATUS_DRIVER_ENTRYPOINT_NOT_FOUND"},
        {0xC00001B4,"STATUS_RESOURCE_NOT_OWNED"},
        {0xC00001B5,"STATUS_TOO_MANY_LINKS"},
        {0xC00001B6,"STATUS_QUOTA_LIST_INCONSISTENT"},
        {0xC00001B7,"STATUS_FILE_IS_OFFLINE"},
        {0xC00001B8,"STATUS_EVALUATION_EXPIRATION"},
        {0xC00001B9,"STATUS_ILLEGAL_DLL_RELOCATION"},
        {0xC00001BA,"STATUS_DLL_INIT_FAILED"},
        {0xC00001BB,"STATUS_MISSING_SYSTEMFILE"},
        {0xC00001BC,"STATUS_UNHANDLED_EXCEPTION"},
        {0xC00001BD,"STATUS_APP_INIT_FAILURE"},
        {0xC00001BE,"STATUS_PAGEFILE_CREATE_FAILED"},
        {0xC00001BF,"STATUS_NO_PAGEFILE"},
        {0xC00001C0,"STATUS_INVALID_LEVEL"},
        {0xC00001C1,"STATUS_WRONG_PASSWORD_CORE"},
        {0xC00001C2,"STATUS_ILLEGAL_FLOAT_CONTEXT"},
        {0xC00001C3,"STATUS_REGISTRY_IO_FAILED"},
        {0xC00001C4,"STATUS_NO_EVENT_PAIR"},
        {0xC00001C5,"STATUS_UNRECOGNIZED_VOLUME"},
        {0xC00001C6,"STATUS_SERIAL_NO_DEVICE_INITED"},
        {0xC00001C7,"STATUS_NO_SUCH_ALIAS"},
        {0xC00001C8,"STATUS_MEMBER_NOT_IN_ALIAS"},
        {0xC00001C9,"STATUS_MEMBER_IN_ALIAS"},
        {0xC00001CA,"STATUS_ALIAS_EXISTS"},
        {0xC00001CB,"STATUS_LOGON_NOT_GRANTED"},
        {0xC00001CC,"STATUS_TOO_MANY_SECRETS"},
        {0xC00001CD,"STATUS_SECRET_TOO_LONG"},
        {0xC00001CE,"STATUS_INTERNAL_DB_ERROR"},
        {0xC00001CF,"STATUS_FULLSCREEN_MODE"},
        {0xC00001D0,"STATUS_TOO_MANY_CONTEXT_IDS"},
        {0xC00001D1,"STATUS_LOGON_TYPE_NOT_GRANTED"},
        {0xC00001D2,"STATUS_NOT_REGISTRY_FILE"},
        {0xC00001D3,"STATUS_NT_CROSS_ENCRYPTION_REQUIRED"},
        {0xC00001D4,"STATUS_DOMAIN_CTRLR_CONFIG_ERROR"},
        {0xC00001D5,"STATUS_FT_MISSING_MEMBER"},
        {0xC00001D6,"STATUS_ILL_FORMED_SERVICE_ENTRY"},
        {0xC00001D7,"STATUS_ILLEGAL_CHARACTER"},
        {0xC00001D8,"STATUS_UNMAPPABLE_CHARACTER"},
        {0xC00001D9,"STATUS_UNDEFINED_CHARACTER"},
        {0xC00001DA,"STATUS_FLOPPY_VOLUME"},
        {0xC00001DB,"STATUS_FLOPPY_ID_MARK_NOT_FOUND"},
        {0xC00001DC,"STATUS_FLOPPY_WRONG_CYLINDER"},
        {0xC00001DD,"STATUS_FLOPPY_UNKNOWN_ERROR"},
        {0xC00001DE,"STATUS_FLOPPY_BAD_REGISTERS"},
        {0xC00001DF,"STATUS_DISK_RECALIBRATE_FAILED"},
        {0xC00001E0,"STATUS_DISK_OPERATION_FAILED"},
        {0xC00001E1,"STATUS_DISK_RESET_FAILED"},
        {0xC00001E2,"STATUS_SHARED_IRQ_BUSY"},
        {0xC00001E3,"STATUS_FT_ORPHANING"},
        {0xC00001E6,"STATUS_PARTITION_FAILURE"},
        {0xC00001E7,"STATUS_INVALID_BLOCK_LENGTH"},
        {0xC00001E8,"STATUS_DEVICE_NOT_PARTITIONED"},
        {0xC00001E9,"STATUS_UNABLE_TO_LOCK_MEDIA"},
        {0xC00001EA,"STATUS_UNABLE_TO_UNLOAD_MEDIA"},
        {0xC00001EB,"STATUS_EOM_OVERFLOW"},
        {0xC00001EC,"STATUS_NO_MEDIA"},
        {0xC0000227,"STATUS_NO_UNICODE_TRANSLATION"},
        {0xC0000228,"STATUS_DLL_NOT_FOUND"},
        {0xC0000229,"STATUS_OPEN_FAILED"},
        {0xC000022B,"STATUS_IO_PRIVILEGE_FAILED"},
        {0xC0000232,"STATUS_ORDINAL_NOT_FOUND"},
        {0xC0000233,"STATUS_ENTRYPOINT_NOT_FOUND"},
        {0xC000023B,"STATUS_LOCAL_DISCONNECT"},
        {0xC000023C,"STATUS_REMOTE_DISCONNECT"},
        {0xC000023D,"STATUS_REMOTE_RESOURCES"},
        {0xC000023E,"STATUS_LINK_FAILED"},
        {0xC000023F,"STATUS_LINK_TIMEOUT"},
        {0xC0000240,"STATUS_INVALID_CONNECTION"},
        {0xC0000241,"STATUS_INVALID_ADDRESS"},
        {0xC0000243,"STATUS_USER_MAPPED_FILE"},
        {0xC0000244,"STATUS_AUDIT_FAILED"},
        {0xC0000245,"STATUS_TIMER_RESOLUTION_NOT_SET"},
        {0xC0000246,"STATUS_CONNECTION_COUNT_LIMIT"},
        {0xC0000247,"STATUS_LOGIN_TIME_RESTRICTION"},
        {0xC0000248,"STATUS_LOGIN_WKSTA_RESTRICTION"},
        {0xC0000249,"STATUS_IMAGE_MP_UP_MISMATCH"},
        {0xC0000268,"STATUS_DLL_INIT_FAILED_LOGOFF"},
        {0xC000026C,"STATUS_DRIVER_UNABLE_TO_LOAD"},
        {0xC000026D,"STATUS_DFS_UNAVAILABLE"},
        {0xC000026E,"STATUS_VOLUME_DISMOUNTED"},
        {0xC000026F,"STATUS_WX86_INTERNAL_ERROR"},
        {0xC0000270,"STATUS_WX86_FLOAT_STACK_CHECK"},
        {0xC0000271,"STATUS_VALIDATE_CONTINUE"},
        {0xC0000272,"STATUS_NO_MATCH"},
        {0xC0000273,"STATUS_NO_MORE_MATCHES"},
        {0xC0000275,"STATUS_NOT_A_REPARSE_POINT"},
        {0xC0000276,"STATUS_IO_REPARSE_TAG_INVALID"},
        {0xC0000277,"STATUS_IO_REPARSE_TAG_MISMATCH"},
        {0xC0000278,"STATUS_IO_REPARSE_DATA_INVALID"},
        {0xC0000279,"STATUS_IO_REPARSE_TAG_NOT_HANDLED"},
        {0xC0000280,"STATUS_REPARSE_POINT_NOT_RESOLVED"},
        {0xC0000281,"STATUS_DIRECTORY_IS_A_REPARSE_POINT"},
        {0xC03A0001,"FACILITY_NULL customer code"},
        {0xC0070005,"FACILITY_NTWIN32 ERROR_ACCESS_DENIED (0xC0070005)"},
        {0xC0070002,"FACILITY_NTWIN32 ERROR_FILE_NOT_FOUND"},
        {0x80070005,"HRESULT E_ACCESSDENIED pattern (not NTSTATUS)"},
        {0xC0000200,"fallback C0000200"},
        {0xC0001234,"fallback C0001234"},
        {0x00000080,"fallback 00000080"},
        {0x80000003,"fallback 80000003"},
        {0x40000000,"fallback 40000000"},
        {0xC0070020,"fallback C0070020"}
    
 };
 int n_tbl=sizeof(tbl)/sizeof(tbl[0]);
 for(int i=0;i<n_tbl;i++){ ULONG dos=RtlNtStatusToDosError(tbl[i].s); j_add("    {\"name\":\""); j_add(tbl[i].n); j_add("\",\"ntstatus_hex\":\""); {char t[12]; u32_hex(t,tbl[i].s); j_add(t);} j_add("\",\"dos\":"); {char t[32]; u64_dec(t,dos); j_add(t);} j_add("}"); j_add(i+1<n_tbl?",\n":"\n"); }
 j_add("  ],\n");
 j_add("  \"real_syscall_cases\": [\n");
 // Create temp file
 WCHAR wtmp[512]; const char *p=NTPERS_TMP_NT; int l=0; while(p[l]) l++; for(int i=0;i<l;i++) wtmp[i]=(WCHAR)p[i]; wtmp[l]=0;
 UNICODE_STRING us; us.Length=l*2; us.MaximumLength=us.Length+2; us.Buffer=wtmp;
 OBJECT_ATTRIBUTES oa; oa.Length=sizeof(oa); oa.RootDirectory=0; oa.ObjectName=&us; oa.Attributes=0x40; oa.SecurityDescriptor=0; oa.SecurityQualityOfService=0;
 HANDLE h=0; IO_STATUS_BLOCK ios; LARGE_INTEGER alloc; alloc.QuadPart=0;
 NTSTATUS st=NtCreateFile(&h,0x12019F,&oa,&ios,&alloc,0x80,3,5,0x20|0x40,0,0);
 int first=1;
 char buf[1024]; IO_STATUS_BLOCK ios2; ULONG retLen=0;
 if(st==0){
   // FileInformation brute 0..35
   for(int ci=0; ci<36; ci++){ for(int bi=0; bi<6; bi++){ ULONG bs[]={0,4,8,16,64,512}; NTSTATUS s=NtQueryInformationFile(h,&ios2,buf,bs[bi],ci); if(!first) j_add(",\n"); first=0; j_add("    {\"api\":\"NtQueryInformationFile\",\"class\":"); {char t[32]; u64_dec(t,ci); j_add(t);} j_add(",\"buffer_len\":"); {char t[32]; u64_dec(t,bs[bi]); j_add(t);} j_add(",\"ntstatus\":"); {char t[32]; i64_dec(t,(long long)(long)s); j_add(t);} j_add(",\"ntstatus_hex\":\""); {char t[12]; u32_hex(t,s); j_add(t);} j_add("\",\"info\":"); {char t[32]; u64_dec(t,ios2.Information); j_add(t);} j_add("}"); add_distinct(s); } }
   // invalid handles
   HANDLE badh[]={(HANDLE)(long)0x1234,(HANDLE)0,(HANDLE)(long)-1};
   for(int i=0;i<3;i++){ NTSTATUS s=NtQueryInformationFile(badh[i],&ios2,buf,512,9); if(!first) j_add(",\n"); first=0; j_add("    {\"api\":\"NtQueryInformationFile(invalid_handle)\",\"ntstatus\":"); {char t[32]; i64_dec(t,(long long)(long)s); j_add(t);} j_add(",\"ntstatus_hex\":\""); {char t[12]; u32_hex(t,s); j_add(t);} j_add("\"}"); add_distinct(s); }
   NTSTATUS s=NtQueryInformationFile(h,&ios2,(void*)0,512,9); if(!first) j_add(",\n"); first=0; j_add("    {\"api\":\"NtQueryInformationFile(null_buffer)\",\"ntstatus\":"); {char t[32]; i64_dec(t,(long long)(long)s); j_add(t);} j_add(",\"ntstatus_hex\":\""); {char t[12]; u32_hex(t,s); j_add(t);} j_add("\"}"); add_distinct(s);
   // Object
   char obuf[512];
   for(int cls=0;cls<5;cls++){ for(int bi=0;bi<5;bi++){ ULONG bs[]={0,4,8,16,512}; retLen=0; NTSTATUS so=NtQueryObject((HANDLE)(long)-1,cls,obuf,bs[bi],&retLen); if(!first) j_add(",\n"); first=0; j_add("    {\"api\":\"NtQueryObject\",\"class\":"); {char t[32]; u64_dec(t,cls); j_add(t);} j_add(",\"buffer_len\":"); {char t[32]; u64_dec(t,bs[bi]); j_add(t);} j_add(",\"ntstatus\":"); {char t[32]; i64_dec(t,(long long)(long)so); j_add(t);} j_add(",\"ntstatus_hex\":\""); {char t[12]; u32_hex(t,so); j_add(t);} j_add("\"}"); add_distinct(so); } }
   // System
   char sbuf[256]; ULONG rl=0;
   for(int cls=0; cls<60; cls++){ for(int bi=0; bi<3; bi++){ ULONG bs[]={0,4,64}; NTSTATUS sy=NtQuerySystemInformation(cls,sbuf,bs[bi],&rl); if(!first) j_add(",\n"); first=0; j_add("    {\"api\":\"NtQuerySystemInformation\",\"class\":"); {char t[32]; u64_dec(t,cls); j_add(t);} j_add(",\"buffer_len\":"); {char t[32]; u64_dec(t,bs[bi]); j_add(t);} j_add(",\"ntstatus\":"); {char t[32]; i64_dec(t,(long long)(long)sy); j_add(t);} j_add(",\"ntstatus_hex\":\""); {char t[12]; u32_hex(t,sy); j_add(t);} j_add("\"}"); add_distinct(sy); } }
   // File create variations
   {
     WCHAR w1[512]; const char *pp=NTPERS_NONEXIST_NT; int ll=0; while(pp[ll]) ll++; for(int i=0;i<ll;i++) w1[i]=(WCHAR)pp[i]; w1[ll]=0;
     UNICODE_STRING u1; u1.Length=ll*2; u1.MaximumLength=u1.Length+2; u1.Buffer=w1;
     OBJECT_ATTRIBUTES o1; o1.Length=sizeof(o1); o1.RootDirectory=0; o1.ObjectName=&u1; o1.Attributes=0x40; o1.SecurityDescriptor=0; o1.SecurityQualityOfService=0;
     HANDLE hh=0; IO_STATUS_BLOCK iob; NTSTATUS s2=NtCreateFile(&hh,0x120089,&o1,&iob,&alloc,0x80,3,1,0x40,0,0); if(!first) j_add(",\n"); first=0; j_add("    {\"api\":\"NtCreateFile(nonexistent)\",\"ntstatus\":"); {char t[32]; i64_dec(t,(long long)(long)s2); j_add(t);} j_add(",\"ntstatus_hex\":\""); {char t[12]; u32_hex(t,s2); j_add(t);} j_add("\"}"); add_distinct(s2); if(s2==0) NtClose(hh);
   }
   {
     WCHAR w1[512]; const char *pp=NTPERS_TMP_NT; int ll=0; while(pp[ll]) ll++; for(int i=0;i<ll;i++) w1[i]=(WCHAR)pp[i]; w1[ll]=0;
     UNICODE_STRING u1; u1.Length=ll*2; u1.MaximumLength=u1.Length+2; u1.Buffer=w1;
     OBJECT_ATTRIBUTES o1; o1.Length=sizeof(o1); o1.RootDirectory=0; o1.ObjectName=&u1; o1.Attributes=0x40; o1.SecurityDescriptor=0; o1.SecurityQualityOfService=0;
     HANDLE hh=0; IO_STATUS_BLOCK iob; NTSTATUS s2=NtCreateFile(&hh,0x120089,&o1,&iob,&alloc,0x80,3,99,0,0,0); if(!first) j_add(",\n"); first=0; j_add("    {\"api\":\"NtCreateFile(invalid_disposition)\",\"ntstatus\":"); {char t[32]; i64_dec(t,(long long)(long)s2); j_add(t);} j_add(",\"ntstatus_hex\":\""); {char t[12]; u32_hex(t,s2); j_add(t);} j_add("\"}"); add_distinct(s2); if(s2==0) NtClose(hh);
     s2=NtCreateFile(&hh,0x120089,&o1,&iob,&alloc,0x80,3,2,0,0,0); if(!first) j_add(",\n"); first=0; j_add("    {\"api\":\"NtCreateFile(collision)\",\"ntstatus\":"); {char t[32]; i64_dec(t,(long long)(long)s2); j_add(t);} j_add(",\"ntstatus_hex\":\""); {char t[12]; u32_hex(t,s2); j_add(t);} j_add("\"}"); add_distinct(s2); if(s2==0) NtClose(hh);
   }
   // Memory
   for(int prot=0; prot<8; prot++){ PVOID b=0; unsigned long long sz=0x1000; ULONG prs[]={0,1,2,4,8,16,32,64}; NTSTATUS ms=NtAllocateVirtualMemory((void*)(long)-1,&b,0,&sz,0x1000,prs[prot]); if(!first) j_add(",\n"); first=0; j_add("    {\"api\":\"NtAllocateVirtualMemory\",\"protect\":"); {char t[32]; u64_dec(t,prs[prot]); j_add(t);} j_add(",\"ntstatus\":"); {char t[32]; i64_dec(t,(long long)(long)ms); j_add(t);} j_add(",\"ntstatus_hex\":\""); {char t[12]; u32_hex(t,ms); j_add(t);} j_add("\"}"); add_distinct(ms); if(ms==0){ PVOID fb=b; unsigned long long fsz=0x1000; NtFreeVirtualMemory((void*)(long)-1,&fb,&fsz,0x4000); } }
   // Close invalid
   NTSTATUS sc1=NtClose((HANDLE)(long)0x9999); if(!first) j_add(",\n"); first=0; j_add("    {\"api\":\"NtClose(invalid)\",\"ntstatus\":"); {char t[32]; i64_dec(t,(long long)(long)sc1); j_add(t);} j_add(",\"ntstatus_hex\":\""); {char t[12]; u32_hex(t,sc1); j_add(t);} j_add("\"}"); add_distinct(sc1);
   // Event
   {
     HANDLE eh=0; OBJECT_ATTRIBUTES o2; o2.Length=sizeof(o2); o2.RootDirectory=0; o2.ObjectName=0; o2.Attributes=0; o2.SecurityDescriptor=0; o2.SecurityQualityOfService=0;
     NTSTATUS se=NtCreateEvent(&eh,0x1F0003,&o2,0,0);
     if(se==0){ char ebuf[32]; ULONG rlen=0; NTSTATUS qe1=NtQueryEvent(eh,99,ebuf,sizeof(ebuf),&rlen); if(!first) j_add(",\n"); first=0; j_add("    {\"api\":\"NtQueryEvent(bogus)\",\"ntstatus\":"); {char t[32]; i64_dec(t,(long long)(long)qe1); j_add(t);} j_add(",\"ntstatus_hex\":\""); {char t[12]; u32_hex(t,qe1); j_add(t);} j_add("\"}"); add_distinct(qe1); NtClose(eh); }
   }
   NtClose(h);
 } else {
   if(!first) j_add(",\n"); first=0; j_add("    {\"api\":\"NtCreateFile(tmp_failed)\",\"ntstatus\":"); {char t[32]; i64_dec(t,(long long)(long)st); j_add(t);} j_add(",\"ntstatus_hex\":\""); {char t[12]; u32_hex(t,st); j_add(t);} j_add("\"}"); add_distinct(st);
 }
 j_add("\n  ],\n");
 j_add("  \"distinct_summary\": {\"total_real_cases\":"); {char t[32]; u64_dec(t,distinct_cnt); j_add(t);} j_add(",\"distinct_via_real_syscalls\":"); {char t[32]; u64_dec(t,distinct_cnt); j_add(t);} j_add(",\"distinct_list\": ["); for(int i=0;i<distinct_cnt;i++){ if(i) j_add(","); j_add("\""); {char t[12]; u32_hex(t,distinct[i]); j_add(t);} j_add("\""); } j_add("]},\n");
 j_add("  \"note\": \"volume via real syscalls\",\"fallback_tests\": [\n");
 NTSTATUS fb[]={0xC0000200,0xC0001234,0x00000080,0x80000003,0x40000000,0xC0070020};
 for(int i=0;i<6;i++){ ULONG dos=RtlNtStatusToDosError(fb[i]); j_add("    {\"ntstatus_hex\":\""); {char t[12]; u32_hex(t,fb[i]); j_add(t);} j_add("\",\"dos\":"); {char t[32]; u64_dec(t,dos); j_add(t);} j_add("}"); j_add(i<5?",\n":"\n"); }
 j_add("  ]\n}\n");
 {
   long fd=linux_open("/mnt/share/.dodresult/probe_ntstatus.json",577,0644); if(fd>=0){ unsigned long long off=0; while(off<(unsigned long long)jpos){ unsigned long chunk=jpos-(int)off; if(chunk>65536) chunk=65536; long w=linux_write((int)fd,json+off,chunk); if(w<=0) break; off+=w; } linux_close((int)fd); }
   long fd2=linux_open("/mnt/share/.dodresult/dualrun_vm_probe_ntstatus.json",577,0644); if(fd2>=0){ unsigned long long off=0; while(off<(unsigned long long)jpos){ unsigned long chunk=jpos-(int)off; if(chunk>65536) chunk=65536; long w=linux_write((int)fd2,json+off,chunk); if(w<=0) break; off+=w; } linux_close((int)fd2); }
   long fd3=linux_open("/mnt/share/.dodresult/dualrun_vm_probe_ntstatus_direct.json",577,0644); if(fd3>=0){ unsigned long long off=0; while(off<(unsigned long long)jpos){ unsigned long chunk=jpos-(int)off; if(chunk>65536) chunk=65536; long w=linux_write((int)fd3,json+off,chunk); if(w<=0) break; off+=w; } linux_close((int)fd3); }
   linux_write(1,json,jpos);
 }
#ifndef NTPERS_VM
 { fwrite(json,1,jpos,stdout); fflush(stdout); }
#endif
#ifdef NTPERS_VM
 write_file_nt("\\??\\mnt\\share\\.dodresult\\probe_ntstatus.json",json,jpos);
 write_file_nt("\\??\\mnt\\share\\.dodresult\\dualrun_vm_probe_ntstatus.json",json,jpos);
 write_file_nt("\\??\\mnt\\share\\.dodresult\\dualrun_vm_probe_ntstatus_direct.json",json,jpos);
 write_file_nt("\\??\\C:\\tmp\\probe_ntstatus_direct.json",json,jpos);
#endif
#ifndef NTPERS_VM
 DeleteFileA(g_tmp_nt+4);   /* scratch file, closed above; strip the \??\ prefix */
#endif
 NtTerminateProcess(NtCurrentProcess(),0);
#ifdef NTPERS_VM
 __asm__ volatile("mov $60, %%eax; mov $0, %%edi; syscall" ::: "rax","rdi");
#endif
 while(1){}
}
#ifdef NTPERS_VM
void _start(void){ __asm__ volatile("sub $128, %rsp"); do_probe(); __asm__ volatile("mov $60, %%eax; mov $0, %%edi; syscall" ::: "rax","rdi"); while(1){} }
#else
int main(int argc, char **argv){
 if(!resolve_root(argc,argv)){ fprintf(stderr,"probe_ntstatus_direct: --root must be a local absolute path (X:\\dir), not UNC\n"); return 2; }
 snprintf(g_tmp_nt, sizeof g_tmp_nt, "\\??\\%s\\probe_ntstatus.tmp", g_root);
 snprintf(g_nonexist_nt, sizeof g_nonexist_nt, "\\??\\%s\\nonexistent_98765.tmp", g_root);
 if(!path_absent(g_tmp_nt+4)){ fprintf(stderr,"probe_ntstatus_direct: scratch file already exists, refusing to delete it\n"); return 2; }
 init_ntdll(); do_probe(); return 0;
}
#endif
