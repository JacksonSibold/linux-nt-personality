/* proc_thread_test.c - unit tests for proc_thread shim (kshim)
 * Covers:
 *  - thread creation and join (exit 777, STILL_ACTIVE 259)
 *  - CREATE_SUSPENDED then resume
 *  - suspend/resume count values matching golden (0,1,2,1)
 *  - mutant abandonment when owning thread dies (unblocks I2)
 *  - process exit-code propagation (42)
 *  - exact size checks (48 only succeeds)
 *  - NtOpenProcess/NtOpenThread
 */
#include "object.h"
#include "handle.h"
#include "proc_thread.h"
#include "sync.h"
#include "kshim.h"
#include "../../ntdll/nt_types.h"
#include "../../ntdll/ntstatus.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <stdatomic.h>

NTSTATUS nt_svc_NtWaitForSingleObject(HANDLE,BOOLEAN,PLARGE_INTEGER);

static int g_pass=0,g_fail=0;
#define ASSERT(cond) do{ if(!(cond)){ fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#cond); g_fail++; return; } }while(0)
#define ASSERT_ST(st,exp) do{ NTSTATUS _s=(st); if(_s!=(exp)){ fprintf(stderr,"FAIL %s:%d: 0x%x != 0x%x\n",__FILE__,__LINE_,(unsigned)_s,(unsigned)exp); g_fail++; return; } }while(0)
#define PASS() do{ g_pass++; printf("  PASS %s\n",__func__); }while(0)
#define RUN(fn) do{ printf("RUN %s\n",#fn); fn(); }while(0)

static atomic_int g_counter;

static ULONG thr_simple(PVOID a){ (void)a; atomic_fetch_add(&g_counter,1); return 777; }
static ULONG thr_sleeper(PVOID a){ (void)a; for(int i=0;i<50;i++){ struct nt_ethread *t=kshim_current_thread(); if(t&&t->terminated) break; usleep(100*1000);} return 12345; }
static ULONG thr_flag(PVOID a){ atomic_int *p=(atomic_int*)a; atomic_store(p,1); return 999; }

static void setup_proc(void){
    ObCleanupObjectManager();
    ObInitObjectManager();
    struct nt_eprocess *proc=NULL;
    ObCreateProcessObject(&proc,0,"test");
    kshim_set_current_process(proc);
    struct nt_ethread *thr=NULL;
    ObCreateThreadObject(&thr,proc,0);
    kshim_set_current_thread(thr);
}

static void test_thread_create_join(void){
    setup_proc();
    struct nt_eprocess *proc=kshim_current_process();
    g_counter=0;
    HANDLE th;
    NTSTATUS st=nt_svc_NtCreateThreadEx(&th,0,NULL,NtCurrentProcess(),thr_simple,NULL,0,0,0,0,NULL);
    ASSERT(st==STATUS_SUCCESS);
    // query before wait should be STILL_ACTIVE
    THREAD_BASIC_INFORMATION tbi; ULONG ret=0;
    NTSTATUS q=nt_svc_NtQueryInformationThread(th,0,&tbi,48,&ret);
    ASSERT(q==STATUS_SUCCESS);
    ASSERT(tbi.ExitStatus==259);
    NTSTATUS wst=nt_svc_NtWaitForSingleObject(th,0,NULL);
    ASSERT(wst==STATUS_SUCCESS);
    q=nt_svc_NtQueryInformationThread(th,0,&tbi,48,&ret);
    ASSERT(q==STATUS_SUCCESS);
    ASSERT(tbi.ExitStatus==777);
    ASSERT(atomic_load(&g_counter)==1);
    ObCloseHandle(proc,th);
    PASS();
}

static void test_create_suspended(void){
    setup_proc();
    struct nt_eprocess *proc=kshim_current_process();
    atomic_int flag=0;
    HANDLE th;
    NTSTATUS st=nt_svc_NtCreateThreadEx(&th,0,NULL,NtCurrentProcess(),thr_flag,&flag,CREATE_SUSPENDED_FLAG,0,0,0,NULL);
    ASSERT(st==STATUS_SUCCESS);
    usleep(300*1000);
    ASSERT(atomic_load(&flag)==0);
    ULONG prev=0;
    NTSTATUS rst=nt_svc_NtResumeThread(th,&prev);
    ASSERT(rst==STATUS_SUCCESS);
    ASSERT(prev==1);
    NTSTATUS wst=nt_svc_NtWaitForSingleObject(th,0,NULL);
    ASSERT(wst==STATUS_SUCCESS);
    ASSERT(atomic_load(&flag)==1);
    ObCloseHandle(proc,th);
    PASS();
}

