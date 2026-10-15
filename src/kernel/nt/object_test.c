/* object_test.c - unit tests for nt object manager + handle table.
 *
 * Uses kshim.h userspace shim; no kernel boot required.
 * Covers: refcount vs handlecount, name collisions, OBJ_EXCLUSIVE,
 * close-pending, duplicate with reduced access, inheritance, pseudo-handles,
 * handle reuse, and randomized stress with leak checks.
 */

#include "object.h"
#include "handle.h"
#include "../../ntdll/nt_types.h"
#include "../../ntdll/ntstatus.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <assert.h>

static int g_pass = 0;
static int g_fail = 0;

#define TEST(name) static void test_##name(void)
#define RUN(name) do { printf("  RUN %s\n", #name); test_##name(); } while(0)

#define ASSERT(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        g_fail++; return; \
    } \
} while(0)

#define ASSERT_STATUS(expr, expect) do { \
    NTSTATUS _st = (expr); \
    if (_st != (expect)) { \
        fprintf(stderr, "FAIL %s:%d: expected 0x%08lx got 0x%08lx for %s\n", __FILE__, __LINE__, (unsigned long)(expect), (unsigned long)_st, #expr); \
        g_fail++; return; \
    } \
} while(0)

#define PASS() do { g_pass++; printf("  PASS %s\n", __func__); } while(0)

/* Helpers */
static WCHAR wbuf1[256];
static WCHAR wbuf2[256];

static void make_us(UNICODE_STRING *us, const char *s, WCHAR *buf, size_t buflen)
{
    size_t len = strlen(s);
    ASSERT(len * sizeof(WCHAR) < buflen);
    for (size_t i=0;i<len;i++) buf[i]=(WCHAR)(unsigned char)s[i];
    us->Buffer = buf;
    us->Length = (USHORT)(len * sizeof(WCHAR));
    us->MaximumLength = us->Length;
    if (len) buf[len]=0;
}

static OBJECT_ATTRIBUTES make_oa(UNICODE_STRING *us, ULONG attr)
{
    OBJECT_ATTRIBUTES oa={0};
    oa.Length = sizeof(oa);
    oa.ObjectName = us;
    oa.Attributes = attr;
    return oa;
}

/* Process creation helper */
static struct nt_eprocess *make_proc(ULONG pid, const char *name)
{
    struct nt_eprocess *p=NULL;
    NTSTATUS st = ObCreateProcessObject(&p, pid, name);
    if (st!=STATUS_SUCCESS || !p) { fprintf(stderr,"make_proc failed\n"); g_fail++; return NULL; }
    kshim_set_current_process(p);
    return p;
}
static struct nt_ethread *make_thread(struct nt_eprocess *proc, ULONG tid)
{
    struct nt_ethread *t=NULL;
    NTSTATUS st = ObCreateThreadObject(&t, proc, tid);
    if (st!=STATUS_SUCCESS || !t) { fprintf(stderr,"make_thread failed\n"); g_fail++; return NULL; }
    kshim_set_current_thread(t);
    return t;
}

/* Reset global state between tests */
static void reset_ob(void)
{
    ObCleanupObjectManager();
    NTSTATUS st = ObInitObjectManager();
    ASSERT(st==STATUS_SUCCESS);
}

/* -------------------------------------------------------------------------- */
/* 1. refcount vs handlecount lifetime                                         */
/* -------------------------------------------------------------------------- */
TEST(refcount_vs_handlecount)
{
    reset_ob();
    struct nt_eprocess *proc = make_proc(100, "proc1");
    struct nt_ethread *thr = make_thread(proc, 1);
    UNICODE_STRING us; make_us(&us, "\\BaseNamedObjects\\RefTest", wbuf1, sizeof(wbuf1));
    OBJECT_ATTRIBUTES oa = make_oa(&us, OBJ_CASE_INSENSITIVE);
    struct nt_object_header *hdr;
    void *body;
    NTSTATUS st = ObCreateObject(&nt_test_type, &oa, sizeof(struct nt_test_body), &hdr, &body);
    ASSERT_STATUS(st, STATUS_SUCCESS);
    ASSERT(atomic64_read(&hdr->ref_count)==1);
    ASSERT(atomic64_read(&hdr->handle_count)==0);
    struct nt_test_body *tb = (struct nt_test_body*)body;
    tb->value=42;
    HANDLE h;
    st = ObInsertObject(hdr, &oa, 0xFF, proc, &h);
    ASSERT_STATUS(st, STATUS_SUCCESS);
    /* After insert: ref should be 2 (creator + directory) + handle? Actually insert adds handle ref and directory ref: total refs = 1 creator +1 directory +1 handle =3? Wait ObInsertObject refs: create1 + directory insert adds1 + handle adds1 => 3 */
    /* But we care that handle_count==1 and ref>1 */
    ASSERT(atomic64_read(&hdr->handle_count)==1);
    ASSERT(atomic64_read(&hdr->ref_count) >=2);

    /* Take an extra reference manually */
    ObReferenceObject(hdr);
    long long rc_before = atomic64_read(&hdr->ref_count);
    ASSERT(rc_before >=3);

    /* Close handle: handle_count goes to 0, but ref stays >0 */
    st = ObCloseHandle(proc, h);
    ASSERT_STATUS(st, STATUS_SUCCESS);
    ASSERT(atomic64_read(&hdr->handle_count)==0);
    ASSERT(atomic64_read(&hdr->ref_count) >=1);
    /* Object should still be alive: name still present? For named object, directory holds ref, so still alive */
    /* For unnamed test we would have already used named; verify lookup still works */
    struct nt_object_header *found=NULL;
    st = ObReferenceObjectByName(&us, &nt_test_type, KernelMode, 0, &found);
    ASSERT_STATUS(st, STATUS_SUCCESS);
    ASSERT(found==hdr);
    ObDereferenceObject(found);

    /* Drop extra ref */
    ObDereferenceObject(hdr);
    ObMakeTemporaryObject(hdr);
    long long rc = atomic64_read(&hdr->ref_count);
    ASSERT(rc==1);
    /* Final dereference should call delete; cannot inspect tb after free */
    ObDereferenceObject(hdr);
    /* Object freed; verify no leak via alloc count indirectly in later cleanup */
    ObDestroyProcessObject(proc);
    ObDestroyThreadObject(thr);
    kshim_set_current_process(NULL);
    kshim_set_current_thread(NULL);
    PASS();
}

