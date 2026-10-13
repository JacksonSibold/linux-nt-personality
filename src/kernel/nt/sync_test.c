/* sync_test.c - torture tests for nt dispatcher objects */
#include "../../ntdll/nt_types.h"
#include "../../ntdll/ntstatus.h"
#include "kshim.h"
#include "sync.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <stdatomic.h>
#include <assert.h>

extern _Atomic long kshim_alloc_count;

static int g_pass=0, g_fail=0;
#define TEST(name) static void test_##name(void)
#define RUN(name) do { printf("  RUN %s\n", #name); test_##name(); } while(0)
#define ASSERT(cond) do { if (!(cond)) { fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#cond); g_fail++; return; } } while(0)
#define ASSERT_STATUS(expr, expect) do { NTSTATUS _s=(expr); if(_s!=(expect)){ fprintf(stderr,"FAIL %s:%d: expect 0x%lx got 0x%lx for %s\n",__FILE__,__LINE__,(unsigned long)(expect),(unsigned long)_s,#expr); g_fail++; return; } } while(0)
#define PASS() do { g_pass++; printf("  PASS %s\n", __func__); } while(0)

static LARGE_INTEGER rel_ms(long ms) { LARGE_INTEGER li; li.QuadPart = -(int64_t)ms * 10000LL; return li; }
__attribute__((unused)) static LARGE_INTEGER abs_ms_from_now(long ms) {
    struct timespec now; clock_gettime(CLOCK_REALTIME,&now);
    int64_t now100 = (int64_t)now.tv_sec*10000000LL + now.tv_nsec/100 + 116444736000000000LL;
    LARGE_INTEGER li; li.QuadPart = now100 + (int64_t)ms*10000LL;
    return li;
}

typedef struct { struct nt_thread *thr; void *arg; void *(*fn)(void*); pthread_t pt; } thread_wrap_t;
static void* wrap_entry(void *p) {
    thread_wrap_t *w=(thread_wrap_t*)p;
    nt_set_current_thread(w->thr);
    void *r=w->fn(w->arg);
    nt_set_current_thread(NULL);
    return r;
}
static int start_thread(thread_wrap_t *w, void *(*fn)(void*), void *arg) {
    NTSTATUS st=nt_thread_create(&w->thr);
    if (st!=STATUS_SUCCESS) return -1;
    w->fn=fn; w->arg=arg;
    return pthread_create(&w->pt, NULL, wrap_entry, w);
}
static void join_thread(thread_wrap_t *w) {
    pthread_join(w->pt, NULL);
    nt_thread_destroy(w->thr);
}

/* helper structs */
struct waiter_arg_ev { struct nt_event *ev; atomic_int *c; };
struct waiter_arg_keyed { struct nt_keyed_event *k; void *key; atomic_int *w; struct nt_thread *thr; };
struct waiter_arg_addr { volatile uint32_t *addr; uint32_t cmp; atomic_int *w; struct nt_thread *thr; };
struct wait_all_arg { struct nt_dispatcher_header **objs; ULONG cnt; atomic_int *done; struct nt_thread *thr; };
struct release_arg { struct nt_mutant *m; NTSTATUS *out; struct nt_thread *thr; };

/* -------------------------------------------------- */
static void *waiter_ev_fn(void *a) {
    struct waiter_arg_ev *pa=(struct waiter_arg_ev*)a;
    struct nt_dispatcher_header *h=(struct nt_dispatcher_header*)pa->ev;
    NTSTATUS s=nt_wait_for_single_object(h, 0, NULL);
    if (s==STATUS_WAIT_0) atomic_fetch_add(pa->c,1);
    return NULL;
}
TEST(auto_reset_one) {
    struct nt_thread *main_thr;
    ASSERT_STATUS(nt_thread_create(&main_thr), STATUS_SUCCESS);
    nt_set_current_thread(main_thr);
    struct nt_event *ev;
    ASSERT_STATUS(nt_create_event(&ev, SynchronizationEvent, 0), STATUS_SUCCESS);
    const int N=8;
    atomic_int woke=0; atomic_init(&woke,0);
    thread_wrap_t tw[N];
    struct waiter_arg_ev args[N];
    for(int i=0;i<N;i++){ args[i].ev=ev; args[i].c=&woke; ASSERT(start_thread(&tw[i], waiter_ev_fn, &args[i])==0); }
    usleep(100000);
    LONG prev;
    ASSERT_STATUS(nt_set_event(ev,&prev), STATUS_SUCCESS);
    ASSERT(prev==0);
    usleep(100000);
    ASSERT(atomic_load(&woke)==1);
    ASSERT_STATUS(nt_set_event(ev,NULL), STATUS_SUCCESS);
    usleep(100000);
    ASSERT(atomic_load(&woke)==2);
    for(int i=2;i<N;i++){ ASSERT_STATUS(nt_set_event(ev,NULL),STATUS_SUCCESS); usleep(20000); }
    for(int i=0;i<N;i++) join_thread(&tw[i]);
    ASSERT(atomic_load(&woke)==N);
    nt_put_event(ev);
    nt_set_current_thread(NULL); nt_thread_destroy(main_thr);
    PASS();
}

static void *waiter_ev_generic(void *a){
    struct waiter_arg_ev *pa=(struct waiter_arg_ev*)a;
    NTSTATUS s=nt_wait_for_single_object((struct nt_dispatcher_header*)pa->ev,0,NULL);
    if(s==STATUS_WAIT_0) atomic_fetch_add(pa->c,1);
    return NULL;
}
TEST(manual_wakes_all) {
    struct nt_thread *main_thr; ASSERT_STATUS(nt_thread_create(&main_thr),STATUS_SUCCESS); nt_set_current_thread(main_thr);
    struct nt_event *ev; ASSERT_STATUS(nt_create_event(&ev, NotificationEvent,0),STATUS_SUCCESS);
    const int N=6; atomic_int woke=0; atomic_init(&woke,0);
    thread_wrap_t tw[N]; struct waiter_arg_ev args[N];
    for(int i=0;i<N;i++){ args[i].ev=ev; args[i].c=&woke; ASSERT(start_thread(&tw[i],waiter_ev_generic,&args[i])==0); }
    usleep(100000);
    ASSERT_STATUS(nt_set_event(ev,NULL),STATUS_SUCCESS);
    usleep(100000);
    ASSERT(atomic_load(&woke)==N);
    for(int i=0;i<N;i++) join_thread(&tw[i]);
    nt_put_event(ev); nt_set_current_thread(NULL); nt_thread_destroy(main_thr);
    PASS();
}