static void test_suspend_counts(void){
    setup_proc();
    struct nt_eprocess *proc=kshim_current_process();
    HANDLE th;
    NTSTATUS st=nt_svc_NtCreateThreadEx(&th,0,NULL,NtCurrentProcess(),thr_sleeper,NULL,0,0,0,0,NULL);
    ASSERT(st==STATUS_SUCCESS);
    usleep(200*1000);
    ULONG p1=0,p2=0;
    NTSTATUS s1=nt_svc_NtSuspendThread(th,&p1);
    NTSTATUS s2=nt_svc_NtSuspendThread(th,&p2);
    ASSERT(s1==STATUS_SUCCESS && p1==0);
    ASSERT(s2==STATUS_SUCCESS && p2==1);
    ULONG r1=0,r2=0;
    NTSTATUS r1s=nt_svc_NtResumeThread(th,&r1);
    NTSTATUS r2s=nt_svc_NtResumeThread(th,&r2);
    ASSERT(r1s==STATUS_SUCCESS && r1==2);
    ASSERT(r2s==STATUS_SUCCESS && r2==1);
    nt_svc_NtTerminateThread(th,0);
    ObCloseHandle(proc,th);
    PASS();
}

static void test_mutant_abandon(void){
    setup_proc();
    struct nt_mutant *m=NULL;
    NTSTATUS st=nt_create_mutant(&m,0);
    ASSERT(st==STATUS_SUCCESS);
    struct nt_thread *owner=NULL; nt_thread_create(&owner); nt_set_current_thread(owner);
    struct nt_dispatcher_header *dh=&m->header;
    NTSTATUS wst=nt_wait_for_single_object(dh,0,NULL);
    ASSERT(wst==STATUS_SUCCESS);
    nt_abandon_owned_mutants(owner);
    nt_thread_destroy(owner);
    struct nt_thread *waiter=NULL; nt_thread_create(&waiter); nt_set_current_thread(waiter);
    LARGE_INTEGER to; to.QuadPart=-10000000LL;
    NTSTATUS wst2=nt_wait_for_single_object(dh,0,&to);
    ASSERT(wst2==STATUS_ABANDONED_WAIT_0);
    nt_thread_destroy(waiter);
    nt_put_mutant(m);
    PASS();
}

static void test_process_exit(void){
    setup_proc();
    struct nt_eprocess *proc=kshim_current_process();
    HANDLE ph;
    NTSTATUS st=nt_svc_NtCreateProcessEx(&ph,0,NULL,NtCurrentProcess(),0,NULL,NULL,NULL,NULL);
    ASSERT(st==STATUS_SUCCESS);
    PROCESS_BASIC_INFORMATION pbi; ULONG ret=0;
    NTSTATUS q1=nt_svc_NtQueryInformationProcess(ph,0,&pbi,48,&ret);
    ASSERT(q1==STATUS_SUCCESS && pbi.ExitStatus==259);
    NTSTATUS tst=nt_svc_NtTerminateProcess(ph,42);
    ASSERT(tst==STATUS_SUCCESS);
    NTSTATUS q2=nt_svc_NtQueryInformationProcess(ph,0,&pbi,48,&ret);
    ASSERT(q2==STATUS_SUCCESS && pbi.ExitStatus==42);
    ObCloseHandle(proc,ph);
    PASS();
}

