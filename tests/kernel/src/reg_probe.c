/* reg_probe.c - registry probe for VM kernel personality
 * Mirrors shim suite at 10/10 plus golden probe_registry.json (26200.9168)
 * Tests: disposition, nine types, embed NUL, 1MB, missing, querykey small, enumerate past, access denied, wow64, delete/enum/flush
 * Exit codes: 0 = all pass, 1-99 = specific failure, 90 = missing service, 42 = fallback
 */
typedef unsigned long long ULONGLONG;
typedef unsigned long ULONG;
typedef unsigned short USHORT;
typedef unsigned short WCHAR;
typedef void* HANDLE;
typedef HANDLE* PHANDLE;
typedef void* PVOID;
typedef PVOID* PPVOID;
typedef unsigned long ACCESS_MASK;
typedef long NTSTATUS;
typedef unsigned char BOOLEAN;
typedef unsigned long long SIZE_T;
typedef SIZE_T* PSIZE_T;
typedef ULONG* PULONG;
typedef struct { ULONGLONG QuadPart; } LARGE_INTEGER;
typedef struct { USHORT Length; USHORT MaximumLength; WCHAR *Buffer; } UNICODE_STRING;
typedef UNICODE_STRING* PUNICODE_STRING;
typedef struct { ULONG Length; HANDLE RootDirectory; UNICODE_STRING *ObjectName; ULONG Attributes; PVOID SecurityDescriptor; PVOID SecurityQualityOfService; } OBJECT_ATTRIBUTES;
typedef OBJECT_ATTRIBUTES* POBJECT_ATTRIBUTES;
typedef struct { ULONGLONG Status; ULONGLONG Information; } IO_STATUS_BLOCK;

#define STATUS_SUCCESS ((NTSTATUS)0x00000000L)
#define STATUS_BUFFER_TOO_SMALL ((NTSTATUS)0xC0000023L)
#define STATUS_BUFFER_OVERFLOW ((NTSTATUS)0x80000005L)
#define STATUS_OBJECT_NAME_NOT_FOUND ((NTSTATUS)0xC0000034L)
#define STATUS_NO_MORE_ENTRIES ((NTSTATUS)0x8000001AL)
#define STATUS_ACCESS_DENIED ((NTSTATUS)0xC0000022L)
#define STATUS_NOT_IMPLEMENTED ((NTSTATUS)0xC0000002L)
#define STATUS_INVALID_PARAMETER ((NTSTATUS)0xC000000DL)

#define REG_CREATED_NEW_KEY 1
#define REG_OPENED_EXISTING_KEY 2
#define REG_SZ 1
#define REG_EXPAND_SZ 2
#define REG_BINARY 3
#define REG_DWORD 4
#define REG_QWORD 11
#define REG_MULTI_SZ 7
#define REG_NONE 0
#define KEY_QUERY_VALUE 0x0001
#define KEY_SET_VALUE 0x0002
#define KEY_CREATE_SUB_KEY 0x0004
#define KEY_ENUMERATE_SUB_KEYS 0x0008
#define KEY_NOTIFY 0x0010
#define KEY_WOW64_64KEY 0x0100
#define KEY_WOW64_32KEY 0x0200
#define KEY_READ 0x00020019
#define KEY_WRITE 0x00020006
#define KEY_ALL_ACCESS 0x000F003F
#define OBJ_CASE_INSENSITIVE 0x00000040
#define KeyBasicInformation 0
#define KeyNodeInformation 1
#define KeyFullInformation 2
#define KeyNameInformation 3
#define KeyValueBasicInformation 0
#define KeyValueFullInformation 1
#define KeyValuePartialInformation 2

