/* thread_exit_test.c - thread exit status through NtQueryInformationThread.
 *
 * A thread that is blocked on a gate it cannot leave reports STILL_ACTIVE (259),
 * never 0. After the gate opens and the thread is joined it reports the value its
 * start routine returned (777). A suspended thread reports 259 until it is
 * resumed and finished. A closed thread handle must not answer for a later thread.
 *
 * The gate is a condition variable, so every blocked-state check is deterministic:
 * the thread cannot exit before the test opens the gate. Built and run by
 * tests/kernel-host/Makefile like the other module tests.
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
#include <pthread.h>

NTSTATUS nt_svc_NtWaitForSingleObject(HANDLE,BOOLEAN,PLARGE_INTEGER);

static int g_pass=0,g_fail=0;
#define CHECK(cond) do{ if(!(cond)){ fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#cond); g_fail++; } }while(0)
#define PASS() do{ g_pass++; printf("  PASS %s\n",__func__); }while(0)
#define RUN(fn) do{ printf("RUN %s\n",#fn); fn(); }while(0)

struct gate { pthread_mutex_t m; pthread_cond_t c; int open; };

static void gate_init(struct gate *g){
    pthread_mutex_init(&g->m,NULL);
    pthread_cond_init(&g->c,NULL);
    g->open=0;
}
static void gate_open(struct gate *g){
    pthread_mutex_lock(&g->m);
    g->open=1;
    pthread_cond_broadcast(&g->c);
    pthread_mutex_unlock(&g->m);
}
static void gate_wait(struct gate *g){
    pthread_mutex_lock(&g->m);
    while(!g->open) pthread_cond_wait(&g->c,&g->m);
    pthread_mutex_unlock(&g->m);
}

static ULONG thr_gated(PVOID a){ gate_wait((struct gate*)a); return 777; }
static ULONG thr_quick(PVOID a){ (void)a; return 777; }

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

static LONG query_exit(HANDLE th, NTSTATUS *st_out){
    THREAD_BASIC_INFORMATION tbi; ULONG ret=0;
    memset(&tbi,0,sizeof(tbi));
    NTSTATUS q=nt_svc_NtQueryInformationThread(th,0,&tbi,48,&ret);
    if(st_out) *st_out=q;
    return tbi.ExitStatus;
}

/* A thread blocked on the gate is STILL_ACTIVE, and after the gate opens and it
 * is joined it reports the start routine's return value. */
static void test_blocked_thread_status(void){
    setup_proc();
    struct nt_eprocess *proc=kshim_current_process();
    struct gate g; gate_init(&g);
    HANDLE th=NULL;
    NTSTATUS st=nt_svc_NtCreateThreadEx(&th,0,NULL,NtCurrentProcess(),thr_gated,&g,0,0,0,0,NULL);
    CHECK(st==STATUS_SUCCESS);
    if(st!=STATUS_SUCCESS){ gate_open(&g); return; }

    NTSTATUS q1=0, q2=0;
    LONG blocked1=query_exit(th,&q1);
    LONG blocked2=query_exit(th,&q2);
    CHECK(q1==STATUS_SUCCESS);
    CHECK(blocked1==259);
    CHECK(blocked2==259);

    gate_open(&g);
    NTSTATUS w=nt_svc_NtWaitForSingleObject(th,0,NULL);
    CHECK(w==STATUS_SUCCESS);
    NTSTATUS q3=0;
    LONG joined=query_exit(th,&q3);
    CHECK(q3==STATUS_SUCCESS);
    CHECK(joined==777);

    ObCloseHandle(proc,th);
    if(g_fail==0) PASS();
}

/* A suspended thread has not run its routine, so it reports 259; after resume and
 * join it reports 777. */
static void test_suspended_thread_status(void){
    setup_proc();
    struct nt_eprocess *proc=kshim_current_process();
    HANDLE th=NULL;
    NTSTATUS st=nt_svc_NtCreateThreadEx(&th,0,NULL,NtCurrentProcess(),thr_quick,NULL,CREATE_SUSPENDED_FLAG,0,0,0,NULL);
    CHECK(st==STATUS_SUCCESS);
    if(st!=STATUS_SUCCESS) return;

    NTSTATUS q1=0;
    LONG before=query_exit(th,&q1);
    CHECK(q1==STATUS_SUCCESS);
    CHECK(before==259);

    ULONG prev=0;
    CHECK(nt_svc_NtResumeThread(th,&prev)==STATUS_SUCCESS);
    CHECK(prev==1);
    CHECK(nt_svc_NtWaitForSingleObject(th,0,NULL)==STATUS_SUCCESS);
    NTSTATUS q2=0;
    LONG after=query_exit(th,&q2);
    CHECK(q2==STATUS_SUCCESS);
    CHECK(after==777);

    ObCloseHandle(proc,th);
    if(g_fail==0) PASS();
}

/* After a thread's handle is closed, a new thread must not inherit its answer:
 * the new thread is blocked, so its query is 259, not the closed thread's 777. */
static void test_closed_thread_does_not_answer_for_new_thread(void){
    setup_proc();
    struct nt_eprocess *proc=kshim_current_process();

    HANDLE th1=NULL;
    NTSTATUS s1=nt_svc_NtCreateThreadEx(&th1,0,NULL,NtCurrentProcess(),thr_quick,NULL,0,0,0,0,NULL);
    CHECK(s1==STATUS_SUCCESS);
    if(s1!=STATUS_SUCCESS) return;
    CHECK(nt_svc_NtWaitForSingleObject(th1,0,NULL)==STATUS_SUCCESS);
    CHECK(query_exit(th1,NULL)==777);
    ObCloseHandle(proc,th1);

    struct gate g; gate_init(&g);
    HANDLE th2=NULL;
    NTSTATUS s2=nt_svc_NtCreateThreadEx(&th2,0,NULL,NtCurrentProcess(),thr_gated,&g,0,0,0,0,NULL);
    CHECK(s2==STATUS_SUCCESS);
    if(s2!=STATUS_SUCCESS){ gate_open(&g); return; }
    CHECK(query_exit(th2,NULL)==259);

    gate_open(&g);
    CHECK(nt_svc_NtWaitForSingleObject(th2,0,NULL)==STATUS_SUCCESS);
    CHECK(query_exit(th2,NULL)==777);
    ObCloseHandle(proc,th2);
    if(g_fail==0) PASS();
}

int main(void){
    printf("thread exit status tests\n");
    RUN(test_blocked_thread_status);
    RUN(test_suspended_thread_status);
    RUN(test_closed_thread_does_not_answer_for_new_thread);
    printf("thread exit status tests: %d passed, %d failed\n", g_pass, g_fail);
    ObCleanupObjectManager();
    return g_fail?1:0;
}