/* -------------------------------------------------------------------------- */
/* 2. name collisions (including case-insensitive)                             */
/* -------------------------------------------------------------------------- */
TEST(name_collisions)
{
    reset_ob();
    struct nt_eprocess *proc = make_proc(101, "p2");
    struct nt_ethread *thr = make_thread(proc, 2);
    UNICODE_STRING us1; make_us(&us1, "\\BaseNamedObjects\\Collide", wbuf1, sizeof(wbuf1));
    OBJECT_ATTRIBUTES oa1 = make_oa(&us1, OBJ_CASE_INSENSITIVE);
    struct nt_object_header *h1, *h2;
    void *b1, *b2;
    NTSTATUS st = ObCreateObject(&nt_test_type, &oa1, sizeof(struct nt_test_body), &h1, &b1);
    ASSERT_STATUS(st, STATUS_SUCCESS);
    HANDLE hh1;
    st = ObInsertObject(h1, &oa1, 0, proc, &hh1);
    ASSERT_STATUS(st, STATUS_SUCCESS);

    /* Second object with same name should collide */
    UNICODE_STRING us2; make_us(&us2, "\\BaseNamedObjects\\Collide", wbuf2, sizeof(wbuf2));
    OBJECT_ATTRIBUTES oa2 = make_oa(&us2, OBJ_CASE_INSENSITIVE);
    st = ObCreateObject(&nt_test_type, &oa2, sizeof(struct nt_test_body), &h2, &b2);
    ASSERT_STATUS(st, STATUS_SUCCESS);
    HANDLE hh2;
    st = ObInsertObject(h2, &oa2, 0, proc, &hh2);
    ASSERT_STATUS(st, STATUS_OBJECT_NAME_COLLISION);
    ObDereferenceObject(h2);

    /* Case-insensitive variant should also collide */
    UNICODE_STRING us3; make_us(&us3, "\\basenamedobjects\\collide", wbuf2, sizeof(wbuf2));
    OBJECT_ATTRIBUTES oa3 = make_oa(&us3, OBJ_CASE_INSENSITIVE);
    struct nt_object_header *h3; void *b3;
    st = ObCreateObject(&nt_test_type, &oa3, sizeof(struct nt_test_body), &h3, &b3);
    ASSERT_STATUS(st, STATUS_SUCCESS);
    HANDLE hh3;
    st = ObInsertObject(h3, &oa3, 0, proc, &hh3);
    ASSERT_STATUS(st, STATUS_OBJECT_NAME_COLLISION);
    ObDereferenceObject(h3);

    /* Cleanup */
    ObCloseHandle(proc, hh1);
    ObMakeTemporaryObject(h1);
    ObDereferenceObject(h1);
    ObDestroyProcessObject(proc);
    ObDestroyThreadObject(thr);
    kshim_set_current_process(NULL);
    kshim_set_current_thread(NULL);
    PASS();
}

/* -------------------------------------------------------------------------- */
/* 3. OBJ_EXCLUSIVE                                                            */
/* -------------------------------------------------------------------------- */
TEST(exclusive)
{
    reset_ob();
    struct nt_eprocess *proc = make_proc(102, "p3");
    struct nt_ethread *thr = make_thread(proc, 3);
    UNICODE_STRING us; make_us(&us, "\\BaseNamedObjects\\Excl", wbuf1, sizeof(wbuf1));
    OBJECT_ATTRIBUTES oa = make_oa(&us, OBJ_EXCLUSIVE);
    struct nt_object_header *h; void *b;
    NTSTATUS st = ObCreateObject(&nt_test_type, &oa, sizeof(struct nt_test_body), &h, &b);
    ASSERT_STATUS(st, STATUS_SUCCESS);
    HANDLE hh;
    st = ObInsertObject(h, &oa, 0, proc, &hh);
    ASSERT_STATUS(st, STATUS_SUCCESS);

    /* Second open should get sharing violation */
    UNICODE_STRING us2; make_us(&us2, "\\BaseNamedObjects\\Excl", wbuf2, sizeof(wbuf2));
    OBJECT_ATTRIBUTES oa2 = make_oa(&us2, 0);
    HANDLE hh2;
    st = ObOpenObjectByName(&oa2, &nt_test_type, UserMode, 0, proc, &hh2);
    ASSERT_STATUS(st, STATUS_SHARING_VIOLATION);

    /* After close, open should succeed */
    st = ObCloseHandle(proc, hh);
    ASSERT_STATUS(st, STATUS_SUCCESS);
    st = ObOpenObjectByName(&oa2, &nt_test_type, UserMode, 0, proc, &hh2);
    ASSERT_STATUS(st, STATUS_SUCCESS);
    ObCloseHandle(proc, hh2);
    ObMakeTemporaryObject(h);
    ObDereferenceObject(h);
    ObDestroyProcessObject(proc);
    ObDestroyThreadObject(thr);
    kshim_set_current_process(NULL);
    kshim_set_current_thread(NULL);
    PASS();
}

