/* registry_test.c - shim test asserting golden behaviours */
#include "registry.h"
#include "object.h"
#include "handle.h"
#include "../../ntdll/nt_types.h"
#include "../../ntdll/ntstatus.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <assert.h>
#include <stddef.h>
#include <fcntl.h>
#include <unistd.h>

static int g_pass=0,g_fail=0;
#define ASSERT(cond) do{ if(!(cond)){ fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#cond); g_fail++; return; } }while(0)
#define ASSERT_STATUS(expr,expect) do{ NTSTATUS _s=(expr); if(_s!=(expect)){ fprintf(stderr,"FAIL %s:%d: expect 0x%08lx got 0x%08lx for %s\n",__FILE__,__LINE__,(unsigned long)(expect),(unsigned long)_s,#expr); g_fail++; return; } }while(0)
#define PASS() do{ g_pass++; printf("  PASS %s\n",__func__); }while(0)

static WCHAR wbuf1[512];
static WCHAR wbuf2[512] __attribute__((unused));

static void make_us(UNICODE_STRING *us, const char *s, WCHAR *buf, size_t blen){
    size_t len=strlen(s);
    ASSERT(len*sizeof(WCHAR) < blen);
    for(size_t i=0;i<len;i++) buf[i]=(WCHAR)(unsigned char)s[i];
    us->Buffer=buf; us->Length=(USHORT)(len*sizeof(WCHAR)); us->MaximumLength=us->Length+2;
    if(len) buf[len]=0;
}
__attribute__((unused)) static void make_us_w(UNICODE_STRING *us, const WCHAR *s, WCHAR *buf, size_t blen){
    size_t len=0; while(s[len]) len++;
    ASSERT(len*sizeof(WCHAR) < blen);
    memcpy(buf,s,len*sizeof(WCHAR));
    us->Buffer=buf; us->Length=(USHORT)(len*sizeof(WCHAR)); us->MaximumLength=us->Length+2;
    buf[len]=0;
}
static OBJECT_ATTRIBUTES make_oa(UNICODE_STRING *us){
    OBJECT_ATTRIBUTES oa={0}; oa.Length=sizeof(oa); oa.ObjectName=us; oa.Attributes=OBJ_CASE_INSENSITIVE; return oa;
}
static struct nt_eprocess *make_proc(ULONG pid,const char *name){
    struct nt_eprocess *p=NULL; NTSTATUS st=ObCreateProcessObject(&p,pid,name);
    if(st!=STATUS_SUCCESS||!p){ fprintf(stderr,"make_proc failed\n"); g_fail++; return NULL; }
    kshim_set_current_process(p);
    return p;
}
static struct nt_ethread *make_thread(struct nt_eprocess *proc, ULONG tid){
    struct nt_ethread *t=NULL; NTSTATUS st=ObCreateThreadObject(&t,proc,tid);
    if(st!=STATUS_SUCCESS||!t){ fprintf(stderr,"make_thread failed\n"); g_fail++; return NULL; }
    kshim_set_current_thread(t); return t;
}
static void reset_state(void){
    ntreg_cleanup();
    ObCleanupObjectManager();
    NTSTATUS st=ObInitObjectManager(); ASSERT(st==STATUS_SUCCESS);
    ntreg_init();
}

/* Test 1: disposition NEW vs EXISTING */
static void test_disposition(void){
    reset_state();
    struct nt_eprocess *proc=make_proc(1000,"regdisp");
    struct nt_ethread *thr=make_thread(proc,1);
    UNICODE_STRING us; make_us(&us,"\\Registry\\User\\Software\\ntpers_probe_999_1",wbuf1,sizeof(wbuf1));
    OBJECT_ATTRIBUTES oa=make_oa(&us);
    HANDLE h1=NULL, h2=NULL;
    ULONG disp=0xDEAD;
    NTSTATUS st=NtCreateKey(&h1, KEY_ALL_ACCESS, &oa, 0, NULL, 0, &disp);
    ASSERT_STATUS(st, STATUS_SUCCESS); ASSERT(disp==REG_CREATED_NEW_KEY);
    disp=0xDEAD;
    NTSTATUS st2=NtCreateKey(&h2, KEY_ALL_ACCESS, &oa, 0, NULL, 0, &disp);
    ASSERT_STATUS(st2, STATUS_SUCCESS); ASSERT(disp==REG_OPENED_EXISTING_KEY);
    ObCloseHandle(proc,h1); ObCloseHandle(proc,h2);
    // cleanup delete
    HANDLE hopen=NULL; st=NtOpenKey(&hopen, KEY_ALL_ACCESS,&oa); ASSERT_STATUS(st,STATUS_SUCCESS);
    NtDeleteKey(hopen); ObCloseHandle(proc,hopen);
    ObDestroyProcessObject(proc); ObDestroyThreadObject(thr);
    kshim_set_current_process(NULL); kshim_set_current_thread(NULL);
    PASS();
}

