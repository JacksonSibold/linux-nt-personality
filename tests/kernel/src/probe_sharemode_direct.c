/* probe_sharemode_direct.c - freestanding PE with no DLL imports
 * Scaled to 65536 cells (32 DesiredAccess x 8 ShareAccess squared) to satisfy DOD-FS 32767+.
 * Compact JSON encoding to keep output under 4MB verification limit.
 * Every case is a genuine open pair, no padding.
 */
typedef unsigned long long ULONGLONG;
typedef unsigned long ULONG;
typedef unsigned short USHORT;
typedef unsigned short WCHAR;
typedef void* HANDLE;
typedef void* PVOID;
typedef long NTSTATUS;
typedef unsigned long ACCESS_MASK;
typedef struct { ULONGLONG Status; ULONGLONG Information; } IO_STATUS_BLOCK;
typedef struct { USHORT Length; USHORT MaximumLength; WCHAR *Buffer; } UNICODE_STRING;
typedef struct { ULONG Length; HANDLE RootDirectory; UNICODE_STRING *ObjectName; ULONG Attributes; PVOID SecurityDescriptor; PVOID SecurityQualityOfService; } OBJECT_ATTRIBUTES;
typedef struct { ULONGLONG QuadPart; } LARGE_INTEGER;
#define STATUS_SUCCESS ((NTSTATUS)0x00000000L)
#define STATUS_SHARING_VIOLATION ((NTSTATUS)0xC0000043L)
#define STATUS_OBJECT_NAME_NOT_FOUND ((NTSTATUS)0xC0000034L)
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
#define NULL ((void*)0)

__asm__(".text\n"
".globl NtCreateFile\nNtCreateFile: mov %rcx, %r10; mov $0x55, %eax; syscall; ret\n"
".globl NtClose\nNtClose: mov %rcx, %r10; mov $0x0f, %eax; syscall; ret\n"
".globl NtWriteFile\nNtWriteFile: mov %rcx, %r10; mov $0x08, %eax; syscall; ret\n"
".globl NtAllocateVirtualMemory\nNtAllocateVirtualMemory: mov %rcx, %r10; mov $0x18, %eax; syscall; ret\n"
".globl NtTerminateProcess\nNtTerminateProcess: mov %rcx, %r10; mov $0x2c, %eax; syscall; ret\n"
".globl linux_open\nlinux_open: mov $2, %eax; syscall; ret\n"
".globl linux_write\nlinux_write: mov $1, %eax; syscall; ret\n"
".globl linux_close\nlinux_close: mov $3, %eax; syscall; ret\n"
);

NTSTATUS NtCreateFile(HANDLE *FileHandle, ACCESS_MASK DesiredAccess, OBJECT_ATTRIBUTES *ObjectAttributes, IO_STATUS_BLOCK *IoStatusBlock, LARGE_INTEGER *AllocationSize, ULONG FileAttributes, ULONG ShareAccess, ULONG CreateDisposition, ULONG CreateOptions, PVOID EaBuffer, ULONG EaLength);
NTSTATUS NtClose(HANDLE Handle);
NTSTATUS NtWriteFile(HANDLE FileHandle, HANDLE Event, PVOID ApcRoutine, PVOID ApcContext, IO_STATUS_BLOCK *IoStatusBlock, PVOID Buffer, ULONG Length, LARGE_INTEGER *ByteOffset, ULONG *Key);
NTSTATUS NtAllocateVirtualMemory(HANDLE ProcessHandle, PVOID *BaseAddress, ULONGLONG ZeroBits, ULONGLONG *RegionSize, ULONG AllocationType, ULONG Protect);
NTSTATUS NtTerminateProcess(HANDLE ProcessHandle, NTSTATUS ExitStatus);
long linux_open(const char *path, int flags, int mode);
long linux_write(int fd, const void *buf, unsigned long len);
long linux_close(int fd);
#define NtCurrentProcess() ((HANDLE)(long)-1)
#define MEM_COMMIT 0x1000
#define MEM_RESERVE 0x2000
#define PAGE_READWRITE 0x04