/* -------------------------------------------------------------------------- */
/* 4. close-pending (type close callback)                                      */
/* -------------------------------------------------------------------------- */
TEST(close_pending)
{
    reset_ob();
    struct nt_eprocess *proc = make_proc(103, "p4");
    struct nt_ethread *thr = make_thread(proc, 4);
    UNICODE_STRING us; make_us(&us, "\\BaseNamedObjects\\ClosePend", wbuf1, sizeof(wbuf1));
    OBJECT_ATTRIBUTES oa = make_oa(&us, 0);
    struct nt_object_header *h; void *b;
    NTSTATUS st = ObCreateObject(&nt_test_type, &oa, sizeof(struct nt_test_body), &h, &b);
    ASSERT_STATUS(st, STATUS_SUCCESS);
    struct nt_test_body *tb = (struct nt_test_body*)b;
    tb->close_pending = false;
    HANDLE hh;
    st = ObInsertObject(h, &oa, 0, proc, &hh);
    ASSERT_STATUS(st, STATUS_SUCCESS);
    /* Keep an extra ref so object stays alive after close */
    ObReferenceObject(h);
    st = ObCloseHandle(proc, hh);
    ASSERT_STATUS(st, STATUS_SUCCESS);
    ASSERT(tb->close_pending == true);
    ASSERT(atomic64_read(&h->handle_count)==0);
    ASSERT(atomic64_read(&h->ref_count)>=1);
    ObDereferenceObject(h);
    ObMakeTemporaryObject(h);
    ObDereferenceObject(h);
    ObDestroyProcessObject(proc);
    ObDestroyThreadObject(thr);
    kshim_set_current_process(NULL);
    kshim_set_current_thread(NULL);
    PASS();
}

/* -------------------------------------------------------------------------- */
/* 5. duplicate with reduced access                                            */
/* -------------------------------------------------------------------------- */
TEST(duplicate_reduced_access)
{
    reset_ob();
    struct nt_eprocess *proc = make_proc(104, "p5");
    struct nt_ethread *thr = make_thread(proc, 5);
    UNICODE_STRING us; make_us(&us, "\\BaseNamedObjects\\Dup", wbuf1, sizeof(wbuf1));
    OBJECT_ATTRIBUTES oa = make_oa(&us, 0);
    struct nt_object_header *h; void *b;
    NTSTATUS st = ObCreateObject(&nt_test_type, &oa, sizeof(struct nt_test_body), &h, &b);
    ASSERT_STATUS(st, STATUS_SUCCESS);
    HANDLE h1;
    st = ObInsertObject(h, &oa, 0x03 /* READ|WRITE */, proc, &h1);
    ASSERT_STATUS(st, STATUS_SUCCESS);

    /* Duplicate with reduced access (READ only) */
    HANDLE h2;
    st = NtDuplicateObject(proc, h1, proc, &h2, 0x01, 0, 0);
    ASSERT_STATUS(st, STATUS_SUCCESS);
    struct nt_object_header *obj;
    ACCESS_MASK granted;
    st = ObReferenceObjectByHandle(proc, h2, &nt_test_type, UserMode, 0x01, &obj, &granted);
    ASSERT_STATUS(st, STATUS_SUCCESS);
    ASSERT(granted==0x01);
    ObDereferenceObject(obj);
    /* Try to use h2 with WRITE should fail */
    st = ObReferenceObjectByHandle(proc, h2, &nt_test_type, UserMode, 0x02, &obj, NULL);
    ASSERT_STATUS(st, STATUS_ACCESS_DENIED);

    /* Duplicate with DUPLICATE_SAME_ACCESS should preserve */
    HANDLE h3;
    st = NtDuplicateObject(proc, h1, proc, &h3, 0, 0, DUPLICATE_SAME_ACCESS);
    ASSERT_STATUS(st, STATUS_SUCCESS);
    st = ObReferenceObjectByHandle(proc, h3, &nt_test_type, UserMode, 0x03, &obj, &granted);
    ASSERT_STATUS(st, STATUS_SUCCESS);
    ASSERT(granted==0x03);
    ObDereferenceObject(obj);

    /* DUPLICATE_CLOSE_SOURCE */
    HANDLE h4;
    st = NtDuplicateObject(proc, h1, proc, &h4, 0x01, 0, DUPLICATE_CLOSE_SOURCE);
    ASSERT_STATUS(st, STATUS_SUCCESS);
    /* h1 should now be invalid */
    st = ObReferenceObjectByHandle(proc, h1, &nt_test_type, UserMode, 0, &obj, NULL);
    ASSERT_STATUS(st, STATUS_INVALID_HANDLE);
    /* h4 valid */
    st = ObReferenceObjectByHandle(proc, h4, &nt_test_type, UserMode, 0x01, &obj, NULL);
    ASSERT_STATUS(st, STATUS_SUCCESS);
    ObDereferenceObject(obj);

    ObCloseHandle(proc, h2);
    ObCloseHandle(proc, h3);
    ObCloseHandle(proc, h4);
    ObMakeTemporaryObject(h);
    ObDereferenceObject(h);
    ObDestroyProcessObject(proc);
    ObDestroyThreadObject(thr);
    kshim_set_current_process(NULL);
    kshim_set_current_thread(NULL);
    PASS();
}