/* Test 2: nine value types round trip */
static void test_nine_types(void){
    reset_state();
    struct nt_eprocess *proc=make_proc(1001,"reg9");
    struct nt_ethread *thr=make_thread(proc,2);
    UNICODE_STRING us; make_us(&us,"\\Registry\\User\\Software\\ntpers_probe_9types",wbuf1,sizeof(wbuf1));
    OBJECT_ATTRIBUTES oa=make_oa(&us);
    HANDLE hk=NULL; ULONG disp; NTSTATUS st=NtCreateKey(&hk, KEY_ALL_ACCESS,&oa,0,NULL,0,&disp); ASSERT_STATUS(st,STATUS_SUCCESS);
    static uint8_t exp_data[17]; for(int k=0;k<17;k++) exp_data[k]=(uint8_t)('A'+(k%26));
    static uint8_t bin_data[4]={0x01,0x02,0x03,0x04};
    static uint32_t dword_data=0x12345678;
    static uint64_t qword_data=0x1122334455667788ULL;
    static uint8_t multi_data[8]={'a',0,'b',0,'c',0,0,0};
    static uint8_t none_data[7]={'r','a','w','n','o','n','e'};
    static uint8_t sz_data[6]={'h','e','l','l','o',0};
    static uint8_t empty_sz[1]={0};
    struct { const char *name; ULONG type; const uint8_t *data; ULONG len; } vals[] = {
        {"Val_SZ", REG_SZ, sz_data,6},
        {"Val_EXPAND", REG_EXPAND_SZ, exp_data,17},
        {"Val_BINARY", REG_BINARY, bin_data,4},
        {"Val_DWORD", REG_DWORD, (uint8_t*)&dword_data,4},
        {"Val_QWORD", REG_QWORD, (uint8_t*)&qword_data,8},
        {"Val_MULTI", REG_MULTI_SZ, multi_data,8},
        {"Val_NONE", REG_NONE, none_data,7},
        {"Val_EMPTY_SZ", REG_SZ, empty_sz,1},
        {"Val_EMPTY_BINARY", REG_BINARY, (uint8_t*)"",0},
    };
    for(size_t i=0;i<sizeof(vals)/sizeof(vals[0]);i++){
        WCHAR wn[64]; UNICODE_STRING vus; make_us(&vus, vals[i].name, wn, sizeof(wn));
        st=NtSetValueKey(hk,&vus,0,vals[i].type,(PVOID)vals[i].data,vals[i].len);
        ASSERT_STATUS(st,STATUS_SUCCESS);
    }
    for(size_t i=0;i<sizeof(vals)/sizeof(vals[0]);i++){
        WCHAR wn[64]; UNICODE_STRING vus; make_us(&vus, vals[i].name, wn, sizeof(wn));
        char out[64]; ULONG rlen=0xDEAD;
        st=NtQueryValueKey(hk,&vus,KeyValuePartialInformation,out,sizeof(out),&rlen);
        ASSERT_STATUS(st,STATUS_SUCCESS);
        PKEY_VALUE_PARTIAL_INFORMATION p=(PKEY_VALUE_PARTIAL_INFORMATION)out;
        ASSERT(p->Type==vals[i].type);
        ASSERT(p->DataLength==vals[i].len);
        if(vals[i].len) ASSERT(memcmp(p->Data, vals[i].data, vals[i].len)==0);
    }
    ObCloseHandle(proc,hk);
    HANDLE hd=NULL; make_us(&us,"\\Registry\\User\\Software\\ntpers_probe_9types",wbuf1,sizeof(wbuf1)); oa=make_oa(&us);
    st=NtOpenKey(&hd,KEY_ALL_ACCESS,&oa); ASSERT_STATUS(st,STATUS_SUCCESS);
    NtDeleteKey(hd); ObCloseHandle(proc,hd);
    ObDestroyProcessObject(proc); ObDestroyThreadObject(thr);
    kshim_set_current_process(NULL); kshim_set_current_thread(NULL);
    PASS();
}

/* Test 3: embedded NUL 9 bytes */
static void test_embed_nul(void){
    reset_state();
    struct nt_eprocess *proc=make_proc(1002,"regnul");
    struct nt_ethread *thr=make_thread(proc,3);
    UNICODE_STRING us; make_us(&us,"\\Registry\\User\\Software\\ntpers_probe_embed",wbuf1,sizeof(wbuf1));
    OBJECT_ATTRIBUTES oa=make_oa(&us);
    HANDLE hk=NULL; ULONG disp; NTSTATUS st=NtCreateKey(&hk,KEY_ALL_ACCESS,&oa,0,NULL,0,&disp); ASSERT_STATUS(st,STATUS_SUCCESS);
    uint8_t embed[]={'h','i','\0','t','h','e','r','e','\0'}; // 9 bytes [104,105,0,116,...]
    WCHAR wn[64]; UNICODE_STRING vus; make_us(&vus,"Val_EMBED_NUL",wn,sizeof(wn));
    st=NtSetValueKey(hk,&vus,0,REG_SZ,embed,sizeof(embed)); ASSERT_STATUS(st,STATUS_SUCCESS);
    char out[64]; ULONG rlen=0;
    st=NtQueryValueKey(hk,&vus,KeyValuePartialInformation,out,sizeof(out),&rlen);
    ASSERT_STATUS(st,STATUS_SUCCESS);
    PKEY_VALUE_PARTIAL_INFORMATION p=(PKEY_VALUE_PARTIAL_INFORMATION)out;
    ASSERT(p->Type==REG_SZ); ASSERT(p->DataLength==9);
    ASSERT(p->Data[0]==104 && p->Data[1]==105 && p->Data[2]==0 && p->Data[3]==116);
    ASSERT(memcmp(p->Data,embed,9)==0);
    ObCloseHandle(proc,hk);
    HANDLE hd; make_us(&us,"\\Registry\\User\\Software\\ntpers_probe_embed",wbuf1,sizeof(wbuf1)); oa=make_oa(&us);
    st=NtOpenKey(&hd,KEY_ALL_ACCESS,&oa); ASSERT_STATUS(st,STATUS_SUCCESS); NtDeleteKey(hd); ObCloseHandle(proc,hd);
    ObDestroyProcessObject(proc); ObDestroyThreadObject(thr);
    kshim_set_current_process(NULL); kshim_set_current_thread(NULL);
    PASS();
}

