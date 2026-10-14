/* i2_test.c - I2 workstream comprehensive tests (userspace shim, ASan+UBSan) */
#include "object.h"
#include "handle.h"
#include "sync.h"
#include "../../ntdll/nt_types.h"
#include "../../ntdll/ntstatus.h"
#include "kshim.h"
#include "ntstatus_kernel.h"

/* Declare nt_obj syscalls (userspace shim) */
NTSTATUS nt_svc_NtCreateEvent(PHANDLE EventHandle, ACCESS_MASK DesiredAccess,
                              POBJECT_ATTRIBUTES ObjectAttributes,
                              ULONG EventType, BOOLEAN InitialState);
NTSTATUS nt_svc_NtOpenEvent(PHANDLE EventHandle, ACCESS_MASK DesiredAccess,
                            POBJECT_ATTRIBUTES ObjectAttributes);
NTSTATUS nt_svc_NtCreateMutant(PHANDLE MutantHandle, ACCESS_MASK DesiredAccess,
                               POBJECT_ATTRIBUTES ObjectAttributes, BOOLEAN InitialOwner);
NTSTATUS nt_svc_NtOpenMutant(PHANDLE MutantHandle, ACCESS_MASK DesiredAccess,
                             POBJECT_ATTRIBUTES ObjectAttributes);
NTSTATUS nt_svc_NtCreateSemaphore(PHANDLE SemaphoreHandle, ACCESS_MASK DesiredAccess,
                                  POBJECT_ATTRIBUTES ObjectAttributes,
                                  LONG InitialCount, LONG MaximumCount);
NTSTATUS nt_svc_NtOpenSemaphore(PHANDLE SemaphoreHandle, ACCESS_MASK DesiredAccess,
                                POBJECT_ATTRIBUTES ObjectAttributes);
NTSTATUS nt_svc_NtDuplicateObject(HANDLE SourceProcessHandle, HANDLE SourceHandle,
                                  HANDLE TargetProcessHandle, PHANDLE TargetHandle,
                                  ACCESS_MASK DesiredAccess, ULONG HandleAttributes, ULONG Options);
NTSTATUS nt_svc_NtQueryObject(HANDLE Handle, ULONG ObjectInformationClass,
                              PVOID ObjectInformation, ULONG Length, PULONG ReturnLength);
NTSTATUS nt_svc_NtClose(HANDLE Handle);
NTSTATUS nt_svc_NtWaitForSingleObject(HANDLE Handle, BOOLEAN Alertable, PLARGE_INTEGER Timeout);
NTSTATUS nt_svc_NtSetEvent(HANDLE EventHandle, PLONG PreviousState);
NTSTATUS nt_svc_NtResetEvent(HANDLE EventHandle, PLONG PreviousState);
NTSTATUS nt_svc_NtReleaseMutant(HANDLE MutantHandle, PLONG PreviousCount);
NTSTATUS nt_svc_NtReleaseSemaphore(HANDLE SemaphoreHandle, LONG ReleaseCount, PLONG PreviousCount);
NTSTATUS nt_svc_NtRtlNtStatusToDosError(NTSTATUS Status, ULONG *DosError);
NTSTATUS nt_svc_NtQuerySystemTime(PLARGE_INTEGER SystemTime);
NTSTATUS nt_svc_NtSetInformationObject(HANDLE Handle, ULONG ObjectInformationClass,
                                       PVOID ObjectInformation, ULONG Length);

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <assert.h>
#include <pthread.h>
#include <unistd.h>
#include <stdatomic.h>
#include <time.h>

static int g_pass=0, g_fail=0;
#define TEST(name) static void test_##name(void)
#define RUN(name) do { printf("  RUN %s\n", #name); test_##name(); } while(0)
#define ASSERT(cond) do { if (!(cond)) { fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#cond); g_fail++; return; } } while(0)
#define ASSERT_STATUS(expr, expect) do { NTSTATUS _s=(expr); if (_s != (expect)) { fprintf(stderr,"FAIL %s:%d: expect 0x%lx got 0x%lx for %s\n",__FILE__,__LINE__,(unsigned long)(expect),(unsigned long)_s,#expr); g_fail++; return; } } while(0)
#define PASS() do { g_pass++; printf("  PASS %s\n", __func__); } while(0)

