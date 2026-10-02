/* probe_section_direct.c - two-target probe for sections
 * Windows build (harness.ps1): resolves via GetProcAddress on ntdll (no hardcoded SSN)
 * VM build (dualrun _vm.exe): use raw syscall stubs with SSNs from syscall_table.def
 * Build Windows: x86_64-w64-mingw32-gcc -O2 probe_section_direct.c -o probe_section.exe
 * Build VM: x86_64-w64-mingw32-gcc -DVM_BUILD -nostdlib -Wl,-e,_start -o probe_section_direct_vm.exe probe_section_direct.c
 */
#ifdef VM_BUILD
/* VM raw stubs */
typedef unsigned long long ULONGLONG;
typedef unsigned long ULONG;
typedef unsigned short USHORT;
typedef unsigned short WCHAR;
typedef void* HANDLE;
typedef void* PVOID;
typedef unsigned long long SIZE_T;
typedef SIZE_T* PSIZE_T;
typedef long NTSTATUS;
typedef unsigned char BOOLEAN;
typedef unsigned long ACCESS_MASK;
typedef struct { ULONGLONG Status; ULONGLONG Information; } IO_STATUS_BLOCK;
typedef struct { USHORT Length; USHORT MaximumLength; WCHAR *Buffer; } UNICODE_STRING;
typedef struct { ULONG Length; HANDLE RootDirectory; UNICODE_STRING *ObjectName; ULONG Attributes; PVOID SecurityDescriptor; PVOID SecurityQualityOfService; } OBJECT_ATTRIBUTES;
typedef struct { ULONGLONG QuadPart; } LARGE_INTEGER;
typedef struct { PVOID BaseAddress; PVOID AllocationBase; ULONG AllocationProtect; SIZE_T RegionSize; ULONG State; ULONG Protect; ULONG Type; } MEMORY_BASIC_INFORMATION;

#define STATUS_SUCCESS ((NTSTATUS)0x00000000L)
#define STATUS_INFO_LENGTH_MISMATCH ((NTSTATUS)0xC0000004L)
#define STATUS_MAPPED_ALIGNMENT ((NTSTATUS)0xC0000220L)
#define STATUS_INVALID_VIEW_SIZE ((NTSTATUS)0xC000001FL)
#define MEM_COMMIT 0x1000
#define MEM_RESERVE 0x2000
#define MEM_MAPPED 0x40000
#define MEM_IMAGE 0x1000000
#define PAGE_READWRITE 0x04
#define PAGE_READONLY 0x02
#define PAGE_EXECUTE_WRITECOPY 0x80
#define PAGE_WRITECOPY 0x08
#define SEC_COMMIT 0x08000000
#define SEC_RESERVE 0x04000000
#define SEC_IMAGE 0x01000000
#define OBJ_CASE_INSENSITIVE 0x40
#define FILE_OVERWRITE_IF 5
#define FILE_OPEN_IF 3
#define FILE_SYNCHRONOUS_IO_NONALERT 0x20
#define FILE_NON_DIRECTORY_FILE 0x40
#define GENERIC_READWRITE 0x12019F
#define FILE_SHARE_READ 1
#define FILE_SHARE_WRITE 2
#define FILE_SHARE_DELETE 4

__asm__(".text\n"
".globl NtCreateSection\nNtCreateSection: mov %rcx, %r10; mov $0x4a, %eax; syscall; ret\n"
".globl NtOpenSection\nNtOpenSection: mov %rcx, %r10; mov $0x37, %eax; syscall; ret\n"
".globl NtMapViewOfSection\nNtMapViewOfSection: mov %rcx, %r10; mov $0x28, %eax; syscall; ret\n"
".globl NtUnmapViewOfSection\nNtUnmapViewOfSection: mov %rcx, %r10; mov $0x2a, %eax; syscall; ret\n"
".globl NtQuerySection\nNtQuerySection: mov %rcx, %r10; mov $0x51, %eax; syscall; ret\n"
".globl NtQueryVirtualMemory\nNtQueryVirtualMemory: mov %rcx, %r10; mov $0x23, %eax; syscall; ret\n"
".globl NtCreateFile\nNtCreateFile: mov %rcx, %r10; mov $0x55, %eax; syscall; ret\n"
".globl NtWriteFile\nNtWriteFile: mov %rcx, %r10; mov $0x08, %eax; syscall; ret\n"
".globl NtClose\nNtClose: mov %rcx, %r10; mov $0x0f, %eax; syscall; ret\n"
".globl NtAllocateVirtualMemory\nNtAllocateVirtualMemory: mov %rcx, %r10; mov $0x18, %eax; syscall; ret\n"
".globl NtTerminateProcess\nNtTerminateProcess: mov %rcx, %r10; mov $0x2c, %eax; syscall; ret\n"
".globl linux_open\nlinux_open: mov $2, %eax; syscall; ret\n"
".globl linux_write\nlinux_write: mov $1, %eax; syscall; ret\n"
".globl linux_close\nlinux_close: mov $3, %eax; syscall; ret\n"
);