/* Test 4: 1 MB value 0xAB */
static void test_1mb(void){
    reset_state();
    struct nt_eprocess *proc=make_proc(1003,"reg1mb");
    struct nt_ethread *thr=make_thread(proc,4);
    UNICODE_STRING us; make_us(&us,"\\Registry\\User\\Software\\ntpers_probe_1mb",wbuf1,sizeof(wbuf1));
    OBJECT_ATTRIBUTES oa=make_oa(&us);
    HANDLE hk=NULL; ULONG disp; NTSTATUS st=NtCreateKey(&hk,KEY_ALL_ACCESS,&oa,0,NULL,0,&disp); ASSERT_STATUS(st,STATUS_SUCCESS);
    ULONG bigLen=1024*1024;
    uint8_t *big=malloc(bigLen); ASSERT(big); memset(big,0xAB,bigLen);
    WCHAR wn[64]; UNICODE_STRING vus; make_us(&vus,"Val_1MB",wn,sizeof(wn));
    st=NtSetValueKey(hk,&vus,0,REG_BINARY,big,bigLen); ASSERT_STATUS(st,STATUS_SUCCESS);
    // query with large buffer
    uint8_t *out=malloc(sizeof(KEY_VALUE_PARTIAL_INFORMATION)+bigLen+16); ASSERT(out);
    ULONG rlen=0;
    st=NtQueryValueKey(hk,&vus,KeyValuePartialInformation,out,sizeof(KEY_VALUE_PARTIAL_INFORMATION)+bigLen+16,&rlen);
    ASSERT_STATUS(st,STATUS_SUCCESS);
    PKEY_VALUE_PARTIAL_INFORMATION p=(PKEY_VALUE_PARTIAL_INFORMATION)out;
    ASSERT(p->DataLength==bigLen);
    for(ULONG i=0;i<bigLen;i++) { if(p->Data[i]!=0xAB){ fprintf(stderr,"mismatch at %lu\n",(unsigned long)i); g_fail++; free(big); free(out); ObCloseHandle(proc,hk); return; } }
    free(big); free(out);
    ObCloseHandle(proc,hk);
    HANDLE hd; make_us(&us,"\\Registry\\User\\Software\\ntpers_probe_1mb",wbuf1,sizeof(wbuf1)); oa=make_oa(&us);
    st=NtOpenKey(&hd,KEY_ALL_ACCESS,&oa); ASSERT_STATUS(st,STATUS_SUCCESS); NtDeleteKey(hd); ObCloseHandle(proc,hd);
    ObDestroyProcessObject(proc); ObDestroyThreadObject(thr);
    kshim_set_current_process(NULL); kshim_set_current_thread(NULL);
    PASS();
}

/* Test 5: missing key returns NOT_FOUND (win32 2) */
static void test_missing_key(void){
    reset_state();
    struct nt_eprocess *proc=make_proc(1004,"regmiss");
    struct nt_ethread *thr=make_thread(proc,5);
    UNICODE_STRING us; make_us(&us,"\\Registry\\User\\Software\\NoSuchKey12345",wbuf1,sizeof(wbuf1));
    OBJECT_ATTRIBUTES oa=make_oa(&us);
    HANDLE h=NULL;
    NTSTATUS st=NtOpenKey(&h,KEY_READ,&oa);
    ASSERT(st==STATUS_OBJECT_NAME_NOT_FOUND);
    // also query missing value
    make_us(&us,"\\Registry\\User\\Software\\ntpers_probe_missval",wbuf1,sizeof(wbuf1)); oa=make_oa(&us);
    HANDLE hk; ULONG disp; st=NtCreateKey(&hk,KEY_ALL_ACCESS,&oa,0,NULL,0,&disp); ASSERT_STATUS(st,STATUS_SUCCESS);
    WCHAR wn[64]; UNICODE_STRING vus; make_us(&vus,"NoSuchValue",wn,sizeof(wn));
    char out[64]; ULONG rlen=0;
    st=NtQueryValueKey(hk,&vus,KeyValuePartialInformation,out,sizeof(out),&rlen);
    ASSERT(st==STATUS_OBJECT_NAME_NOT_FOUND);
    ObCloseHandle(proc,hk);
    HANDLE hd; make_us(&us,"\\Registry\\User\\Software\\ntpers_probe_missval",wbuf1,sizeof(wbuf1)); oa=make_oa(&us);
    st=NtOpenKey(&hd,KEY_ALL_ACCESS,&oa); ASSERT_STATUS(st,STATUS_SUCCESS); NtDeleteKey(hd); ObCloseHandle(proc,hd);
    ObDestroyProcessObject(proc); ObDestroyThreadObject(thr);
    kshim_set_current_process(NULL); kshim_set_current_thread(NULL);
    PASS();
}