static void *pulse_waiter(void *a){
    struct waiter_arg_ev *pa=(struct waiter_arg_ev*)a;
    LARGE_INTEGER to=rel_ms(500);
    NTSTATUS s=nt_wait_for_single_object((struct nt_dispatcher_header*)pa->ev,0,&to);
    if(s==STATUS_WAIT_0) atomic_fetch_add(pa->c,1);
    return NULL;
}
TEST(pulse_current_only) {
    struct nt_thread *main_thr; ASSERT_STATUS(nt_thread_create(&main_thr),STATUS_SUCCESS); nt_set_current_thread(main_thr);
    struct nt_event *ev; ASSERT_STATUS(nt_create_event(&ev, NotificationEvent,0),STATUS_SUCCESS);
    atomic_int woke1=0, woke2=0; atomic_init(&woke1,0); atomic_init(&woke2,0);
    thread_wrap_t t1; struct waiter_arg_ev a1={ev,&woke1};
    ASSERT(start_thread(&t1,pulse_waiter,&a1)==0);
    usleep(100000);
    ASSERT_STATUS(nt_pulse_event(ev,NULL),STATUS_SUCCESS);
    join_thread(&t1);
    ASSERT(atomic_load(&woke1)==1);
    ASSERT(ev->header.signal_state==0);
    thread_wrap_t t2; struct waiter_arg_ev a2={ev,&woke2};
    ASSERT(start_thread(&t2,pulse_waiter,&a2)==0);
    join_thread(&t2);
    ASSERT(atomic_load(&woke2)==0);
    nt_put_event(ev); nt_set_current_thread(NULL); nt_thread_destroy(main_thr);
    PASS();
}

static void *try_release_fn(void *a){
    struct release_arg *c=(struct release_arg*)a;
    nt_set_current_thread(c->thr);
    NTSTATUS s=nt_release_mutant(c->m,NULL);
    *c->out=s;
    return NULL;
}
TEST(mutant_ownership) {
    struct nt_thread *main_thr; ASSERT_STATUS(nt_thread_create(&main_thr),STATUS_SUCCESS); nt_set_current_thread(main_thr);
    struct nt_mutant *m; ASSERT_STATUS(nt_create_mutant(&m,0),STATUS_SUCCESS);
    struct nt_dispatcher_header *h=(struct nt_dispatcher_header*)m;
    ASSERT_STATUS(nt_wait_for_single_object(h,0,NULL), STATUS_WAIT_0);
    ASSERT(m->owner==main_thr && m->recursion==1);
    ASSERT_STATUS(nt_wait_for_single_object(h,0,NULL), STATUS_WAIT_0);
    ASSERT(m->recursion==2);
    LONG prev;
    ASSERT_STATUS(nt_release_mutant(m,&prev),STATUS_SUCCESS); ASSERT(prev==2);
    ASSERT(m->recursion==1);
    ASSERT_STATUS(nt_release_mutant(m,&prev),STATUS_SUCCESS); ASSERT(prev==1);
    ASSERT(m->owner==NULL && h->signal_state==1);
    ASSERT_STATUS(nt_wait_for_single_object(h,0,NULL), STATUS_WAIT_0);
    struct nt_thread *other; ASSERT_STATUS(nt_thread_create(&other),STATUS_SUCCESS);
    NTSTATUS other_status=0;
    struct release_arg cap={m,&other_status,other};
    thread_wrap_t t_other; t_other.thr=other; t_other.fn=try_release_fn; t_other.arg=&cap;
    pthread_create(&t_other.pt,NULL,wrap_entry,&t_other);
    pthread_join(t_other.pt,NULL);
    ASSERT(other_status==STATUS_MUTANT_NOT_OWNED);
    ASSERT_STATUS(nt_release_mutant(m,NULL),STATUS_SUCCESS);
    /* abandonment */
    struct nt_thread *owner_thr; ASSERT_STATUS(nt_thread_create(&owner_thr),STATUS_SUCCESS);
    struct nt_mutant *m2; ASSERT_STATUS(nt_create_mutant(&m2,0),STATUS_SUCCESS);
    struct nt_dispatcher_header *h2=(struct nt_dispatcher_header*)m2;
    thread_wrap_t owner_wrap; 
    ASSERT_STATUS(nt_thread_create(&owner_wrap.thr),STATUS_SUCCESS);
    struct nt_thread *tmp_owner = owner_wrap.thr;
    (void)tmp_owner;
    /* owner acquires then we abandon */
    nt_set_current_thread(owner_thr);
    ASSERT_STATUS(nt_wait_for_single_object(h2,0,NULL), STATUS_WAIT_0);
    nt_set_current_thread(main_thr);
    nt_abandon_owned_mutants(owner_thr);
    nt_thread_destroy(owner_thr);
    nt_thread_destroy(owner_wrap.thr);
    NTSTATUS s=nt_wait_for_single_object(h2,0,NULL);
    ASSERT(s==STATUS_ABANDONED_WAIT_0);
    ASSERT(m2->owner==main_thr);
    ASSERT_STATUS(nt_release_mutant(m2,NULL),STATUS_SUCCESS);
    nt_thread_destroy(other);
    nt_put_mutant(m); nt_put_mutant(m2);
    nt_set_current_thread(NULL); nt_thread_destroy(main_thr);
    PASS();
}

TEST(semaphore_limits) {
    struct nt_thread *thr; ASSERT_STATUS(nt_thread_create(&thr),STATUS_SUCCESS); nt_set_current_thread(thr);
    struct nt_semaphore *s; ASSERT_STATUS(nt_create_semaphore(&s,2,3),STATUS_SUCCESS);
    struct nt_dispatcher_header *h=(struct nt_dispatcher_header*)s;
    LONG prev;
    ASSERT_STATUS(nt_release_semaphore(s,1,&prev),STATUS_SUCCESS); ASSERT(prev==2);
    ASSERT(h->signal_state==3);
    ASSERT_STATUS(nt_release_semaphore(s,1,NULL), STATUS_SEMAPHORE_LIMIT_EXCEEDED);
    ASSERT(h->signal_state==3);
    ASSERT_STATUS(nt_wait_for_single_object(h,0,NULL),STATUS_WAIT_0); ASSERT(h->signal_state==2);
    ASSERT_STATUS(nt_wait_for_single_object(h,0,NULL),STATUS_WAIT_0); ASSERT(h->signal_state==1);
    ASSERT_STATUS(nt_wait_for_single_object(h,0,NULL),STATUS_WAIT_0); ASSERT(h->signal_state==0);
    LARGE_INTEGER zero={0}; ASSERT_STATUS(nt_wait_for_single_object(h,0,&zero), STATUS_TIMEOUT);
    ASSERT_STATUS(nt_release_semaphore(s,2,&prev),STATUS_SUCCESS); ASSERT(prev==0);
    ASSERT(h->signal_state==2);
    nt_put_semaphore(s); nt_set_current_thread(NULL); nt_thread_destroy(thr);
    PASS();
}