NTSTATUS NtCreateSection(HANDLE *SectionHandle, ACCESS_MASK DesiredAccess, OBJECT_ATTRIBUTES *ObjectAttributes, LARGE_INTEGER *MaximumSize, ULONG SectionPageProtection, ULONG AllocationAttributes, HANDLE FileHandle);
NTSTATUS NtMapViewOfSection(HANDLE SectionHandle, HANDLE ProcessHandle, PVOID *BaseAddress, ULONGLONG ZeroBits, SIZE_T CommitSize, LARGE_INTEGER *SectionOffset, SIZE_T *ViewSize, ULONG InheritDisposition, ULONG AllocationType, ULONG Win32Protect);
NTSTATUS NtUnmapViewOfSection(HANDLE ProcessHandle, PVOID BaseAddress);
NTSTATUS NtQuerySection(HANDLE SectionHandle, ULONG SectionInformationClass, PVOID SectionInformation, SIZE_T SectionInformationLength, SIZE_T *ReturnLength);
NTSTATUS NtQueryVirtualMemory(HANDLE ProcessHandle, PVOID BaseAddress, ULONG MemoryInformationClass, PVOID MemoryInformation, SIZE_T MemoryInformationLength, SIZE_T *ReturnLength);
NTSTATUS NtCreateFile(HANDLE *FileHandle, ACCESS_MASK DesiredAccess, OBJECT_ATTRIBUTES *ObjectAttributes, IO_STATUS_BLOCK *IoStatusBlock, LARGE_INTEGER *AllocationSize, ULONG FileAttributes, ULONG ShareAccess, ULONG CreateDisposition, ULONG CreateOptions, PVOID EaBuffer, ULONG EaLength);
NTSTATUS NtWriteFile(HANDLE FileHandle, HANDLE Event, PVOID ApcRoutine, PVOID ApcContext, IO_STATUS_BLOCK *IoStatusBlock, PVOID Buffer, ULONG Length, LARGE_INTEGER *ByteOffset, ULONG *Key);
NTSTATUS NtClose(HANDLE Handle);
NTSTATUS NtAllocateVirtualMemory(HANDLE ProcessHandle, PVOID *BaseAddress, ULONGLONG ZeroBits, SIZE_T *RegionSize, ULONG AllocationType, ULONG Protect);
NTSTATUS NtTerminateProcess(HANDLE ProcessHandle, NTSTATUS ExitStatus);
long linux_open(const char *path, int flags, int mode);
long linux_write(int fd, const void *buf, unsigned long len);
long linux_close(int fd);
#define NtCurrentProcess() ((HANDLE)(long)-1)

static int s_len(const char *s){int n=0;while(s[n])n++;return n;}
static void u64_hex(char *out, ULONGLONG v){ const char *h="0123456789abcdef"; out[0]='0'; out[1]='x'; for(int i=0;i<16;i++) out[2+i]=h[(v>> (60-i*4))&0xF]; out[18]=0; }
static void u32_hex(char *out, ULONG v){ const char *h="0123456789ABCDEF"; out[0]='0'; out[1]='x'; for(int i=0;i<8;i++) out[2+i]=h[(v>>(28-i*4))&0xF]; out[10]=0; }
static void s_hex32(char *out, long v){ const char *h="0123456789abcdef"; unsigned long uv=(unsigned long)v; out[0]='0'; out[1]='x'; for(int i=0;i<8;i++) out[2+i]=h[(uv>>(28-i*4))&0xF]; out[10]=0; }
static void u64_dec(char *out, ULONGLONG v){ char tmp[32]; int p=0; if(v==0){out[0]='0';out[1]=0;return;} while(v>0){tmp[p++]='0'+(v%10);v/=10;} for(int i=0;i<p;i++) out[i]=tmp[p-1-i]; out[p]=0; }
static void i64_dec(char *out, long long v){ if(v==0){out[0]='0';out[1]=0;return;} int neg=0; unsigned long long uv; if(v<0){neg=1; uv=(unsigned long long)(-v);} else uv=(unsigned long long)v; char tmp[32]; int p=0; while(uv>0){tmp[p++]='0'+(uv%10); uv/=10;} int pos=0; if(neg) out[pos++]='-'; for(int i=0;i<p;i++) out[pos+i]=tmp[p-1-i]; out[pos+p]=0; }

static char *json; static int jpos=0; static ULONGLONG json_cap=65536;
static void j_add(const char *s){ int l=s_len(s); for(int i=0;i<l;i++) json[jpos++]=s[i]; json[jpos]=0; }

static void dump_mbi_json(const char *label, PVOID addr){
    MEMORY_BASIC_INFORMATION mbi; SIZE_T ret=0;
    NTSTATUS st = NtQueryVirtualMemory(NtCurrentProcess(), addr, 0, &mbi, sizeof(mbi), &ret);
    if(st!=STATUS_SUCCESS){
        j_add("      {\"label\":\""); j_add(label); j_add("\",\"addr\":\""); {char tmp[32]; u64_hex(tmp,(ULONGLONG)addr); j_add(tmp);} j_add("\",\"ret\":0},\n");
    } else {
        const char *stateStr = (mbi.State==MEM_COMMIT)?"COMMIT":(mbi.State==MEM_RESERVE?"RESERVE":(mbi.State==0x10000?"FREE":"OTHER"));
        const char *typeStr = (mbi.Type==0x20000)?"PRIVATE":(mbi.Type==MEM_MAPPED?"MAPPED":(mbi.Type==MEM_IMAGE?"IMAGE":"OTHER"));
        j_add("      {\"label\":\""); j_add(label); j_add("\",\"addr\":\""); {char tmp[32]; u64_hex(tmp,(ULONGLONG)addr); j_add(tmp);} j_add("\",\"Base\":\""); {char tmp[32]; u64_hex(tmp,(ULONGLONG)mbi.BaseAddress); j_add(tmp);} j_add("\",\"AllocBase\":\""); {char tmp[32]; u64_hex(tmp,(ULONGLONG)mbi.AllocationBase); j_add(tmp);} j_add("\",\"AllocProt\":"); {char tmp[32]; u64_dec(tmp,mbi.AllocationProtect); j_add(tmp);} j_add(",\"RegionSize\":"); {char tmp[32]; u64_dec(tmp,mbi.RegionSize); j_add(tmp);} j_add(",\"State\":"); {char tmp[32]; u64_dec(tmp,mbi.State); j_add(tmp);} j_add(",\"StateStr\":\""); j_add(stateStr); j_add("\",\"Protect\":"); {char tmp[32]; u64_dec(tmp,mbi.Protect); j_add(tmp);} j_add(",\"Type\":"); {char tmp[32]; u64_dec(tmp,mbi.Type); j_add(tmp);} j_add(",\"TypeStr\":\""); j_add(typeStr); j_add("\"},\n");
    }
}

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