/* Test 6: NtQueryKey small buffer sets ReturnLength */
static void test_querykey_small(void){
    reset_state();
    struct nt_eprocess *proc=make_proc(1005,"regqkey");
    struct nt_ethread *thr=make_thread(proc,6);
    UNICODE_STRING us; make_us(&us,"\\Registry\\User\\Software\\ntpers_probe_qkey",wbuf1,sizeof(wbuf1));
    OBJECT_ATTRIBUTES oa=make_oa(&us);
    HANDLE hk; ULONG disp; NTSTATUS st=NtCreateKey(&hk,KEY_ALL_ACCESS,&oa,0,NULL,0,&disp); ASSERT_STATUS(st,STATUS_SUCCESS);
    for(ULONG cls=0;cls<4;cls++){
        char buf[512]; ULONG rlen=0xDEADBEEF;
        st=NtQueryKey(hk,cls,buf,sizeof(buf),&rlen);
        ASSERT_STATUS(st,STATUS_SUCCESS); ASSERT(rlen!=0xDEADBEEF && rlen>0);
        char small[4]; ULONG rlen2=0xDEADBEEF;
        NTSTATUS stSmall=NtQueryKey(hk,cls,small,4,&rlen2);
        if(cls==3) ASSERT_STATUS(stSmall, STATUS_BUFFER_OVERFLOW);
        else ASSERT_STATUS(stSmall, STATUS_BUFFER_TOO_SMALL);
        ASSERT(rlen2!=0xDEADBEEF && rlen2==rlen);
    }
    ObCloseHandle(proc,hk);
    HANDLE hd; make_us(&us,"\\Registry\\User\\Software\\ntpers_probe_qkey",wbuf1,sizeof(wbuf1)); oa=make_oa(&us);
    st=NtOpenKey(&hd,KEY_ALL_ACCESS,&oa); ASSERT_STATUS(st,STATUS_SUCCESS); NtDeleteKey(hd); ObCloseHandle(proc,hd);
    ObDestroyProcessObject(proc); ObDestroyThreadObject(thr);
    kshim_set_current_process(NULL); kshim_set_current_thread(NULL);
    PASS();
}

/* Test 7: enumerate past end */
static void test_enumerate_past(void){
    reset_state();
    struct nt_eprocess *proc=make_proc(1006,"regen");
    struct nt_ethread *thr=make_thread(proc,7);
    UNICODE_STRING us; make_us(&us,"\\Registry\\Machine\\Software",wbuf1,sizeof(wbuf1));
    OBJECT_ATTRIBUTES oa=make_oa(&us);
    HANDLE hm; NTSTATUS st=NtOpenKey(&hm,KEY_READ,&oa); ASSERT_STATUS(st,STATUS_SUCCESS);
    char buf[512]; ULONG rlen=0;
    st=NtEnumerateKey(hm, 9999, KeyBasicInformation, buf, sizeof(buf), &rlen);
    ASSERT_STATUS(st, STATUS_NO_MORE_ENTRIES);
    ObCloseHandle(proc,hm);
    ObDestroyProcessObject(proc); ObDestroyThreadObject(thr);
    kshim_set_current_process(NULL); kshim_set_current_thread(NULL);
    PASS();
}

/* Test 8: HKLM write denied vs read allowed */
static void test_access_denied(void){
    reset_state();
    struct nt_eprocess *proc=make_proc(1007,"regaccess");
    struct nt_ethread *thr=make_thread(proc,8);
    UNICODE_STRING us; make_us(&us,"\\Registry\\Machine\\Software",wbuf1,sizeof(wbuf1));
    OBJECT_ATTRIBUTES oa=make_oa(&us);
    HANDLE hro, hrw;
    NTSTATUS st=NtOpenKey(&hro, KEY_READ, &oa);
    ASSERT_STATUS(st,STATUS_SUCCESS); ObCloseHandle(proc,hro);
    st=NtOpenKey(&hrw, KEY_WRITE, &oa);
    ASSERT_STATUS(st,STATUS_ACCESS_DENIED);
    ObDestroyProcessObject(proc); ObDestroyThreadObject(thr);
    kshim_set_current_process(NULL); kshim_set_current_thread(NULL);
    PASS();
}

/* Test 9: WOW64 flags both return 0 */
static void test_wow64(void){
    reset_state();
    struct nt_eprocess *proc=make_proc(1008,"regwow");
    struct nt_ethread *thr=make_thread(proc,9);
    UNICODE_STRING us; make_us(&us,"\\Registry\\Machine\\Software",wbuf1,sizeof(wbuf1));
    OBJECT_ATTRIBUTES oa=make_oa(&us);
    HANDLE h32,h64;
    NTSTATUS st=NtOpenKey(&h32, KEY_READ|KEY_WOW64_32KEY, &oa); ASSERT_STATUS(st,STATUS_SUCCESS); ObCloseHandle(proc,h32);
    st=NtOpenKey(&h64, KEY_READ|KEY_WOW64_64KEY, &oa); ASSERT_STATUS(st,STATUS_SUCCESS); ObCloseHandle(proc,h64);
    ObDestroyProcessObject(proc); ObDestroyThreadObject(thr);
    kshim_set_current_process(NULL); kshim_set_current_thread(NULL);
    PASS();
}