static void test_exact_sizes(void){
    setup_proc();
    char buf[1024]; ULONG ret=0;
    int lens[]={0,4,48,49,1024};
    for(int i=0;i<5;i++){
        NTSTATUS s=nt_svc_NtQueryInformationProcess(NtCurrentProcess(),0,buf,lens[i],&ret);
        if(lens[i]==48) ASSERT(s==STATUS_SUCCESS);
        else ASSERT(s==STATUS_INFO_LENGTH_MISMATCH);
    }
    int lens2[]={0,4,48,49,512};
    for(int i=0;i<5;i++){
        NTSTATUS s=nt_svc_NtQueryInformationThread(NtCurrentThread(),0,buf,lens2[i],&ret);
        if(lens2[i]==48) ASSERT(s==STATUS_SUCCESS);
        else ASSERT(s==STATUS_INFO_LENGTH_MISMATCH);
    }
    // invalid class
    NTSTATUS s=nt_svc_NtQueryInformationProcess(NtCurrentProcess(),9999,buf,512,&ret);
    ASSERT(s==STATUS_INVALID_INFO_CLASS);
    s=nt_svc_NtQueryInformationThread(NtCurrentThread(),9999,buf,512,&ret);
    ASSERT(s==STATUS_INVALID_INFO_CLASS);
    PASS();
}

static void test_open(void){
    setup_proc();
    struct nt_eprocess *proc=kshim_current_process();
    // create a new process to open
    HANDLE ph;
    NTSTATUS st=nt_svc_NtCreateProcessEx(&ph,0,NULL,NtCurrentProcess(),0,NULL,NULL,NULL,NULL);
    ASSERT(st==STATUS_SUCCESS);
    // get pid via query
    PROCESS_BASIC_INFORMATION pbi; ULONG ret=0;
    nt_svc_NtQueryInformationProcess(ph,0,&pbi,48,&ret);
    ULONG pid=(ULONG)(ULONG_PTR)pbi.UniqueProcessId;
    CLIENT_ID cid; cid.UniqueProcess=(HANDLE)(ULONG_PTR)pid; cid.UniqueThread=0;
    HANDLE opened;
    NTSTATUS ost=nt_svc_NtOpenProcess(&opened,0,NULL,&cid);
    ASSERT(ost==STATUS_SUCCESS);
    ObCloseHandle(proc,opened);
    // thread open
    THREAD_BASIC_INFORMATION tbi;
    nt_svc_NtQueryInformationThread(NtCurrentThread(),0,&tbi,48,&ret);
    ULONG tid=(ULONG)(ULONG_PTR)tbi.ClientId.UniqueThread;
    cid.UniqueProcess=0; cid.UniqueThread=(HANDLE)(ULONG_PTR)tid;
    HANDLE th;
    NTSTATUS tst=nt_svc_NtOpenThread(&th,0,NULL,&cid);
    ASSERT(tst==STATUS_SUCCESS);
    ObCloseHandle(proc,th);
    ObCloseHandle(proc,ph);
    PASS();
}