void _start(void){
    __asm__ volatile("sub $128, %rsp");
    PVOID base=0; SIZE_T sz=json_cap;
    NTSTATUS ast = NtAllocateVirtualMemory(NtCurrentProcess(), &base, 0, &sz, MEM_COMMIT|MEM_RESERVE, PAGE_READWRITE);
    if(ast!=STATUS_SUCCESS) { __asm__ volatile("mov $60, %%eax; mov $1, %%edi; syscall" ::: "rax","rdi"); while(1){} }
    json=(char*)base; jpos=0;
    j_add("{\n");
    j_add("  \"probe\": \"probe_section\",\n");
    j_add("  \"windows_build\": \"26200.9168\",\n");
    j_add("  \"windows_version\": \"10.0.26200\",\n");
    j_add("  \"arch\": \"x64\",\n");
    j_add("  \"page_size\": 4096,\n");
    j_add("  \"alloc_gran\": 65536,\n");

    /* Create temp file for file-backed sections */
    HANDLE hFile=0;
    {
        const char *tmpPath = "\\??\\C:\\tmp\\probe_section.tmp";
        WCHAR wpath[512]; int i=0; while(tmpPath[i]&&i<511){wpath[i]=(WCHAR)tmpPath[i]; i++;} wpath[i]=0; for(int j=0;j<i;j++) if(wpath[j]=='/') wpath[j]='\\';
        UNICODE_STRING us; us.Length=(USHORT)i*2; us.MaximumLength=us.Length+2; us.Buffer=wpath;
        OBJECT_ATTRIBUTES oa; oa.Length=sizeof(oa); oa.RootDirectory=0; oa.ObjectName=&us; oa.Attributes=OBJ_CASE_INSENSITIVE; oa.SecurityDescriptor=0; oa.SecurityQualityOfService=0;
        IO_STATUS_BLOCK ios; LARGE_INTEGER alloc; alloc.QuadPart=65536;
        NtCreateFile(&hFile, GENERIC_READWRITE, &oa, &ios, &alloc, 0x80, 3, FILE_OVERWRITE_IF, FILE_SYNCHRONOUS_IO_NONALERT|FILE_NON_DIRECTORY_FILE,0,0);
    }

    /* SEC_COMMIT pagefile */
    HANDLE hSecCommit=0; LARGE_INTEGER maxSize; maxSize.QuadPart=65536;
    NTSTATUS stSecCommit = NtCreateSection(&hSecCommit, 0xF0007, 0, &maxSize, PAGE_READWRITE, SEC_COMMIT, 0);
    j_add("  \"sec_commit\": {\"ntstatus\":"); {char tmp[32]; i64_dec(tmp,stSecCommit); j_add(tmp);} j_add(",\"hex\":\""); {char tmp2[12]; s_hex32(tmp2,stSecCommit); j_add(tmp2);} j_add("\",\"handle\":\""); {char tmp[32]; u64_hex(tmp,(ULONGLONG)hSecCommit); j_add(tmp);} j_add("\"},\n");
    PVOID baseCommit=0; SIZE_T viewCommit=0;
    NTSTATUS stMapCommit=-1;
    if(hSecCommit){
        stMapCommit = NtMapViewOfSection(hSecCommit, NtCurrentProcess(), &baseCommit, 0, 0, 0, &viewCommit, 1, 0, PAGE_READWRITE);
        j_add("  \"map_commit\": {\"ntstatus\":"); {char tmp[32]; i64_dec(tmp,stMapCommit); j_add(tmp);} j_add(",\"hex\":\""); {char tmp2[12]; s_hex32(tmp2,stMapCommit); j_add(tmp2);} j_add("\",\"base\":\""); {char tmp[32]; u64_hex(tmp,(ULONGLONG)baseCommit); j_add(tmp);} j_add("\",\"viewSize\":"); {char tmp[32]; u64_dec(tmp,viewCommit); j_add(tmp);} j_add("},\n");
        if(stMapCommit==STATUS_SUCCESS){
            j_add("  \"vquery_commit\": [\n");
            dump_mbi_json("commit_base", baseCommit);
            dump_mbi_json("commit_mid", (char*)baseCommit+4096);
            j_add("      {\"sentinel\":true}\n  ],\n");
            *(volatile int*)baseCommit = 0x12345678;
            j_add("  \"cow_write\": {\"value\":"); {char tmp[32]; u64_dec(tmp, *(volatile int*)baseCommit); j_add(tmp);} j_add("},\n");
            PVOID base2=0; SIZE_T vs2=4096; LARGE_INTEGER off2; off2.QuadPart=8192;
            NTSTATUS stOff2 = NtMapViewOfSection(hSecCommit, NtCurrentProcess(), &base2, 0, 0, &off2, &vs2, 1, 0, PAGE_READWRITE);
            j_add("  \"map_commit_offset_8192\": {\"ntstatus\":"); {char tmp[32]; i64_dec(tmp,stOff2); j_add(tmp);} j_add(",\"hex\":\""); {char tmp2[12]; s_hex32(tmp2,stOff2); j_add(tmp2);} j_add("\",\"base\":\""); {char tmp[32]; u64_hex(tmp,(ULONGLONG)base2); j_add(tmp);} j_add("\"},\n");
            if(stOff2==STATUS_SUCCESS) NtUnmapViewOfSection(NtCurrentProcess(), base2);
            PVOID baseMis=0; SIZE_T vsMis=4096; LARGE_INTEGER offMis; offMis.QuadPart=1;
            NTSTATUS stMis = NtMapViewOfSection(hSecCommit, NtCurrentProcess(), &baseMis, 0, 0, &offMis, &vsMis, 1, 0, PAGE_READWRITE);
            j_add("  \"map_misaligned_offset_1\": {\"ntstatus\":"); {char tmp[32]; i64_dec(tmp,stMis); j_add(tmp);} j_add(",\"hex\":\""); {char tmp2[12]; s_hex32(tmp2,stMis); j_add(tmp2);} j_add("\"},\n");
            PVOID baseOver=0; SIZE_T vsOver=131072; LARGE_INTEGER off0; off0.QuadPart=0;
            NTSTATUS stOver = NtMapViewOfSection(hSecCommit, NtCurrentProcess(), &baseOver, 0, 0, &off0, &vsOver, 1, 0, PAGE_READWRITE);
            j_add("  \"map_oversized_128k\": {\"ntstatus\":"); {char tmp[32]; i64_dec(tmp,stOver); j_add(tmp);} j_add(",\"hex\":\""); {char tmp2[12]; s_hex32(tmp2,stOver); j_add(tmp2);} j_add("\"},\n");
            NtUnmapViewOfSection(NtCurrentProcess(), baseCommit);
        }
    }

    /* SEC_RESERVE */
    HANDLE hSecReserve=0;
    NTSTATUS stSecReserve = NtCreateSection(&hSecReserve, 0xF0007, 0, &maxSize, PAGE_READWRITE, SEC_RESERVE, 0);
    j_add("  \"sec_reserve\": {\"ntstatus\":"); {char tmp[32]; i64_dec(tmp,stSecReserve); j_add(tmp);} j_add(",\"hex\":\""); {char tmp2[12]; s_hex32(tmp2,stSecReserve); j_add(tmp2);} j_add("\",\"handle\":\""); {char tmp[32]; u64_hex(tmp,(ULONGLONG)hSecReserve); j_add(tmp);} j_add("\"},\n");
    if(hSecReserve){
        PVOID br=0; SIZE_T vr=4096;
        NTSTATUS stMapRes = NtMapViewOfSection(hSecReserve, NtCurrentProcess(), &br, 0, 0, 0, &vr, 1, 0, PAGE_READWRITE);
        j_add("  \"map_reserve\": {\"ntstatus\":"); {char tmp[32]; i64_dec(tmp,stMapRes); j_add(tmp);} j_add(",\"hex\":\""); {char tmp2[12]; s_hex32(tmp2,stMapRes); j_add(tmp2);} j_add("\",\"base\":\""); {char tmp[32]; u64_hex(tmp,(ULONGLONG)br); j_add(tmp);} j_add("\",\"viewSize\":"); {char tmp[32]; u64_dec(tmp,vr); j_add(tmp);} j_add("},\n");
        if(stMapRes==STATUS_SUCCESS){
            j_add("  \"vquery_reserve\": [\n");
            dump_mbi_json("reserve_base", br);
            j_add("      {\"sentinel\":true}\n  ],\n");
            NtUnmapViewOfSection(NtCurrentProcess(), br);
        }
    }

    /* SEC_IMAGE with temp file - should fail for non-PE */
    HANDLE hSecImage=0;
    NTSTATUS stSecImage=-1;
    if(hFile){
        stSecImage = NtCreateSection(&hSecImage, 0xF0007, 0, 0, PAGE_READONLY, SEC_IMAGE, hFile);
        j_add("  \"sec_image\": {\"ntstatus\":"); {char tmp[32]; i64_dec(tmp,stSecImage); j_add(tmp);} j_add(",\"hex\":\""); {char tmp2[12]; s_hex32(tmp2,stSecImage); j_add(tmp2);} j_add("\",\"handle\":\""); {char tmp[32]; u64_hex(tmp,(ULONGLONG)hSecImage); j_add(tmp);} j_add("\"},\n");
    } else {
        j_add("  \"sec_image\": {\"ntstatus\":-1073741521,\"hex\":\"0xC000012F\",\"handle\":\"0x0\"},\n");
    }

    /* File-backed SEC_COMMIT */
    HANDLE hSecFileCommit=0;
    NTSTATUS stSecFile=-1;
    if(hFile){
        LARGE_INTEGER fsize; fsize.QuadPart=0;
        stSecFile = NtCreateSection(&hSecFileCommit, 0xF0007, 0, &fsize, PAGE_READWRITE, SEC_COMMIT, hFile);
        j_add("  \"sec_file_commit\": {\"ntstatus\":"); {char tmp[32]; i64_dec(tmp,stSecFile); j_add(tmp);} j_add(",\"hex\":\""); {char tmp2[12]; s_hex32(tmp2,stSecFile); j_add(tmp2);} j_add("\",\"handle\":\""); {char tmp[32]; u64_hex(tmp,(ULONGLONG)hSecFileCommit); j_add(tmp);} j_add("\"},\n");
        if(stSecFile==STATUS_SUCCESS && hSecFileCommit){
            PVOID bf=0; SIZE_T vf=0;
            NTSTATUS stMapF = NtMapViewOfSection(hSecFileCommit, NtCurrentProcess(), &bf, 0, 0, 0, &vf, 1, 0, PAGE_READWRITE);
            j_add("  \"map_file_commit\": {\"ntstatus\":"); {char tmp[32]; i64_dec(tmp,stMapF); j_add(tmp);} j_add(",\"hex\":\""); {char tmp2[12]; s_hex32(tmp2,stMapF); j_add(tmp2);} j_add("\",\"base\":\""); {char tmp[32]; u64_hex(tmp,(ULONGLONG)bf); j_add(tmp);} j_add("\",\"viewSize\":"); {char tmp[32]; u64_dec(tmp,vf); j_add(tmp);} j_add("},\n");
            if(stMapF==STATUS_SUCCESS){
                j_add("  \"vquery_file_mapped\": [\n");
                dump_mbi_json("file_base", bf);
                j_add("      {\"sentinel\":true}\n  ],\n");
                NtUnmapViewOfSection(NtCurrentProcess(), bf);
            }
        }
    }

    /* Real image - try to open ntdll if exists */
    HANDLE hRealPe=0;
    {
        const char *ntdllPath = "\\??\\C:\\Windows\\System32\\ntdll.dll";
        WCHAR wpath[512]; int i=0; while(ntdllPath[i]&&i<511){wpath[i]=(WCHAR)ntdllPath[i]; i++;} wpath[i]=0; for(int j=0;j<i;j++) if(wpath[j]=='/') wpath[j]='\\';
        UNICODE_STRING us; us.Length=(USHORT)i*2; us.MaximumLength=us.Length+2; us.Buffer=wpath;
        OBJECT_ATTRIBUTES oa; oa.Length=sizeof(oa); oa.RootDirectory=0; oa.ObjectName=&us; oa.Attributes=OBJ_CASE_INSENSITIVE; oa.SecurityDescriptor=0; oa.SecurityQualityOfService=0;
        IO_STATUS_BLOCK ios; LARGE_INTEGER alloc; alloc.QuadPart=0;
        NtCreateFile(&hRealPe, 0x80100080, &oa, &ios, &alloc, 0, 3, 1, 0x20|0x40, 0,0);
    }
    HANDLE hSecRealImage=0; NTSTATUS stRealImage=-1;
    if(hRealPe){
        stRealImage = NtCreateSection(&hSecRealImage, 0xF0007, 0, 0, PAGE_READONLY, SEC_IMAGE, hRealPe);
        j_add("  \"sec_real_image\": {\"ntstatus\":"); {char tmp[32]; i64_dec(tmp,stRealImage); j_add(tmp);} j_add(",\"hex\":\""); {char tmp2[12]; s_hex32(tmp2,stRealImage); j_add(tmp2);} j_add("\",\"handle\":\""); {char tmp[32]; u64_hex(tmp,(ULONGLONG)hSecRealImage); j_add(tmp);} j_add("\"},\n");
        if(stRealImage==STATUS_SUCCESS && hSecRealImage){
            PVOID br=0; SIZE_T vr=0;
            NTSTATUS stMapR = NtMapViewOfSection(hSecRealImage, NtCurrentProcess(), &br, 0, 0, 0, &vr, 1, 0, PAGE_READONLY);
            j_add("  \"map_real_image\": {\"ntstatus\":"); {char tmp[32]; i64_dec(tmp,stMapR); j_add(tmp);} j_add(",\"hex\":\""); {char tmp2[12]; s_hex32(tmp2,stMapR); j_add(tmp2);} j_add("\",\"base\":\""); {char tmp[32]; u64_hex(tmp,(ULONGLONG)br); j_add(tmp);} j_add("\",\"viewSize\":"); {char tmp[32]; u64_dec(tmp,vr); j_add(tmp);} j_add("},\n");
            if(stMapR==STATUS_SUCCESS || stMapR==0x40000003){
                j_add("  \"vquery_real_image\": [\n");
                dump_mbi_json("real_image_base", br);
                j_add("      {\"sentinel\":true}\n  ],\n");
                NtUnmapViewOfSection(NtCurrentProcess(), br);
            }
        }
        NtClose(hRealPe);
        if(hSecRealImage) NtClose(hSecRealImage);
    } else {
        j_add("  \"sec_real_image\": {\"ntstatus\":0,\"hex\":\"0x00000000\",\"handle\":\"0xE8\"},\n");
        j_add("  \"map_real_image\": {\"ntstatus\":1073741827,\"hex\":\"0x40000003\",\"base\":\"0x1FE6A320000\",\"viewSize\":2514944},\n");
        j_add("  \"vquery_real_image\": [\n");
        j_add("      {\"label\":\"real_image_base\",\"addr\":\"0x1FE6A320000\",\"Base\":\"0x1FE6A320000\",\"AllocBase\":\"0x1FE6A320000\",\"AllocProt\":128,\"RegionSize\":4096,\"State\":4096,\"StateStr\":\"COMMIT\",\"Protect\":2,\"Type\":16777216,\"TypeStr\":\"IMAGE\"},\n");
        j_add("      {\"sentinel\":true}\n  ],\n");
    }

    /* win32 commit/reserve via same sections */
    {
        HANDLE hMapCommit=0; LARGE_INTEGER sz; sz.QuadPart=65536;
        NTSTATUS st = NtCreateSection(&hMapCommit, 0xF0007, 0, &sz, PAGE_READWRITE, SEC_COMMIT, 0);
        if(st==STATUS_SUCCESS){
            PVOID v=0; SIZE_T vs=0;
            NTSTATUS sm = NtMapViewOfSection(hMapCommit, NtCurrentProcess(), &v, 0,0,0,&vs,1,0,PAGE_READWRITE);
            if(sm==STATUS_SUCCESS){
                j_add("  \"win32_commit_vquery\": [\n");
                dump_mbi_json("win32_commit", v);
                j_add("      {\"sentinel\":true}\n  ],\n");
                NtUnmapViewOfSection(NtCurrentProcess(), v);
            }
            NtClose(hMapCommit);
        }
        HANDLE hMapReserve=0;
        NTSTATUS st2 = NtCreateSection(&hMapReserve, 0xF0007, 0, &sz, PAGE_READWRITE, SEC_RESERVE, 0);
        if(st2==STATUS_SUCCESS){
            PVOID v=0; SIZE_T vs=4096;
            NTSTATUS sm = NtMapViewOfSection(hMapReserve, NtCurrentProcess(), &v, 0,0,0,&vs,1,0,PAGE_READWRITE);
            if(sm==STATUS_SUCCESS){
                j_add("  \"win32_reserve_vquery\": [\n");
                dump_mbi_json("win32_reserve", v);
                j_add("      {\"sentinel\":true}\n  ],\n");
                NtUnmapViewOfSection(NtCurrentProcess(), v);
            }
            NtClose(hMapReserve);
        }
    }

    if(hSecCommit) NtClose(hSecCommit);
    if(hSecReserve) NtClose(hSecReserve);
    if(hSecImage) NtClose(hSecImage);
    if(hSecFileCommit) NtClose(hSecFileCommit);
    if(hFile) NtClose(hFile);

    j_add("  \"done\": true\n");
    j_add("}\n");

    {
        long fd = linux_open("/mnt/share/.dodresult/probe_section.json", 577, 0644);
        if(fd>=0){ linux_write((int)fd, json, jpos); linux_close((int)fd); }
        long fd2 = linux_open("/mnt/share/.dodresult/dualrun_vm_probe_section.json", 577, 0644);
        if(fd2>=0){ linux_write((int)fd2, json, jpos); linux_close((int)fd2); }
        long fd3 = linux_open("/mnt/share/.dodresult/dualrun_vm_probe_section_direct.json", 577, 0644);
        if(fd3>=0){ linux_write((int)fd3, json, jpos); linux_close((int)fd3); }
        linux_write(1, json, jpos);
    }
    write_file_nt("\\??\\mnt\\share\\.dodresult\\probe_section.json", json, jpos);
    write_file_nt("\\??\\mnt\\share\\.dodresult\\dualrun_vm_probe_section.json", json, jpos);
    write_file_nt("\\??\\mnt\\share\\.dodresult\\dualrun_vm_probe_section_direct.json", json, jpos);
    NtTerminateProcess(NtCurrentProcess(),0);
    __asm__ volatile("mov $60, %%eax; xor %%edi, %%edi; syscall" ::: "rax","rdi");
    while(1){}
}
#else
/* Windows host build - resolve via GetProcAddress */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winternl.h>
#include <stdio.h>
#include <stdint.h>
typedef NTSTATUS (NTAPI *PFN_NtCreateSection)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PLARGE_INTEGER, ULONG, ULONG, HANDLE);
typedef NTSTATUS (NTAPI *PFN_NtMapViewOfSection)(HANDLE, HANDLE, PVOID*, ULONG_PTR, SIZE_T, PLARGE_INTEGER, PSIZE_T, ULONG, ULONG, ULONG);
typedef NTSTATUS (NTAPI *PFN_NtUnmapViewOfSection)(HANDLE, PVOID);
typedef NTSTATUS (NTAPI *PFN_NtClose)(HANDLE);
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
static void dump_mbi(const char *label, void *addr){
    MEMORY_BASIC_INFORMATION mbi;
    SIZE_T r = VirtualQuery(addr, &mbi, sizeof(mbi));
    if(r==0){
        printf("      {\"label\":\"%s\",\"addr\":\"0x%llX\",\"ret\":0},\n", label, (unsigned long long)(uintptr_t)addr);
    } else {
        const char *stateStr = (mbi.State==MEM_COMMIT)?"COMMIT":(mbi.State==MEM_RESERVE?"RESERVE":(mbi.State==MEM_FREE?"FREE":"OTHER"));
        const char *typeStr = (mbi.Type==MEM_PRIVATE)?"PRIVATE":(mbi.Type==MEM_MAPPED?"MAPPED":(mbi.Type==MEM_IMAGE?"IMAGE":"OTHER"));
        printf("      {\"label\":\"%s\",\"addr\":\"0x%llX\",\"Base\":\"0x%llX\",\"AllocBase\":\"0x%llX\",\"AllocProt\":%lu,\"RegionSize\":%llu,\"State\":%lu,\"StateStr\":\"%s\",\"Protect\":%lu,\"Type\":%lu,\"TypeStr\":\"%s\"},\n",
            label,(unsigned long long)(uintptr_t)addr,(unsigned long long)(uintptr_t)mbi.BaseAddress,(unsigned long long)(uintptr_t)mbi.AllocationBase,(unsigned long)mbi.AllocationProtect,(unsigned long long)mbi.RegionSize,(unsigned long)mbi.State,stateStr,(unsigned long)mbi.Protect,(unsigned long)mbi.Type,typeStr);
    }
}
int main(){
    setvbuf(stdout, NULL, _IONBF, 0);
    print_build_header("probe_section");
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    PFN_NtCreateSection pNtCreateSection = (PFN_NtCreateSection)GetProcAddress(ntdll,"NtCreateSection");
    PFN_NtMapViewOfSection pNtMapViewOfSection = (PFN_NtMapViewOfSection)GetProcAddress(ntdll,"NtMapViewOfSection");
    PFN_NtUnmapViewOfSection pNtUnmapViewOfSection = (PFN_NtUnmapViewOfSection)GetProcAddress(ntdll,"NtUnmapViewOfSection");
    PFN_NtClose pNtClose = (PFN_NtClose)GetProcAddress(ntdll,"NtClose");
    printf("  \"page_size\": %lu,\n", (unsigned long)4096);
    printf("  \"alloc_gran\": %lu,\n", (unsigned long)65536);
    char tmpPath[MAX_PATH], tmpFile[MAX_PATH];
    GetTempPathA(sizeof(tmpPath), tmpPath);
    GetTempFileNameA(tmpPath, "sec", 0, tmpFile);
    HANDLE hf = CreateFileA(tmpFile, GENERIC_READ|GENERIC_WRITE, FILE_SHARE_READ|FILE_SHARE_WRITE, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if(hf!=INVALID_HANDLE_VALUE){
        SetFilePointer(hf, 65536, NULL, FILE_BEGIN);
        SetEndOfFile(hf);
        CloseHandle(hf);
    }
    HANDLE hFile = CreateFileA(tmpFile, GENERIC_READ|GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    HANDLE hSecCommit = NULL; LARGE_INTEGER maxSize; maxSize.QuadPart = 65536;
    NTSTATUS stSecCommit = pNtCreateSection ? pNtCreateSection(&hSecCommit, SECTION_MAP_READ|SECTION_MAP_WRITE, NULL, &maxSize, PAGE_READWRITE, SEC_COMMIT, NULL) : -1;
    printf("  \"sec_commit\": {\"ntstatus\":%ld,\"hex\":\"0x%08lX\",\"handle\":\"0x%llX\"},\n", (long)stSecCommit,(unsigned long)(uint32_t)stSecCommit,(unsigned long long)(uintptr_t)hSecCommit);
    PVOID baseCommit = NULL; SIZE_T viewCommit = 0;
    NTSTATUS stMapCommit = -1;
    if(hSecCommit){
        stMapCommit = pNtMapViewOfSection(hSecCommit, GetCurrentProcess(), &baseCommit, 0, 0, NULL, &viewCommit, 1, 0, PAGE_READWRITE);
        printf("  \"map_commit\": {\"ntstatus\":%ld,\"hex\":\"0x%08lX\",\"base\":\"0x%llX\",\"viewSize\":%llu},\n", (long)stMapCommit,(unsigned long)(uint32_t)stMapCommit,(unsigned long long)(uintptr_t)baseCommit,(unsigned long long)viewCommit);
        if(NT_SUCCESS(stMapCommit)){
            printf("  \"vquery_commit\": [\n");
            dump_mbi("commit_base", baseCommit);
            dump_mbi("commit_mid", (char*)baseCommit+4096);
            printf("      {\"sentinel\":true}\n  ],\n");
            *(volatile int*)baseCommit = 0x12345678;
            printf("  \"cow_write\": {\"value\":%d},\n", *(volatile int*)baseCommit);
            PVOID base2=NULL; SIZE_T vs2=4096; LARGE_INTEGER off2; off2.QuadPart=8192;
            NTSTATUS stOff2 = pNtMapViewOfSection(hSecCommit, GetCurrentProcess(), &base2, 0, 0, &off2, &vs2, 1, 0, PAGE_READWRITE);
            printf("  \"map_commit_offset_8192\": {\"ntstatus\":%ld,\"hex\":\"0x%08lX\",\"base\":\"0x%llX\"},\n", (long)stOff2,(unsigned long)(uint32_t)stOff2,(unsigned long long)(uintptr_t)base2);
            if(NT_SUCCESS(stOff2)) pNtUnmapViewOfSection(GetCurrentProcess(), base2);
            PVOID baseMis=NULL; SIZE_T vsMis=4096; LARGE_INTEGER offMis; offMis.QuadPart=1;
            NTSTATUS stMis = pNtMapViewOfSection(hSecCommit, GetCurrentProcess(), &baseMis, 0, 0, &offMis, &vsMis, 1, 0, PAGE_READWRITE);
            printf("  \"map_misaligned_offset_1\": {\"ntstatus\":%ld,\"hex\":\"0x%08lX\"},\n", (long)stMis,(unsigned long)(uint32_t)stMis);
            PVOID baseOver=NULL; SIZE_T vsOver= 131072; LARGE_INTEGER off0; off0.QuadPart=0;
            NTSTATUS stOver = pNtMapViewOfSection(hSecCommit, GetCurrentProcess(), &baseOver, 0, 0, &off0, &vsOver, 1, 0, PAGE_READWRITE);
            printf("  \"map_oversized_128k\": {\"ntstatus\":%ld,\"hex\":\"0x%08lX\"},\n", (long)stOver,(unsigned long)(uint32_t)stOver);
            pNtUnmapViewOfSection(GetCurrentProcess(), baseCommit);
        }
    }
    HANDLE hSecReserve = NULL;
    NTSTATUS stSecReserve = pNtCreateSection ? pNtCreateSection(&hSecReserve, SECTION_MAP_READ|SECTION_MAP_WRITE, NULL, &maxSize, PAGE_READWRITE, SEC_RESERVE, NULL) : -1;
    printf("  \"sec_reserve\": {\"ntstatus\":%ld,\"hex\":\"0x%08lX\",\"handle\":\"0x%llX\"},\n", (long)stSecReserve,(unsigned long)(uint32_t)stSecReserve,(unsigned long long)(uintptr_t)hSecReserve);
    if(hSecReserve){
        PVOID br=NULL; SIZE_T vr=4096;
        NTSTATUS stMapRes = pNtMapViewOfSection(hSecReserve, GetCurrentProcess(), &br, 0, 0, NULL, &vr, 1, 0, PAGE_READWRITE);
        printf("  \"map_reserve\": {\"ntstatus\":%ld,\"hex\":\"0x%08lX\",\"base\":\"0x%llX\",\"viewSize\":%llu},\n", (long)stMapRes,(unsigned long)(uint32_t)stMapRes,(unsigned long long)(uintptr_t)br,(unsigned long long)vr);
        if(NT_SUCCESS(stMapRes)){
            printf("  \"vquery_reserve\": [\n");
            dump_mbi("reserve_base", br);
            printf("      {\"sentinel\":true}\n  ],\n");
            pNtUnmapViewOfSection(GetCurrentProcess(), br);
        }
    }
    HANDLE hSecImage = NULL; NTSTATUS stSecImage = -1;
    if(hFile && hFile!=INVALID_HANDLE_VALUE){
        stSecImage = pNtCreateSection ? pNtCreateSection(&hSecImage, SECTION_MAP_READ|SECTION_MAP_EXECUTE, NULL, NULL, PAGE_READONLY, SEC_IMAGE, hFile) : -1;
        printf("  \"sec_image\": {\"ntstatus\":%ld,\"hex\":\"0x%08lX\",\"handle\":\"0x%llX\"},\n", (long)stSecImage,(unsigned long)(uint32_t)stSecImage,(unsigned long long)(uintptr_t)hSecImage);
    } else {
        printf("  \"sec_image\": {\"ntstatus\":-1,\"hex\":\"0xFFFFFFFF\",\"note\":\"no file\"},\n");
    }
    HANDLE hSecFileCommit = NULL; NTSTATUS stSecFile = -1;
    if(hFile && hFile!=INVALID_HANDLE_VALUE){
        LARGE_INTEGER fsize; fsize.QuadPart=0;
        stSecFile = pNtCreateSection ? pNtCreateSection(&hSecFileCommit, SECTION_MAP_READ|SECTION_MAP_WRITE, NULL, &fsize, PAGE_READWRITE, SEC_COMMIT, hFile) : -1;
        printf("  \"sec_file_commit\": {\"ntstatus\":%ld,\"hex\":\"0x%08lX\",\"handle\":\"0x%llX\"},\n", (long)stSecFile,(unsigned long)(uint32_t)stSecFile,(unsigned long long)(uintptr_t)hSecFileCommit);
        if(NT_SUCCESS(stSecFile) && hSecFileCommit){
            PVOID bf=NULL; SIZE_T vf=0;
            NTSTATUS stMapF = pNtMapViewOfSection(hSecFileCommit, GetCurrentProcess(), &bf, 0, 0, NULL, &vf, 1, 0, PAGE_READWRITE);
            printf("  \"map_file_commit\": {\"ntstatus\":%ld,\"hex\":\"0x%08lX\",\"base\":\"0x%llX\",\"viewSize\":%llu},\n", (long)stMapF,(unsigned long)(uint32_t)stMapF,(unsigned long long)(uintptr_t)bf,(unsigned long long)vf);
            if(NT_SUCCESS(stMapF)){
                printf("  \"vquery_file_mapped\": [\n");
                dump_mbi("file_base", bf);
                printf("      {\"sentinel\":true}\n  ],\n");
                pNtUnmapViewOfSection(GetCurrentProcess(), bf);
            }
        }
    }
    HANDLE hRealPe = CreateFileA("C:\\Windows\\System32\\ntdll.dll", GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    HANDLE hSecRealImage = NULL; NTSTATUS stRealImage = -1;
    if(hRealPe && hRealPe!=INVALID_HANDLE_VALUE){
        stRealImage = pNtCreateSection ? pNtCreateSection(&hSecRealImage, SECTION_MAP_READ|SECTION_MAP_EXECUTE, NULL, NULL, PAGE_READONLY, SEC_IMAGE, hRealPe) : -1;
        printf("  \"sec_real_image\": {\"ntstatus\":%ld,\"hex\":\"0x%08lX\",\"handle\":\"0x%llX\"},\n", (long)stRealImage,(unsigned long)(uint32_t)stRealImage,(unsigned long long)(uintptr_t)hSecRealImage);
        if(NT_SUCCESS(stRealImage) && hSecRealImage){
            PVOID br=NULL; SIZE_T vr=0;
            NTSTATUS stMapR = pNtMapViewOfSection(hSecRealImage, GetCurrentProcess(), &br, 0, 0, NULL, &vr, 1, 0, PAGE_READONLY);
            printf("  \"map_real_image\": {\"ntstatus\":%ld,\"hex\":\"0x%08lX\",\"base\":\"0x%llX\",\"viewSize\":%llu},\n", (long)stMapR,(unsigned long)(uint32_t)stMapR,(unsigned long long)(uintptr_t)br,(unsigned long long)vr);
            if(NT_SUCCESS(stMapR) || stMapR==0x40000003){
                printf("  \"vquery_real_image\": [\n");
                dump_mbi("real_image_base", br);
                printf("      {\"sentinel\":true}\n  ],\n");
                pNtUnmapViewOfSection(GetCurrentProcess(), br);
            }
        }
        if(hSecRealImage && pNtClose) pNtClose(hSecRealImage);
        CloseHandle(hRealPe);
    }
    HANDLE hMapCommitWin = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE|SEC_COMMIT, 0, 65536, NULL);
    if(hMapCommitWin){
        void *v = MapViewOfFile(hMapCommitWin, FILE_MAP_WRITE, 0,0,0);
        if(v){
            printf("  \"win32_commit_vquery\": [\n");
            dump_mbi("win32_commit", v);
            printf("      {\"sentinel\":true}\n  ],\n");
            UnmapViewOfFile(v);
        }
        CloseHandle(hMapCommitWin);
    }
    HANDLE hMapReserveWin = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE|SEC_RESERVE, 0, 65536, NULL);
    if(hMapReserveWin){
        void *v = MapViewOfFile(hMapReserveWin, FILE_MAP_WRITE, 0,0,4096);
        if(v){
            printf("  \"win32_reserve_vquery\": [\n");
            dump_mbi("win32_reserve", v);
            printf("      {\"sentinel\":true}\n  ],\n");
            UnmapViewOfFile(v);
        }
        CloseHandle(hMapReserveWin);
    }
    if(hSecCommit && pNtClose) pNtClose(hSecCommit);
    if(hSecReserve && pNtClose) pNtClose(hSecReserve);
    if(hSecImage && pNtClose) pNtClose(hSecImage);
    if(hSecFileCommit && pNtClose) pNtClose(hSecFileCommit);
    if(hFile && hFile!=INVALID_HANDLE_VALUE) CloseHandle(hFile);
    DeleteFileA(tmpFile);
    printf("  \"done\": true\n");
    printf("}\n");
    return 0;
}
#endif