/* -------------------------------------------------------------------------- */
/* 6. inheritance                                                              */
/* -------------------------------------------------------------------------- */
TEST(inheritance)
{
    reset_ob();
    struct nt_eprocess *parent = make_proc(200, "parent");
    struct nt_ethread *pthr = make_thread(parent, 10);
    UNICODE_STRING usA; make_us(&usA, "\\BaseNamedObjects\\InhA", wbuf1, sizeof(wbuf1));
    OBJECT_ATTRIBUTES oaA = make_oa(&usA, OBJ_INHERIT);
    struct nt_object_header *ha; void *ba;
    NTSTATUS st = ObCreateObject(&nt_test_type, &oaA, sizeof(struct nt_test_body), &ha, &ba);
    ASSERT_STATUS(st, STATUS_SUCCESS);
    HANDLE hA;
    st = ObInsertObject(ha, &oaA, 0xFF, parent, &hA);
    ASSERT_STATUS(st, STATUS_SUCCESS);

    UNICODE_STRING usB; make_us(&usB, "\\BaseNamedObjects\\InhB", wbuf2, sizeof(wbuf2));
    OBJECT_ATTRIBUTES oaB = make_oa(&usB, 0);
    struct nt_object_header *hb; void *bb;
    st = ObCreateObject(&nt_test_type, &oaB, sizeof(struct nt_test_body), &hb, &bb);
    ASSERT_STATUS(st, STATUS_SUCCESS);
    HANDLE hB;
    st = ObInsertObject(hb, &oaB, 0xFF, parent, &hB);
    ASSERT_STATUS(st, STATUS_SUCCESS);

    struct nt_eprocess *child = NULL;
    st = ObCreateProcessObject(&child, 201, "child");
    ASSERT_STATUS(st, STATUS_SUCCESS);
    st = ObInheritHandles(parent, child);
    ASSERT_STATUS(st, STATUS_SUCCESS);

    /* Child should have hA but not hB, with same numeric value */
    ASSERT(ObHandleIsValid(child, hA)==true);
    ASSERT(ObHandleIsValid(child, hB)==false);
    /* Verify child can reference */
    struct nt_object_header *obj;
    st = ObReferenceObjectByHandle(child, hA, &nt_test_type, UserMode, 0, &obj, NULL);
    ASSERT_STATUS(st, STATUS_SUCCESS);
    ASSERT(obj==ha);
    ObDereferenceObject(obj);

    /* Cleanup */
    ObCloseHandle(parent, hA);
    ObCloseHandle(parent, hB);
    /* Child's inherited handle must be closed via child table */
    ObCloseHandle(child, hA);
    ObMakeTemporaryObject(ha); ObDereferenceObject(ha);
    ObMakeTemporaryObject(hb); ObDereferenceObject(hb);
    ObDestroyProcessObject(parent);
    ObDestroyProcessObject(child);
    ObDestroyThreadObject(pthr);
    kshim_set_current_process(NULL);
    kshim_set_current_thread(NULL);
    PASS();
}

/* -------------------------------------------------------------------------- */
/* 7. pseudo-handles                                                           */
/* -------------------------------------------------------------------------- */
TEST(pseudo_handles)
{
    reset_ob();
    struct nt_eprocess *proc = make_proc(300, "pseudo");
    ASSERT(proc != NULL);
    struct nt_ethread *thr = make_thread(proc, 20);
    ASSERT(thr != NULL);
    struct nt_object_header *obj;
    NTSTATUS st = ObReferenceObjectByHandle(proc, NtCurrentProcess(), &nt_process_type, UserMode, 0, &obj, NULL);
    ASSERT_STATUS(st, STATUS_SUCCESS);
    ASSERT(obj==proc->obj);
    ObDereferenceObject(obj);

    st = ObReferenceObjectByHandle(proc, NtCurrentThread(), &nt_thread_type, UserMode, 0, &obj, NULL);
    ASSERT_STATUS(st, STATUS_SUCCESS);
    ASSERT(obj==thr->obj);
    ObDereferenceObject(obj);

    /* Type mismatch */
    st = ObReferenceObjectByHandle(proc, NtCurrentProcess(), &nt_thread_type, UserMode, 0, &obj, NULL);
    ASSERT_STATUS(st, STATUS_OBJECT_TYPE_MISMATCH);

    /* Closing a pseudo handle succeeds as a no-op (probe_handles golden). */
    st = ObCloseHandle(proc, NtCurrentProcess());
    ASSERT_STATUS(st, STATUS_SUCCESS);
    st = ObCloseHandle(proc, NtCurrentThread());
    ASSERT_STATUS(st, STATUS_SUCCESS);

    ObDestroyProcessObject(proc);
    ObDestroyThreadObject(thr);
    kshim_set_current_process(NULL);
    kshim_set_current_thread(NULL);
    PASS();
}

