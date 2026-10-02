/* large_io.c - Test large NtWriteFile/NtReadFile truncation */
typedef unsigned long long ULONGLONG;
typedef unsigned long ULONG;
typedef unsigned short USHORT;
typedef unsigned short WCHAR;
typedef void* HANDLE;
typedef void* PVOID;
typedef long NTSTATUS;
typedef unsigned long ACCESS_MASK;
typedef struct { ULONGLONG Status; ULONGLONG Information; } IO_STATUS_BLOCK;
typedef unsigned long long ULONG_PTR;
typedef struct { USHORT Length; USHORT MaximumLength; WCHAR *Buffer; } UNICODE_STRING;
typedef struct { ULONG Length; HANDLE RootDirectory; UNICODE_STRING *ObjectName; ULONG Attributes; PVOID SecurityDescriptor; PVOID SecurityQualityOfService; } OBJECT_ATTRIBUTES;
typedef struct { ULONGLONG QuadPart; } LARGE_INTEGER;
#define STATUS_SUCCESS ((NTSTATUS)0x00000000L)
#define OBJ_CASE_INSENSITIVE 0x40
#define FILE_OVERWRITE_IF 5
#define FILE_ATTRIBUTE_NORMAL 0x80
#define FILE_SYNCHRONOUS_IO_NONALERT 0x20
#define FILE_NON_DIRECTORY_FILE 0x40
#define GENERIC_READWRITE 0x12019F
#define NtCurrentProcess() ((HANDLE)(long)-1)
#define MEM_COMMIT 0x1000
#define MEM_RESERVE 0x2000
#define PAGE_READWRITE 0x04
#define NULL ((void*)0)

__asm__(".text\n"
".globl NtCreateFile\nNtCreateFile: mov %rcx, %r10; mov $0x55, %eax; syscall; ret\n"
".globl NtClose\nNtClose: mov %rcx, %r10; mov $0x0f, %eax; syscall; ret\n"
".globl NtWriteFile\nNtWriteFile: mov %rcx, %r10; mov $0x08, %eax; syscall; ret\n"
".globl NtReadFile\nNtReadFile: mov %rcx, %r10; mov $0x06, %eax; syscall; ret\n"
".globl NtAllocateVirtualMemory\nNtAllocateVirtualMemory: mov %rcx, %r10; mov $0x18, %eax; syscall; ret\n"
".globl NtTerminateProcess\nNtTerminateProcess: mov %rcx, %r10; mov $0x2c, %eax; syscall; ret\n"
".globl linux_open\nlinux_open: mov $2, %eax; syscall; ret\n"
".globl linux_write\nlinux_write: mov $1, %eax; syscall; ret\n"
".globl linux_close\nlinux_close: mov $3, %eax; syscall; ret\n"
);
NTSTATUS NtCreateFile(HANDLE *FileHandle, ACCESS_MASK DesiredAccess, OBJECT_ATTRIBUTES *ObjectAttributes, IO_STATUS_BLOCK *IoStatusBlock, LARGE_INTEGER *AllocationSize, ULONG FileAttributes, ULONG ShareAccess, ULONG CreateDisposition, ULONG CreateOptions, PVOID EaBuffer, ULONG EaLength);
NTSTATUS NtClose(HANDLE Handle);
NTSTATUS NtWriteFile(HANDLE FileHandle, HANDLE Event, PVOID ApcRoutine, PVOID ApcContext, IO_STATUS_BLOCK *IoStatusBlock, PVOID Buffer, ULONG Length, LARGE_INTEGER *ByteOffset, ULONG *Key);
NTSTATUS NtReadFile(HANDLE FileHandle, HANDLE Event, PVOID ApcRoutine, PVOID ApcContext, IO_STATUS_BLOCK *IoStatusBlock, PVOID Buffer, ULONG Length, LARGE_INTEGER *ByteOffset, ULONG *Key);
NTSTATUS NtAllocateVirtualMemory(HANDLE ProcessHandle, PVOID *BaseAddress, ULONGLONG ZeroBits, ULONGLONG *RegionSize, ULONG AllocationType, ULONG Protect);
NTSTATUS NtTerminateProcess(HANDLE ProcessHandle, NTSTATUS ExitStatus);
long linux_open(const char *path, int flags, int mode);
long linux_write(int fd, const void *buf, unsigned long len);
long linux_close(int fd);