static WCHAR wbuf1[512];
static WCHAR wbuf2[512] __attribute__((unused));
static void make_us(UNICODE_STRING *us, const char *s, WCHAR *buf, size_t buflen) {
    size_t len=strlen(s);
    ASSERT(len*sizeof(WCHAR) < buflen);
    for (size_t i=0;i<len;i++) buf[i]=(WCHAR)(unsigned char)s[i];
    us->Buffer=buf; us->Length=(USHORT)(len*sizeof(WCHAR)); us->MaximumLength=us->Length;
    if (len) buf[len]=0;
}
static OBJECT_ATTRIBUTES make_oa(UNICODE_STRING *us, ULONG attr) {
    OBJECT_ATTRIBUTES oa={0}; oa.Length=sizeof(oa); oa.ObjectName=us; oa.Attributes=attr; return oa;
}
static struct nt_eprocess *make_proc(ULONG pid, const char *name) {
    struct nt_eprocess *p=NULL; NTSTATUS st=ObCreateProcessObject(&p,pid,name);
    if (st!=STATUS_SUCCESS||!p) {fprintf(stderr,"make_proc failed\n"); g_fail++; return NULL;}
    kshim_set_current_process(p); return p;
}
static struct nt_ethread *make_thread(struct nt_eprocess *proc, ULONG tid) {
    struct nt_ethread *t=NULL; NTSTATUS st=ObCreateThreadObject(&t,proc,tid);
    if (st!=STATUS_SUCCESS||!t) {fprintf(stderr,"make_thread fail\n"); g_fail++; return NULL;}
    kshim_set_current_thread(t);
    struct nt_thread *nt_thr=NULL;
    if (nt_thread_create(&nt_thr)==STATUS_SUCCESS) {
        nt_set_current_thread(nt_thr);
        /* link nt_thread to ethread for abandonment tracking? not needed for shim - use nt_thread directly */
    }
    return t;
}
static void reset_ob(void) { ObCleanupObjectManager(); NTSTATUS st=ObInitObjectManager(); ASSERT(st==STATUS_SUCCESS); }

/* --------------------------------------------------------------- */
/* NtCreateEvent / NtOpenEvent / NtQueryObject type name            */
/* --------------------------------------------------------------- */
TEST(event_create_query) {
    reset_ob();
    struct nt_eprocess *proc=make_proc(1000,"evproc");
    struct nt_ethread *thr=make_thread(proc,1);
    UNICODE_STRING us; make_us(&us, "\\BaseNamedObjects\\I2Event", wbuf1, sizeof(wbuf1));
    OBJECT_ATTRIBUTES oa=make_oa(&us, OBJ_CASE_INSENSITIVE);
    HANDLE hEv;
    ASSERT_STATUS(nt_svc_NtCreateEvent(&hEv, 0x1FFFFF, &oa, NotificationEvent, 0), STATUS_SUCCESS);
    /* Query type */
    char buf[1024]; ULONG ret;
    ASSERT_STATUS(nt_svc_NtQueryObject(hEv, 2, buf, sizeof(buf), &ret), STATUS_SUCCESS);
    /* Check TypeName contains "Event" */
    struct { UNICODE_STRING TypeName; char extra[1024]; } *ti = (void*)buf;
    ASSERT(ti->TypeName.Length == 5*sizeof(WCHAR)); /* "Event" =5 */
    ASSERT(ti->TypeName.Buffer[0]==L'E' && ti->TypeName.Buffer[1]==L'v');
    /* Basic info */
    char bi_buf[64];
    ASSERT_STATUS(nt_svc_NtQueryObject(hEv, 0, bi_buf, sizeof(bi_buf), &ret), STATUS_SUCCESS);
    /* Name info */
    char ni_buf[512];
    ASSERT_STATUS(nt_svc_NtQueryObject(hEv, 1, ni_buf, sizeof(ni_buf), &ret), STATUS_SUCCESS);
    /* Open existing via NtOpenEvent */
    HANDLE h2;
    ASSERT_STATUS(nt_svc_NtOpenEvent(&h2, 0x1FFFFF, &oa), STATUS_SUCCESS);
    ASSERT(h2 != hEv);
    /* Set and wait */
    ASSERT_STATUS(nt_svc_NtSetEvent(hEv, NULL), STATUS_SUCCESS);
    LARGE_INTEGER zero={0};
    ASSERT_STATUS(nt_svc_NtWaitForSingleObject(h2, 0, &zero), STATUS_WAIT_0);
    /* Reset */
    ASSERT_STATUS(nt_svc_NtResetEvent(hEv, NULL), STATUS_SUCCESS);
    ASSERT_STATUS(nt_svc_NtWaitForSingleObject(h2, 0, &zero), STATUS_TIMEOUT);
    /* Cleanup */
    ASSERT_STATUS(nt_svc_NtClose(hEv), STATUS_SUCCESS);
    ASSERT_STATUS(nt_svc_NtClose(h2), STATUS_SUCCESS);
    /* Query after close should fail */
    ASSERT_STATUS(nt_svc_NtQueryObject(hEv, 2, buf, sizeof(buf), &ret), STATUS_INVALID_HANDLE);
    ObDestroyProcessObject(proc); ObDestroyThreadObject(thr);
    kshim_set_current_process(NULL); kshim_set_current_thread(NULL);
    struct nt_thread *cur=nt_current_thread(); if(cur){nt_set_current_thread(NULL); nt_thread_destroy(cur);}
    PASS();
}