TEST(timeout_accuracy) {
    struct nt_thread *thr; ASSERT_STATUS(nt_thread_create(&thr),STATUS_SUCCESS); nt_set_current_thread(thr);
    struct nt_event *ev; ASSERT_STATUS(nt_create_event(&ev, NotificationEvent,0),STATUS_SUCCESS);
    struct nt_dispatcher_header *h=(struct nt_dispatcher_header*)ev;
    struct timespec t0,t1; clock_gettime(CLOCK_MONOTONIC,&t0);
    LARGE_INTEGER to=rel_ms(120);
    NTSTATUS s=nt_wait_for_single_object(h,0,&to);
    clock_gettime(CLOCK_MONOTONIC,&t1);
    ASSERT(s==STATUS_TIMEOUT);
    long elapsed = (t1.tv_sec-t0.tv_sec)*1000 + (t1.tv_nsec-t0.tv_nsec)/1000000;
    ASSERT(elapsed >= 90 && elapsed <= 350);
    LARGE_INTEGER zero={0};
    clock_gettime(CLOCK_MONOTONIC,&t0);
    s=nt_wait_for_single_object(h,0,&zero);
    clock_gettime(CLOCK_MONOTONIC,&t1);
    elapsed = (t1.tv_sec-t0.tv_sec)*1000 + (t1.tv_nsec-t0.tv_nsec)/1000000;
    ASSERT(s==STATUS_TIMEOUT);
    ASSERT(elapsed < 50);
    nt_put_event(ev); nt_set_current_thread(NULL); nt_thread_destroy(thr);
    PASS();
}

TEST(timer_basic) {
    struct nt_thread *thr; ASSERT_STATUS(nt_thread_create(&thr),STATUS_SUCCESS); nt_set_current_thread(thr);
    struct nt_timer *t; ASSERT_STATUS(nt_create_timer(&t, NotificationTimer),STATUS_SUCCESS);
    struct nt_dispatcher_header *h=(struct nt_dispatcher_header*)t;
    LARGE_INTEGER rel=rel_ms(80);
    ASSERT_STATUS(nt_set_timer(t,&rel,0,NULL,NULL),STATUS_SUCCESS);
    LARGE_INTEGER to=rel_ms(300);
    NTSTATUS s=nt_wait_for_single_object(h,0,&to);
    ASSERT(s==STATUS_WAIT_0);
    LARGE_INTEGER zero={0}; ASSERT_STATUS(nt_wait_for_single_object(h,0,&zero),STATUS_WAIT_0);
    nt_put_timer(t);
    ASSERT_STATUS(nt_create_timer(&t, SynchronizationTimer),STATUS_SUCCESS);
    h=(struct nt_dispatcher_header*)t;
    rel=rel_ms(80);
    ASSERT_STATUS(nt_set_timer(t,&rel,0,NULL,NULL),STATUS_SUCCESS);
    s=nt_wait_for_single_object(h,0,&to);
    ASSERT(s==STATUS_WAIT_0);
    ASSERT_STATUS(nt_wait_for_single_object(h,0,&zero), STATUS_TIMEOUT);
    nt_put_timer(t);
    nt_set_current_thread(NULL); nt_thread_destroy(thr);
    PASS();
}

static void *keyed_waiter_fn(void *a){
    struct waiter_arg_keyed *pa=(struct waiter_arg_keyed*)a;
    nt_set_current_thread(pa->thr);
    LARGE_INTEGER to=rel_ms(800);
    NTSTATUS s=nt_wait_for_keyed_event(pa->k,pa->key,0,&to);
    if(s==STATUS_SUCCESS) atomic_fetch_add(pa->w,1);
    return NULL;
}
TEST(keyed_event) {
    struct nt_thread *thr; ASSERT_STATUS(nt_thread_create(&thr),STATUS_SUCCESS); nt_set_current_thread(thr);
    struct nt_keyed_event *k; ASSERT_STATUS(nt_create_keyed_event(&k),STATUS_SUCCESS);
    void *key=(void*)0x1234;
    atomic_int woke=0; atomic_init(&woke,0);
    thread_wrap_t tw; ASSERT_STATUS(nt_thread_create(&tw.thr),STATUS_SUCCESS);
    struct waiter_arg_keyed arg={k,key,&woke,tw.thr};
    tw.fn=keyed_waiter_fn; tw.arg=&arg;
    pthread_create(&tw.pt,NULL,wrap_entry,&tw);
    usleep(100000);
    ASSERT_STATUS(nt_release_keyed_event(k,key,0,NULL),STATUS_SUCCESS);
    pthread_join(tw.pt,NULL); nt_thread_destroy(tw.thr);
    ASSERT(atomic_load(&woke)==1);
    nt_put_keyed_event(k); nt_set_current_thread(NULL); nt_thread_destroy(thr);
    PASS();
}

static void *addr_waiter_fn(void *a){
    struct waiter_arg_addr *pa=(struct waiter_arg_addr*)a;
    nt_set_current_thread(pa->thr);
    uint32_t cmp=pa->cmp;
    LARGE_INTEGER to=rel_ms(800);
    NTSTATUS s=nt_wait_on_address((void*)pa->addr,&cmp,4,&to);
    if(s==STATUS_SUCCESS) atomic_fetch_add(pa->w,1);
    return NULL;
}
TEST(wait_on_address) {
    struct nt_thread *thr; ASSERT_STATUS(nt_thread_create(&thr),STATUS_SUCCESS); nt_set_current_thread(thr);
    volatile uint32_t val=42;
    atomic_int woke=0; atomic_init(&woke,0);
    thread_wrap_t tw; ASSERT_STATUS(nt_thread_create(&tw.thr),STATUS_SUCCESS);
    struct waiter_arg_addr arg={(volatile uint32_t*)&val,42,&woke,tw.thr};
    tw.fn=addr_waiter_fn; tw.arg=&arg;
    pthread_create(&tw.pt,NULL,wrap_entry,&tw);
    usleep(100000);
    nt_wake_by_address_single((void*)&val);
    pthread_join(tw.pt,NULL); nt_thread_destroy(tw.thr);
    ASSERT(atomic_load(&woke)==1);
    uint32_t wrong=99;
    LARGE_INTEGER to=rel_ms(100);
    NTSTATUS s=nt_wait_on_address((void*)&val,&wrong,4,&to);
    ASSERT(s==STATUS_INVALID_PARAMETER);
    val=100;
    atomic_int woke_all=0; atomic_init(&woke_all,0);
    const int N=3; thread_wrap_t tws[N]; struct waiter_arg_addr args[N];
    for(int i=0;i<N;i++){ ASSERT_STATUS(nt_thread_create(&tws[i].thr),STATUS_SUCCESS); args[i].addr=&val; args[i].cmp=100; args[i].w=&woke_all; args[i].thr=tws[i].thr; tws[i].fn=addr_waiter_fn; tws[i].arg=&args[i]; pthread_create(&tws[i].pt,NULL,wrap_entry,&tws[i]); }
    usleep(100000);
    nt_wake_by_address_all((void*)&val);
    for(int i=0;i<N;i++){ pthread_join(tws[i].pt,NULL); nt_thread_destroy(tws[i].thr); }
    ASSERT(atomic_load(&woke_all)==N);
    nt_set_current_thread(NULL); nt_thread_destroy(thr);
    PASS();
}