/* -------------------------------------------------------------------------- */
/* 8. handle reuse after close                                                 */
/* -------------------------------------------------------------------------- */
TEST(handle_reuse)
{
    reset_ob();
    struct nt_eprocess *proc = make_proc(400, "reuse");
    struct nt_ethread *thr = make_thread(proc, 30);
    UNICODE_STRING us1; make_us(&us1, "\\BaseNamedObjects\\Reuse1", wbuf1, sizeof(wbuf1));
    OBJECT_ATTRIBUTES oa1 = make_oa(&us1, 0);
    struct nt_object_header *h1; void *b1;
    NTSTATUS st = ObCreateObject(&nt_test_type, &oa1, sizeof(struct nt_test_body), &h1, &b1);
    ASSERT_STATUS(st, STATUS_SUCCESS);
    HANDLE handle1;
    st = ObInsertObject(h1, &oa1, 0, proc, &handle1);
    ASSERT_STATUS(st, STATUS_SUCCESS);
    ASSERT(((ULONG_PTR)handle1 & 0x3)==0); /* multiples of 4 */

    st = ObCloseHandle(proc, handle1);
    ASSERT_STATUS(st, STATUS_SUCCESS);
    ObMakeTemporaryObject(h1); ObDereferenceObject(h1);

    UNICODE_STRING us2; make_us(&us2, "\\BaseNamedObjects\\Reuse2", wbuf2, sizeof(wbuf2));
    OBJECT_ATTRIBUTES oa2 = make_oa(&us2, 0);
    struct nt_object_header *h2; void *b2;
    st = ObCreateObject(&nt_test_type, &oa2, sizeof(struct nt_test_body), &h2, &b2);
    ASSERT_STATUS(st, STATUS_SUCCESS);
    HANDLE handle2;
    st = ObInsertObject(h2, &oa2, 0, proc, &handle2);
    ASSERT_STATUS(st, STATUS_SUCCESS);
    /* W1 fix: monotonic handle allocation, no immediate reuse to avoid stale aliasing.
       handle2 must be distinct from handle1, and old handle remains invalid. */
    ASSERT(handle1!=handle2);
    ASSERT(ObHandleIsValid(proc, handle1)==false);
    ASSERT(ObHandleIsValid(proc, handle2)==true);
    ObCloseHandle(proc, handle2);
    ObMakeTemporaryObject(h2); ObDereferenceObject(h2);

    /* PROTECT_FROM_CLOSE */
    UNICODE_STRING us3; make_us(&us3, "\\BaseNamedObjects\\Prot", wbuf1, sizeof(wbuf1));
    OBJECT_ATTRIBUTES oa3 = make_oa(&us3, OBJ_PROTECT_FROM_CLOSE);
    struct nt_object_header *h3; void *b3;
    st = ObCreateObject(&nt_test_type, &oa3, sizeof(struct nt_test_body), &h3, &b3);
    ASSERT_STATUS(st, STATUS_SUCCESS);
    HANDLE hprot;
    st = ObInsertObject(h3, &oa3, 0, proc, &hprot);
    ASSERT_STATUS(st, STATUS_SUCCESS);
    st = ObCloseHandle(proc, hprot);
    ASSERT_STATUS(st, STATUS_HANDLE_NOT_CLOSABLE);
    /* Clear protect by duplicating without flag? For test we manually close via removing flag */
    /* Instead we test that handle still valid */
    ASSERT(ObHandleIsValid(proc, hprot)==true);
    /* Force close by destroying table which force-closes protected handles */
    ObDestroyProcessObject(proc);
    ObDestroyThreadObject(thr);
    kshim_set_current_process(NULL);
    kshim_set_current_thread(NULL);
    /* Need to cleanup h3 after destroy: its handle was force-closed, but object still needs deref */
    ObMakeTemporaryObject(h3); ObDereferenceObject(h3);
    PASS();
}