/* --------------------------------------------------------------- */
/* Mutant create, acquire, release, abandonment via thread death     */
/* --------------------------------------------------------------- */
TEST(mutant_abandon) {
    reset_ob();
    struct nt_eprocess *proc=make_proc(1001,"mutproc");
    struct nt_ethread *thr=make_thread(proc,2);
    struct nt_thread *init_nt = nt_current_thread();
    UNICODE_STRING us; make_us(&us, "\\BaseNamedObjects\\I2Mutant", wbuf1, sizeof(wbuf1));
    OBJECT_ATTRIBUTES oa=make_oa(&us,0);
    HANDLE hM;
    ASSERT_STATUS(nt_svc_NtCreateMutant(&hM, 0x1FFFFF, &oa, 0), STATUS_SUCCESS);
    /* Type name */
    char buf[1024]; ULONG ret;
    ASSERT_STATUS(nt_svc_NtQueryObject(hM, 2, buf, sizeof(buf), &ret), STATUS_SUCCESS);
    /* Verify Mutant */
    struct { UNICODE_STRING tn; char x[1024];} *ti=(void*)buf;
    ASSERT(ti->tn.Length==6*sizeof(WCHAR)); /* Mutant */
    /* Acquire */
    LARGE_INTEGER zero={0};
    ASSERT_STATUS(nt_svc_NtWaitForSingleObject(hM, 0, &zero), STATUS_WAIT_0);
    ASSERT_STATUS(nt_svc_NtReleaseMutant(hM, NULL), STATUS_SUCCESS);
    /* Now test abandonment: create a thread that acquires and then abandon */
    struct nt_thread *owner_thr;
    ASSERT_STATUS(nt_thread_create(&owner_thr), STATUS_SUCCESS);
    struct nt_mutant *mut_body;
    struct nt_object_header *hdr;
    ASSERT_STATUS(ObReferenceObjectByHandle(proc, hM, NULL, UserMode, 0, &hdr, NULL), STATUS_SUCCESS);
    mut_body=(struct nt_mutant*)nt_object_body(hdr);
    /* Simulate owner acquiring */
    nt_set_current_thread(owner_thr);
    ASSERT_STATUS(nt_wait_for_single_object(&mut_body->header, 0, NULL), STATUS_WAIT_0);
    ASSERT(mut_body->owner==owner_thr);
    /* Abandon */
    nt_abandon_owned_mutants(owner_thr);
    nt_set_current_thread(nt_current_thread()); /* restore? we lost main */
    /* Now main thread wait should get ABANDONED */
    /* Need to set main thread as current */
    struct nt_thread *main_thr;
    ASSERT_STATUS(nt_thread_create(&main_thr), STATUS_SUCCESS);
    nt_set_current_thread(main_thr);
    NTSTATUS ws = nt_wait_for_single_object(&mut_body->header, 0, &zero);
    ASSERT(ws==STATUS_ABANDONED_WAIT_0);
    ASSERT_STATUS(nt_release_mutant(mut_body, NULL), STATUS_SUCCESS);
    ObDereferenceObject(hdr);
    nt_thread_destroy(owner_thr);
    nt_thread_destroy(main_thr);
    nt_set_current_thread(init_nt);
    nt_thread_destroy(init_nt);
    nt_set_current_thread(NULL);
    ASSERT_STATUS(nt_svc_NtClose(hM), STATUS_SUCCESS);
    ObDestroyProcessObject(proc); ObDestroyThreadObject(thr);
    kshim_set_current_process(NULL); kshim_set_current_thread(NULL);
    PASS();
}