/* ---- Gap 1: Mutant abandonment across three exit paths (proc_thread level) ---- */
static struct nt_mutant *g_gap_mutant = NULL;
static ULONG thr_self_acquire(PVOID a){
    struct nt_mutant *m=(struct nt_mutant*)a;
    NTSTATUS s=nt_wait_for_single_object((struct nt_dispatcher_header*)m,0,NULL);
    if(s!=STATUS_WAIT_0 && s!=STATUS_ABANDONED_WAIT_0) return 1;
    nt_svc_NtTerminateThread(NtCurrentThread(), 0);
    return 0;
}
static ULONG thr_peer_hold(PVOID a){
    struct nt_mutant *m=(struct nt_mutant*)a;
    NTSTATUS s=nt_wait_for_single_object((struct nt_dispatcher_header*)m,0,NULL);
    if(s!=STATUS_WAIT_0) return 1;
    for(int i=0;i<50;i++){ struct nt_ethread *t=kshim_current_thread(); if(t&&t->terminated) break; usleep(100*1000); }
    return 0;
}
static void test_mutant_abandon_self_terminate(void){
    setup_proc();
    struct nt_thread *waiter=NULL; nt_thread_create(&waiter); nt_set_current_thread(waiter);
    struct nt_mutant *m=NULL; NTSTATUS st=nt_create_mutant(&m,0); ASSERT(st==STATUS_SUCCESS);
    g_gap_mutant=m;
    HANDLE th;
    st=nt_svc_NtCreateThreadEx(&th,0,NULL,NtCurrentProcess(),thr_self_acquire,m,0,0,0,0,NULL);
    ASSERT(st==STATUS_SUCCESS);
    nt_svc_NtWaitForSingleObject(th,0,NULL);
    ObCloseHandle(kshim_current_process(), th);
    struct nt_dispatcher_header *h=(struct nt_dispatcher_header*)m;
    LARGE_INTEGER zero={0};
    NTSTATUS ws=nt_wait_for_single_object(h,0,&zero);
    if(ws!=STATUS_ABANDONED_WAIT_0){ fprintf(stderr,"FAIL self_terminate got 0x%lx exp ABANDONED\n",(unsigned long)ws); g_fail++; nt_put_mutant(m); nt_set_current_thread(NULL); nt_thread_destroy(waiter); return; }
    nt_release_mutant(m,NULL);
    nt_put_mutant(m);
    nt_set_current_thread(NULL); nt_thread_destroy(waiter);
    PASS();
}
static void test_mutant_abandon_peer_terminate(void){
    setup_proc();
    struct nt_thread *waiter=NULL; nt_thread_create(&waiter); nt_set_current_thread(waiter);
    struct nt_mutant *m=NULL; NTSTATUS st=nt_create_mutant(&m,0); ASSERT(st==STATUS_SUCCESS);
    HANDLE th;
    st=nt_svc_NtCreateThreadEx(&th,0,NULL,NtCurrentProcess(),thr_peer_hold,m,0,0,0,0,NULL);
    ASSERT(st==STATUS_SUCCESS);
    usleep(150000);
    NTSTATUS term=nt_svc_NtTerminateThread(th,0);
    ASSERT(term==STATUS_SUCCESS);
    nt_svc_NtWaitForSingleObject(th,0,NULL);
    ObCloseHandle(kshim_current_process(), th);
    struct nt_dispatcher_header *h=(struct nt_dispatcher_header*)m;
    LARGE_INTEGER to; to.QuadPart=-2000000LL; /* 200ms */
    NTSTATUS ws=nt_wait_for_single_object(h,0,&to);
    if(ws!=STATUS_ABANDONED_WAIT_0){ fprintf(stderr,"FAIL peer_terminate got 0x%lx exp ABANDONED (reproduces kernel bug: peer NtTerminateThread did not abandon mutant)\n",(unsigned long)ws); g_fail++; nt_put_mutant(m); nt_set_current_thread(NULL); nt_thread_destroy(waiter); return; }
    nt_release_mutant(m,NULL);
    nt_put_mutant(m);
    nt_set_current_thread(NULL); nt_thread_destroy(waiter);
    PASS();
}
static struct nt_mutant *g_proc_mutant = NULL;
static ULONG thr_proc_hold(PVOID a){
    struct nt_mutant *m=(struct nt_mutant*)a;
    NTSTATUS s=nt_wait_for_single_object((struct nt_dispatcher_header*)m,0,NULL);
    if(s!=STATUS_WAIT_0) return 1;
    for(int i=0;i<100;i++){ struct nt_ethread *t=kshim_current_thread(); if(t&&t->terminated) break; usleep(100*1000); }
    return 0;
}
static void test_mutant_abandon_process_exit(void){
    setup_proc();
    struct nt_thread *waiter=NULL; nt_thread_create(&waiter); nt_set_current_thread(waiter);
    struct nt_eprocess *cur=kshim_current_process();
    struct nt_mutant *m=NULL; NTSTATUS st=nt_create_mutant(&m,0); ASSERT(st==STATUS_SUCCESS);
    g_proc_mutant=m;
    HANDLE ph; st=nt_svc_NtCreateProcessEx(&ph,0,NULL,NtCurrentProcess(),0,NULL,NULL,NULL,NULL);
    ASSERT(st==STATUS_SUCCESS);
    HANDLE th;
    st=nt_svc_NtCreateThreadEx(&th,0,NULL,ph,thr_proc_hold,m,0,0,0,0,NULL);
    ASSERT(st==STATUS_SUCCESS);
    usleep(150000);
    NTSTATUS term=nt_svc_NtTerminateProcess(ph, 99);
    ASSERT(term==STATUS_SUCCESS);
    nt_svc_NtWaitForSingleObject(th,0,NULL);
    ObCloseHandle(cur, th);
    ObCloseHandle(cur, ph);
    struct nt_dispatcher_header *h=(struct nt_dispatcher_header*)m;
    LARGE_INTEGER to; to.QuadPart=-2000000LL;
    NTSTATUS ws=nt_wait_for_single_object(h,0,&to);
    if(ws!=STATUS_ABANDONED_WAIT_0){ fprintf(stderr,"FAIL process_exit got 0x%lx exp ABANDONED (reproduces kernel bug: NtTerminateProcess did not abandon mutants)\n",(unsigned long)ws); g_fail++; nt_put_mutant(m); nt_set_current_thread(NULL); nt_thread_destroy(waiter); return; }
    nt_release_mutant(m,NULL);
    nt_put_mutant(m);
    nt_set_current_thread(NULL); nt_thread_destroy(waiter);
    PASS();
}