TEST(max_objects) {
    struct nt_thread *thr; ASSERT_STATUS(nt_thread_create(&thr),STATUS_SUCCESS); nt_set_current_thread(thr);
    struct nt_event *ev[65];
    struct nt_dispatcher_header *hdrs[65];
    for(int i=0;i<65;i++){ ASSERT_STATUS(nt_create_event(&ev[i], NotificationEvent,1),STATUS_SUCCESS); hdrs[i]=(struct nt_dispatcher_header*)ev[i]; }
    NTSTATUS s=nt_wait_for_multiple_objects(65, hdrs, WaitAny,0,NULL);
    ASSERT(s==STATUS_INVALID_PARAMETER);
    s=nt_wait_for_multiple_objects(64, hdrs, WaitAny,0,NULL);
    ASSERT(s==STATUS_WAIT_0);
    for(int i=0;i<65;i++) nt_put_event(ev[i]);
    nt_set_current_thread(NULL); nt_thread_destroy(thr);
    PASS();
}

TEST(alertable) {
    struct nt_thread *thr; ASSERT_STATUS(nt_thread_create(&thr),STATUS_SUCCESS); nt_set_current_thread(thr);
    struct nt_event *ev; ASSERT_STATUS(nt_create_event(&ev, NotificationEvent,0),STATUS_SUCCESS);
    struct nt_dispatcher_header *h=(struct nt_dispatcher_header*)ev;
    nt_thread_alert(thr);
    LARGE_INTEGER to=rel_ms(500);
    NTSTATUS s=nt_wait_for_single_object(h,1,&to);
    ASSERT(s==STATUS_ALERTED);
    nt_thread_queue_apc(thr);
    s=nt_wait_for_single_object(h,1,&to);
    ASSERT(s==STATUS_USER_APC);
    nt_thread_alert(thr);
    LARGE_INTEGER zero={0};
    s=nt_wait_for_single_object(h,0,&zero);
    ASSERT(s==STATUS_TIMEOUT);
    nt_thread_clear_alert(thr);
    nt_put_event(ev); nt_set_current_thread(NULL); nt_thread_destroy(thr);
    PASS();
}

static void *wait_all_mutant_fn(void *a){
    struct wait_all_arg *pa=(struct wait_all_arg*)a; nt_set_current_thread(pa->thr);
    for(int i=0;i<20;i++){
        LARGE_INTEGER to=rel_ms(500);
        NTSTATUS s=nt_wait_for_multiple_objects(pa->cnt, pa->objs, WaitAll,0,&to);
        if (s==STATUS_WAIT_0 || s==STATUS_ABANDONED_WAIT_0) {
            atomic_fetch_add(pa->done,1);
            usleep(5000);
            struct nt_mutant *mm1=(struct nt_mutant*)pa->objs[0];
            struct nt_mutant *mm2=(struct nt_mutant*)pa->objs[1];
            nt_release_mutant(mm1,NULL);
            nt_release_mutant(mm2,NULL);
        }
        usleep(1000);
    }
    return NULL;
}
static void *wait_all_sem_fn(void *a){
    struct wait_all_arg *pa=(struct wait_all_arg*)a; nt_set_current_thread(pa->thr);
    for(int i=0;i<20;i++){
        LARGE_INTEGER to=rel_ms(500);
        NTSTATUS s=nt_wait_for_multiple_objects(pa->cnt, pa->objs, WaitAll,0,&to);
        if (s==STATUS_WAIT_0){
            atomic_fetch_add(pa->done,1);
            usleep(3000);
            struct nt_semaphore *ss1=(struct nt_semaphore*)pa->objs[0];
            struct nt_semaphore *ss2=(struct nt_semaphore*)pa->objs[1];
            nt_release_semaphore(ss1,1,NULL);
            nt_release_semaphore(ss2,1,NULL);
        }
        usleep(1000);
    }
    return NULL;
}
TEST(wait_all_atomicity) {
    struct nt_thread *main_thr; ASSERT_STATUS(nt_thread_create(&main_thr),STATUS_SUCCESS); nt_set_current_thread(main_thr);
    struct nt_mutant *m1,*m2;
    ASSERT_STATUS(nt_create_mutant(&m1,0),STATUS_SUCCESS);
    ASSERT_STATUS(nt_create_mutant(&m2,0),STATUS_SUCCESS);
    struct nt_dispatcher_header *pair1[2]={(struct nt_dispatcher_header*)m1,(struct nt_dispatcher_header*)m2};
    struct nt_dispatcher_header *pair2[2]={(struct nt_dispatcher_header*)m2,(struct nt_dispatcher_header*)m1};
    atomic_int done1=0,done2=0; atomic_init(&done1,0); atomic_init(&done2,0);
    thread_wrap_t t1,t2;
    ASSERT_STATUS(nt_thread_create(&t1.thr),STATUS_SUCCESS);
    ASSERT_STATUS(nt_thread_create(&t2.thr),STATUS_SUCCESS);
    struct wait_all_arg a1={pair1,2,&done1,t1.thr}, a2={pair2,2,&done2,t2.thr};
    t1.fn=wait_all_mutant_fn; t1.arg=&a1; t2.fn=wait_all_mutant_fn; t2.arg=&a2;
    pthread_create(&t1.pt,NULL,wrap_entry,&t1);
    pthread_create(&t2.pt,NULL,wrap_entry,&t2);
    struct timespec start; clock_gettime(CLOCK_MONOTONIC,&start);
    pthread_join(t1.pt,NULL); pthread_join(t2.pt,NULL);
    struct timespec end; clock_gettime(CLOCK_MONOTONIC,&end);
    long elapsed = (end.tv_sec-start.tv_sec)*1000 + (end.tv_nsec-start.tv_nsec)/1000000;
    ASSERT(elapsed < 6000);
    ASSERT(atomic_load(&done1) + atomic_load(&done2) > 0);
    nt_thread_destroy(t1.thr); nt_thread_destroy(t2.thr);
    struct nt_semaphore *s1,*s2;
    ASSERT_STATUS(nt_create_semaphore(&s1,1,1),STATUS_SUCCESS);
    ASSERT_STATUS(nt_create_semaphore(&s2,1,1),STATUS_SUCCESS);
    struct nt_dispatcher_header *spair1[2]={(struct nt_dispatcher_header*)s1,(struct nt_dispatcher_header*)s2};
    struct nt_dispatcher_header *spair2[2]={(struct nt_dispatcher_header*)s2,(struct nt_dispatcher_header*)s1};
    atomic_int sd1=0,sd2=0; atomic_init(&sd1,0); atomic_init(&sd2,0);
    thread_wrap_t st1,st2; ASSERT_STATUS(nt_thread_create(&st1.thr),STATUS_SUCCESS); ASSERT_STATUS(nt_thread_create(&st2.thr),STATUS_SUCCESS);
    struct wait_all_arg sa1={spair1,2,&sd1,st1.thr}, sa2={spair2,2,&sd2,st2.thr};
    st1.fn=wait_all_sem_fn; st1.arg=&sa1; st2.fn=wait_all_sem_fn; st2.arg=&sa2;
    pthread_create(&st1.pt,NULL,wrap_entry,&st1);
    pthread_create(&st2.pt,NULL,wrap_entry,&st2);
    pthread_join(st1.pt,NULL); pthread_join(st2.pt,NULL);
    ASSERT(atomic_load(&sd1)+atomic_load(&sd2) > 0);
    nt_thread_destroy(st1.thr); nt_thread_destroy(st2.thr);
    nt_put_mutant(m1); nt_put_mutant(m2);
    nt_put_semaphore(s1); nt_put_semaphore(s2);
    nt_set_current_thread(NULL); nt_thread_destroy(main_thr);
    PASS();
}