/* --------------------------------------------------------------- */
/* Semaphore create, query, release limits                           */
/* --------------------------------------------------------------- */
TEST(semaphore_basic) {
    reset_ob();
    struct nt_eprocess *proc=make_proc(1002,"semproc");
    struct nt_ethread *thr=make_thread(proc,3);
    UNICODE_STRING us; make_us(&us, "\\BaseNamedObjects\\I2Sem", wbuf1, sizeof(wbuf1));
    OBJECT_ATTRIBUTES oa=make_oa(&us,0);
    HANDLE hS;
    ASSERT_STATUS(nt_svc_NtCreateSemaphore(&hS, 0x1FFFFF, &oa, 2, 5), STATUS_SUCCESS);
    char buf[1024]; ULONG ret;
    ASSERT_STATUS(nt_svc_NtQueryObject(hS, 2, buf, sizeof(buf), &ret), STATUS_SUCCESS);
    struct { UNICODE_STRING tn; char x[1024];} *ti=(void*)buf;
    ASSERT(ti->tn.Length==9*sizeof(WCHAR)); /* Semaphore */
    LARGE_INTEGER zero={0};
    ASSERT_STATUS(nt_svc_NtWaitForSingleObject(hS,0,&zero), STATUS_WAIT_0);
    ASSERT_STATUS(nt_svc_NtWaitForSingleObject(hS,0,&zero), STATUS_WAIT_0);
    ASSERT_STATUS(nt_svc_NtWaitForSingleObject(hS,0,&zero), STATUS_TIMEOUT);
    LONG prev;
    ASSERT_STATUS(nt_svc_NtReleaseSemaphore(hS,2,&prev), STATUS_SUCCESS);
    ASSERT(prev==0);
    ASSERT_STATUS(nt_svc_NtReleaseSemaphore(hS,10,NULL), STATUS_SEMAPHORE_LIMIT_EXCEEDED);
    ASSERT_STATUS(nt_svc_NtClose(hS), STATUS_SUCCESS);
    ObDestroyProcessObject(proc); ObDestroyThreadObject(thr);
    kshim_set_current_process(NULL); kshim_set_current_thread(NULL);
    struct nt_thread *c=nt_current_thread(); if(c){nt_set_current_thread(NULL); nt_thread_destroy(c);}
    PASS();
}