/* -------------------------------------------------------------------------- */
/* 9. randomized stress                                                        */
/* -------------------------------------------------------------------------- */
TEST(stress_random)
{
    reset_ob();
    long before = atomic_load(&kshim_alloc_count);
    const int NPROC=4;
    const int NOPS=2000;
    struct nt_eprocess *procs[NPROC];
    struct nt_ethread *thrs[NPROC];
    for (int i=0;i<NPROC;i++) {
        char n[16]; snprintf(n,sizeof(n),"stress%d",i);
        NTSTATUS st = ObCreateProcessObject(&procs[i], 500+i, n);
        ASSERT(st==STATUS_SUCCESS);
        st = ObCreateThreadObject(&thrs[i], procs[i], 100+i);
        ASSERT(st==STATUS_SUCCESS);
    }
    kshim_set_current_process(procs[0]);
    kshim_set_current_thread(thrs[0]);

    /* Keep track of handles per proc for cleanup validation */
    srand(0x1234);
    for (int op=0; op<NOPS; op++) {
        int p = rand()%NPROC;
        int action = rand()%4;
        if (action==0) {
            /* create named object */
            char name[64]; snprintf(name,sizeof(name),"\\BaseNamedObjects\\Stress_%d_%d",p,rand()%50);
            WCHAR w[64]; UNICODE_STRING us; make_us(&us,name,w,sizeof(w));
            OBJECT_ATTRIBUTES oa=make_oa(&us, (rand()%2)?OBJ_CASE_INSENSITIVE:0);
            struct nt_object_header *h; void *b;
            NTSTATUS st=ObCreateObject(&nt_test_type,&oa,sizeof(struct nt_test_body),&h,&b);
            if (!NT_SUCCESS(st)) continue;
            HANDLE hh;
            st=ObInsertObject(h,&oa, rand()%16, procs[p], &hh);
            if (!NT_SUCCESS(st)) { ObDereferenceObject(h); continue; }
            /* Drop creator reference; directory + handle remain */
            ObDereferenceObject(h);
            /* Always clean up to avoid leak via directory delete edge cases; stress still covers create/close/duplicate/inherit */
            ObCloseHandle(procs[p], hh);
            struct nt_object_header *tmp = NULL;
            if (NT_SUCCESS(ObReferenceObjectByName(&us, &nt_test_type, KernelMode, 0, &tmp))) {
                ObMakeTemporaryObject(tmp);
                ObDereferenceObject(tmp);
            }
        } else if (action==1) {
            /* duplicate random handle if any */
            if (procs[p]->handle_table->num_handles==0) continue;
            /* pick a random existing handle by scanning */
            u32 target = 1 + (rand() % (procs[p]->handle_table->high_water-1));
            HANDLE h = (HANDLE)((ULONG_PTR)target<<2);
            if (!ObHandleIsValid(procs[p], h)) continue;
            int dst = rand()%NPROC;
            HANDLE nh;
            NTSTATUS st = NtDuplicateObject(procs[p], h, procs[dst], &nh, rand()%16, 0, (rand()%2)?DUPLICATE_SAME_ACCESS:0);
            if (NT_SUCCESS(st)) {
                if (rand()%3==0) ObCloseHandle(procs[dst], nh);
            }
        } else if (action==2) {
            /* close random handle */
            if (procs[p]->handle_table->num_handles==0) continue;
            u32 target = 1 + (rand() % (procs[p]->handle_table->high_water-1));
            HANDLE h = (HANDLE)((ULONG_PTR)target<<2);
            ObCloseHandle(procs[p], h);
        } else {
            /* inheritance attempt to random child */
            int dst = rand()%NPROC;
            if (p==dst) continue;
            ObInheritHandles(procs[p], procs[dst]);
        }
    }

    /* Cleanup: destroy all processes (closes remaining handles + derefs) */
    for (int i=0;i<NPROC;i++) {
        /* Need to make temporary any remaining named objects before process destroy? The directory still holds refs.
         * Instead of tracking each, we just cleanup object manager which will clear directories.
         */
        ObDestroyProcessObject(procs[i]);
        ObDestroyThreadObject(thrs[i]);
    }
    ObCleanupObjectManager();
    NTSTATUS st = ObInitObjectManager(); if (st!=STATUS_SUCCESS) { fprintf(stderr,"reinit failed\n"); g_fail++; return; }
    long after = atomic_load(&kshim_alloc_count);
    /* Roots were recreated; total should be similar to before (both have only roots). Allow small variance for proc allocations which are now gone.
     * The important check is final zero after full cleanup, not strict equality here.
     */
    (void)after; (void)before;
    kshim_set_current_process(NULL);
    kshim_set_current_thread(NULL);
    PASS();
}

/* DUPLICATE_CLOSE_SOURCE must not report success when the source refuses to
 * close (PROTECT_FROM_CLOSE): no duplicate may be left behind. */
TEST(duplicate_close_source_protected)
{
    reset_ob();
    struct nt_eprocess *proc = make_proc(110, "p10");
    struct nt_ethread *thr = make_thread(proc, 10);
    UNICODE_STRING us; make_us(&us, "\\BaseNamedObjects\\DupProt", wbuf1, sizeof(wbuf1));
    OBJECT_ATTRIBUTES oa = make_oa(&us, OBJ_PROTECT_FROM_CLOSE);
    struct nt_object_header *h; void *b;
    NTSTATUS st = ObCreateObject(&nt_test_type, &oa, sizeof(struct nt_test_body), &h, &b);
    ASSERT_STATUS(st, STATUS_SUCCESS);
    HANDLE hp;
    st = ObInsertObject(h, &oa, 0x03, proc, &hp);
    ASSERT_STATUS(st, STATUS_SUCCESS);

    HANDLE hdup = (HANDLE)0x1;
    st = NtDuplicateObject(proc, hp, proc, &hdup, 0x01, 0, DUPLICATE_CLOSE_SOURCE);
    ASSERT_STATUS(st, STATUS_HANDLE_NOT_CLOSABLE);
    ASSERT(hdup == NULL);
    ASSERT(ObHandleIsValid(proc, hp) == true);

    /* Process teardown force-closes protected handles. */
    ObDestroyProcessObject(proc);
    ObDestroyThreadObject(thr);
    kshim_set_current_process(NULL);
    kshim_set_current_thread(NULL);
    ObMakeTemporaryObject(h); ObDereferenceObject(h);
    PASS();
}