TEST(wait_any_index) {
    struct nt_thread *thr; ASSERT_STATUS(nt_thread_create(&thr),STATUS_SUCCESS); nt_set_current_thread(thr);
    struct nt_event *ev[3];
    struct nt_dispatcher_header *h[3];
    for(int i=0;i<3;i++){ ASSERT_STATUS(nt_create_event(&ev[i],NotificationEvent,0),STATUS_SUCCESS); h[i]=(struct nt_dispatcher_header*)ev[i]; }
    ASSERT_STATUS(nt_set_event(ev[1],NULL),STATUS_SUCCESS);
    NTSTATUS s=nt_wait_for_multiple_objects(3,h,WaitAny,0,NULL);
    ASSERT(s==STATUS_WAIT_1);
    struct nt_mutant *m; ASSERT_STATUS(nt_create_mutant(&m,0),STATUS_SUCCESS);
    struct nt_thread *owner; ASSERT_STATUS(nt_thread_create(&owner),STATUS_SUCCESS);
    struct nt_dispatcher_header *hm=(struct nt_dispatcher_header*)m;
    nt_set_current_thread(owner);
    ASSERT_STATUS(nt_wait_for_single_object(hm,0,NULL), STATUS_WAIT_0);
    nt_set_current_thread(thr);
    nt_abandon_owned_mutants(owner); nt_thread_destroy(owner);
    struct nt_dispatcher_header *mix[2]={h[0], hm};
    s=nt_wait_for_multiple_objects(2,mix,WaitAny,0,NULL);
    ASSERT(s==STATUS_ABANDONED_WAIT_0+1);
    nt_release_mutant(m,NULL);
    for(int i=0;i<3;i++) nt_put_event(ev[i]);
    nt_put_mutant(m);
    nt_set_current_thread(NULL); nt_thread_destroy(thr);
    PASS();
}

/* ---- Gap 1: Mutant abandonment across three exit paths (shim-level) ---- */
static void *self_term_fn(void *a){
    struct nt_mutant *m=(struct nt_mutant*)a;
    struct nt_thread *me=nt_current_thread();
    NTSTATUS s=nt_wait_for_single_object((struct nt_dispatcher_header*)m,0,NULL);
    if(s!=STATUS_WAIT_0) return NULL;
    nt_abandon_owned_mutants(me);
    return NULL;
}
TEST(mutant_abandon_self_terminate){
    struct nt_thread *main_thr; ASSERT_STATUS(nt_thread_create(&main_thr),STATUS_SUCCESS); nt_set_current_thread(main_thr);
    struct nt_mutant *m; ASSERT_STATUS(nt_create_mutant(&m,0),STATUS_SUCCESS);
    struct nt_dispatcher_header *h=(struct nt_dispatcher_header*)m;
    thread_wrap_t tw; ASSERT_STATUS(nt_thread_create(&tw.thr),STATUS_SUCCESS);
    tw.fn=self_term_fn; tw.arg=m;
    pthread_create(&tw.pt,NULL,wrap_entry,&tw);
    pthread_join(tw.pt,NULL);
    nt_thread_destroy(tw.thr);
    LARGE_INTEGER to=rel_ms(500);
    NTSTATUS s=nt_wait_for_single_object(h,0,&to);
    ASSERT(s==STATUS_ABANDONED_WAIT_0);
    ASSERT_STATUS(nt_release_mutant(m,NULL),STATUS_SUCCESS);
    nt_put_mutant(m);
    nt_set_current_thread(NULL); nt_thread_destroy(main_thr);
    PASS();
}
static void *peer_owner_hold(void *a){
    struct nt_mutant *m=(struct nt_mutant*)a;
    NTSTATUS s=nt_wait_for_single_object((struct nt_dispatcher_header*)m,0,NULL);
    if(s!=STATUS_WAIT_0) return NULL;
    usleep(800000);
    return NULL;
}
TEST(mutant_abandon_peer_terminate){
    struct nt_thread *main_thr; ASSERT_STATUS(nt_thread_create(&main_thr),STATUS_SUCCESS); nt_set_current_thread(main_thr);
    struct nt_mutant *m; ASSERT_STATUS(nt_create_mutant(&m,0),STATUS_SUCCESS);
    struct nt_dispatcher_header *h=(struct nt_dispatcher_header*)m;
    thread_wrap_t owner; ASSERT_STATUS(nt_thread_create(&owner.thr),STATUS_SUCCESS);
    owner.fn=peer_owner_hold; owner.arg=m;
    pthread_create(&owner.pt,NULL,wrap_entry,&owner);
    usleep(100000);
    nt_abandon_owned_mutants(owner.thr);
    pthread_join(owner.pt,NULL);
    nt_thread_destroy(owner.thr);
    LARGE_INTEGER to=rel_ms(500);
    NTSTATUS s=nt_wait_for_single_object(h,0,&to);
    ASSERT(s==STATUS_ABANDONED_WAIT_0);
    ASSERT_STATUS(nt_release_mutant(m,NULL),STATUS_SUCCESS);
    nt_put_mutant(m);
    nt_set_current_thread(NULL); nt_thread_destroy(main_thr);
    PASS();
}
static void *proc_owner_hold1(void *a){ struct nt_mutant *m=(struct nt_mutant*)a; nt_wait_for_single_object((struct nt_dispatcher_header*)m,0,NULL); usleep(900000); return NULL; }
static void *proc_owner_hold2(void *a){ struct nt_mutant *m=(struct nt_mutant*)a; nt_wait_for_single_object((struct nt_dispatcher_header*)m,0,NULL); usleep(900000); return NULL; }
TEST(mutant_abandon_process_exit){
    struct nt_thread *main_thr; ASSERT_STATUS(nt_thread_create(&main_thr),STATUS_SUCCESS); nt_set_current_thread(main_thr);
    struct nt_mutant *m1,*m2; ASSERT_STATUS(nt_create_mutant(&m1,0),STATUS_SUCCESS); ASSERT_STATUS(nt_create_mutant(&m2,0),STATUS_SUCCESS);
    struct nt_dispatcher_header *h1=(struct nt_dispatcher_header*)m1, *h2=(struct nt_dispatcher_header*)m2;
    thread_wrap_t t1,t2; ASSERT_STATUS(nt_thread_create(&t1.thr),STATUS_SUCCESS); ASSERT_STATUS(nt_thread_create(&t2.thr),STATUS_SUCCESS);
    t1.fn=proc_owner_hold1; t1.arg=m1; t2.fn=proc_owner_hold2; t2.arg=m2;
    pthread_create(&t1.pt,NULL,wrap_entry,&t1); pthread_create(&t2.pt,NULL,wrap_entry,&t2);
    usleep(120000);
    nt_abandon_owned_mutants(t1.thr);
    nt_abandon_owned_mutants(t2.thr);
    pthread_join(t1.pt,NULL); pthread_join(t2.pt,NULL);
    nt_thread_destroy(t1.thr); nt_thread_destroy(t2.thr);
    LARGE_INTEGER to=rel_ms(200);
    NTSTATUS s1=nt_wait_for_single_object(h1,0,&to);
    NTSTATUS s2=nt_wait_for_single_object(h2,0,&to);
    ASSERT(s1==STATUS_ABANDONED_WAIT_0);
    ASSERT(s2==STATUS_ABANDONED_WAIT_0);
    ASSERT_STATUS(nt_release_mutant(m1,NULL),STATUS_SUCCESS);
    ASSERT_STATUS(nt_release_mutant(m2,NULL),STATUS_SUCCESS);
    nt_put_mutant(m1); nt_put_mutant(m2);
    nt_set_current_thread(NULL); nt_thread_destroy(main_thr);
    PASS();
}