/* --------------------------------------------------------------- */
/* Duplicate with access narrowing, SAME_ACCESS, CLOSE_SOURCE, cross-process */
/* --------------------------------------------------------------- */
TEST(duplicate_access) {
    reset_ob();
    struct nt_eprocess *procA=make_proc(2000,"procA");
    struct nt_ethread *thrA=make_thread(procA,10);
    UNICODE_STRING us; make_us(&us, "\\BaseNamedObjects\\DupEvent", wbuf1, sizeof(wbuf1));
    OBJECT_ATTRIBUTES oa=make_oa(&us,0);
    HANDLE hOrig;
    ASSERT_STATUS(nt_svc_NtCreateEvent(&hOrig, 0x03, &oa, NotificationEvent, 0), STATUS_SUCCESS);
    /* Duplicate with reduced access */
    HANDLE hDup;
    ASSERT_STATUS(nt_svc_NtDuplicateObject(NtCurrentProcess(), hOrig, NtCurrentProcess(), &hDup, 0x01, 0, 0), STATUS_SUCCESS);
    /* Check granted_access narrowed */
    struct nt_object_header *hdr=NULL; ACCESS_MASK granted;
    ASSERT_STATUS(ObReferenceObjectByHandle(procA, hDup, NULL, UserMode, 0x01, &hdr, &granted), STATUS_SUCCESS);
    ASSERT(granted==0x01);
    ObDereferenceObject(hdr);
    ASSERT_STATUS(ObReferenceObjectByHandle(procA, hDup, NULL, UserMode, 0x02, &hdr, NULL), STATUS_ACCESS_DENIED);
    /* SAME_ACCESS */
    HANDLE hSame;
    ASSERT_STATUS(nt_svc_NtDuplicateObject(NtCurrentProcess(), hOrig, NtCurrentProcess(), &hSame, 0, 0, DUPLICATE_SAME_ACCESS), STATUS_SUCCESS);
    ASSERT_STATUS(ObReferenceObjectByHandle(procA, hSame, NULL, UserMode, 0x03, &hdr, &granted), STATUS_SUCCESS);
    ASSERT(granted==0x03);
    ObDereferenceObject(hdr);
    /* CLOSE_SOURCE */
    HANDLE hClose;
    ASSERT_STATUS(nt_svc_NtDuplicateObject(NtCurrentProcess(), hOrig, NtCurrentProcess(), &hClose, 0x01, 0, DUPLICATE_CLOSE_SOURCE), STATUS_SUCCESS);
    ASSERT_STATUS(ObReferenceObjectByHandle(procA, hOrig, NULL, UserMode, 0, &hdr, NULL), STATUS_INVALID_HANDLE);
    ASSERT_STATUS(ObReferenceObjectByHandle(procA, hClose, NULL, UserMode, 0x01, &hdr, NULL), STATUS_SUCCESS);
    ObDereferenceObject(hdr);
    /* Cross-process: procB */
    struct nt_eprocess *procB=NULL;
    ASSERT_STATUS(ObCreateProcessObject(&procB,2001,"procB"), STATUS_SUCCESS);
    /* Need to make procA's handle for procB visible? Create process handle for procB in procA */
    HANDLE hProcB;
    ASSERT_STATUS(ObCreateHandle(procA, procB->obj, 0x1FFFFF, 0, &hProcB), STATUS_SUCCESS);
    HANDLE hCross;
    /* Duplicate hClose (in procA) to procB */
    ASSERT_STATUS(nt_svc_NtDuplicateObject(NtCurrentProcess(), hClose, hProcB, &hCross, 0x01, 0, DUPLICATE_SAME_ACCESS), STATUS_SUCCESS);
    /* Verify hCross valid in procB */
    kshim_set_current_process(procB);
    ASSERT_STATUS(ObReferenceObjectByHandle(procB, hCross, NULL, UserMode, 0x01, &hdr, NULL), STATUS_SUCCESS);
    ObDereferenceObject(hdr);
    kshim_set_current_process(procA);
    /* Access narrowing should fail if requesting more than granted */
    HANDLE hFail;
    ASSERT_STATUS(nt_svc_NtDuplicateObject(NtCurrentProcess(), hClose, NtCurrentProcess(), &hFail, 0xFF, 0, 0), STATUS_ACCESS_DENIED);
    /* Cleanup */
    nt_svc_NtClose(hDup); nt_svc_NtClose(hSame); nt_svc_NtClose(hClose);
    /* hCross owned by procB, close there */
    kshim_set_current_process(procB); ObCloseHandle(procB, hCross);
    kshim_set_current_process(procA);
    ObCloseHandle(procA, hProcB);
    ObDestroyProcessObject(procA); ObDestroyProcessObject(procB);
    ObDestroyThreadObject(thrA);
    kshim_set_current_process(NULL); kshim_set_current_thread(NULL);
    struct nt_thread *c=nt_current_thread(); if(c){nt_set_current_thread(NULL); nt_thread_destroy(c);}
    PASS();
}