typedef struct { LARGE_INTEGER LastWriteTime; ULONG TitleIndex; ULONG NameLength; WCHAR Name[1]; } KEY_BASIC_INFORMATION;
typedef struct { LARGE_INTEGER LastWriteTime; ULONG TitleIndex; ULONG ClassOffset; ULONG ClassLength; ULONG NameLength; WCHAR Name[1]; } KEY_NODE_INFORMATION;
typedef struct { LARGE_INTEGER LastWriteTime; ULONG TitleIndex; ULONG ClassOffset; ULONG ClassLength; ULONG SubKeys; ULONG MaxNameLen; ULONG MaxClassLen; ULONG Values; ULONG MaxValueNameLen; ULONG MaxValueDataLen; WCHAR Class[1]; } KEY_FULL_INFORMATION;
typedef struct { ULONG NameLength; WCHAR Name[1]; } KEY_NAME_INFORMATION;
typedef struct { ULONG TitleIndex; ULONG Type; ULONG NameLength; WCHAR Name[1]; } KEY_VALUE_BASIC_INFORMATION;
typedef struct { ULONG TitleIndex; ULONG Type; ULONG DataOffset; ULONG DataLength; ULONG NameLength; WCHAR Name[1]; } KEY_VALUE_FULL_INFORMATION;
typedef struct { ULONG TitleIndex; ULONG Type; ULONG DataLength; unsigned char Data[1]; } KEY_VALUE_PARTIAL_INFORMATION;

__asm__(".text\n"
".globl NtCreateKey\nNtCreateKey: mov %rcx, %r10; mov $0x1d, %eax; syscall; ret\n"
".globl NtOpenKey\nNtOpenKey: mov %rcx, %r10; mov $0x12, %eax; syscall; ret\n"
".globl NtOpenKeyEx\nNtOpenKeyEx: mov %rcx, %r10; mov $0x126, %eax; syscall; ret\n"
".globl NtDeleteKey\nNtDeleteKey: mov %rcx, %r10; mov $0xd8, %eax; syscall; ret\n"
".globl NtFlushKey\nNtFlushKey: mov %rcx, %r10; mov $0xee, %eax; syscall; ret\n"
".globl NtEnumerateKey\nNtEnumerateKey: mov %rcx, %r10; mov $0x32, %eax; syscall; ret\n"
".globl NtEnumerateValueKey\nNtEnumerateValueKey: mov %rcx, %r10; mov $0x13, %eax; syscall; ret\n"
".globl NtQueryKey\nNtQueryKey: mov %rcx, %r10; mov $0x16, %eax; syscall; ret\n"
".globl NtQueryValueKey\nNtQueryValueKey: mov %rcx, %r10; mov $0x17, %eax; syscall; ret\n"
".globl NtSetValueKey\nNtSetValueKey: mov %rcx, %r10; mov $0x60, %eax; syscall; ret\n"
".globl NtDeleteValueKey\nNtDeleteValueKey: mov %rcx, %r10; mov $0xdb, %eax; syscall; ret\n"
".globl NtClose\nNtClose: mov %rcx, %r10; mov $0x0f, %eax; syscall; ret\n"
".globl NtTerminateProcess\nNtTerminateProcess: mov %rcx, %r10; mov $0x2c, %eax; syscall; ret\n"
".globl NtAllocateVirtualMemory\nNtAllocateVirtualMemory: mov %rcx, %r10; mov $0x18, %eax; syscall; ret\n"
".globl NtFreeVirtualMemory\nNtFreeVirtualMemory: mov %rcx, %r10; mov $0x1e, %eax; syscall; ret\n"
);

NTSTATUS NtCreateKey(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, ULONG, PUNICODE_STRING, ULONG, PULONG);
NTSTATUS NtOpenKey(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES);
NTSTATUS NtOpenKeyEx(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, ULONG);
NTSTATUS NtDeleteKey(HANDLE);
NTSTATUS NtFlushKey(HANDLE);
NTSTATUS NtEnumerateKey(HANDLE, ULONG, ULONG, PVOID, ULONG, PULONG);
NTSTATUS NtEnumerateValueKey(HANDLE, ULONG, ULONG, PVOID, ULONG, PULONG);
NTSTATUS NtQueryKey(HANDLE, ULONG, PVOID, ULONG, PULONG);
NTSTATUS NtQueryValueKey(HANDLE, PUNICODE_STRING, ULONG, PVOID, ULONG, PULONG);
NTSTATUS NtSetValueKey(HANDLE, PUNICODE_STRING, ULONG, ULONG, PVOID, ULONG);
NTSTATUS NtDeleteValueKey(HANDLE, PUNICODE_STRING);
NTSTATUS NtClose(HANDLE);
NTSTATUS NtTerminateProcess(HANDLE, NTSTATUS);
NTSTATUS NtAllocateVirtualMemory(HANDLE, PVOID*, ULONGLONG, SIZE_T*, ULONG, ULONG);
NTSTATUS NtFreeVirtualMemory(HANDLE, PVOID*, SIZE_T*, ULONG);
#define NtCurrentProcess() ((HANDLE)(long)-1)