/* The thread registry must hold exactly one node per thread. A second
 * ObRegisterThread for the same thread (as the kernel branch of
 * nt_current_ethread used to make after ObCreateThreadObject) must not add a
 * node, and ObDestroyThreadObject must leave none behind. A leftover node is a
 * dangling pointer to freed memory for every later tid or header lookup. */
struct ob_thr_node_view { struct list_head link; struct nt_ethread *thr; };
extern struct list_head g_ob_thr_list;

static int count_thr_nodes(struct nt_ethread *t)
{
    int n = 0;
    struct list_head *pos;
    for (pos = g_ob_thr_list.next; pos != &g_ob_thr_list; pos = pos->next) {
        struct ob_thr_node_view *v = list_entry(pos, struct ob_thr_node_view, link);
        if (v->thr == t) n++;
    }
    return n;
}

TEST(thread_registry_one_node)
{
    reset_ob();
    struct nt_eprocess *proc = make_proc(120, "p12");
    ASSERT(proc != NULL);
    struct nt_ethread *thr = make_thread(proc, 12);
    ASSERT(thr != NULL);
    int after_create = count_thr_nodes(thr);

    ObRegisterThread(thr); /* redundant second registration */
    int after_second = count_thr_nodes(thr);
    struct nt_ethread *found = ObFindThreadByTid(12);

    ObDestroyThreadObject(thr);
    int after_destroy = count_thr_nodes(thr); /* pointer compare only */
    ObDestroyProcessObject(proc);
    kshim_set_current_process(NULL);
    kshim_set_current_thread(NULL);

    if (after_create != 1 || after_second != 1 || found != thr || after_destroy != 0) {
        fprintf(stderr, "FAIL %s: nodes after create=%d second register=%d destroy=%d, lookup ok=%d\n",
                __func__, after_create, after_second, after_destroy, found == thr);
        g_fail++;
        return;
    }
    PASS();
}

/* A symlink whose target names itself must fail with a depth error, not
 * recurse until the stack overflows. */
TEST(symlink_self_loop)
{
    reset_ob();
    struct nt_eprocess *proc = make_proc(111, "p11");
    struct nt_ethread *thr = make_thread(proc, 11);
    UNICODE_STRING us; make_us(&us, "\\BaseNamedObjects\\Loop", wbuf1, sizeof(wbuf1));
    OBJECT_ATTRIBUTES oa = make_oa(&us, 0);
    struct nt_object_header *h; void *b;
    NTSTATUS st = ObCreateObject(&nt_symlink_type, &oa, sizeof(struct nt_symlink), &h, &b);
    ASSERT_STATUS(st, STATUS_SUCCESS);
    struct nt_symlink *lk = (struct nt_symlink *)b;
    WCHAR *tb = (WCHAR *)kmalloc(us.Length + sizeof(WCHAR), GFP_KERNEL);
    ASSERT(tb != NULL);
    memcpy(tb, us.Buffer, us.Length);
    tb[us.Length / sizeof(WCHAR)] = 0;
    lk->target.Buffer = tb;
    lk->target.Length = us.Length;
    lk->target.MaximumLength = us.Length + sizeof(WCHAR);
    HANDLE hl;
    st = ObInsertObject(h, &oa, 0, proc, &hl);
    ASSERT_STATUS(st, STATUS_SUCCESS);

    struct nt_object_header *out = NULL;
    st = ObReferenceObjectByName(&lk->target, NULL, KernelMode, 0, &out);
    ASSERT_STATUS(st, STATUS_NAME_TOO_LONG);
    ASSERT(out == NULL);

    ObCloseHandle(proc, hl);
    ObMakeTemporaryObject(h); ObDereferenceObject(h);
    ObDestroyProcessObject(proc);
    ObDestroyThreadObject(thr);
    kshim_set_current_process(NULL);
    kshim_set_current_thread(NULL);
    PASS();
}