/* Test 10: delete verify + enumerate value + deletevalue + flush */
static void test_delete_and_enum(void){
    reset_state();
    struct nt_eprocess *proc=make_proc(1009,"regdel");
    struct nt_ethread *thr=make_thread(proc,10);
    UNICODE_STRING us; make_us(&us,"\\Registry\\User\\Software\\ntpers_probe_del",wbuf1,sizeof(wbuf1));
    OBJECT_ATTRIBUTES oa=make_oa(&us);
    HANDLE hk; ULONG disp; NTSTATUS st=NtCreateKey(&hk,KEY_ALL_ACCESS,&oa,0,NULL,0,&disp); ASSERT_STATUS(st,STATUS_SUCCESS);
    WCHAR wn[64]; UNICODE_STRING vus; make_us(&vus,"ToDelete",wn,sizeof(wn));
    uint8_t data[]="todelete";
    st=NtSetValueKey(hk,&vus,0,REG_SZ,data,sizeof(data)); ASSERT_STATUS(st,STATUS_SUCCESS);
    st=NtDeleteValueKey(hk,&vus); ASSERT_STATUS(st,STATUS_SUCCESS);
    char out[64]; ULONG rlen;
    st=NtQueryValueKey(hk,&vus,KeyValuePartialInformation,out,sizeof(out),&rlen); ASSERT(st==STATUS_OBJECT_NAME_NOT_FOUND);
    // enumerate value should be no more entries now
    char ebuf[64]; ULONG elen;
    st=NtEnumerateValueKey(hk,0,KeyValueBasicInformation,ebuf,sizeof(ebuf),&elen);
    ASSERT(st==STATUS_NO_MORE_ENTRIES);
    // set two values and enumerate
    WCHAR wn1[64], wn2[64]; UNICODE_STRING v1,v2;
    make_us(&v1,"A",wn1,sizeof(wn1)); make_us(&v2,"B",wn2,sizeof(wn2));
    st=NtSetValueKey(hk,&v1,0,REG_SZ,(uint8_t*)"a",2); ASSERT_STATUS(st,STATUS_SUCCESS);
    st=NtSetValueKey(hk,&v2,0,REG_SZ,(uint8_t*)"b",2); ASSERT_STATUS(st,STATUS_SUCCESS);
    st=NtEnumerateValueKey(hk,0,KeyValueBasicInformation,ebuf,sizeof(ebuf),&elen); ASSERT_STATUS(st,STATUS_SUCCESS);
    st=NtEnumerateValueKey(hk,1,KeyValueBasicInformation,ebuf,sizeof(ebuf),&elen); ASSERT_STATUS(st,STATUS_SUCCESS);
    st=NtEnumerateValueKey(hk,2,KeyValueBasicInformation,ebuf,sizeof(ebuf),&elen); ASSERT_STATUS(st,STATUS_NO_MORE_ENTRIES);
    st=NtFlushKey(hk); ASSERT_STATUS(st,STATUS_SUCCESS);
    ObCloseHandle(proc,hk);
    HANDLE hd; make_us(&us,"\\Registry\\User\\Software\\ntpers_probe_del",wbuf1,sizeof(wbuf1)); oa=make_oa(&us);
    st=NtOpenKey(&hd,KEY_ALL_ACCESS,&oa); ASSERT_STATUS(st,STATUS_SUCCESS);
    NtDeleteKey(hd); ObCloseHandle(proc,hd);
    HANDLE hcheck; st=NtOpenKey(&hcheck,KEY_READ,&oa); ASSERT(st==STATUS_OBJECT_NAME_NOT_FOUND);
    ObDestroyProcessObject(proc); ObDestroyThreadObject(thr);
    kshim_set_current_process(NULL); kshim_set_current_thread(NULL);
    PASS();
}

/* Test 11: NtFlushKey writes a snapshot that describes the tree (not a zero count).
   Layout (see registry.h): "NTREG\x01", u32 key count, then per key:
   u32 path bytes, path (UTF-16LE), u32 value count, then per value:
   u32 name bytes, name, u32 type, u32 data length, data. */