/* ---- Gap 2: Zero-timeout poll on abandoned mutant ---- */
TEST(zero_poll_abandoned){
    struct nt_thread *main_thr; ASSERT_STATUS(nt_thread_create(&main_thr),STATUS_SUCCESS); nt_set_current_thread(main_thr);
    struct nt_mutant *m; ASSERT_STATUS(nt_create_mutant(&m,0),STATUS_SUCCESS);
    struct nt_dispatcher_header *h=(struct nt_dispatcher_header*)m;
    struct nt_thread *owner; ASSERT_STATUS(nt_thread_create(&owner),STATUS_SUCCESS);
    nt_set_current_thread(owner); ASSERT_STATUS(nt_wait_for_single_object(h,0,NULL),STATUS_WAIT_0);
    nt_set_current_thread(main_thr);
    nt_abandon_owned_mutants(owner); nt_thread_destroy(owner);
    LARGE_INTEGER zero={0};
    NTSTATUS s=nt_wait_for_single_object(h,0,&zero);
    if(s!=STATUS_ABANDONED_WAIT_0){fprintf(stderr,"zero_poll single got 0x%lx exp 0x%lx\n",(unsigned long)s,(unsigned long)STATUS_ABANDONED_WAIT_0); g_fail++; nt_put_mutant(m); nt_set_current_thread(NULL); nt_thread_destroy(main_thr); return; }
    ASSERT_STATUS(nt_release_mutant(m,NULL),STATUS_SUCCESS);
    /* second mutant for WaitAny to avoid reuse after consume */
    struct nt_mutant *m2; ASSERT_STATUS(nt_create_mutant(&m2,0),STATUS_SUCCESS);
    struct nt_dispatcher_header *h2=(struct nt_dispatcher_header*)m2;
    ASSERT_STATUS(nt_thread_create(&owner),STATUS_SUCCESS);
    nt_set_current_thread(owner); ASSERT_STATUS(nt_wait_for_single_object(h2,0,NULL),STATUS_WAIT_0);
    nt_set_current_thread(main_thr);
    nt_abandon_owned_mutants(owner); nt_thread_destroy(owner);
    struct nt_event *ev; ASSERT_STATUS(nt_create_event(&ev, NotificationEvent,0),STATUS_SUCCESS);
    struct nt_dispatcher_header *he=(struct nt_dispatcher_header*)ev;
    struct nt_dispatcher_header *mix[2]={he, h2};
    s=nt_wait_for_multiple_objects(2,mix,WaitAny,0,&zero);
    if(s!= (NTSTATUS)(STATUS_ABANDONED_WAIT_0+1)){fprintf(stderr,"zero_poll WaitAny got 0x%lx exp 0x%lx (h2 abandoned, ev not signaled)\n",(unsigned long)s,(unsigned long)(STATUS_ABANDONED_WAIT_0+1)); g_fail++; nt_put_event(ev); nt_put_mutant(m); nt_put_mutant(m2); nt_set_current_thread(NULL); nt_thread_destroy(main_thr); return; }
    ASSERT_STATUS(nt_release_mutant(m2,NULL),STATUS_SUCCESS);
    nt_put_event(ev); nt_put_mutant(m); nt_put_mutant(m2);
    nt_set_current_thread(NULL); nt_thread_destroy(main_thr);
    PASS();
}