/* ---- Gap 5: ReturnLength discipline (Windows table) ---- */
static void test_query_returnlength_discipline(void){
    setup_proc();
    char buf[1024];
    ULONG ret;
    /* Fixed-size classes must leave ReturnLength untouched on error (0xDEADBEEF sentinel) */
    struct { ULONG cls; ULONG bad_len; } fixed_proc[] = {
        {0, 4}, {1, 4}, {2, 4}, {7, 4}, {12, 8}, {26, 4}, {30, 4}, {36, 8}
    };
    for(size_t i=0;i<sizeof(fixed_proc)/sizeof(fixed_proc[0]);i++){
        ret=0xDEADBEEF;
        NTSTATUS s=nt_svc_NtQueryInformationProcess(NtCurrentProcess(), fixed_proc[i].cls, buf, fixed_proc[i].bad_len, &ret);
        ASSERT(s==STATUS_INFO_LENGTH_MISMATCH);
        ASSERT(ret==0xDEADBEEF);
    }
    /* Variable-size ProcessImageFileName (27) must write required size even on mismatch */
    ret=0xDEADBEEF;
    NTSTATUS s=nt_svc_NtQueryInformationProcess(NtCurrentProcess(), 27, buf, 4, &ret);
    ASSERT(s==STATUS_INFO_LENGTH_MISMATCH);
    ASSERT(ret!=0xDEADBEEF);
    ASSERT(ret >= sizeof(UNICODE_STRING));
    /* Also test with zero length but ReturnLength != NULL should be written for variable */
    ret=0xDEADBEEF;
    s=nt_svc_NtQueryInformationProcess(NtCurrentProcess(), 27, buf, 0, &ret);
    ASSERT(s==STATUS_INFO_LENGTH_MISMATCH);
    ASSERT(ret!=0xDEADBEEF);
    /* Thread fixed classes */
    struct { ULONG cls; ULONG bad_len; } fixed_thread[] = {{0, 4}, {1, 16}, {9, 4}};
    for(size_t i=0;i<sizeof(fixed_thread)/sizeof(fixed_thread[0]);i++){
        ret=0xDEADBEEF;
        NTSTATUS st=nt_svc_NtQueryInformationThread(NtCurrentThread(), fixed_thread[i].cls, buf, fixed_thread[i].bad_len, &ret);
        ASSERT(st==STATUS_INFO_LENGTH_MISMATCH);
        ASSERT(ret==0xDEADBEEF);
    }
    /* NULL ReturnLength should not crash */
    s=nt_svc_NtQueryInformationProcess(NtCurrentProcess(), 0, buf, 4, NULL);
    ASSERT(s==STATUS_INFO_LENGTH_MISMATCH);
    s=nt_svc_NtQueryInformationProcess(NtCurrentProcess(), 27, buf, 4, NULL);
    ASSERT(s==STATUS_INFO_LENGTH_MISMATCH);
    PASS();
}

int main(void){
    printf("proc_thread tests\n");
    RUN(test_thread_create_join);
    RUN(test_create_suspended);
    RUN(test_suspend_counts);
    RUN(test_mutant_abandon);
    RUN(test_process_exit);
    RUN(test_exact_sizes);
    RUN(test_open);
    RUN(test_mutant_abandon_self_terminate);
    RUN(test_mutant_abandon_peer_terminate);
    RUN(test_mutant_abandon_process_exit);
    RUN(test_query_returnlength_discipline);
    printf("proc_thread tests: %d passed, %d failed\n", g_pass, g_fail);
    long leaks=atomic_load(&kshim_alloc_count);
    printf("leaks %ld\n", leaks);
    ObCleanupObjectManager();
    long after=atomic_load(&kshim_alloc_count);
    if(after!=0) { fprintf(stderr,"LEAK %ld\n",after); return 1; }
    return g_fail?1:0;
}