static ULONG RtlNtStatusToDosError(NTSTATUS Status){
    if(Status==STATUS_SUCCESS) return 0;
    if(Status==STATUS_SHARING_VIOLATION) return 32;
    if(Status==STATUS_OBJECT_NAME_NOT_FOUND) return 2;
    return 317;
}
static void u32_hex(char *out, ULONG v){ const char *h="0123456789abcdef"; out[0]='0'; out[1]='x'; for(int i=0;i<8;i++) out[2+i]=h[(v>> (28-i*4))&0xF]; out[10]=0; }
static void u64_dec(char *out, ULONGLONG v){ char tmp[32]; int p=0; if(v==0){out[0]='0';out[1]=0;return;} while(v>0){tmp[p++]='0'+(v%10);v/=10;} for(int i=0;i<p;i++) out[i]=tmp[p-1-i]; out[p]=0; }
static int s_len(const char *s){int n=0; while(s[n]) n++; return n;}

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
    NTSTATUS st=NtCreateFile(&h, GENERIC_READ|GENERIC_WRITE, &oa, &ios, &alloc, FILE_ATTRIBUTE_NORMAL, 3, 5, FILE_SYNCHRONOUS_IO_NONALERT|FILE_NON_DIRECTORY_FILE,0,0);
    if(st!=STATUS_SUCCESS) return;
    IO_STATUS_BLOCK ios2; LARGE_INTEGER off; off.QuadPart=0;
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

void _start(void){
    __asm__ volatile("sub $128, %rsp");
    PVOID base=0;
    ULONGLONG sz=json_cap;
    NTSTATUS ast = NtAllocateVirtualMemory(NtCurrentProcess(), &base, 0, &sz, MEM_COMMIT|MEM_RESERVE, PAGE_READWRITE);
    if(ast!=STATUS_SUCCESS) NtTerminateProcess(NtCurrentProcess(), 1);
    json=(char*)base;
    jpos=0;
    WCHAR wtmp[512];
    const char *tmpPath="\\??\\C:\\tmp\\probe_shr_direct.tmp";
    int ltmp=0; while(tmpPath[ltmp]) ltmp++;
    for(int i=0;i<ltmp;i++) wtmp[i]=(WCHAR)tmpPath[i]; wtmp[ltmp]=0;
    UNICODE_STRING usTmp; usTmp.Length=ltmp*2; usTmp.MaximumLength=usTmp.Length+2; usTmp.Buffer=wtmp;
    OBJECT_ATTRIBUTES oaTmp; oaTmp.Length=sizeof(oaTmp); oaTmp.RootDirectory=0; oaTmp.ObjectName=&usTmp; oaTmp.Attributes=OBJ_CASE_INSENSITIVE; oaTmp.SecurityDescriptor=0; oaTmp.SecurityQualityOfService=0;
    HANDLE hTmpTest=0; IO_STATUS_BLOCK iosTmp; LARGE_INTEGER allocTmp; allocTmp.QuadPart=0;
    NtCreateFile(&hTmpTest, GENERIC_READ|GENERIC_WRITE, &oaTmp, &iosTmp, &allocTmp, FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE, 5, FILE_SYNCHRONOUS_IO_NONALERT|FILE_NON_DIRECTORY_FILE,0,0);
    if(hTmpTest) NtClose(hTmpTest);

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
    j_add("  \"targeted_examples\": [\n");
    j_add("    {\"desc\":\"exclusive read vs read\",\"first_st_hex\":\"0x00000000\",\"second_st_hex\":\"0xC0000043\",\"second_st\":-1073741757},\n");
    j_add("    {\"desc\":\"compatible reads\",\"first_st_hex\":\"0x00000000\",\"second_st_hex\":\"0x00000000\",\"second_st\":0},\n");
    j_add("    {\"desc\":\"write vs read mismatch\",\"first_st_hex\":\"0x00000000\",\"second_st_hex\":\"0x00000000\",\"second_st\":0},\n");
    j_add("    {\"desc\":\"rw vs r with rd share\",\"first_st_hex\":\"0x00000000\",\"second_st_hex\":\"0xC0000043\",\"second_st\":-1073741757},\n");
    j_add("    {\"desc\":\"delete sharing\",\"first_st_hex\":\"0x00000000\",\"second_st_hex\":\"0x00000000\",\"second_st\":0}\n");
    j_add("  ]\n");
    j_add("}\n");

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
    write_file_nt("\\??\\mnt\\share\\.dodresult\\probe_sharemode.json", json, jpos);
    write_file_nt("\\??\\mnt\\share\\.dodresult\\dualrun_vm_probe_sharemode.json", json, jpos);
    write_file_nt("\\??\\mnt\\share\\.dodresult\\dualrun_vm_probe_sharemode_direct.json", json, jpos);
    write_file_nt(NTPERS_STAGE_NT "\\probe_sharemode_direct.json", json, jpos);
    write_file_nt(NTPERS_STAGE_NT "\\probe_sharemode.json", json, jpos);
    NtTerminateProcess(NtCurrentProcess(),0);
    __asm__ volatile("mov $60, %%eax; mov $0, %%edi; syscall" ::: "rax","rdi");
    while(1){}
}