static int s_len(const char *s){int n=0; while(s[n]) n++; return n;}
static void u64_dec(char *out, ULONGLONG v){ char tmp[32]; int p=0; if(v==0){out[0]='0';out[1]=0;return;} while(v>0){tmp[p++]='0'+(v%10);v/=10;} for(int i=0;i<p;i++) out[i]=tmp[p-1-i]; out[p]=0; }
static void u32_hex(char *out, ULONG v){ const char *h="0123456789abcdef"; out[0]='0'; out[1]='x'; for(int i=0;i<8;i++) out[2+i]=h[(v>>(28-i*4))&0xF]; out[10]=0; }

/* simple json buffer */
static char *outbuf;
static int outpos=0;
static ULONGLONG outcap=200000;
static void o_add(const char *s){ int l=s_len(s); for(int i=0;i<l;i++) outbuf[outpos++]=s[i]; }
static void o_add_num(ULONGLONG v){ char tmp[32]; u64_dec(tmp,v); o_add(tmp); }
static void o_add_hex(ULONG v){ char tmp[12]; u32_hex(tmp,v); o_add(tmp); }

void _start(void){
    __asm__ volatile("sub $128, %rsp");
    PVOID base=0;
    ULONGLONG sz=outcap;
    NTSTATUS st = NtAllocateVirtualMemory(NtCurrentProcess(), &base, 0, &sz, MEM_COMMIT|MEM_RESERVE, PAGE_READWRITE);
    if(st!=STATUS_SUCCESS) NtTerminateProcess(NtCurrentProcess(),1);
    outbuf=(char*)base;
    outpos=0;

    /* allocate pattern buffer 4MB */
    PVOID pbase=0;
    ULONGLONG psz=4*1024*1024 + 4096;
    st = NtAllocateVirtualMemory(NtCurrentProcess(), &pbase, 0, &psz, MEM_COMMIT|MEM_RESERVE, PAGE_READWRITE);
    if(st!=STATUS_SUCCESS) NtTerminateProcess(NtCurrentProcess(),2);
    char *pat=(char*)pbase;
    for(ULONGLONG i=0;i<psz;i++) pat[i]=(char)(0x41 + (i%26)); // A-Z pattern

    ULONG sizes[] = {1024, 8000, 8192, 8193, 64000, 65536, 1024*1024, 2*1024*1024, 4*1024*1024};
    int nsizes = sizeof(sizes)/sizeof(sizes[0]);

    o_add("{\n \"test\": \"large_io\",\n \"writes\": [\n");
    for(int i=0;i<nsizes;i++){
        ULONG len = sizes[i];
        // Create file \??\C:\tmp\large_io_test.bin
        WCHAR wpath[512];
        const char *path="\\??\\C:\\tmp\\large_io_test.bin";
        int lp=0; while(path[lp]) lp++;
        for(int j=0;j<lp;j++) wpath[j]=(WCHAR)path[j]; wpath[lp]=0;
        UNICODE_STRING us; us.Length=lp*2; us.MaximumLength=us.Length+2; us.Buffer=wpath;
        OBJECT_ATTRIBUTES oa; oa.Length=sizeof(oa); oa.RootDirectory=0; oa.ObjectName=&us; oa.Attributes=OBJ_CASE_INSENSITIVE; oa.SecurityDescriptor=0; oa.SecurityQualityOfService=0;
        HANDLE h=0; IO_STATUS_BLOCK ios; LARGE_INTEGER alloc; alloc.QuadPart=0;
        st = NtCreateFile(&h, GENERIC_READWRITE, &oa, &ios, &alloc, FILE_ATTRIBUTE_NORMAL, 3, FILE_OVERWRITE_IF, FILE_SYNCHRONOUS_IO_NONALERT|FILE_NON_DIRECTORY_FILE,0,0);
        if(st!=STATUS_SUCCESS){
            if(i>0) o_add(",\n");
            o_add("  {\"size\":"); o_add_num(len); o_add(",\"create_status\":\""); o_add_hex((ULONG)st); o_add("\"}");
            continue;
        }
        IO_STATUS_BLOCK ios2; LARGE_INTEGER off; off.QuadPart=0;
        st = NtWriteFile(h,0,0,0,&ios2, pat, len, &off, 0);
        ULONGLONG info = ios2.Information;
        if(i>0) o_add(",\n");
        o_add("  {\"size\":"); o_add_num(len);
        o_add(",\"ntstatus\":\""); o_add_hex((ULONG)st); o_add("\"");
        o_add(",\"info\":"); o_add_num(info);
        o_add(",\"expected\":"); o_add_num(len);
        o_add(",\"match\":"); o_add(info==len ? "true":"false");
        o_add("}");
        NtClose(h);
    }
    o_add("\n ],\n \"reads\": [\n");
    // Also test read path: write 2MB then read back
    {
        ULONG wlen = 2*1024*1024;
        // create and write
        WCHAR wpath[512];
        const char *path="\\??\\C:\\tmp\\large_io_read.bin";
        int lp=0; while(path[lp]) lp++;
        for(int j=0;j<lp;j++) wpath[j]=(WCHAR)path[j]; wpath[lp]=0;
        UNICODE_STRING us; us.Length=lp*2; us.MaximumLength=us.Length+2; us.Buffer=wpath;
        OBJECT_ATTRIBUTES oa; oa.Length=sizeof(oa); oa.RootDirectory=0; oa.ObjectName=&us; oa.Attributes=OBJ_CASE_INSENSITIVE; oa.SecurityDescriptor=0; oa.SecurityQualityOfService=0;
        HANDLE h=0; IO_STATUS_BLOCK ios; LARGE_INTEGER alloc; alloc.QuadPart=0;
        NtCreateFile(&h, GENERIC_READWRITE, &oa, &ios, &alloc, FILE_ATTRIBUTE_NORMAL, 3, FILE_OVERWRITE_IF, FILE_SYNCHRONOUS_IO_NONALERT|FILE_NON_DIRECTORY_FILE,0,0);
        if(h){
            IO_STATUS_BLOCK ios2; LARGE_INTEGER off; off.QuadPart=0;
            NtWriteFile(h,0,0,0,&ios2, pat, wlen, &off,0);
            NtClose(h);
        }
        // reopen and read
        HANDLE h2=0;
        // FILE_OPEN =1
        st = NtCreateFile(&h2, GENERIC_READWRITE, &oa, &ios, &alloc, FILE_ATTRIBUTE_NORMAL, 3, 1, FILE_SYNCHRONOUS_IO_NONALERT|FILE_NON_DIRECTORY_FILE,0,0);
        if(h2 && st==STATUS_SUCCESS){
            PVOID rbase=0;
            ULONGLONG rsz=wlen+4096;
            NtAllocateVirtualMemory(NtCurrentProcess(), &rbase, 0, &rsz, MEM_COMMIT|MEM_RESERVE, PAGE_READWRITE);
            char *rbuf=(char*)rbase;
            for(ULONGLONG i=0;i<wlen;i++) rbuf[i]=0;
            IO_STATUS_BLOCK ios3; LARGE_INTEGER off2; off2.QuadPart=0;
            NTSTATUS rst = NtReadFile(h2,0,0,0,&ios3, rbuf, wlen, &off2,0);
            ULONGLONG rinfo=ios3.Information;
            int equal=1;
            if(rinfo!=wlen) equal=0;
            else {
                for(ULONGLONG i=0;i<wlen;i++) if(rbuf[i]!=pat[i]){ equal=0; break; }
            }
            o_add("  {\"write_len\":"); o_add_num(wlen);
            o_add(",\"read_status\":\""); o_add_hex((ULONG)rst); o_add("\"");
            o_add(",\"read_info\":"); o_add_num(rinfo);
            o_add(",\"equal\":"); o_add(equal?"true":"false"); o_add("}\n");
            NtClose(h2);
        } else {
            o_add("  {\"error\":\"open_read_failed\",\"status\":\""); o_add_hex((ULONG)st); o_add("\"}\n");
        }
    }
    o_add(" ],\n \"byte_equality\": \"pending\"\n}\n");
    // write output via NT to share file \??\C:\...\large_io.json via NtWriteFile
    {
        WCHAR wout[1024]; const char *p="/mnt/share/.dodresult/large_io.json";
        // translate to NT path \??\mnt\share\.dodresult\large_io.json
        const char *np="\\??\\mnt\\share\\.dodresult\\large_io.json";
        int ln=0; while(np[ln]) ln++;
        for(int j=0;j<ln;j++) wout[j]=(WCHAR)np[j]; wout[ln]=0;
        UNICODE_STRING us; us.Length=ln*2; us.MaximumLength=us.Length+2; us.Buffer=wout;
        OBJECT_ATTRIBUTES oa; oa.Length=sizeof(oa); oa.RootDirectory=0; oa.ObjectName=&us; oa.Attributes=OBJ_CASE_INSENSITIVE; oa.SecurityDescriptor=0; oa.SecurityQualityOfService=0;
        HANDLE hout=0; IO_STATUS_BLOCK ios; LARGE_INTEGER alloc; alloc.QuadPart=0;
        if(NtCreateFile(&hout, GENERIC_READWRITE, &oa, &ios, &alloc, FILE_ATTRIBUTE_NORMAL, 3, FILE_OVERWRITE_IF, FILE_SYNCHRONOUS_IO_NONALERT|FILE_NON_DIRECTORY_FILE,0,0)==STATUS_SUCCESS){
            IO_STATUS_BLOCK ios2; LARGE_INTEGER off; off.QuadPart=0;
            NtWriteFile(hout,0,0,0,&ios2, outbuf, outpos, &off,0);
            NtClose(hout);
        }
        // also write to stdout via NtWriteFile to console -11 and via simple loop to capture? Use fallback: try linux via direct host syscall bypass? Instead we rely on file above.
        // Additionally try to echo via console for dmesg
        // Use NtWriteFile to -11 if available, but we already have file
    }
    // verify byte equality with 4MB pattern file
    {
        // byte equality test: write 4MB pattern, read back, compare
        ULONG blen = 4*1024*1024;
        WCHAR wpath[512];
        const char *path="\\??\\C:\\tmp\\large_io_equality.bin";
        int lp=0; while(path[lp]) lp++;
        for(int j=0;j<lp;j++) wpath[j]=(WCHAR)path[j]; wpath[lp]=0;
        UNICODE_STRING us; us.Length=lp*2; us.MaximumLength=us.Length+2; us.Buffer=wpath;
        OBJECT_ATTRIBUTES oa; oa.Length=sizeof(oa); oa.RootDirectory=0; oa.ObjectName=&us; oa.Attributes=OBJ_CASE_INSENSITIVE; oa.SecurityDescriptor=0; oa.SecurityQualityOfService=0;
        HANDLE h=0; IO_STATUS_BLOCK ios; LARGE_INTEGER alloc; alloc.QuadPart=0;
        NtCreateFile(&h, GENERIC_READWRITE, &oa, &ios, &alloc, FILE_ATTRIBUTE_NORMAL, 3, FILE_OVERWRITE_IF, FILE_SYNCHRONOUS_IO_NONALERT|FILE_NON_DIRECTORY_FILE,0,0);
        if(h){
            IO_STATUS_BLOCK ios2; LARGE_INTEGER off; off.QuadPart=0;
            NtWriteFile(h,0,0,0,&ios2, pat, blen, &off,0);
            NtClose(h);
            // read back
            HANDLE hr=0;
            NtCreateFile(&hr, GENERIC_READWRITE, &oa, &ios, &alloc, FILE_ATTRIBUTE_NORMAL, 3, 1, FILE_SYNCHRONOUS_IO_NONALERT|FILE_NON_DIRECTORY_FILE,0,0);
            if(hr){
                PVOID rbase2=0;
                ULONGLONG rsz2=blen+4096;
                NtAllocateVirtualMemory(NtCurrentProcess(), &rbase2, 0, &rsz2, MEM_COMMIT|MEM_RESERVE, PAGE_READWRITE);
                char *rbuf2=(char*)rbase2;
                IO_STATUS_BLOCK ios3; LARGE_INTEGER off2; off2.QuadPart=0;
                NtReadFile(hr,0,0,0,&ios3, rbuf2, blen, &off2,0);
                int eq=1;
                if(ios3.Information != blen) eq=0;
                else for(ULONGLONG i=0;i<blen;i++) if(rbuf2[i]!=pat[i]){eq=0; break;}
                // append to share as separate file via NT
                const char *pfx = eq ? "{\"equality\": true, \"bytes\": 4194304}\n" : "{\"equality\": false}\n";
                int l=s_len(pfx);
                {
                    WCHAR wout2[1024]; const char *np2="\\??\\mnt\\share\\.dodresult\\large_io_equality.json";
                    int ln2=0; while(np2[ln2]) ln2++;
                    for(int j=0;j<ln2;j++) wout2[j]=(WCHAR)np2[j]; wout2[ln2]=0;
                    UNICODE_STRING us2; us2.Length=ln2*2; us2.MaximumLength=us2.Length+2; us2.Buffer=wout2;
                    OBJECT_ATTRIBUTES oa2; oa2.Length=sizeof(oa2); oa2.RootDirectory=0; oa2.ObjectName=&us2; oa2.Attributes=OBJ_CASE_INSENSITIVE; oa2.SecurityDescriptor=0; oa2.SecurityQualityOfService=0;
                    HANDLE hout2=0; IO_STATUS_BLOCK ios2; LARGE_INTEGER alloc2; alloc2.QuadPart=0;
                    if(NtCreateFile(&hout2, GENERIC_READWRITE, &oa2, &ios2, &alloc2, FILE_ATTRIBUTE_NORMAL, 3, FILE_OVERWRITE_IF, FILE_SYNCHRONOUS_IO_NONALERT|FILE_NON_DIRECTORY_FILE,0,0)==STATUS_SUCCESS){
                        IO_STATUS_BLOCK ios2b; LARGE_INTEGER off2; off2.QuadPart=0;
                        NtWriteFile(hout2,0,0,0,&ios2b, (PVOID)pfx, l, &off2,0);
                        NtClose(hout2);
                    }
                }
                NtClose(hr);
                // exit code reflects equality
                if(!eq) NtTerminateProcess(NtCurrentProcess(), 10);
            }
        }
    }
    // 9p byte-equality test: write 2MB to 9p share and read back
    {
        ULONG blen = 2*1024*1024;
        WCHAR wpath[512];
        const char *path="\\??\\mnt\\share\\.dodresult\\large_9p_equality.bin";
        int lp=0; while(path[lp]) lp++;
        for(int j=0;j<lp;j++) wpath[j]=(WCHAR)path[j]; wpath[lp]=0;
        UNICODE_STRING us; us.Length=lp*2; us.MaximumLength=us.Length+2; us.Buffer=wpath;
        OBJECT_ATTRIBUTES oa; oa.Length=sizeof(oa); oa.RootDirectory=0; oa.ObjectName=&us; oa.Attributes=OBJ_CASE_INSENSITIVE; oa.SecurityDescriptor=0; oa.SecurityQualityOfService=0;
        HANDLE h=0; IO_STATUS_BLOCK ios; LARGE_INTEGER alloc; alloc.QuadPart=0;
        if(NtCreateFile(&h, GENERIC_READWRITE, &oa, &ios, &alloc, FILE_ATTRIBUTE_NORMAL, 3, FILE_OVERWRITE_IF, FILE_SYNCHRONOUS_IO_NONALERT|FILE_NON_DIRECTORY_FILE,0,0)==STATUS_SUCCESS){
            IO_STATUS_BLOCK ios2; LARGE_INTEGER off; off.QuadPart=0;
            NTSTATUS wst = NtWriteFile(h,0,0,0,&ios2, pat, blen, &off,0);
            ULONG_PTR winfo = ios2.Information;
            NtClose(h);
            // read back via 9p
            HANDLE hr=0;
            if(NtCreateFile(&hr, GENERIC_READWRITE, &oa, &ios, &alloc, FILE_ATTRIBUTE_NORMAL, 3, 1, FILE_SYNCHRONOUS_IO_NONALERT|FILE_NON_DIRECTORY_FILE,0,0)==STATUS_SUCCESS){
                PVOID rbase3=0;
                ULONGLONG rsz3=blen+4096;
                NtAllocateVirtualMemory(NtCurrentProcess(), &rbase3, 0, &rsz3, MEM_COMMIT|MEM_RESERVE, PAGE_READWRITE);
                char *rbuf3=(char*)rbase3;
                IO_STATUS_BLOCK ios3; LARGE_INTEGER off2; off2.QuadPart=0;
                NTSTATUS rst = NtReadFile(hr,0,0,0,&ios3, rbuf3, blen, &off2,0);
                int eq2=1;
                if(ios3.Information != blen) eq2=0;
                else if(winfo != blen) eq2=0;
                else for(ULONGLONG i=0;i<blen;i++) if(rbuf3[i]!=pat[i]){eq2=0; break;}
                const char *pfx2 = eq2 ? "{\"9p_equality\": true, \"bytes\": 2097152, \"write_info\": 2097152}\n" : "{\"9p_equality\": false}\n";
                // write result to separate file via NT
                {
                    WCHAR wout3[1024]; const char *np3="\\??\\mnt\\share\\.dodresult\\large_9p_result.json";
                    int ln3=0; while(np3[ln3]) ln3++;
                    for(int j=0;j<ln3;j++) wout3[j]=(WCHAR)np3[j]; wout3[ln3]=0;
                    UNICODE_STRING us3; us3.Length=ln3*2; us3.MaximumLength=us3.Length+2; us3.Buffer=wout3;
                    OBJECT_ATTRIBUTES oa3; oa3.Length=sizeof(oa3); oa3.RootDirectory=0; oa3.ObjectName=&us3; oa3.Attributes=OBJ_CASE_INSENSITIVE; oa3.SecurityDescriptor=0; oa3.SecurityQualityOfService=0;
                    HANDLE hout3=0; IO_STATUS_BLOCK ios3b; LARGE_INTEGER alloc3; alloc3.QuadPart=0;
                    if(NtCreateFile(&hout3, GENERIC_READWRITE, &oa3, &ios3b, &alloc3, FILE_ATTRIBUTE_NORMAL, 3, FILE_OVERWRITE_IF, FILE_SYNCHRONOUS_IO_NONALERT|FILE_NON_DIRECTORY_FILE,0,0)==STATUS_SUCCESS){
                        IO_STATUS_BLOCK ios3c; LARGE_INTEGER off3; off3.QuadPart=0;
                        int l2=s_len(pfx2);
                        NtWriteFile(hout3,0,0,0,&ios3c, (PVOID)pfx2, l2, &off3,0);
                        NtClose(hout3);
                    }
                }
                NtClose(hr);
                if(!eq2) NtTerminateProcess(NtCurrentProcess(), 11);
            } else {
                NtTerminateProcess(NtCurrentProcess(), 12);
            }
        } else {
            NtTerminateProcess(NtCurrentProcess(), 13);
        }
    }
    NtTerminateProcess(NtCurrentProcess(),0);
    __asm__ volatile("mov $60, %%rax; mov $0, %%rdi; syscall" ::: "rax","rdi");
    while(1){}
}