/* --------------------------------------------------------------- */
/* NtQueryObject for File type (via Io) and invalid info class      */
/* --------------------------------------------------------------- */
TEST(query_invalid) {
    reset_ob();
    struct nt_eprocess *proc=make_proc(1003,"qproc");
    struct nt_ethread *thr=make_thread(proc,4);
    UNICODE_STRING us; make_us(&us, "\\BaseNamedObjects\\QEvent", wbuf1, sizeof(wbuf1));
    OBJECT_ATTRIBUTES oa=make_oa(&us,0);
    HANDLE h;
    ASSERT_STATUS(nt_svc_NtCreateEvent(&h, 0x1FFFFF, &oa, NotificationEvent, 0), STATUS_SUCCESS);
    char buf[4]; ULONG ret;
    ASSERT_STATUS(nt_svc_NtQueryObject(h, 2, buf, sizeof(buf), &ret), STATUS_INFO_LENGTH_MISMATCH);
    ASSERT_STATUS(nt_svc_NtQueryObject(h, 99, buf, sizeof(buf), &ret), STATUS_INVALID_INFO_CLASS);
    ASSERT_STATUS(nt_svc_NtQueryObject((HANDLE)0x1234, 0, buf, sizeof(buf), &ret), STATUS_INVALID_HANDLE);
    nt_svc_NtClose(h);
    ObDestroyProcessObject(proc); ObDestroyThreadObject(thr);
    kshim_set_current_process(NULL); kshim_set_current_thread(NULL);
    struct nt_thread *c=nt_current_thread(); if(c){nt_set_current_thread(NULL); nt_thread_destroy(c);}
    PASS();
}

/* --------------------------------------------------------------- */
/* NTSTATUS->DOS table spot check via syscall                        */
/* --------------------------------------------------------------- */
TEST(ntstatus_dos_spot) {
    reset_ob();
    struct {
        NTSTATUS st; ULONG expect;
    } cases[] = {
        { STATUS_SUCCESS, 0 },
        { STATUS_ACCESS_DENIED, 5 },
        { STATUS_INVALID_HANDLE, 6 },
        { STATUS_OBJECT_NAME_NOT_FOUND, 2 },
        { STATUS_OBJECT_NAME_COLLISION, 183 },
        { STATUS_BUFFER_TOO_SMALL, 122 },
        { STATUS_BUFFER_OVERFLOW, 234 },
        { STATUS_NO_MEMORY, 8 },
        { STATUS_TIMEOUT, 1460 },
        { STATUS_MUTANT_NOT_OWNED, 288 },
    };
    for (size_t i=0;i<sizeof(cases)/sizeof(cases[0]);i++) {
        ULONG dos=0xFFFFFFFF;
        ASSERT_STATUS(nt_svc_NtRtlNtStatusToDosError(cases[i].st, &dos), STATUS_SUCCESS);
        ASSERT(dos == cases[i].expect);
        /* Also compare direct call */
        ULONG direct = RtlNtStatusToDosError(cases[i].st);
        ASSERT(direct == cases[i].expect);
    }
    /* Fuzz many random codes across table via syscall - ensure no crash and returns something */
    for (int i=0;i<100;i++) {
        NTSTATUS st = (NTSTATUS)(0xC0000000 + i);
        ULONG dos; ASSERT_STATUS(nt_svc_NtRtlNtStatusToDosError(st, &dos), STATUS_SUCCESS);
        (void)dos;
    }
    /* Verify fallback for unknown status returns 317 */
    ULONG dos; ASSERT_STATUS(nt_svc_NtRtlNtStatusToDosError((NTSTATUS)0xC1234567, &dos), STATUS_SUCCESS);
    ASSERT(dos==317);
    /* Verify customer bit returns itself */
    ASSERT_STATUS(nt_svc_NtRtlNtStatusToDosError((NTSTATUS)0x20000001, &dos), STATUS_SUCCESS);
    ASSERT(dos==0x20000001);
    PASS();
}

/* --------------------------------------------------------------- */
/* NtQuerySystemTime: 100 ns units since 1601-01-01 UTC            */
/* --------------------------------------------------------------- */
TEST(query_system_time) {
    LARGE_INTEGER t;
    ASSERT_STATUS(nt_svc_NtQuerySystemTime(NULL), STATUS_ACCESS_VIOLATION);
    time_t s0 = time(NULL);
    ASSERT_STATUS(nt_svc_NtQuerySystemTime(&t), STATUS_SUCCESS);
    time_t s1 = time(NULL);
    /* 1601-01-01 to 1970-01-01 is 11644473600 s; Unix seconds must lie in [s0, s1]. */
    LONGLONG unix100 = t.QuadPart - 116444736000000000LL;
    ASSERT(t.QuadPart > 116444736000000000LL);
    ASSERT(unix100 >= (LONGLONG)s0 * 10000000LL);
    ASSERT(unix100 < ((LONGLONG)s1 + 1) * 10000000LL);
    PASS();
}