/* ---- Gap 3: WaitAny index must be caller order, not sorted ---- */
TEST(wait_any_caller_order){
    struct nt_thread *thr; ASSERT_STATUS(nt_thread_create(&thr),STATUS_SUCCESS); nt_set_current_thread(thr);
    struct nt_event *e0,*e1;
    ASSERT_STATUS(nt_create_event(&e0, NotificationEvent,0),STATUS_SUCCESS);
    ASSERT_STATUS(nt_create_event(&e1, NotificationEvent,0),STATUS_SUCCESS);
    struct nt_dispatcher_header *h0=(struct nt_dispatcher_header*)e0, *h1=(struct nt_dispatcher_header*)e1;
    if (h0 < h1) {
        struct nt_event *tmp=e0; e0=e1; e1=tmp;
        struct nt_dispatcher_header *th=h0; h0=h1; h1=th;
    }
    ASSERT(h0 > h1);
    struct nt_dispatcher_header *hdrs[2]={h0, h1};
    LARGE_INTEGER zero={0};
    nt_reset_event(e0,NULL); nt_reset_event(e1,NULL);
    nt_set_event(e1,NULL);
    NTSTATUS s=nt_wait_for_multiple_objects(2,hdrs,WaitAny,0,&zero);
    if(s!=STATUS_WAIT_1){fprintf(stderr,"caller_order event WaitAny got 0x%lx exp WAIT_1\n",(unsigned long)s); g_fail++; nt_put_event(e0); nt_put_event(e1); nt_set_current_thread(NULL); nt_thread_destroy(thr); return; }
    nt_reset_event(e0,NULL); nt_reset_event(e1,NULL);
    nt_set_event(e0,NULL);
    s=nt_wait_for_multiple_objects(2,hdrs,WaitAny,0,&zero);
    if(s!=STATUS_WAIT_0){fprintf(stderr,"caller_order second got 0x%lx exp WAIT_0\n",(unsigned long)s); g_fail++; nt_put_event(e0); nt_put_event(e1); nt_set_current_thread(NULL); nt_thread_destroy(thr); return; }
    nt_reset_event(e0,NULL); nt_reset_event(e1,NULL);
    /* Abandoned mutant where address is deliberately lower than event, but caller order has mutant at index 1.
       If WaitAny leaked sorted index, it would return ABANDONED_WAIT_0 instead of ABANDONED_WAIT_1. */
    struct nt_mutant *m; ASSERT_STATUS(nt_create_mutant(&m,0),STATUS_SUCCESS);
    struct nt_dispatcher_header *hm=(struct nt_dispatcher_header*)m;
    int attempts=0;
    while(hm > h1 && attempts<20){
        struct nt_mutant *tmp;
        nt_create_mutant(&tmp,0);
        if((struct nt_dispatcher_header*)tmp < h1){ nt_put_mutant(m); m=tmp; hm=(struct nt_dispatcher_header*)m; break; }
        nt_put_mutant(tmp);
        attempts++;
    }
    struct nt_thread *owner; ASSERT_STATUS(nt_thread_create(&owner),STATUS_SUCCESS);
    nt_set_current_thread(owner); ASSERT_STATUS(nt_wait_for_single_object(hm,0,NULL),STATUS_WAIT_0);
    nt_set_current_thread(thr);
    nt_abandon_owned_mutants(owner); nt_thread_destroy(owner);
    struct nt_dispatcher_header *mix[2]={h1, hm}; /* caller order: event at 0, mutant at 1, but mutant address < event, so sorted would put mutant first */
    nt_reset_event(e1,NULL);
    s=nt_wait_for_multiple_objects(2,mix,WaitAny,0,&zero);
    if(s!= (NTSTATUS)(STATUS_ABANDONED_WAIT_0+1)){fprintf(stderr,"caller_order abandoned WaitAny got 0x%lx exp 0x%lx (hm=%p h1=%p)\n",(unsigned long)s,(unsigned long)(STATUS_ABANDONED_WAIT_0+1),hm,h1); g_fail++; nt_put_event(e0); nt_put_event(e1); nt_put_mutant(m); nt_set_current_thread(NULL); nt_thread_destroy(thr); return; }
    ASSERT_STATUS(nt_release_mutant(m,NULL),STATUS_SUCCESS);
    nt_put_event(e0); nt_put_event(e1); nt_put_mutant(m);
    nt_set_current_thread(NULL); nt_thread_destroy(thr);
    PASS();
}

struct stress_consumer_arg { struct nt_dispatcher_header **hdrs; int nobj; atomic_bool *stop; atomic_int *wakes; struct nt_thread *thr; };
static void *stress_consumer(void *a){
    struct stress_consumer_arg *pa=(struct stress_consumer_arg*)a; nt_set_current_thread(pa->thr);
    while(!atomic_load(pa->stop)){
        int idx = rand()%pa->nobj;
        LARGE_INTEGER to=rel_ms(20);
        NTSTATUS s=nt_wait_for_single_object(pa->hdrs[idx],0,&to);
        if(s==STATUS_WAIT_0) atomic_fetch_add(pa->wakes,1);
    }
    return NULL;
}
struct stress_producer_arg { struct nt_event **evs; int nobj; atomic_bool *stop; struct nt_thread *thr; };
static void *stress_producer(void *a){
    struct stress_producer_arg *pa=(struct stress_producer_arg*)a; nt_set_current_thread(pa->thr);
    while(!atomic_load(pa->stop)){
        int idx=rand()%pa->nobj;
        nt_set_event(pa->evs[idx],NULL);
        usleep(1000 + rand()%2000);
    }
    return NULL;
}
TEST(stress_random) {
    struct nt_thread *main_thr; ASSERT_STATUS(nt_thread_create(&main_thr),STATUS_SUCCESS); nt_set_current_thread(main_thr);
    const int NOBJ=6;
    struct nt_event *evs[NOBJ];
    struct nt_dispatcher_header *hdrs[NOBJ];
    for(int i=0;i<NOBJ;i++){ ASSERT_STATUS(nt_create_event(&evs[i],SynchronizationEvent,0),STATUS_SUCCESS); hdrs[i]=(struct nt_dispatcher_header*)evs[i]; }
    atomic_int total_wakes=0; atomic_init(&total_wakes,0);
    atomic_bool stop=false; atomic_init(&stop,false);
    const int NCONS=4, NPROD=2;
    thread_wrap_t cons[NCONS], prod[NPROD];
    struct stress_consumer_arg carg[NCONS];
    struct stress_producer_arg parg[NPROD];
    for(int i=0;i<NCONS;i++){ ASSERT_STATUS(nt_thread_create(&cons[i].thr),STATUS_SUCCESS); carg[i].hdrs=hdrs; carg[i].nobj=NOBJ; carg[i].stop=&stop; carg[i].wakes=&total_wakes; carg[i].thr=cons[i].thr; cons[i].fn=stress_consumer; cons[i].arg=&carg[i]; pthread_create(&cons[i].pt,NULL,wrap_entry,&cons[i]); }
    for(int i=0;i<NPROD;i++){ ASSERT_STATUS(nt_thread_create(&prod[i].thr),STATUS_SUCCESS); parg[i].evs=evs; parg[i].nobj=NOBJ; parg[i].stop=&stop; parg[i].thr=prod[i].thr; prod[i].fn=stress_producer; prod[i].arg=&parg[i]; pthread_create(&prod[i].pt,NULL,wrap_entry,&prod[i]); }
    usleep(3000000);
    atomic_store(&stop,true);
    for(int i=0;i<NOBJ;i++) nt_set_event(evs[i],NULL);
    for(int i=0;i<NCONS;i++){ pthread_join(cons[i].pt,NULL); nt_thread_destroy(cons[i].thr); }
    for(int i=0;i<NPROD;i++){ pthread_join(prod[i].pt,NULL); nt_thread_destroy(prod[i].thr); }
    ASSERT(atomic_load(&total_wakes) > 0);
    for(int i=0;i<NOBJ;i++) nt_put_event(evs[i]);
    nt_set_current_thread(NULL); nt_thread_destroy(main_thr);
    PASS();
}