static void wstr_copy(WCHAR *dst, const char *src, ULONG len){
    for(ULONG i=0;i<len;i++) dst[i]=(WCHAR)(unsigned char)src[i];
}
static void make_us(UNICODE_STRING *us, const char *s, WCHAR *buf){
    ULONG len=0; while(s[len]) len++;
    wstr_copy(buf, s, len);
    us->Buffer=buf; us->Length=(USHORT)(len*2); us->MaximumLength=us->Length+2;
    if(len) buf[len]=0;
}
static void make_oa(OBJECT_ATTRIBUTES *oa, UNICODE_STRING *us){
    oa->Length=sizeof(OBJECT_ATTRIBUTES);
    oa->RootDirectory=(HANDLE)0;
    oa->ObjectName=us;
    oa->Attributes=OBJ_CASE_INSENSITIVE;
    oa->SecurityDescriptor=(PVOID)0;
    oa->SecurityQualityOfService=(PVOID)0;
}

void _start(){
 __asm__ volatile("sub $4096, %rsp");
 NTSTATUS st;
 HANDLE h1=0, h2=0, hk=0, hd=0, hm=0;
 ULONG disp=0;
 UNICODE_STRING us;
 OBJECT_ATTRIBUTES oa;
 WCHAR wbuf[512];
 WCHAR wbuf2[512];
 // Test placeholders for exit codes
 // 1: disposition first create != NEW
 // 2: disposition second != EXISTING
 // 11-19: nine types
 // 20: embed NUL
 // 30: 1MB
 // 40: missing key
 // 41: missing value
 // 50-53: querykey small buffer checks
 // 60: enumerate past
 // 70: access denied ro
 // 71: access denied rw
 // 80: wow64 32
 // 81: wow64 64
 // 90: delete/enum

 // ---- Test 1: disposition ----
 {
   make_us(&us, "\\Registry\\User\\Software\\ntpers_probe_vm1", wbuf);
   make_oa(&oa, &us);
   disp=0xDEAD;
   st = NtCreateKey(&h1, KEY_ALL_ACCESS, &oa, 0, (PUNICODE_STRING)0, 0, &disp);
   if(st==STATUS_NOT_IMPLEMENTED){ NtTerminateProcess(NtCurrentProcess(),90); while(1){} }
   if(st!=STATUS_SUCCESS){ NtTerminateProcess(NtCurrentProcess(),1); while(1){} }
   if(disp!=REG_CREATED_NEW_KEY){ NtTerminateProcess(NtCurrentProcess(),1); while(1){} }
   disp=0xDEAD;
   st = NtCreateKey(&h2, KEY_ALL_ACCESS, &oa, 0, (PUNICODE_STRING)0, 0, &disp);
   if(st!=STATUS_SUCCESS){ NtTerminateProcess(NtCurrentProcess(),2); while(1){} }
   if(disp!=REG_OPENED_EXISTING_KEY){ NtTerminateProcess(NtCurrentProcess(),2); while(1){} }
   NtClose(h1); NtClose(h2);
   // cleanup delete
   HANDLE hopen=0;
   st = NtOpenKey(&hopen, KEY_ALL_ACCESS, &oa);
   if(st==STATUS_SUCCESS){ NtDeleteKey(hopen); NtClose(hopen); }
 }
 // ---- Test 2: nine value types ----
 {
   make_us(&us, "\\Registry\\User\\Software\\ntpers_probe_9types", wbuf);
   make_oa(&oa, &us);
   st = NtCreateKey(&hk, KEY_ALL_ACCESS, &oa, 0, (PUNICODE_STRING)0, 0, &disp);
   if(st!=STATUS_SUCCESS){ NtTerminateProcess(NtCurrentProcess(),11); while(1){} }
   // Prepare data
   unsigned char sz_data[6]={'h','e','l','l','o',0};
   unsigned char exp_data[17]; for(int k=0;k<17;k++) exp_data[k]=(unsigned char)('A'+(k%26));
   unsigned char bin_data[4]={0x01,0x02,0x03,0x04};
   ULONG dword_data=0x12345678;
   ULONGLONG qword_data=0x1122334455667788ULL;
   unsigned char multi_data[8]={'a',0,'b',0,'c',0,0,0};
   unsigned char none_data[7]={'r','a','w','n','o','n','e'};
   unsigned char empty_sz[1]={0};
   struct { const char *name; ULONG type; unsigned char *data; ULONG len; } vals[] = {
       {"Val_SZ", REG_SZ, sz_data,6},
       {"Val_EXPAND", REG_EXPAND_SZ, exp_data,17},
       {"Val_BINARY", REG_BINARY, bin_data,4},
       {"Val_DWORD", REG_DWORD, (unsigned char*)&dword_data,4},
       {"Val_QWORD", REG_QWORD, (unsigned char*)&qword_data,8},
       {"Val_MULTI", REG_MULTI_SZ, multi_data,8},
       {"Val_NONE", REG_NONE, none_data,7},
       {"Val_EMPTY_SZ", REG_SZ, empty_sz,1},
       {"Val_EMPTY_BINARY", REG_BINARY, (unsigned char*)"",0},
   };
   for(int i=0;i<9;i++){
       WCHAR wn[64]; UNICODE_STRING vus; make_us(&vus, vals[i].name, wn);
       st = NtSetValueKey(hk, &vus, 0, vals[i].type, vals[i].data, vals[i].len);
       if(st!=STATUS_SUCCESS){ NtTerminateProcess(NtCurrentProcess(),12+i); while(1){} }
       if(st==STATUS_NOT_IMPLEMENTED){ NtTerminateProcess(NtCurrentProcess(),90); while(1){} }
   }
   for(int i=0;i<9;i++){
       WCHAR wn[64]; UNICODE_STRING vus; make_us(&vus, vals[i].name, wn);
       unsigned char out[128]; ULONG rlen=0xDEAD;
       st = NtQueryValueKey(hk, &vus, KeyValuePartialInformation, out, sizeof(out), &rlen);
       if(st!=STATUS_SUCCESS){ NtTerminateProcess(NtCurrentProcess(),20+i); while(1){} }
       KEY_VALUE_PARTIAL_INFORMATION *p=(KEY_VALUE_PARTIAL_INFORMATION*)out;
       if(p->Type!=vals[i].type){ NtTerminateProcess(NtCurrentProcess(),30+i); while(1){} }
       if(p->DataLength!=vals[i].len){ NtTerminateProcess(NtCurrentProcess(),40+i); while(1){} }
       for(ULONG k=0;k<vals[i].len;k++) if(p->Data[k]!=vals[i].data[k]){ NtTerminateProcess(NtCurrentProcess(),50+i); while(1){} }
   }
   NtClose(hk);
   make_us(&us, "\\Registry\\User\\Software\\ntpers_probe_9types", wbuf); make_oa(&oa,&us);
   st=NtOpenKey(&hd,KEY_ALL_ACCESS,&oa); if(st==STATUS_SUCCESS){ NtDeleteKey(hd); NtClose(hd); }
 }
 // ---- Test 3: embedded NUL 9 bytes ----
 {
   make_us(&us, "\\Registry\\User\\Software\\ntpers_probe_embed", wbuf);
   make_oa(&oa,&us);
   st=NtCreateKey(&hk,KEY_ALL_ACCESS,&oa,0,(PUNICODE_STRING)0,0,&disp); if(st!=STATUS_SUCCESS){ NtTerminateProcess(NtCurrentProcess(),60); while(1){} }
   unsigned char embed[9]={'h','i',0,'t','h','e','r','e',0};
   WCHAR wn[64]; UNICODE_STRING vus; make_us(&vus,"Val_EMBED_NUL",wn);
   st=NtSetValueKey(hk,&vus,0,REG_SZ,embed,9); if(st!=STATUS_SUCCESS){ NtTerminateProcess(NtCurrentProcess(),61); while(1){} }
   unsigned char out[64]; ULONG rlen=0;
   st=NtQueryValueKey(hk,&vus,KeyValuePartialInformation,out,sizeof(out),&rlen);
   if(st!=STATUS_SUCCESS){ NtTerminateProcess(NtCurrentProcess(),62); while(1){} }
   KEY_VALUE_PARTIAL_INFORMATION *p=(KEY_VALUE_PARTIAL_INFORMATION*)out;
   if(p->Type!=REG_SZ || p->DataLength!=9){ NtTerminateProcess(NtCurrentProcess(),63); while(1){} }
   if(p->Data[0]!=104 || p->Data[1]!=105 || p->Data[2]!=0 || p->Data[3]!=116){ NtTerminateProcess(NtCurrentProcess(),64); while(1){} }
   for(int i=0;i<9;i++) if(p->Data[i]!=embed[i]){ NtTerminateProcess(NtCurrentProcess(),65); while(1){} }
   NtClose(hk);
   make_us(&us, "\\Registry\\User\\Software\\ntpers_probe_embed", wbuf); make_oa(&oa,&us);
   st=NtOpenKey(&hd,KEY_ALL_ACCESS,&oa); if(st==STATUS_SUCCESS){ NtDeleteKey(hd); NtClose(hd); }
 }
 // ---- Test 4: 1 MB value 0xAB ----
 {
   make_us(&us, "\\Registry\\User\\Software\\ntpers_probe_1mb", wbuf);
   make_oa(&oa,&us);
   st=NtCreateKey(&hk,KEY_ALL_ACCESS,&oa,0,(PUNICODE_STRING)0,0,&disp); if(st!=STATUS_SUCCESS){ NtTerminateProcess(NtCurrentProcess(),70); while(1){} }
   SIZE_T bigLen=1024*1024;
   PVOID base=0; SIZE_T region=bigLen;
   st=NtAllocateVirtualMemory(NtCurrentProcess(), &base, 0, &region, 0x3000, 0x04); // RESERVE|COMMIT RW
   if(st!=STATUS_SUCCESS){ NtTerminateProcess(NtCurrentProcess(),71); while(1){} }
   for(SIZE_T i=0;i<bigLen;i++) ((unsigned char*)base)[i]=0xAB;
   WCHAR wn[64]; UNICODE_STRING vus; make_us(&vus,"Val_1MB",wn);
   st=NtSetValueKey(hk,&vus,0,REG_BINARY,base,(ULONG)bigLen); if(st!=STATUS_SUCCESS){ NtTerminateProcess(NtCurrentProcess(),72); while(1){} }
   // Use static buffer for out to avoid VAD access_ok issues
   static unsigned char outStatic[1024*1024+8192];
   PVOID outBase=(PVOID)outStatic; (void)outBase;
   ULONG rlen=0;
   st=NtQueryValueKey(hk,&vus,KeyValuePartialInformation,outStatic,(ULONG)(bigLen+4096),&rlen);
   if(st!=STATUS_SUCCESS){ NTSTATUS code = (st & 0xFF) + 100; NtTerminateProcess(NtCurrentProcess(), code); while(1){} }
   KEY_VALUE_PARTIAL_INFORMATION *p=(KEY_VALUE_PARTIAL_INFORMATION*)outBase;
   if(p->DataLength!=bigLen){ NtTerminateProcess(NtCurrentProcess(),75); while(1){} }
   for(SIZE_T i=0;i<bigLen;i++) if(p->Data[i]!=0xAB){ NtTerminateProcess(NtCurrentProcess(),76); while(1){} }
   // free
   PVOID b1=base; SIZE_T s1=0; NtFreeVirtualMemory(NtCurrentProcess(),&b1,&s1,0x8000);
   NtClose(hk);
   make_us(&us, "\\Registry\\User\\Software\\ntpers_probe_1mb", wbuf); make_oa(&oa,&us);
   st=NtOpenKey(&hd,KEY_ALL_ACCESS,&oa); if(st==STATUS_SUCCESS){ NtDeleteKey(hd); NtClose(hd); }
 }
 // ---- Test 5: missing key/value ----
 {
   make_us(&us, "\\Registry\\User\\Software\\NoSuchKey12345", wbuf);
   make_oa(&oa,&us);
   HANDLE h=0;
   st=NtOpenKey(&h,KEY_READ,&oa);
   if(st!=STATUS_OBJECT_NAME_NOT_FOUND){ NtTerminateProcess(NtCurrentProcess(),80); while(1){} }
   make_us(&us, "\\Registry\\User\\Software\\ntpers_probe_missval", wbuf); make_oa(&oa,&us);
   st=NtCreateKey(&hk,KEY_ALL_ACCESS,&oa,0,(PUNICODE_STRING)0,0,&disp); if(st!=STATUS_SUCCESS){ NtTerminateProcess(NtCurrentProcess(),81); while(1){} }
   WCHAR wn[64]; UNICODE_STRING vus; make_us(&vus,"NoSuchValue",wn);
   unsigned char out[64]; ULONG rlen=0;
   st=NtQueryValueKey(hk,&vus,KeyValuePartialInformation,out,sizeof(out),&rlen);
   if(st!=STATUS_OBJECT_NAME_NOT_FOUND){ NtTerminateProcess(NtCurrentProcess(),82); while(1){} }
   NtClose(hk);
   make_us(&us, "\\Registry\\User\\Software\\ntpers_probe_missval", wbuf); make_oa(&oa,&us);
   st=NtOpenKey(&hd,KEY_ALL_ACCESS,&oa); if(st==STATUS_SUCCESS){ NtDeleteKey(hd); NtClose(hd); }
 }
 // ---- Test 6: NtQueryKey small buffer ----
 {
   make_us(&us, "\\Registry\\User\\Software\\ntpers_probe_qkey", wbuf);
   make_oa(&oa,&us);
   st=NtCreateKey(&hk,KEY_ALL_ACCESS,&oa,0,(PUNICODE_STRING)0,0,&disp); if(st!=STATUS_SUCCESS){ NtTerminateProcess(NtCurrentProcess(),90); while(1){} }
   for(ULONG cls=0;cls<4;cls++){
       unsigned char buf[512]; ULONG rlen=0xDEADBEEF;
       st=NtQueryKey(hk,cls,buf,sizeof(buf),&rlen);
       if(st!=STATUS_SUCCESS){ NtTerminateProcess(NtCurrentProcess(),91+cls); while(1){} }
       if(rlen==0xDEADBEEF || rlen==0){ NtTerminateProcess(NtCurrentProcess(),95); while(1){} }
       unsigned char small[4]; ULONG rlen2=0xDEADBEEF;
       NTSTATUS stSmall=NtQueryKey(hk,cls,small,4,&rlen2);
       if(cls==3){
           if(stSmall!=STATUS_BUFFER_OVERFLOW){ NtTerminateProcess(NtCurrentProcess(),96); while(1){} }
       } else {
           if(stSmall!=STATUS_BUFFER_TOO_SMALL){ NtTerminateProcess(NtCurrentProcess(),97); while(1){} }
       }
       if(rlen2==0xDEADBEEF || rlen2!=rlen){ NtTerminateProcess(NtCurrentProcess(),98); while(1){} }
   }
   NtClose(hk);
   make_us(&us, "\\Registry\\User\\Software\\ntpers_probe_qkey", wbuf); make_oa(&oa,&us);
   st=NtOpenKey(&hd,KEY_ALL_ACCESS,&oa); if(st==STATUS_SUCCESS){ NtDeleteKey(hd); NtClose(hd); }
 }
 // ---- Test 7: enumerate past end ----
 {
   make_us(&us, "\\Registry\\Machine\\Software", wbuf);
   make_oa(&oa,&us);
   st=NtOpenKey(&hm,KEY_READ,&oa); if(st!=STATUS_SUCCESS){ NtTerminateProcess(NtCurrentProcess(),100); while(1){} }
   unsigned char buf[512]; ULONG rlen=0;
   st=NtEnumerateKey(hm, 9999, KeyBasicInformation, buf, sizeof(buf), &rlen);
   if(st!=STATUS_NO_MORE_ENTRIES){ NtTerminateProcess(NtCurrentProcess(),101); while(1){} }
   NtClose(hm);
 }
 // ---- Test 8: HKLM access denied ----
 {
   make_us(&us, "\\Registry\\Machine\\Software", wbuf);
   make_oa(&oa,&us);
   HANDLE hro=0, hrw=0;
   st=NtOpenKey(&hro, KEY_READ, &oa);
   if(st!=STATUS_SUCCESS){ NtTerminateProcess(NtCurrentProcess(),110); while(1){} }
   NtClose(hro);
   st=NtOpenKey(&hrw, KEY_WRITE, &oa);
   if(st!=STATUS_ACCESS_DENIED){ NtTerminateProcess(NtCurrentProcess(),111); while(1){} }
 }
 // ---- Test 9: WOW64 ----
 {
   make_us(&us, "\\Registry\\Machine\\Software", wbuf);
   make_oa(&oa,&us);
   HANDLE h32=0,h64=0;
   st=NtOpenKey(&h32, KEY_READ|KEY_WOW64_32KEY, &oa); if(st!=STATUS_SUCCESS){ NtTerminateProcess(NtCurrentProcess(),120); while(1){} } NtClose(h32);
   st=NtOpenKey(&h64, KEY_READ|KEY_WOW64_64KEY, &oa); if(st!=STATUS_SUCCESS){ NtTerminateProcess(NtCurrentProcess(),121); while(1){} } NtClose(h64);
 }
 // ---- Test 10: delete verify + enum value + deletevalue + flush ----
 {
   make_us(&us, "\\Registry\\User\\Software\\ntpers_probe_del", wbuf);
   make_oa(&oa,&us);
   st=NtCreateKey(&hk,KEY_ALL_ACCESS,&oa,0,(PUNICODE_STRING)0,0,&disp); if(st!=STATUS_SUCCESS){ NtTerminateProcess(NtCurrentProcess(),130); while(1){} }
   WCHAR wn[64]; UNICODE_STRING vus; make_us(&vus,"ToDelete",wn);
   unsigned char data[]="todelete";
   st=NtSetValueKey(hk,&vus,0,REG_SZ,data,sizeof(data)); if(st!=STATUS_SUCCESS){ NtTerminateProcess(NtCurrentProcess(),131); while(1){} }
   st=NtDeleteValueKey(hk,&vus); if(st!=STATUS_SUCCESS){ NtTerminateProcess(NtCurrentProcess(),132); while(1){} }
   unsigned char out[64]; ULONG rlen;
   st=NtQueryValueKey(hk,&vus,KeyValuePartialInformation,out,sizeof(out),&rlen); if(st!=STATUS_OBJECT_NAME_NOT_FOUND){ NtTerminateProcess(NtCurrentProcess(),133); while(1){} }
   unsigned char ebuf[64]; ULONG elen;
   st=NtEnumerateValueKey(hk,0,KeyValueBasicInformation,ebuf,sizeof(ebuf),&elen);
   if(st!=STATUS_NO_MORE_ENTRIES){ NtTerminateProcess(NtCurrentProcess(),134); while(1){} }
   WCHAR wn1[64], wn2[64]; UNICODE_STRING v1,v2;
   make_us(&v1,"A",wn1); make_us(&v2,"B",wn2);
   st=NtSetValueKey(hk,&v1,0,REG_SZ,(unsigned char*)"a",2); if(st!=STATUS_SUCCESS){ NtTerminateProcess(NtCurrentProcess(),135); while(1){} }
   st=NtSetValueKey(hk,&v2,0,REG_SZ,(unsigned char*)"b",2); if(st!=STATUS_SUCCESS){ NtTerminateProcess(NtCurrentProcess(),136); while(1){} }
   st=NtEnumerateValueKey(hk,0,KeyValueBasicInformation,ebuf,sizeof(ebuf),&elen); if(st!=STATUS_SUCCESS){ NtTerminateProcess(NtCurrentProcess(),137); while(1){} }
   st=NtEnumerateValueKey(hk,1,KeyValueBasicInformation,ebuf,sizeof(ebuf),&elen); if(st!=STATUS_SUCCESS){ NtTerminateProcess(NtCurrentProcess(),138); while(1){} }
   st=NtEnumerateValueKey(hk,2,KeyValueBasicInformation,ebuf,sizeof(ebuf),&elen); if(st!=STATUS_NO_MORE_ENTRIES){ NtTerminateProcess(NtCurrentProcess(),139); while(1){} }
   st=NtFlushKey(hk); if(st!=STATUS_SUCCESS){ NtTerminateProcess(NtCurrentProcess(),140); while(1){} }
   NtClose(hk);
   make_us(&us, "\\Registry\\User\\Software\\ntpers_probe_del", wbuf); make_oa(&oa,&us);
   st=NtOpenKey(&hd,KEY_ALL_ACCESS,&oa); if(st!=STATUS_SUCCESS){ NtTerminateProcess(NtCurrentProcess(),141); while(1){} }
   NtDeleteKey(hd); NtClose(hd);
   HANDLE hcheck=0; st=NtOpenKey(&hcheck,KEY_READ,&oa); if(st!=STATUS_OBJECT_NAME_NOT_FOUND){ NtTerminateProcess(NtCurrentProcess(),142); while(1){} }
 }
 NtTerminateProcess(NtCurrentProcess(),0);
 while(1){}
}