/* --------------------------------------------------------------- */
/* NtSetInformationObject(ObjectHandleFlagInformation): Inherit and ProtectFromClose */
/* --------------------------------------------------------------- */
TEST(set_handle_flag_information) {
    reset_ob();
    struct nt_eprocess *proc=make_proc(4000,"hflags");
    struct nt_ethread *thr=make_thread(proc,20);
    UNICODE_STRING us; make_us(&us, "\\BaseNamedObjects\\HFlagEv", wbuf1, sizeof(wbuf1));
    OBJECT_ATTRIBUTES oa=make_oa(&us,0);
    HANDLE h;
    ASSERT_STATUS(nt_svc_NtCreateEvent(&h, 0x1FFFFF, &oa, NotificationEvent, 0), STATUS_SUCCESS);
    struct { BOOLEAN Inherit; BOOLEAN ProtectFromClose; } fi = {0, 0};
    ASSERT_STATUS(nt_svc_NtSetInformationObject(h, 4, &fi, 1), STATUS_INFO_LENGTH_MISMATCH);
    ASSERT_STATUS(nt_svc_NtSetInformationObject(h, 99, &fi, 2), STATUS_INVALID_INFO_CLASS);
    ASSERT_STATUS(nt_svc_NtSetInformationObject(h, 4, NULL, 2), STATUS_ACCESS_VIOLATION);
    fi.ProtectFromClose = 1;
    ASSERT_STATUS(nt_svc_NtSetInformationObject(h, 4, &fi, 2), STATUS_SUCCESS);
    ASSERT_STATUS(nt_svc_NtClose(h), STATUS_HANDLE_NOT_CLOSABLE);
    fi.ProtectFromClose = 0; fi.Inherit = 1;
    ASSERT_STATUS(nt_svc_NtSetInformationObject(h, 4, &fi, 2), STATUS_SUCCESS);
    ASSERT_STATUS(nt_svc_NtSetInformationObject((HANDLE)0x1234, 4, &fi, 2), STATUS_INVALID_HANDLE);
    ASSERT_STATUS(nt_svc_NtClose(h), STATUS_SUCCESS);
    ObDestroyProcessObject(proc); ObDestroyThreadObject(thr);
    kshim_set_current_process(NULL); kshim_set_current_thread(NULL);
    struct nt_thread *c=nt_current_thread(); if(c){nt_set_current_thread(NULL); nt_thread_destroy(c);}
    PASS();
}

/* --------------------------------------------------------------- */
/* Handle table lifetime: created on first use, destroyed on exit   */
/* --------------------------------------------------------------- */
TEST(handle_table_lifetime) {
    reset_ob();
    struct nt_eprocess *proc=make_proc(3000,"life");
    ASSERT(proc != NULL);
    struct nt_ethread *thr=make_thread(proc,5);
    ASSERT(thr != NULL);
    ASSERT(proc->handle_table != NULL);
    /* Create and close to ensure table still alive */
    UNICODE_STRING us; make_us(&us, "\\BaseNamedObjects\\LifeEv", wbuf1,sizeof(wbuf1));
    OBJECT_ATTRIBUTES oa=make_oa(&us,0);
    HANDLE h;
    ASSERT_STATUS(nt_svc_NtCreateEvent(&h,0x1FFFFF,&oa,NotificationEvent,0), STATUS_SUCCESS);
    ASSERT_STATUS(nt_svc_NtClose(h), STATUS_SUCCESS);
    /* Simulate exit: destroy */
    ObDestroyProcessObject(proc);
    ObDestroyThreadObject(thr);
    kshim_set_current_process(NULL); kshim_set_current_thread(NULL);
    struct nt_thread *c=nt_current_thread(); if(c){nt_set_current_thread(NULL); nt_thread_destroy(c);}
    PASS();
}

NTSTATUS nt_svc_NtQueryInformationProcess(HANDLE ProcessHandle, ULONG ProcessInformationClass,
                                        PVOID ProcessInformation, ULONG ProcessInformationLength,
                                        PULONG ReturnLength);
