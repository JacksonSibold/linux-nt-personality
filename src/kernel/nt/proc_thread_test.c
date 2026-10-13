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
#include "security.h"
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

/* Waits until the sync.c allocations are back to baseline, i.e. the threads
 * created since the baseline have run their exit path (see thread_trampoline).
 * Waiting on the thread handle is not enough: it is signalled before the
 * thread's trampoline has finished. */
static void wait_sync_released(long baseline){
    for(int i=0; i<300 && kshim_live_site_count("sync.c")>baseline; i++) usleep(10000);
}

static void test_suspend_counts(void){
    setup_proc();
    struct nt_eprocess *proc=kshim_current_process();
    long before = kshim_live_site_count("sync.c");
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
    wait_sync_released(before);
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

/* ---- Thread exit releases its sync thread (tracked allocation check) ----
 * Each thread_trampoline allocates an nt_thread (sync.c nt_thread_create) and
 * frees it on return. Self-termination (pthread_exit) skips that return path,
 * so the nt_thread must be released by a cleanup handler instead. */
static ULONG thr_self_term(PVOID a){ (void)a; nt_svc_NtTerminateThread(NtCurrentThread(), 0); return 0; }
static void test_thread_exit_frees_sync_thread(void){
    setup_proc();
    long before = kshim_live_site_count("sync.c");
    HANDLE th;
    NTSTATUS st = nt_svc_NtCreateThreadEx(&th,0,NULL,NtCurrentProcess(),thr_self_term,NULL,0,0,0,0,NULL);
    ASSERT(st==STATUS_SUCCESS);
    nt_svc_NtWaitForSingleObject(th,0,NULL);
    long now = kshim_live_site_count("sync.c");
    for(int i=0; i<200 && now!=before; i++){ usleep(10000); now = kshim_live_site_count("sync.c"); }
    ObCloseHandle(kshim_current_process(), th);
    if(now!=before){ fprintf(stderr,"FAIL thread_exit: sync.c live allocations %ld after exit, expected %ld\n", now, before); g_fail++; return; }
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

/* Open by an unknown process or thread id is STATUS_INVALID_CID; a missing
 * ClientId is STATUS_INVALID_PARAMETER. */
static void test_open_bad_cid(void){
    setup_proc();
    HANDLE h=NULL;
    CLIENT_ID cid; cid.UniqueProcess=(HANDLE)(ULONG_PTR)0x7FFFFFF0; cid.UniqueThread=0;
    ASSERT(nt_svc_NtOpenProcess(&h,0,NULL,&cid)==STATUS_INVALID_CID);
    cid.UniqueProcess=0; cid.UniqueThread=(HANDLE)(ULONG_PTR)0x7FFFFFF0;
    ASSERT(nt_svc_NtOpenThread(&h,0,NULL,&cid)==STATUS_INVALID_CID);
    ASSERT(nt_svc_NtOpenProcess(&h,0,NULL,NULL)==STATUS_INVALID_PARAMETER);
    ASSERT(nt_svc_NtOpenThread(&h,0,NULL,NULL)==STATUS_INVALID_PARAMETER);
    PASS();
}

/* An exiting process refuses a second terminate and any suspend; the first
 * exit status is kept. */
static void test_process_terminate_twice(void){
    setup_proc();
    struct nt_eprocess *proc=kshim_current_process();
    HANDLE ph=NULL;
    NTSTATUS st=nt_svc_NtCreateProcessEx(&ph,0,NULL,NtCurrentProcess(),0,NULL,NULL,NULL,NULL);
    ASSERT(st==STATUS_SUCCESS);
    ASSERT(nt_svc_NtTerminateProcess(ph,7)==STATUS_SUCCESS);
    ASSERT(nt_svc_NtTerminateProcess(ph,8)==STATUS_PROCESS_IS_TERMINATING);
    ASSERT(nt_svc_NtSuspendProcess(ph)==STATUS_PROCESS_IS_TERMINATING);
    PROCESS_BASIC_INFORMATION pbi; ULONG ret=0;
    NTSTATUS q=nt_svc_NtQueryInformationProcess(ph,0,&pbi,48,&ret);
    ASSERT(q==STATUS_SUCCESS && pbi.ExitStatus==7);
    ObCloseHandle(proc,ph);
    PASS();
}

/* ---- Thread set-information: ThreadPriority / ThreadBasePriority / ThreadAffinityMask ---- */
static void test_set_thread_priority(void){
    setup_proc();
    struct nt_ethread *thr=kshim_current_thread();
    LONG pri; NTSTATUS s; ULONG ret=0;
    THREAD_BASIC_INFORMATION tbi;
    /* ThreadPriority takes exactly one LONG */
    pri=5;
    s=nt_svc_NtSetInformationThread(NtCurrentThread(),2,&pri,8);
    ASSERT(s==STATUS_INFO_LENGTH_MISMATCH);
    s=nt_svc_NtSetInformationThread(NtCurrentThread(),2,NULL,4);
    ASSERT(s==STATUS_INVALID_PARAMETER);
    /* Relative range is -15 (THREAD_PRIORITY_LOWEST band) .. 15 */
    pri=16; s=nt_svc_NtSetInformationThread(NtCurrentThread(),2,&pri,4);
    ASSERT(s==STATUS_INVALID_PARAMETER);
    pri=-16; s=nt_svc_NtSetInformationThread(NtCurrentThread(),2,&pri,4);
    ASSERT(s==STATUS_INVALID_PARAMETER);
    /* Rejected values must not change the stored priority */
    s=nt_svc_NtQueryInformationThread(NtCurrentThread(),0,&tbi,48,&ret);
    ASSERT(s==STATUS_SUCCESS && tbi.Priority==8 && tbi.BasePriority==8);
    /* Valid set is visible in ThreadBasicInformation */
    pri=15; s=nt_svc_NtSetInformationThread(NtCurrentThread(),2,&pri,4);
    ASSERT(s==STATUS_SUCCESS);
    s=nt_svc_NtQueryInformationThread(NtCurrentThread(),0,&tbi,48,&ret);
    ASSERT(s==STATUS_SUCCESS && tbi.Priority==23);
    ASSERT(tbi.BasePriority==23);   /* process base 8 + relative 15 */
    /* ThreadBasePriority (3) shares the same rules */
    pri=-2; s=nt_svc_NtSetInformationThread(NtCurrentThread(),3,&pri,4);
    ASSERT(s==STATUS_SUCCESS);
    s=nt_svc_NtQueryInformationThread(NtCurrentThread(),0,&tbi,48,&ret);
    ASSERT(s==STATUS_SUCCESS && tbi.Priority==6);
    /* ThreadPriority is set-only: query is an invalid class */
    LONG qbuf=0;
    s=nt_svc_NtQueryInformationThread(NtCurrentThread(),2,&qbuf,4,&ret);
    ASSERT(s==STATUS_INVALID_INFO_CLASS);
    /* Terminated threads refuse the set */
    pthread_mutex_lock(&thr->lock); thr->terminated=true; pthread_mutex_unlock(&thr->lock);
    pri=1; s=nt_svc_NtSetInformationThread(NtCurrentThread(),2,&pri,4);
    ASSERT(s==STATUS_THREAD_IS_TERMINATING);
    pthread_mutex_lock(&thr->lock); thr->terminated=false; pthread_mutex_unlock(&thr->lock);
    /* Bad handle */
    s=nt_svc_NtSetInformationThread((HANDLE)(ULONG_PTR)0x1234,2,&pri,4);
    ASSERT(s==STATUS_INVALID_HANDLE);
    PASS();
}

static void test_set_thread_affinity(void){
    setup_proc();
    ULONG_PTR mask; NTSTATUS s; ULONG ret=0;
    THREAD_BASIC_INFORMATION tbi;
    /* ThreadAffinityMask takes exactly one ULONG_PTR */
    mask=0x3;
    s=nt_svc_NtSetInformationThread(NtCurrentThread(),4,&mask,4);
    ASSERT(s==STATUS_INFO_LENGTH_MISMATCH);
    s=nt_svc_NtSetInformationThread(NtCurrentThread(),4,NULL,sizeof(ULONG_PTR));
    ASSERT(s==STATUS_INVALID_PARAMETER);
    /* Empty mask is invalid */
    mask=0;
    s=nt_svc_NtSetInformationThread(NtCurrentThread(),4,&mask,sizeof(ULONG_PTR));
    ASSERT(s==STATUS_INVALID_PARAMETER);
    /* Must be a subset of the process mask (0xFF in this build) */
    mask=0x100;
    s=nt_svc_NtSetInformationThread(NtCurrentThread(),4,&mask,sizeof(ULONG_PTR));
    ASSERT(s==STATUS_INVALID_PARAMETER);
    /* Untouched thread reports the process mask */
    s=nt_svc_NtQueryInformationThread(NtCurrentThread(),0,&tbi,48,&ret);
    ASSERT(s==STATUS_SUCCESS && tbi.AffinityMask==0xFF);
    /* Valid subset is reported back */
    mask=0x3;
    s=nt_svc_NtSetInformationThread(NtCurrentThread(),4,&mask,sizeof(ULONG_PTR));
    ASSERT(s==STATUS_SUCCESS);
    s=nt_svc_NtQueryInformationThread(NtCurrentThread(),0,&tbi,48,&ret);
    ASSERT(s==STATUS_SUCCESS && tbi.AffinityMask==0x3);
    /* ThreadAffinityMask is set-only */
    ULONG_PTR qbuf=0;
    s=nt_svc_NtQueryInformationThread(NtCurrentThread(),4,&qbuf,sizeof(ULONG_PTR),&ret);
    ASSERT(s==STATUS_INVALID_INFO_CLASS);
    PASS();
}

/* ---- NtAlertResumeThread, NtAlertThread, NtTestAlert alert state ---- */
static void test_alert_resume_thread(void){
    setup_proc();
    struct nt_eprocess *proc=kshim_current_process();
    struct nt_ethread *cur=kshim_current_thread();
    struct nt_thread *cursync=NULL;
    HANDLE th=NULL; ULONG prev=99;
    NTSTATUS s;
    ASSERT(nt_thread_create(&cursync)==STATUS_SUCCESS);
    cur->sync_thread=cursync;
    /* Open a second thread object to act on (current thread is a pseudo handle) */
    CLIENT_ID cid; cid.UniqueProcess=0; cid.UniqueThread=(HANDLE)(ULONG_PTR)cur->tid;
    s=nt_svc_NtOpenThread(&th,0,NULL,&cid);
    ASSERT(s==STATUS_SUCCESS);
    /* Suspend, then AlertResume: previous count 1, count back to 0, alert pending */
    s=nt_svc_NtSuspendThread(th,&prev);
    ASSERT(s==STATUS_SUCCESS && prev==0);
    ASSERT(atomic_load(&cur->suspend_count)==1);
    prev=99;
    s=nt_svc_NtAlertResumeThread(th,&prev);
    ASSERT(s==STATUS_SUCCESS && prev==1);
    ASSERT(atomic_load(&cur->suspend_count)==0);
    ASSERT(nt_thread_alert_pending(cur->sync_thread));
    /* Alerting a running thread still alerts it; previous count is 0 */
    ASSERT(nt_thread_consume_alert(cur->sync_thread));
    prev=99;
    s=nt_svc_NtAlertResumeThread(th,&prev);
    ASSERT(s==STATUS_SUCCESS && prev==0);
    ASSERT(nt_thread_alert_pending(cur->sync_thread));
    /* NtAlertThread alerts without touching the suspend count */
    ASSERT(nt_thread_consume_alert(cur->sync_thread));
    s=nt_svc_NtAlertThread(th);
    ASSERT(s==STATUS_SUCCESS && nt_thread_alert_pending(cur->sync_thread));
    ASSERT(atomic_load(&cur->suspend_count)==0);
    ASSERT(nt_thread_consume_alert(cur->sync_thread));
    /* Terminated thread refuses alert and alert-resume */
    pthread_mutex_lock(&cur->lock); cur->terminated=true; pthread_mutex_unlock(&cur->lock);
    ASSERT(nt_svc_NtAlertThread(th)==STATUS_THREAD_IS_TERMINATING);
    ASSERT(nt_svc_NtAlertResumeThread(th,&prev)==STATUS_THREAD_IS_TERMINATING);
    pthread_mutex_lock(&cur->lock); cur->terminated=false; pthread_mutex_unlock(&cur->lock);
    /* NULL PreviousSuspendCount is allowed; bad handle is INVALID_HANDLE */
    ASSERT(nt_svc_NtAlertResumeThread(th,NULL)==STATUS_SUCCESS);
    ASSERT(nt_svc_NtAlertResumeThread((HANDLE)(ULONG_PTR)0x1234,&prev)==STATUS_INVALID_HANDLE);
    ObCloseHandle(proc,th);
    cur->sync_thread=NULL;
    nt_thread_destroy(cursync);
    PASS();
}

/* ---- An alert satisfies an alertable wait (NT: KTHREAD.Alerted). An
 * alertable wait returns STATUS_ALERTED and consumes the alert; a
 * non-alertable wait ignores it. ---- */
NTSTATUS nt_svc_NtCreateEvent(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, ULONG, BOOLEAN);
static void test_alert_satisfies_alertable_wait(void){
    setup_proc();
    struct nt_eprocess *proc=kshim_current_process();
    struct nt_thread *synct=NULL;
    ASSERT(nt_thread_create(&synct)==STATUS_SUCCESS);
    nt_set_current_thread(synct);
    kshim_current_thread()->sync_thread=synct;
    HANDLE ev=NULL;
    ASSERT(nt_svc_NtCreateEvent(&ev,0x1F0003,NULL,NotificationEvent,0)==STATUS_SUCCESS);
    LARGE_INTEGER zero; zero.QuadPart=0;
    ASSERT(nt_svc_NtAlertThread(NtCurrentThread())==STATUS_SUCCESS);
    /* Non-alertable wait on an unsignalled event: times out, alert kept */
    ASSERT(nt_svc_NtWaitForSingleObject(ev,FALSE,&zero)==STATUS_TIMEOUT);
    /* Alertable wait: the alert satisfies it and is consumed */
    ASSERT(nt_svc_NtWaitForSingleObject(ev,TRUE,&zero)==STATUS_ALERTED);
    ASSERT(nt_svc_NtWaitForSingleObject(ev,TRUE,&zero)==STATUS_TIMEOUT);
    /* NtAlertResumeThread on the current thread also satisfies it */
    HANDLE cur=NULL; ULONG prev=0;
    CLIENT_ID cid; cid.UniqueProcess=0; cid.UniqueThread=(HANDLE)(ULONG_PTR)kshim_current_thread()->tid;
    ASSERT(nt_svc_NtOpenThread(&cur,0,NULL,&cid)==STATUS_SUCCESS);
    ASSERT(nt_svc_NtAlertResumeThread(cur,&prev)==STATUS_SUCCESS);
    ASSERT(nt_svc_NtWaitForSingleObject(ev,TRUE,&zero)==STATUS_ALERTED);
    ObCloseHandle(proc,cur);
    ObCloseHandle(proc,ev);
    kshim_current_thread()->sync_thread=NULL;
    nt_set_current_thread(NULL);
    nt_thread_destroy(synct);
    PASS();
}

/* ---- NtWaitForAlertByThreadId: the by-thread-id token. NtAlertThreadByThreadId
 * sets it and the wait consumes it (STATUS_ALERTED). It is separate from the
 * alertable-wait flag: NtAlertThread does not satisfy this wait, and the token
 * does not satisfy an alertable wait. ---- */
NTSTATUS nt_svc_NtWaitForAlertByThreadId(PVOID Address, PLARGE_INTEGER Timeout);
NTSTATUS nt_svc_NtAlertThreadByThreadId(HANDLE ThreadId);
NTSTATUS nt_svc_NtCreateEvent(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, ULONG, BOOLEAN);

struct pt_test_tid_alert_arg { ULONG tid; long delay_ms; NTSTATUS st; };
static void *pt_test_tid_alert_thread(void *p){
    struct pt_test_tid_alert_arg *a=p;
    usleep(a->delay_ms*1000);
    a->st=nt_svc_NtAlertThreadByThreadId((HANDLE)(ULONG_PTR)a->tid);
    return NULL;
}

static void test_wait_for_alert_by_thread_id(void){
    setup_proc();
    struct nt_eprocess *proc=kshim_current_process();
    struct nt_ethread *cur=kshim_current_thread();
    struct nt_thread *cursync=NULL;
    LARGE_INTEGER zero, rel;
    HANDLE ev=NULL;
    zero.QuadPart=0;
    ASSERT(nt_thread_create(&cursync)==STATUS_SUCCESS);
    cur->sync_thread=cursync;
    /* Token set before the wait: the wait returns at once and consumes it */
    ASSERT(nt_svc_NtAlertThreadByThreadId((HANDLE)(ULONG_PTR)cur->tid)==STATUS_SUCCESS);
    ASSERT(nt_svc_NtWaitForAlertByThreadId(NULL,&zero)==STATUS_ALERTED);
    ASSERT(nt_svc_NtWaitForAlertByThreadId(NULL,&zero)==STATUS_TIMEOUT);
    /* NtAlertThreadByThreadId sets the token, not the alertable flag */
    ASSERT(nt_svc_NtAlertThreadByThreadId((HANDLE)(ULONG_PTR)cur->tid)==STATUS_SUCCESS);
    ASSERT(!nt_thread_alert_pending(cursync));
    /* The token does not satisfy an alertable wait (it is not the alert flag) */
    nt_set_current_thread(cursync);
    ASSERT(nt_svc_NtCreateEvent(&ev,0x1F0003,NULL,NotificationEvent,0)==STATUS_SUCCESS);
    ASSERT(nt_svc_NtAlertThreadByThreadId((HANDLE)(ULONG_PTR)cur->tid)==STATUS_SUCCESS);
    ASSERT(nt_svc_NtWaitForSingleObject(ev,TRUE,&zero)==STATUS_TIMEOUT);
    ASSERT(nt_svc_NtWaitForAlertByThreadId(NULL,&zero)==STATUS_ALERTED);
    nt_set_current_thread(NULL);
    /* NtAlertThread does not satisfy the by-thread-id wait; the alert is kept */
    ASSERT(nt_svc_NtAlertThread(NtCurrentThread())==STATUS_SUCCESS);
    ASSERT(nt_svc_NtWaitForAlertByThreadId(NULL,&zero)==STATUS_TIMEOUT);
    ASSERT(nt_thread_alert_pending(cursync));
    ASSERT(nt_thread_consume_alert(cursync));
    /* Relative timeout (10 ms) with no token: STATUS_TIMEOUT */
    rel.QuadPart=-100000;
    ASSERT(nt_svc_NtWaitForAlertByThreadId(NULL,&rel)==STATUS_TIMEOUT);
    /* Blocking wait (NULL timeout) woken by a token from another thread */
    pthread_t th;
    struct pt_test_tid_alert_arg arg={ (ULONG)cur->tid, 30, STATUS_UNSUCCESSFUL };
    ASSERT(pthread_create(&th,NULL,pt_test_tid_alert_thread,&arg)==0);
    ASSERT(nt_svc_NtWaitForAlertByThreadId(NULL,NULL)==STATUS_ALERTED);
    pthread_join(th,NULL);
    ASSERT(arg.st==STATUS_SUCCESS);
    /* Unknown thread id, and a terminated thread, are refused */
    ASSERT(nt_svc_NtAlertThreadByThreadId((HANDLE)(ULONG_PTR)0x7777)==STATUS_INVALID_CID);
    pthread_mutex_lock(&cur->lock); cur->terminated=true; pthread_mutex_unlock(&cur->lock);
    ASSERT(nt_svc_NtAlertThreadByThreadId((HANDLE)(ULONG_PTR)cur->tid)==STATUS_THREAD_IS_TERMINATING);
    pthread_mutex_lock(&cur->lock); cur->terminated=false; pthread_mutex_unlock(&cur->lock);
    ObCloseHandle(proc,ev);
    cur->sync_thread=NULL;
    nt_thread_destroy(cursync);
    PASS();
}

/* ---- NtContinue, NtContinueEx, NtRaiseException (shim branch). The shim records
 * the continue context on the thread; it has no CPU state to resume, so
 * NtContinue returns STATUS_SUCCESS after recording. A pending alert consumed
 * by TestAlert returns STATUS_ALERTED and records nothing. NtRaiseException
 * validates and then returns STATUS_NOT_IMPLEMENTED: this build has no dispatch
 * queue. ---- */
NTSTATUS nt_svc_NtContinue(PCONTEXT ContextRecord, BOOLEAN TestAlert);
NTSTATUS nt_svc_NtContinueEx(PCONTEXT ContextRecord, PVOID ContinueArgument);
NTSTATUS nt_svc_NtRaiseException(PEXCEPTION_RECORD ExceptionRecord, PCONTEXT ContextRecord, BOOLEAN FirstChance);

typedef struct { USHORT ContinueFlags; } PT_TEST_CONTINUE_ARG;

static void test_continue_and_raise(void){
    setup_proc();
    struct nt_ethread *cur=kshim_current_thread();
    struct nt_thread *cursync=NULL;
    CONTEXT ctx __attribute__((aligned(16)));
    CONTEXT ctx2 __attribute__((aligned(16)));
    EXCEPTION_RECORD rec;
    PT_TEST_CONTINUE_ARG arg;
    ASSERT(nt_thread_create(&cursync)==STATUS_SUCCESS);
    cur->sync_thread=cursync;
    memset(&ctx,0,sizeof(ctx)); memset(&ctx2,0,sizeof(ctx2)); memset(&rec,0,sizeof(rec));
    ctx.ContextFlags=CONTEXT_FULL; ctx.Rip=0x1234;
    ctx2.ContextFlags=CONTEXT_FULL; ctx2.Rip=0x9999;
    /* Argument validation: NULL is an access violation, an unaligned CONTEXT is
     * a misalignment, and ContextFlags must carry CONTEXT_AMD64 and no unknown bits */
    ASSERT(nt_svc_NtContinue(NULL,FALSE)==STATUS_ACCESS_VIOLATION);
    ASSERT(nt_svc_NtContinue((PCONTEXT)((char*)&ctx+8),FALSE)==STATUS_DATATYPE_MISALIGNMENT);
    ctx2.ContextFlags=0;
    ASSERT(nt_svc_NtContinue(&ctx2,FALSE)==STATUS_INVALID_PARAMETER);
    ctx2.ContextFlags=CONTEXT_AMD64|0x100;
    ASSERT(nt_svc_NtContinue(&ctx2,FALSE)==STATUS_INVALID_PARAMETER);
    ASSERT(!cur->pending_continue_valid);
    ctx2.ContextFlags=CONTEXT_FULL; ctx2.Rip=0x9999;
    /* Valid context, no alert: recorded and STATUS_SUCCESS */
    ASSERT(nt_svc_NtContinue(&ctx,FALSE)==STATUS_SUCCESS);
    ASSERT(cur->pending_continue_valid && cur->pending_continue.Rip==0x1234);
    /* TestAlert with no pending alert: recorded as well */
    ASSERT(nt_svc_NtContinue(&ctx2,TRUE)==STATUS_SUCCESS);
    ASSERT(cur->pending_continue.Rip==0x9999);
    /* TestAlert with a pending alert: consumed, STATUS_ALERTED, nothing recorded */
    ctx2.Rip=0x7777;
    ASSERT(nt_svc_NtAlertThread(NtCurrentThread())==STATUS_SUCCESS);
    ASSERT(nt_svc_NtContinue(&ctx2,TRUE)==STATUS_ALERTED);
    ASSERT(!nt_thread_alert_pending(cursync));
    ASSERT(cur->pending_continue.Rip==0x9999);
    /* NtContinueEx: the argument's ContinueFlags bit 0 is TestAlert */
    ASSERT(nt_svc_NtContinueEx(NULL,&arg)==STATUS_ACCESS_VIOLATION);
    ASSERT(nt_svc_NtContinueEx(&ctx,NULL)==STATUS_ACCESS_VIOLATION);
    arg.ContinueFlags=0;
    ASSERT(nt_svc_NtContinueEx(&ctx,&arg)==STATUS_SUCCESS);
    ASSERT(cur->pending_continue.Rip==0x1234);
    ASSERT(nt_svc_NtAlertThread(NtCurrentThread())==STATUS_SUCCESS);
    arg.ContinueFlags=1;
    ASSERT(nt_svc_NtContinueEx(&ctx2,&arg)==STATUS_ALERTED);
    ASSERT(!nt_thread_alert_pending(cursync));
    /* Terminated thread refuses */
    pthread_mutex_lock(&cur->lock); cur->terminated=true; pthread_mutex_unlock(&cur->lock);
    ASSERT(nt_svc_NtContinue(&ctx,FALSE)==STATUS_THREAD_IS_TERMINATING);
    pthread_mutex_lock(&cur->lock); cur->terminated=false; pthread_mutex_unlock(&cur->lock);
    /* NtRaiseException: NULL record or context is an access violation, bad
     * flags are invalid, and a valid pair is validated then not implemented */
    ASSERT(nt_svc_NtRaiseException(NULL,&ctx,FALSE)==STATUS_ACCESS_VIOLATION);
    ASSERT(nt_svc_NtRaiseException(&rec,NULL,FALSE)==STATUS_ACCESS_VIOLATION);
    ctx2.ContextFlags=0;
    ASSERT(nt_svc_NtRaiseException(&rec,&ctx2,TRUE)==STATUS_INVALID_PARAMETER);
    ASSERT(nt_svc_NtRaiseException(&rec,&ctx,TRUE)==STATUS_NOT_IMPLEMENTED);
    cur->pending_continue_valid=false;
    cur->sync_thread=NULL;
    nt_thread_destroy(cursync);
    PASS();
}

/* ---- NtSetInformationProcess: priority classes (ProcessBasePriority 5,
 * ProcessPriorityClass 18). Set-only base priority; the class is set/query. ---- */
#define PT_TEST_PROC_QUERY 0x0400   /* PROCESS_QUERY_INFORMATION */
#define PT_TEST_PRIO_HIGH     3
#define PT_TEST_PRIO_REALTIME 4
#define PT_TEST_PRIO_NORMAL   2
typedef struct { BOOLEAN Foreground; UCHAR PriorityClass; } PT_TEST_PRIORITY_CLASS;

/* Token with (or without) one privilege, for the privilege-gated classes. */
static struct _ACCESS_TOKEN *pt_test_make_token(ULONG privilege, BOOLEAN with_privilege)
{
    PSID user = NULL;
    LUID_AND_ATTRIBUTES priv;
    struct _ACCESS_TOKEN *tok = NULL;
    if (sid_from_string("S-1-5-21-0-0-0-1000", &user) != STATUS_SUCCESS)
        return NULL;
    priv.Luid.LowPart = privilege;
    priv.Luid.HighPart = 0;
    priv.Attributes = SE_PRIVILEGE_ENABLED;
    tok = token_create_primary(user, NULL, 0, with_privilege ? &priv : NULL,
                               with_privilege ? 1 : 0, NULL, NULL, NULL, NULL, 0);
    sid_free(user);
    return tok;
}

static void pt_test_set_token(struct nt_eprocess *proc, struct _ACCESS_TOKEN *tok)
{
    if (proc->primary_token)
        token_dereference(proc->primary_token);
    proc->primary_token = tok;
}

/* A process handle that grants PROCESS_QUERY_INFORMATION but not SET. */
static NTSTATUS open_query_only_process(HANDLE *out)
{
    HANDLE ph = NULL;
    PROCESS_BASIC_INFORMATION pbi; ULONG ret = 0;
    NTSTATUS st = nt_svc_NtCreateProcessEx(&ph, 0, NULL, NtCurrentProcess(), 0, NULL, NULL, NULL, NULL);
    if (!NT_SUCCESS(st)) return st;
    nt_svc_NtQueryInformationProcess(ph, 0, &pbi, 48, &ret);
    CLIENT_ID cid; cid.UniqueProcess = pbi.UniqueProcessId; cid.UniqueThread = 0;
    st = nt_svc_NtOpenProcess(out, PT_TEST_PROC_QUERY, NULL, &cid);
    ObCloseHandle(kshim_current_process(), ph);   /* the creating handle is not needed */
    return st;
}

static void test_set_process_base_priority(void){
    setup_proc();
    LONG pri; NTSTATUS s; ULONG ret = 0;
    PROCESS_BASIC_INFORMATION pbi;
    /* ProcessBasePriority takes exactly one LONG */
    pri = 5;
    s = nt_svc_NtSetInformationProcess(NtCurrentProcess(), 5, &pri, 8);
    ASSERT(s == STATUS_INFO_LENGTH_MISMATCH);
    s = nt_svc_NtSetInformationProcess(NtCurrentProcess(), 5, NULL, 4);
    ASSERT(s == STATUS_INVALID_PARAMETER);
    /* Values are clamped to the 0..31 priority range and stored */
    pri = 40; s = nt_svc_NtSetInformationProcess(NtCurrentProcess(), 5, &pri, 4);
    ASSERT(s == STATUS_SUCCESS);
    s = nt_svc_NtQueryInformationProcess(NtCurrentProcess(), 0, &pbi, 48, &ret);
    ASSERT(s == STATUS_SUCCESS && pbi.BasePriority == 31);
    pri = -7; s = nt_svc_NtSetInformationProcess(NtCurrentProcess(), 5, &pri, 4);
    ASSERT(s == STATUS_SUCCESS);
    s = nt_svc_NtQueryInformationProcess(NtCurrentProcess(), 0, &pbi, 48, &ret);
    ASSERT(s == STATUS_SUCCESS && pbi.BasePriority == 0);
    pri = 6; s = nt_svc_NtSetInformationProcess(NtCurrentProcess(), 5, &pri, 4);
    ASSERT(s == STATUS_SUCCESS);
    s = nt_svc_NtQueryInformationProcess(NtCurrentProcess(), 0, &pbi, 48, &ret);
    ASSERT(s == STATUS_SUCCESS && pbi.BasePriority == 6);
    /* A handle without PROCESS_SET_INFORMATION is refused */
    HANDLE qh = NULL;
    s = open_query_only_process(&qh);
    ASSERT(NT_SUCCESS(s));
    pri = 9; s = nt_svc_NtSetInformationProcess(qh, 5, &pri, 4);
    ASSERT(s == STATUS_ACCESS_DENIED);
    s = nt_svc_NtQueryInformationProcess(qh, 0, &pbi, 48, &ret);
    ASSERT(s == STATUS_SUCCESS && pbi.BasePriority == 8);   /* new process default */
    ObCloseHandle(kshim_current_process(), qh);
    /* ProcessBasePriority is set-only */
    LONG qbuf = 0;
    s = nt_svc_NtQueryInformationProcess(NtCurrentProcess(), 5, &qbuf, 4, &ret);
    ASSERT(s == STATUS_INVALID_INFO_CLASS);
    PASS();
}

static void test_set_process_priority_class(void){
    setup_proc();
    struct nt_eprocess *cur = kshim_current_process();
    PT_TEST_PRIORITY_CLASS pc; NTSTATUS s; ULONG ret = 0;
    PT_TEST_PRIORITY_CLASS qpc; PROCESS_BASIC_INFORMATION pbi;
    /* PROCESS_PRIORITY_CLASS is exactly two bytes */
    pc.Foreground = 0; pc.PriorityClass = PT_TEST_PRIO_HIGH;
    s = nt_svc_NtSetInformationProcess(NtCurrentProcess(), 18, &pc, 4);
    ASSERT(s == STATUS_INFO_LENGTH_MISMATCH);
    s = nt_svc_NtSetInformationProcess(NtCurrentProcess(), 18, NULL, 2);
    ASSERT(s == STATUS_INVALID_PARAMETER);
    /* Unknown classes are invalid parameters */
    pc.PriorityClass = 0; s = nt_svc_NtSetInformationProcess(NtCurrentProcess(), 18, &pc, 2);
    ASSERT(s == STATUS_INVALID_PARAMETER);
    pc.PriorityClass = 7; s = nt_svc_NtSetInformationProcess(NtCurrentProcess(), 18, &pc, 2);
    ASSERT(s == STATUS_INVALID_PARAMETER);
    /* Query before any set reports NORMAL */
    s = nt_svc_NtQueryInformationProcess(NtCurrentProcess(), 18, &qpc, 2, &ret);
    ASSERT(s == STATUS_SUCCESS && qpc.PriorityClass == PT_TEST_PRIO_NORMAL && ret == 2);
    /* A valid HIGH set maps to base priority 13 and is reported back */
    pc.Foreground = 1; pc.PriorityClass = PT_TEST_PRIO_HIGH;
    s = nt_svc_NtSetInformationProcess(NtCurrentProcess(), 18, &pc, 2);
    ASSERT(s == STATUS_SUCCESS);
    s = nt_svc_NtQueryInformationProcess(NtCurrentProcess(), 18, &qpc, 2, &ret);
    ASSERT(s == STATUS_SUCCESS && qpc.PriorityClass == PT_TEST_PRIO_HIGH && qpc.Foreground == 1);
    s = nt_svc_NtQueryInformationProcess(NtCurrentProcess(), 0, &pbi, 48, &ret);
    ASSERT(s == STATUS_SUCCESS && pbi.BasePriority == 13);
    /* REALTIME needs SeIncreaseBasePriority: refused without the privilege,
     * and a refused set changes neither the class nor the base priority */
    pc.Foreground = 0; pc.PriorityClass = PT_TEST_PRIO_REALTIME;
    s = nt_svc_NtSetInformationProcess(NtCurrentProcess(), 18, &pc, 2);
    ASSERT(s == STATUS_PRIVILEGE_NOT_HELD);
    s = nt_svc_NtQueryInformationProcess(NtCurrentProcess(), 18, &qpc, 2, &ret);
    ASSERT(s == STATUS_SUCCESS && qpc.PriorityClass == PT_TEST_PRIO_HIGH);
    s = nt_svc_NtQueryInformationProcess(NtCurrentProcess(), 0, &pbi, 48, &ret);
    ASSERT(s == STATUS_SUCCESS && pbi.BasePriority == 13);
    /* With the privilege REALTIME is accepted */
    pt_test_set_token(cur, pt_test_make_token(SE_INC_BASE_PRIORITY_PRIVILEGE, TRUE));
    s = nt_svc_NtSetInformationProcess(NtCurrentProcess(), 18, &pc, 2);
    ASSERT(s == STATUS_SUCCESS);
    s = nt_svc_NtQueryInformationProcess(NtCurrentProcess(), 18, &qpc, 2, &ret);
    ASSERT(s == STATUS_SUCCESS && qpc.PriorityClass == PT_TEST_PRIO_REALTIME);
    s = nt_svc_NtQueryInformationProcess(NtCurrentProcess(), 0, &pbi, 48, &ret);
    ASSERT(s == STATUS_SUCCESS && pbi.BasePriority == 24);
    /* A handle without PROCESS_SET_INFORMATION is refused */
    HANDLE qh = NULL;
    s = open_query_only_process(&qh);
    ASSERT(NT_SUCCESS(s));
    pc.PriorityClass = PT_TEST_PRIO_NORMAL;
    s = nt_svc_NtSetInformationProcess(qh, 18, &pc, 2);
    ASSERT(s == STATUS_ACCESS_DENIED);
    ObCloseHandle(cur, qh);
    PASS();
}

/* ---- NtSetInformationProcess: affinity, hard error mode, break on
 * termination, refused classes ---- */
static void test_set_process_affinity(void){
    setup_proc();
    ULONG_PTR mask; NTSTATUS s; ULONG ret = 0;
    PROCESS_BASIC_INFORMATION pbi;
    /* ProcessAffinityMask takes exactly one ULONG_PTR */
    mask = 0x3;
    s = nt_svc_NtSetInformationProcess(NtCurrentProcess(), 21, &mask, 4);
    ASSERT(s == STATUS_INFO_LENGTH_MISMATCH);
    s = nt_svc_NtSetInformationProcess(NtCurrentProcess(), 21, NULL, sizeof(ULONG_PTR));
    ASSERT(s == STATUS_INVALID_PARAMETER);
    /* Empty mask, and a bit outside the system mask (0xFF), are invalid */
    mask = 0;
    s = nt_svc_NtSetInformationProcess(NtCurrentProcess(), 21, &mask, sizeof(ULONG_PTR));
    ASSERT(s == STATUS_INVALID_PARAMETER);
    mask = 0x100;
    s = nt_svc_NtSetInformationProcess(NtCurrentProcess(), 21, &mask, sizeof(ULONG_PTR));
    ASSERT(s == STATUS_INVALID_PARAMETER);
    /* A rejected value leaves the stored mask alone (new process: 0xFF) */
    ULONG_PTR qmask = 0;
    s = nt_svc_NtQueryInformationProcess(NtCurrentProcess(), 21, &qmask, sizeof(ULONG_PTR), &ret);
    ASSERT(s == STATUS_SUCCESS && qmask == 0xFF && ret == sizeof(ULONG_PTR));
    /* A valid subset is stored and reported by both queries */
    mask = 0x3;
    s = nt_svc_NtSetInformationProcess(NtCurrentProcess(), 21, &mask, sizeof(ULONG_PTR));
    ASSERT(s == STATUS_SUCCESS);
    s = nt_svc_NtQueryInformationProcess(NtCurrentProcess(), 21, &qmask, sizeof(ULONG_PTR), &ret);
    ASSERT(s == STATUS_SUCCESS && qmask == 0x3);
    s = nt_svc_NtQueryInformationProcess(NtCurrentProcess(), 0, &pbi, 48, &ret);
    ASSERT(s == STATUS_SUCCESS && pbi.AffinityMask == 0x3);
    /* Query length must match exactly */
    s = nt_svc_NtQueryInformationProcess(NtCurrentProcess(), 21, &qmask, 4, &ret);
    ASSERT(s == STATUS_INFO_LENGTH_MISMATCH);
    /* A handle without PROCESS_SET_INFORMATION is refused */
    HANDLE qh = NULL;
    s = open_query_only_process(&qh);
    ASSERT(NT_SUCCESS(s));
    mask = 0x1;
    s = nt_svc_NtSetInformationProcess(qh, 21, &mask, sizeof(ULONG_PTR));
    ASSERT(s == STATUS_ACCESS_DENIED);
    ObCloseHandle(kshim_current_process(), qh);
    PASS();
}

static void test_set_process_hard_error_mode(void){
    setup_proc();
    ULONG mode; NTSTATUS s; ULONG ret = 0; ULONG q = 0xFFFF;
    /* ProcessDefaultHardErrorMode takes exactly one ULONG */
    mode = 1;
    s = nt_svc_NtSetInformationProcess(NtCurrentProcess(), 12, &mode, 2);
    ASSERT(s == STATUS_INFO_LENGTH_MISMATCH);
    s = nt_svc_NtSetInformationProcess(NtCurrentProcess(), 12, NULL, 4);
    ASSERT(s == STATUS_INVALID_PARAMETER);
    /* New process reports 0 */
    s = nt_svc_NtQueryInformationProcess(NtCurrentProcess(), 12, &q, 4, &ret);
    ASSERT(s == STATUS_SUCCESS && q == 0 && ret == 4);
    /* Stored flags are reported back unchanged */
    mode = 0x8003;   /* SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX */
    s = nt_svc_NtSetInformationProcess(NtCurrentProcess(), 12, &mode, 4);
    ASSERT(s == STATUS_SUCCESS);
    s = nt_svc_NtQueryInformationProcess(NtCurrentProcess(), 12, &q, 4, &ret);
    ASSERT(s == STATUS_SUCCESS && q == 0x8003);
    /* A query-only handle is refused and the stored value is kept */
    HANDLE qh = NULL;
    s = open_query_only_process(&qh);
    ASSERT(NT_SUCCESS(s));
    mode = 0;
    s = nt_svc_NtSetInformationProcess(qh, 12, &mode, 4);
    ASSERT(s == STATUS_ACCESS_DENIED);
    s = nt_svc_NtQueryInformationProcess(NtCurrentProcess(), 12, &q, 4, &ret);
    ASSERT(s == STATUS_SUCCESS && q == 0x8003);
    ObCloseHandle(kshim_current_process(), qh);
    PASS();
}

static void test_set_process_break_on_termination(void){
    setup_proc();
    struct nt_eprocess *cur = kshim_current_process();
    ULONG flag; NTSTATUS s; ULONG ret = 0; ULONG q = 0xFFFF;
    /* ProcessBreakOnTermination takes exactly one ULONG */
    flag = 1;
    s = nt_svc_NtSetInformationProcess(NtCurrentProcess(), 29, &flag, 2);
    ASSERT(s == STATUS_INFO_LENGTH_MISMATCH);
    /* Without SeDebugPrivilege the set is refused and nothing is stored */
    s = nt_svc_NtSetInformationProcess(NtCurrentProcess(), 29, &flag, 4);
    ASSERT(s == STATUS_PRIVILEGE_NOT_HELD);
    s = nt_svc_NtQueryInformationProcess(NtCurrentProcess(), 29, &q, 4, &ret);
    ASSERT(s == STATUS_SUCCESS && q == 0);
    /* With the privilege a nonzero value is stored as 1, and zero clears it */
    pt_test_set_token(cur, pt_test_make_token(SE_DEBUG_PRIVILEGE, TRUE));
    flag = 0x55;
    s = nt_svc_NtSetInformationProcess(NtCurrentProcess(), 29, &flag, 4);
    ASSERT(s == STATUS_SUCCESS);
    s = nt_svc_NtQueryInformationProcess(NtCurrentProcess(), 29, &q, 4, &ret);
    ASSERT(s == STATUS_SUCCESS && q == 1);
    flag = 0;
    s = nt_svc_NtSetInformationProcess(NtCurrentProcess(), 29, &flag, 4);
    ASSERT(s == STATUS_SUCCESS);
    s = nt_svc_NtQueryInformationProcess(NtCurrentProcess(), 29, &q, 4, &ret);
    ASSERT(s == STATUS_SUCCESS && q == 0);
    PASS();
}

/* Classes that are refused: handle tracing (STATUS_NOT_SUPPORTED); the
 * foreground and Wx86 classes and unknown numbers (STATUS_INVALID_INFO_CLASS). */
static void test_set_process_refused_classes(void){
    setup_proc();
    ULONG buf[2] = {0, 0};
    ASSERT(nt_svc_NtSetInformationProcess(NtCurrentProcess(), 32, buf, 4) == STATUS_NOT_SUPPORTED);
    ASSERT(nt_svc_NtSetInformationProcess(NtCurrentProcess(), 25, buf, 2) == STATUS_INVALID_INFO_CLASS);
    ASSERT(nt_svc_NtSetInformationProcess(NtCurrentProcess(), 19, buf, 4) == STATUS_INVALID_INFO_CLASS);
    ASSERT(nt_svc_NtSetInformationProcess(NtCurrentProcess(), 9999, buf, 4) == STATUS_INVALID_INFO_CLASS);
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
    RUN(test_thread_exit_frees_sync_thread);
    RUN(test_query_returnlength_discipline);
    RUN(test_set_thread_priority);
    RUN(test_set_thread_affinity);
    RUN(test_alert_resume_thread);
    RUN(test_alert_satisfies_alertable_wait);
    RUN(test_wait_for_alert_by_thread_id);
    RUN(test_continue_and_raise);
    RUN(test_process_terminate_twice);
    RUN(test_open_bad_cid);
    RUN(test_set_process_base_priority);
    RUN(test_set_process_priority_class);
    RUN(test_set_process_affinity);
    RUN(test_set_process_hard_error_mode);
    RUN(test_set_process_break_on_termination);
    RUN(test_set_process_refused_classes);
    printf("proc_thread tests: %d passed, %d failed\n", g_pass, g_fail);
    long leaks=atomic_load(&kshim_alloc_count);
    printf("leaks %ld\n", leaks);
    ObCleanupObjectManager();
    long after=atomic_load(&kshim_alloc_count);
    if(after!=0) { fprintf(stderr,"LEAK %ld\n",after); kshim_leak_report(); return 1; }
    return g_fail?1:0;
}