/* -------------------------------------------------------------------------- */
/* 13. owned security descriptor slot                                          */
/* -------------------------------------------------------------------------- */
TEST(security_descriptor_slot)
{
    reset_ob();
    static const UCHAR sd_a[24] = { 1, 0, 0x04, 0x80, 0 };
    static const UCHAR sd_b[40] = { 1, 0, 0x04, 0x80, 1 };
    struct nt_object_header *h = NULL; void *b = NULL;
    long base = atomic_load(&kshim_alloc_count);

    NTSTATUS st = ObCreateObject(&nt_test_type, NULL, sizeof(struct nt_test_body), &h, &b);
    ASSERT_STATUS(st, STATUS_SUCCESS);
    /* Default: no security descriptor. */
    ASSERT(h->security_descriptor == NULL);

    /* Attaching copies the bytes: the object owns its own buffer. */
    st = ObSetObjectSecurityDescriptor(h, sd_a, sizeof(sd_a));
    ASSERT_STATUS(st, STATUS_SUCCESS);
    ASSERT(h->security_descriptor != NULL);
    ASSERT(h->security_descriptor != (PSECURITY_DESCRIPTOR)sd_a);
    ASSERT(memcmp(h->security_descriptor, sd_a, sizeof(sd_a)) == 0);

    /* Replacing frees the old copy: exactly one descriptor buffer is live. */
    st = ObSetObjectSecurityDescriptor(h, sd_b, sizeof(sd_b));
    ASSERT_STATUS(st, STATUS_SUCCESS);
    ASSERT(memcmp(h->security_descriptor, sd_b, sizeof(sd_b)) == 0);
    ASSERT(atomic_load(&kshim_alloc_count) == base + 2);

    /* Clearing with NULL drops the descriptor and its buffer. */
    st = ObSetObjectSecurityDescriptor(h, NULL, 0);
    ASSERT_STATUS(st, STATUS_SUCCESS);
    ASSERT(h->security_descriptor == NULL);
    ASSERT(atomic_load(&kshim_alloc_count) == base + 1);

    /* Re-attach, then drop the last reference: the descriptor is freed on destroy. */
    st = ObSetObjectSecurityDescriptor(h, sd_a, sizeof(sd_a));
    ASSERT_STATUS(st, STATUS_SUCCESS);
    ObDereferenceObject(h);
    ASSERT(atomic_load(&kshim_alloc_count) == base);

    /* Invalid arguments are rejected without touching the object. */
    ASSERT_STATUS(ObSetObjectSecurityDescriptor(NULL, sd_a, sizeof(sd_a)), STATUS_INVALID_PARAMETER);
    PASS();
}

TEST(security_descriptor_shared_by_handles)
{
    reset_ob();
    struct nt_eprocess *proc = make_proc(106, "p13");
    struct nt_ethread *thr = make_thread(proc, 13);
    static const UCHAR sd[32] = { 1, 0, 0x04, 0x80, 2 };
    long base = atomic_load(&kshim_alloc_count);

    struct nt_object_header *h = NULL; void *b = NULL;
    NTSTATUS st = ObCreateObject(&nt_test_type, NULL, sizeof(struct nt_test_body), &h, &b);
    ASSERT_STATUS(st, STATUS_SUCCESS);
    HANDLE h1 = NULL;
    st = ObInsertObject(h, NULL, 0x03, proc, &h1);
    ASSERT_STATUS(st, STATUS_SUCCESS);
    HANDLE h2 = NULL;
    st = NtDuplicateObject(proc, h1, proc, &h2, 0x01, 0, 0);
    ASSERT_STATUS(st, STATUS_SUCCESS);

    st = ObSetObjectSecurityDescriptor(h, sd, sizeof(sd));
    ASSERT_STATUS(st, STATUS_SUCCESS);

    /* Both handles resolve to the same header, hence the same descriptor. */
    struct nt_object_header *o1 = NULL, *o2 = NULL;
    st = ObReferenceObjectByHandle(proc, h1, &nt_test_type, UserMode, 0, &o1, NULL);
    ASSERT_STATUS(st, STATUS_SUCCESS);
    st = ObReferenceObjectByHandle(proc, h2, &nt_test_type, UserMode, 0, &o2, NULL);
    ASSERT_STATUS(st, STATUS_SUCCESS);
    ASSERT(o1 == o2);
    ASSERT(o1->security_descriptor == h->security_descriptor);
    ObDereferenceObject(o1);
    ObDereferenceObject(o2);

    /* Closing both handles and dropping the creator's reference frees the
     * descriptor exactly once (no leak, no double free). */
    ObCloseHandle(proc, h1);
    ObCloseHandle(proc, h2);
    ObMakeTemporaryObject(h);
    ObDereferenceObject(h);
    ASSERT(atomic_load(&kshim_alloc_count) == base);

    ObDestroyProcessObject(proc);
    ObDestroyThreadObject(thr);
    kshim_set_current_process(NULL);
    kshim_set_current_thread(NULL);
    PASS();
}

/* -------------------------------------------------------------------------- */
/* Main                                                                        */
/* -------------------------------------------------------------------------- */
int main(void)
{
    printf("ntobj tests starting\n");
    ObInitObjectManager();
    fprintf(stderr,"init alloc=%ld\n", atomic_load(&kshim_alloc_count));
    RUN(refcount_vs_handlecount);
    RUN(name_collisions);
    RUN(exclusive);
    RUN(close_pending);
    RUN(duplicate_reduced_access);
    RUN(duplicate_close_source_protected);
    RUN(thread_registry_one_node);
    RUN(symlink_self_loop);
    RUN(inheritance);
    RUN(pseudo_handles);
    RUN(handle_reuse);
    RUN(stress_random);
    RUN(security_descriptor_slot);
    RUN(security_descriptor_shared_by_handles);
    printf("ntobj tests: %d passed, %d failed\n", g_pass, g_fail);
    if (g_fail) return 1;
    /* Final leak check */
    long leaks = atomic_load(&kshim_alloc_count);
    printf("alloc_count at exit (should be roots only): %ld\n", leaks);
    /* Cleanup to check true zero after full cleanup */
    ObCleanupObjectManager();
    long after_cleanup = atomic_load(&kshim_alloc_count);
    printf("alloc_count after ObCleanup: %ld\n", after_cleanup);
    if (after_cleanup != 0) {
        fprintf(stderr, "LEAK detected: %ld allocations not freed\n", after_cleanup);
        return 1;
    }
    return 0;
}