static size_t snap_read(unsigned char *buf, size_t cap){
    char path[2 * PATH_MAX];
    snprintf(path, sizeof(path), "%s/nt_registry/hive_snapshot.bin", g_kshim_root);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    size_t total = 0;
    for (;;) {
        if (total == cap) break;
        ssize_t r = read(fd, buf + total, cap - total);
        if (r <= 0) break;
        total += (size_t)r;
    }
    close(fd);
    return total;
}
static unsigned u32le(const unsigned char *p){
    return (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}
/* true when the UTF-16LE bytes equal the ASCII string s */
static int wmatch(const unsigned char *p, size_t bytes, const char *s){
    size_t n = strlen(s);
    if (bytes != n * 2) return 0;
    for (size_t i = 0; i < n; i++)
        if (p[2*i] != (unsigned char)s[i] || p[2*i+1] != 0) return 0;
    return 1;
}
static void test_flush_snapshot(void){
    reset_state();
    struct nt_eprocess *proc=make_proc(1000,"regflush");
    struct nt_ethread *thr=make_thread(proc,1);
    UNICODE_STRING us; make_us(&us,"\\Registry\\User\\Software\\ntpers_flush_probe",wbuf1,sizeof(wbuf1));
    OBJECT_ATTRIBUTES oa=make_oa(&us);
    HANDLE hk=NULL; ULONG disp=0;
    ASSERT_STATUS(NtCreateKey(&hk, KEY_ALL_ACCESS, &oa, 0, NULL, 0, &disp), STATUS_SUCCESS);
    WCHAR vb[64]; UNICODE_STRING vn; make_us(&vn,"Alpha",vb,sizeof(vb));
    ULONG v=42;
    ASSERT_STATUS(NtSetValueKey(hk, &vn, 0, REG_DWORD, &v, sizeof(v)), STATUS_SUCCESS);
    ASSERT_STATUS(NtFlushKey(hk), STATUS_SUCCESS);

    static unsigned char snap[1<<16];
    size_t n = snap_read(snap, sizeof(snap));
    ASSERT(n >= 10);
    ASSERT(memcmp(snap, "NTREG\x01", 6) == 0);
    unsigned cnt = u32le(snap + 6);
    ASSERT(cnt >= 5); /* \Registry\Machine, \Registry\Machine\Software, \Registry\User, \Registry\User\Software, probe */
    size_t off = 10;
    unsigned parsed = 0, hits = 0;
    for (unsigned r = 0; r < cnt; r++) {
        ASSERT(off + 4 <= n);
        unsigned plen = u32le(snap + off); off += 4;
        ASSERT(off + plen <= n);
        int is_probe = wmatch(snap + off, plen, "\\Registry\\User\\Software\\ntpers_flush_probe");
        off += plen;
        ASSERT(off + 4 <= n);
        unsigned vcnt = u32le(snap + off); off += 4;
        if (is_probe) {
            hits++;
            ASSERT(vcnt == 1);
        }
        for (unsigned j = 0; j < vcnt; j++) {
            ASSERT(off + 4 <= n);
            unsigned nlen = u32le(snap + off); off += 4;
            ASSERT(off + nlen <= n);
            int is_alpha = is_probe && wmatch(snap + off, nlen, "Alpha");
            off += nlen;
            ASSERT(off + 8 <= n);
            unsigned type = u32le(snap + off), dlen = u32le(snap + off + 4); off += 8;
            ASSERT(off + dlen <= n);
            if (is_alpha) {
                ASSERT(type == REG_DWORD);
                ASSERT(dlen == 4);
                ASSERT(u32le(snap + off) == 42);
                hits++;
            }
            off += dlen;
        }
        parsed++;
    }
    ASSERT(parsed == cnt);
    ASSERT(off == n);
    ASSERT(hits == 2);
    ASSERT_STATUS(NtDeleteKey(hk), STATUS_SUCCESS);
    ObCloseHandle(proc,hk);
    ObDestroyProcessObject(proc); ObDestroyThreadObject(thr);
    kshim_set_current_process(NULL); kshim_set_current_thread(NULL);
    PASS();
}

/* Test 13: deleting a subkey removes it from its parent's list and namespace. */
static void test_delete_nested(void){
    reset_state();
    struct nt_eprocess *proc=make_proc(1000,"regnest");
    struct nt_ethread *thr=make_thread(proc,1);
    UNICODE_STRING pus; make_us(&pus,"\\Registry\\User\\Software\\ntpers_nest_probe",wbuf1,sizeof(wbuf1));
    OBJECT_ATTRIBUTES poa=make_oa(&pus);
    HANDLE hp=NULL; ULONG disp=0;
    ASSERT_STATUS(NtCreateKey(&hp, KEY_ALL_ACCESS, &poa, 0, NULL, 0, &disp), STATUS_SUCCESS);
    UNICODE_STRING cus; make_us(&cus,"\\Registry\\User\\Software\\ntpers_nest_probe\\child",wbuf2,sizeof(wbuf2));
    OBJECT_ATTRIBUTES coa=make_oa(&cus);
    HANDLE hc=NULL;
    ASSERT_STATUS(NtCreateKey(&hc, KEY_ALL_ACCESS, &coa, 0, NULL, 0, &disp), STATUS_SUCCESS);
    ASSERT_STATUS(NtDeleteKey(hc), STATUS_SUCCESS);

    unsigned char buf[256]; ULONG rlen=0;
    ASSERT_STATUS(NtEnumerateKey(hp, 0, KeyBasicInformation, buf, sizeof(buf), &rlen), STATUS_NO_MORE_ENTRIES);
    HANDLE h2=NULL;
    ASSERT_STATUS(NtOpenKey(&h2, KEY_READ, &coa), STATUS_OBJECT_NAME_NOT_FOUND);
    ASSERT_STATUS(NtDeleteKey(hp), STATUS_SUCCESS);

    ObCloseHandle(proc,hc); ObCloseHandle(proc,hp);
    ObDestroyProcessObject(proc); ObDestroyThreadObject(thr);
    kshim_set_current_process(NULL); kshim_set_current_thread(NULL);
    PASS();
}

/* Test 12: NtEnumerateKey KeyNodeInformation: name, class and their offsets. */
static void test_enumerate_node(void){
    reset_state();
    struct nt_eprocess *proc=make_proc(1000,"regnode");
    struct nt_ethread *thr=make_thread(proc,1);
    UNICODE_STRING pus; make_us(&pus,"\\Registry\\User\\Software\\ntpers_node_probe",wbuf1,sizeof(wbuf1));
    OBJECT_ATTRIBUTES poa=make_oa(&pus);
    HANDLE hp=NULL; ULONG disp=0;
    ASSERT_STATUS(NtCreateKey(&hp, KEY_ALL_ACCESS, &poa, 0, NULL, 0, &disp), STATUS_SUCCESS);
    UNICODE_STRING cus; make_us(&cus,"\\Registry\\User\\Software\\ntpers_node_probe\\child",wbuf2,sizeof(wbuf2));
    OBJECT_ATTRIBUTES coa=make_oa(&cus);
    WCHAR cbuf[64]; UNICODE_STRING cls; make_us(&cls,"ClassOne",cbuf,sizeof(cbuf));
    HANDLE hc=NULL;
    ASSERT_STATUS(NtCreateKey(&hc, KEY_ALL_ACCESS, &coa, 0, &cls, 0, &disp), STATUS_SUCCESS);

    unsigned char buf[256]; ULONG rlen=0;
    ASSERT_STATUS(NtEnumerateKey(hp, 0, KeyNodeInformation, buf, sizeof(buf), &rlen), STATUS_SUCCESS);
    KEY_NODE_INFORMATION *ni=(KEY_NODE_INFORMATION*)buf;
    ASSERT(ni->NameLength == 5*sizeof(WCHAR));
    ASSERT(wmatch((const unsigned char*)ni->Name, ni->NameLength, "child"));
    ASSERT(ni->ClassLength == 8*sizeof(WCHAR));
    ASSERT(ni->ClassOffset == offsetof(KEY_NODE_INFORMATION, Name) + ni->NameLength);
    ASSERT(wmatch(buf + ni->ClassOffset, ni->ClassLength, "ClassOne"));
    ASSERT(rlen == ni->ClassOffset + ni->ClassLength);

    /* a buffer that cannot hold the name reports the required size */
    ULONG small = (ULONG)offsetof(KEY_NODE_INFORMATION, Name) + 2;
    ASSERT_STATUS(NtEnumerateKey(hp, 0, KeyNodeInformation, buf, small, &rlen), STATUS_BUFFER_TOO_SMALL);
    ASSERT(rlen == (ULONG)offsetof(KEY_NODE_INFORMATION, Name) + 5*sizeof(WCHAR) + 8*sizeof(WCHAR));

    ASSERT_STATUS(NtDeleteKey(hc), STATUS_SUCCESS);
    ASSERT_STATUS(NtDeleteKey(hp), STATUS_SUCCESS);
    ObCloseHandle(proc,hc); ObCloseHandle(proc,hp);
    ObDestroyProcessObject(proc); ObDestroyThreadObject(thr);
    kshim_set_current_process(NULL); kshim_set_current_thread(NULL);
    PASS();
}

/* Test 14: value information classes with 64-bit alignment (Align64).
   Full: the data starts at an 8-byte aligned DataOffset, padding is zero.
   Partial: Type, DataLength, then Data at offset 8 (no TitleIndex). */
static void test_value_align64(void){
    reset_state();
    struct nt_eprocess *proc=make_proc(1000,"regalign");
    struct nt_ethread *thr=make_thread(proc,1);
    UNICODE_STRING us; make_us(&us,"\\\\Registry\\\\User\\\\Software\\\\ntpers_align_probe",wbuf1,sizeof(wbuf1));
    OBJECT_ATTRIBUTES oa=make_oa(&us);
    HANDLE hk=NULL; ULONG disp=0;
    ASSERT_STATUS(NtCreateKey(&hk, KEY_ALL_ACCESS, &oa, 0, NULL, 0, &disp), STATUS_SUCCESS);
    WCHAR vb[64]; UNICODE_STRING vn; make_us(&vn,"Big",vb,sizeof(vb));
    static const unsigned char data[5] = {1,2,3,4,5};
    ASSERT_STATUS(NtSetValueKey(hk, &vn, 0, REG_BINARY, (PVOID)data, sizeof(data)), STATUS_SUCCESS);

    unsigned char buf[128]; ULONG rlen=0;
    memset(buf, 0xAA, sizeof(buf));
    ASSERT_STATUS(NtQueryValueKey(hk, &vn, KeyValueFullInformationAlign64, buf, sizeof(buf), &rlen), STATUS_SUCCESS);
    KEY_VALUE_FULL_INFORMATION *fi=(KEY_VALUE_FULL_INFORMATION*)buf;
    ASSERT(fi->Type == REG_BINARY);
    ASSERT(fi->NameLength == 3*sizeof(WCHAR));
    ASSERT(wmatch((const unsigned char*)fi->Name, fi->NameLength, "Big"));
    ASSERT(fi->DataOffset == 32);            /* 20 + 6 name bytes, rounded up to 8 */
    ASSERT(fi->DataLength == 5);
    ASSERT(rlen == 37);
    for (ULONG i=26;i<32;i++) ASSERT(buf[i] == 0);
    ASSERT(memcmp(buf+32, data, 5) == 0);

    memset(buf, 0xAA, sizeof(buf)); rlen=0;
    ASSERT_STATUS(NtQueryValueKey(hk, &vn, KeyValuePartialInformationAlign64, buf, sizeof(buf), &rlen), STATUS_SUCCESS);
    KEY_VALUE_PARTIAL_INFORMATION_ALIGN64 *pa=(KEY_VALUE_PARTIAL_INFORMATION_ALIGN64*)buf;
    ASSERT(pa->Type == REG_BINARY);
    ASSERT(pa->DataLength == 5);
    ASSERT(rlen == 13);
    ASSERT(memcmp(pa->Data, data, 5) == 0);

    memset(buf, 0xAA, sizeof(buf)); rlen=0;
    ASSERT_STATUS(NtEnumerateValueKey(hk, 0, KeyValueFullInformationAlign64, buf, sizeof(buf), &rlen), STATUS_SUCCESS);
    ASSERT(((KEY_VALUE_FULL_INFORMATION*)buf)->DataOffset == 32);
    ASSERT(rlen == 37);
    ASSERT(memcmp(buf+32, data, 5) == 0);

    /* too small for the data: the required length is still reported */
    ASSERT_STATUS(NtQueryValueKey(hk, &vn, KeyValuePartialInformationAlign64, buf, 12, &rlen), STATUS_BUFFER_TOO_SMALL);
    ASSERT(rlen == 13);
    ASSERT_STATUS(NtQueryValueKey(hk, &vn, 7, buf, sizeof(buf), &rlen), STATUS_INVALID_INFO_CLASS);

    ASSERT_STATUS(NtDeleteKey(hk), STATUS_SUCCESS);
    ObCloseHandle(proc,hk);
    ObDestroyProcessObject(proc); ObDestroyThreadObject(thr);
    kshim_set_current_process(NULL); kshim_set_current_thread(NULL);
    PASS();
}

/* Test 15: REG_OPTION_CREATE_LINK is refused (STATUS_NOT_IMPLEMENTED) and
   creates nothing; it must not silently create an ordinary key. */
static void test_create_link_refused(void){
    reset_state();
    struct nt_eprocess *proc=make_proc(1000,"reglink");
    struct nt_ethread *thr=make_thread(proc,1);
    UNICODE_STRING us; make_us(&us,"\\Registry\\User\\Software\\ntpers_link_probe",wbuf1,sizeof(wbuf1));
    OBJECT_ATTRIBUTES oa=make_oa(&us);
    HANDLE hk=NULL; ULONG disp=0;
    ASSERT_STATUS(NtCreateKey(&hk, KEY_ALL_ACCESS, &oa, 0, NULL, REG_OPTION_CREATE_LINK, &disp), STATUS_NOT_IMPLEMENTED);
    ASSERT(hk == NULL);
    HANDLE ho=NULL;
    ASSERT_STATUS(NtOpenKey(&ho, KEY_READ, &oa), STATUS_OBJECT_NAME_NOT_FOUND);
    ObDestroyProcessObject(proc); ObDestroyThreadObject(thr);
    kshim_set_current_process(NULL); kshim_set_current_thread(NULL);
    PASS();
}

int main(void){
    printf("registry shim tests starting\n");
    ntreg_init();
    struct { void (*fn)(void); const char *name; } tests[] = {
        {test_disposition,"disposition"},
        {test_nine_types,"nine_types"},
        {test_embed_nul,"embed_nul"},
        {test_1mb,"1mb"},
        {test_missing_key,"missing_key"},
        {test_querykey_small,"querykey_small"},
        {test_enumerate_past,"enumerate_past"},
        {test_access_denied,"access_denied"},
        {test_wow64,"wow64"},
        {test_delete_and_enum,"delete_and_enum"},
        {test_flush_snapshot,"flush_snapshot"},
        {test_delete_nested,"delete_nested"},
        {test_enumerate_node,"enumerate_node"},
        {test_value_align64,"value_align64"},
        {test_create_link_refused,"create_link_refused"},
    };
    for(size_t i=0;i<sizeof(tests)/sizeof(tests[0]);i++){
        printf(" RUN %s\n",tests[i].name);
        tests[i].fn();
    }
    printf("registry tests: %d passed, %d failed\n",g_pass,g_fail);
    if(g_fail) return 1;
    long leaks=atomic_load(&kshim_alloc_count);
    printf("alloc_count at exit: %ld\n",leaks);
    ntreg_cleanup();
    ObCleanupObjectManager();
    long after=atomic_load(&kshim_alloc_count);
    printf("alloc_count after cleanup: %ld\n",after);
    if(after!=0){ fprintf(stderr,"LEAK %ld\n",after); return 1; }
    return 0;
}