/* DuplicateHandle maps a generic right through the object type's GENERIC_MAPPING
 * before the granted-access check. nt_file_type maps GENERIC_READ to
 * FILE_GENERIC_READ (0x120089, probe_handles golden). Before the mapping, the
 * generic bit was never granted and the duplicate failed with ACCESS_DENIED. */
TEST(dup_generic_maps_file_type) {
    reset_ob();
    struct nt_eprocess *procA=make_proc(2300,"dupgen");
    OBJECT_ATTRIBUTES oa; memset(&oa,0,sizeof(oa)); oa.Length=sizeof(oa);
    struct nt_object_header *obj=NULL; void *body=NULL;
    ASSERT_STATUS(ObCreateObject(&nt_file_type,&oa,16,&obj,&body), STATUS_SUCCESS);
    HANDLE h=NULL;
    ASSERT_STATUS(ObCreateHandle(procA, obj, 0x00120089, 0, &h), STATUS_SUCCESS);
    HANDLE hd=NULL;
    ASSERT_STATUS(nt_svc_NtDuplicateObject(NtCurrentProcess(), h, NtCurrentProcess(), &hd, 0x80000000UL, 0, 0), STATUS_SUCCESS);
    struct nt_object_header *hdr=NULL; ACCESS_MASK granted=0;
    ASSERT_STATUS(ObReferenceObjectByHandle(procA, hd, NULL, UserMode, 0, &hdr, &granted), STATUS_SUCCESS);
    ASSERT(granted==0x00120089);
    ObDereferenceObject(hdr);
    ObCloseHandle(procA, hd);
    ObCloseHandle(procA, h);
    ObDereferenceObject(obj);
    ObDestroyProcessObject(procA);
    PASS();
}

/* ProcessHandleCount (class 20) is the live count of the target's handle table:
 * it rises by one per open handle and falls back on close. */
TEST(process_handle_count) {
    reset_ob();
    struct nt_eprocess *proc=make_proc(2200,"hcount");
    ULONG before=0, after=0, after_close=0, rl=0;
    ASSERT_STATUS(nt_svc_NtQueryInformationProcess(NtCurrentProcess(), 20, &before, sizeof(before), &rl), STATUS_SUCCESS);
    ASSERT(rl==4);
    UNICODE_STRING us; make_us(&us, "\\BaseNamedObjects\\HCountEvent", wbuf1, sizeof(wbuf1));
    OBJECT_ATTRIBUTES oa=make_oa(&us,0);
    HANDLE h;
    ASSERT_STATUS(nt_svc_NtCreateEvent(&h, 0x03, &oa, NotificationEvent, 0), STATUS_SUCCESS);
    ASSERT_STATUS(nt_svc_NtQueryInformationProcess(NtCurrentProcess(), 20, &after, sizeof(after), &rl), STATUS_SUCCESS);
    ASSERT(after==before+1);
    nt_svc_NtClose(h);
    ASSERT_STATUS(nt_svc_NtQueryInformationProcess(NtCurrentProcess(), 20, &after_close, sizeof(after_close), &rl), STATUS_SUCCESS);
    ASSERT(after_close==before);
    ASSERT_STATUS(nt_svc_NtQueryInformationProcess(NtCurrentProcess(), 20, &after_close, 2, &rl), STATUS_INFO_LENGTH_MISMATCH);
    ObDestroyProcessObject(proc);
    PASS();
}

int main(void) {
    printf("I2 tests starting\n");
    ObInitObjectManager();
    RUN(event_create_query);
    RUN(dup_generic_maps_file_type);
    RUN(process_handle_count);
    RUN(mutant_abandon);
    RUN(semaphore_basic);
    RUN(duplicate_access);
    RUN(query_invalid);
    RUN(ntstatus_dos_spot);
    RUN(query_system_time);
    RUN(set_handle_flag_information);
    RUN(handle_table_lifetime);
    printf("I2 tests: %d passed, %d failed\n", g_pass, g_fail);
    if (g_fail) return 1;
    long leaks = atomic_load(&kshim_alloc_count);
    printf("alloc_count at exit (roots only): %ld\n", leaks);
    ObCleanupObjectManager();
    long after = atomic_load(&kshim_alloc_count);
    printf("alloc_count after cleanup: %ld\n", after);
    if (after!=0) {fprintf(stderr,"LEAK %ld\n",after); return 1;}
    return 0;
}