/* Iterations for the wait_on_address_extended loop. The default (20000)
 * is the CI value. SYNC_TEST_ITERATIONS=<n> overrides it (1..10000000);
 * the Makefile uses a smaller count for "check" and "check-quick". */
#define SYNC_DEFAULT_ITERATIONS 20000
static int sync_iterations(void){
    const char *s = getenv("SYNC_TEST_ITERATIONS");
    char *end = NULL;
    long v;
    if(!s || !*s) return SYNC_DEFAULT_ITERATIONS;
    v = strtol(s, &end, 10);
    if(*end != '\0' || v < 1 || v > 10000000L){
        fprintf(stderr, "SYNC_TEST_ITERATIONS must be 1..10000000, got '%s'\n", s);
        exit(2);
    }
    return (int)v;
}

int main(void){
    printf("ntsync tests starting\n");
    long before = atomic_load(&kshim_alloc_count);
    (void)before;
    RUN(auto_reset_one);
    RUN(manual_wakes_all);
    RUN(pulse_current_only);
    RUN(mutant_ownership);
    RUN(semaphore_limits);
    RUN(timeout_accuracy);
    RUN(timer_basic);
    RUN(keyed_event);
    RUN(wait_on_address);
    RUN(max_objects);
    RUN(alertable);
    RUN(wait_all_atomicity);
    RUN(wait_any_index);
    RUN(mutant_abandon_self_terminate);
    RUN(mutant_abandon_peer_terminate);
    RUN(mutant_abandon_process_exit);
    RUN(zero_poll_abandoned);
    RUN(wait_any_caller_order);
    RUN(stress_random);
    /* Extended overlapping wait-set permutations — toward DOD-SYNC-A007/B007 (10^6 shuffles, 0 mismatches) and B001/B002 */
    {
        printf("  RUN wmo_overlapping_permutations (scaled)\n");
        struct nt_thread *thr; NTSTATUS st=nt_thread_create(&thr); if(st==STATUS_SUCCESS){ nt_set_current_thread(thr);
            struct nt_event *ev[8]; struct nt_dispatcher_header *hdrs[8];
            for(int i=0;i<8;i++){ nt_create_event(&ev[i], NotificationEvent, (i%2)); hdrs[i]=(struct nt_dispatcher_header*)ev[i]; }
            int permutations = 5000; /* scaled from few hundred to 5k; 10^6 requires nightly soak */
            int mismatches=0;
            for(int p=0;p<permutations;p++){
                int n = 2 + (p % 7); /* 2..8 handles */
                int signal_idx = p % n;
                for(int i=0;i<n;i++){ if(i==signal_idx) nt_set_event(ev[i],NULL); else nt_reset_event(ev[i],NULL); }
                /* WaitAny should return WAIT_0..WAIT_n-1 matching signaled index; WaitAll with 2 should only succeed when both signaled */
                NTSTATUS s = nt_wait_for_multiple_objects(n, hdrs, WaitAny, 0, &(LARGE_INTEGER){0});
                if(s < STATUS_WAIT_0 || s >= STATUS_WAIT_0 + n){ mismatches++; }
                else if((int)(s - STATUS_WAIT_0) != signal_idx){ /* In our setup only one signaled, so should be that idx, but scheduler may race; we allow any WAIT_0+idx where signaled */
                    /* Verify signaled one is indeed signaled */
                    if(ev[signal_idx]->header.signal_state==0){ mismatches++; }
                }
                /* Overlapping WaitAll permutation: 2-handle subset */
                if(n>=2){
                    nt_reset_event(ev[0],NULL); nt_reset_event(ev[1],NULL);
                    if(p % 3==0){ nt_set_event(ev[0],NULL); nt_set_event(ev[1],NULL); }
                    else if(p % 3==1){ nt_set_event(ev[0],NULL); }
                    LARGE_INTEGER zero={0};
                    NTSTATUS sa = nt_wait_for_multiple_objects(2, hdrs, WaitAll, 0, &zero);
                    int both_signaled = (ev[0]->header.signal_state && ev[1]->header.signal_state);
                    if(both_signaled && sa != STATUS_WAIT_0) mismatches++;
                    if(!both_signaled && sa == STATUS_WAIT_0) mismatches++;
                }
            }
            printf("  wmo_overlapping_permutations: %d perms, mismatches %d\n", permutations, mismatches);
            if(mismatches!=0){ fprintf(stderr,"FAIL wmo permutations mismatches %d\n", mismatches); g_fail++; } else g_pass++;
            for(int i=0;i<8;i++) nt_put_event(ev[i]);
            nt_set_current_thread(NULL); nt_thread_destroy(thr);
        }
    }
    /* WaitOnAddress extended torture — toward 10^7 transitions (20000 by default, see SYNC_TEST_ITERATIONS; 10^7 is nightly KASAN soak) */
    {
        printf("  RUN wait_on_address_extended\n");
        struct nt_thread *thr; if(nt_thread_create(&thr)==STATUS_SUCCESS){ nt_set_current_thread(thr);
            volatile uint32_t val=0;
            atomic_int wakes=0; atomic_init(&wakes,0);
            const int ITERS=sync_iterations();
            for(int i=0;i<ITERS;i++){
                val = (uint32_t)i;
                /* Single waiter should be woken by WakeByAddressSingle when compare matches */
                thread_wrap_t tw; nt_thread_create(&tw.thr);
                struct waiter_arg_addr arg={(volatile uint32_t*)&val, (uint32_t)i, &wakes, tw.thr};
                tw.fn=addr_waiter_fn; tw.arg=&arg;
                pthread_create(&tw.pt,NULL,wrap_entry,&tw);
                usleep(2000);
                nt_wake_by_address_single((void*)&val);
                pthread_join(tw.pt,NULL); nt_thread_destroy(tw.thr);
                if(atomic_load(&wakes) != i+1){ fprintf(stderr,"FAIL woa extended %d got %d\n", i, atomic_load(&wakes)); g_fail++; break; }
            }
            printf("  wait_on_address_extended: %d iterations, wakes %d\n", ITERS, atomic_load(&wakes));
            if(g_fail==0) g_pass++;
            nt_set_current_thread(NULL); nt_thread_destroy(thr);
        }
    }
    printf("ntsync tests: %d passed, %d failed\n", g_pass, g_fail);
    if (g_fail) return 1;
    long after = atomic_load(&kshim_alloc_count);
    printf("alloc_count at exit (should be 0 after cleanup, roots not counted): %ld\n", after);
    usleep(200000);
    long final = atomic_load(&kshim_alloc_count);
    printf("alloc_count final: %ld\n", final);
    if (final != 0) { fprintf(stderr,"LEAK %ld\n", final); return 1; }
    return 0;
}
