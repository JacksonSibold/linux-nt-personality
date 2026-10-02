#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winternl.h>
#include <stdio.h>
#include <stdint.h>

typedef NTSTATUS (NTAPI *PFN_NtPulseEvent)(HANDLE, PLONG);
typedef NTSTATUS (NTAPI *PFN_NtReleaseMutant)(HANDLE, PLONG);
typedef NTSTATUS (NTAPI *PFN_NtReleaseSemaphore)(HANDLE, LONG, PLONG);

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

struct thread_ctx { HANDLE mutex; volatile LONG *ready; };
static DWORD WINAPI abandon_owner_thread(LPVOID p){
    struct thread_ctx *c = (struct thread_ctx*)p;
    DWORD r = WaitForSingleObject(c->mutex, INFINITE);
    if(c->ready) InterlockedExchange(c->ready, 1);
    Sleep(100);
    return r;
}
static DWORD WINAPI event_waiter(LPVOID p){
    HANDLE e = (HANDLE)p;
    DWORD r = WaitForSingleObject(e, 5000);
    return r;
}

// globals for non-owner test
static HANDLE g_mNonOwner;
static volatile BOOL g_nonOwnerSuccess = FALSE;
static volatile DWORD g_nonOwnerErr = 0;
static volatile LONG g_nonOwnerPrev = -99;
static volatile NTSTATUS g_nonOwnerNt = -1;
static DWORD WINAPI nonOwnerThreadFunc(LPVOID unused){
    (void)unused;
    HANDLE m = g_mNonOwner;
    BOOL ok = ReleaseMutex(m);
    g_nonOwnerSuccess = ok;
    g_nonOwnerErr = ok ? 0 : GetLastError();
    HMODULE n = GetModuleHandleA("ntdll.dll");
    PFN_NtReleaseMutant fn = (PFN_NtReleaseMutant)GetProcAddress(n,"NtReleaseMutant");
    LONG prev = -99;
    NTSTATUS st = fn ? fn(m, &prev) : -1;
    g_nonOwnerNt = st;
    g_nonOwnerPrev = prev;
    return 0;
}

int main(){
    setvbuf(stdout, NULL, _IONBF, 0);
    print_build_header("probe_sync");
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    PFN_NtReleaseMutant pNtReleaseMutant = (PFN_NtReleaseMutant)GetProcAddress(ntdll,"NtReleaseMutant");
    PFN_NtReleaseSemaphore pNtReleaseSemaphore = (PFN_NtReleaseSemaphore)GetProcAddress(ntdll,"NtReleaseSemaphore");
    PFN_NtPulseEvent pNtPulseEvent = (PFN_NtPulseEvent)GetProcAddress(ntdll,"NtPulseEvent");

    // ---- Event auto vs manual ----
    HANDLE eAuto = CreateEventA(NULL, FALSE, FALSE, NULL);
    HANDLE eManual = CreateEventA(NULL, TRUE, FALSE, NULL);
    DWORD r0 = WaitForSingleObject(eAuto, 0);
    DWORD r0m = WaitForSingleObject(eManual, 0);
    SetEvent(eAuto);
    DWORD r1 = WaitForSingleObject(eAuto, 0);
    DWORD r2 = WaitForSingleObject(eAuto, 0);
    SetEvent(eManual);
    DWORD r1m = WaitForSingleObject(eManual, 0);
    DWORD r2m = WaitForSingleObject(eManual, 0);
    ResetEvent(eManual);
    DWORD r3m = WaitForSingleObject(eManual, 0);
    // Pulse
    HANDLE ePulseAuto = CreateEventA(NULL, FALSE, FALSE, NULL);
    HANDLE ePulseManual = CreateEventA(NULL, TRUE, FALSE, NULL);
    HANDLE thPulseAuto = CreateThread(NULL,0,event_waiter,ePulseAuto,0,NULL);
    Sleep(100);
    LONG prevPulse = -1;
    NTSTATUS stPulseAuto = pNtPulseEvent ? pNtPulseEvent(ePulseAuto, &prevPulse) : -1;
    DWORD waitPulseAuto = 0;
    if(thPulseAuto){
        WaitForSingleObject(thPulseAuto, 1000);
        GetExitCodeThread(thPulseAuto, &waitPulseAuto);
        CloseHandle(thPulseAuto);
    }
    DWORD afterPulseAuto = WaitForSingleObject(ePulseAuto, 0);
    HANDLE thPulseMan1 = CreateThread(NULL,0,event_waiter,ePulseManual,0,NULL);
    HANDLE thPulseMan2 = CreateThread(NULL,0,event_waiter,ePulseManual,0,NULL);
    Sleep(100);
    LONG prevPulseM = -1;
    NTSTATUS stPulseMan = pNtPulseEvent ? pNtPulseEvent(ePulseManual, &prevPulseM) : -1;
    DWORD ec1=0, ec2=0;
    if(thPulseMan1){ WaitForSingleObject(thPulseMan1, 1000); GetExitCodeThread(thPulseMan1, &ec1); CloseHandle(thPulseMan1); }
    if(thPulseMan2){ WaitForSingleObject(thPulseMan2, 1000); GetExitCodeThread(thPulseMan2, &ec2); CloseHandle(thPulseMan2); }
    DWORD afterPulseMan = WaitForSingleObject(ePulseManual, 0);

    // ---- Mutant recursive ----
    HANDLE mRec = CreateMutexA(NULL, FALSE, NULL);
    DWORD mr1 = WaitForSingleObject(mRec, 0);
    DWORD mr2 = WaitForSingleObject(mRec, 0);
    DWORD mr3 = WaitForSingleObject(mRec, 0);
    BOOL rel1 = ReleaseMutex(mRec);
    DWORD errRel1 = rel1 ? 0 : GetLastError();
    BOOL rel2 = ReleaseMutex(mRec);
    DWORD errRel2 = rel2 ? 0 : GetLastError();
    BOOL rel3 = ReleaseMutex(mRec);
    DWORD errRel3 = rel3 ? 0 : GetLastError();
    // release by non-owner
    HANDLE mForNonOwner = CreateMutexA(NULL, FALSE, NULL);
    WaitForSingleObject(mForNonOwner, INFINITE);
    g_mNonOwner = mForNonOwner;
    g_nonOwnerSuccess = FALSE; g_nonOwnerErr=0; g_nonOwnerPrev=-99; g_nonOwnerNt=-1;
    HANDLE thNonOwner = CreateThread(NULL,0,nonOwnerThreadFunc,NULL,0,NULL);
    if(thNonOwner){ WaitForSingleObject(thNonOwner, 2000); CloseHandle(thNonOwner); }
    // main still owns, then release correctly
    BOOL relOwnerAfter = ReleaseMutex(mForNonOwner);
    DWORD errOwnerAfter = relOwnerAfter?0:GetLastError();
    // extra release should fail (not owner after release)
    BOOL relExtra = ReleaseMutex(mForNonOwner);
    DWORD errExtra = relExtra?0:GetLastError();
    LONG prevNtExtra = -99;
    NTSTATUS stNtExtra = pNtReleaseMutant ? pNtReleaseMutant(mForNonOwner, &prevNtExtra) : -1;

    // ---- Abandonment ----
    HANDLE mAbandon = CreateMutexA(NULL, FALSE, NULL);
    volatile LONG ready = 0;
    struct thread_ctx tctx; tctx.mutex = mAbandon; tctx.ready = &ready;
    HANDLE thAbandon = CreateThread(NULL,0,abandon_owner_thread,&tctx,0,NULL);
    for(int i=0;i<50 && ready==0;i++) Sleep(20);
    DWORD thExit = 0;
    if(thAbandon){
        WaitForSingleObject(thAbandon, 2000);
        GetExitCodeThread(thAbandon, &thExit);
        CloseHandle(thAbandon);
    }
    DWORD abandonWait = WaitForSingleObject(mAbandon, 0);
    BOOL relAbandon = FALSE; DWORD errRelAb = 0;
    if(abandonWait == WAIT_ABANDONED || abandonWait == WAIT_OBJECT_0){
        relAbandon = ReleaseMutex(mAbandon);
        errRelAb = relAbandon ? 0 : GetLastError();
    }

    // ---- Semaphore ----
    HANDLE sem = CreateSemaphoreA(NULL, 1, 3, NULL);
    LONG prevSem = -1;
    BOOL relSem1 = ReleaseSemaphore(sem, 1, &prevSem); DWORD errSem1 = relSem1?0:GetLastError();
    LONG prevSem2 = -1;
    BOOL relSem2 = ReleaseSemaphore(sem, 1, &prevSem2); DWORD errSem2 = relSem2?0:GetLastError();
    LONG prevSemOver = -1;
    BOOL relSemOver = ReleaseSemaphore(sem, 1, &prevSemOver); DWORD errSemOver = relSemOver?0:GetLastError();
    HANDLE sem2 = CreateSemaphoreA(NULL, 2, 2, NULL);
    LONG prevNt = -1;
    NTSTATUS stNtOver = pNtReleaseSemaphore ? pNtReleaseSemaphore(sem2, 1, &prevNt) : -1;
    HANDLE sem3 = CreateSemaphoreA(NULL, 0, 5, NULL);
    LONG prev3 = -1;
    BOOL rel3b = ReleaseSemaphore(sem3, 2, &prev3);
    DWORD waitSem3_1 = WaitForSingleObject(sem3, 0);
    DWORD waitSem3_2 = WaitForSingleObject(sem3, 0);
    DWORD waitSem3_3 = WaitForSingleObject(sem3, 0);

    // ---- WaitForMultipleObjects ----
    HANDLE ev1 = CreateEventA(NULL, FALSE, FALSE, NULL);
    HANDLE ev2 = CreateEventA(NULL, FALSE, FALSE, NULL);
    HANDLE ev3 = CreateEventA(NULL, FALSE, FALSE, NULL);
    SetEvent(ev2);
    HANDLE arrAny[3] = {ev1, ev2, ev3};
    DWORD anyRes = WaitForMultipleObjects(3, arrAny, FALSE, 0);
    ResetEvent(ev2);
    SetEvent(ev1); SetEvent(ev2);
    HANDLE arrAll2[2] = {ev1, ev2};
    DWORD allRes2 = WaitForMultipleObjects(2, arrAll2, TRUE, 0);
    ResetEvent(ev2);
    DWORD allResTimeout = WaitForMultipleObjects(2, arrAll2, TRUE, 0);
    HANDLE mAb2 = CreateMutexA(NULL, FALSE, NULL);
    volatile LONG ready2=0;
    struct thread_ctx tc2; tc2.mutex=mAb2; tc2.ready=&ready2;
    HANDLE thAb2 = CreateThread(NULL,0,abandon_owner_thread,&tc2,0,NULL);
    for(int i=0;i<50 && ready2==0;i++) Sleep(20);
    if(thAb2){ WaitForSingleObject(thAb2,2000); CloseHandle(thAb2); }
    HANDLE evSig = CreateEventA(NULL, FALSE, TRUE, NULL);
    HANDLE arrAb[2] = {mAb2, evSig};
    DWORD abWaitAny = WaitForMultipleObjects(2, arrAb, FALSE, 0);
    // fresh abandoned for wait-all
    HANDLE mAb3 = CreateMutexA(NULL, FALSE, NULL);
    volatile LONG r3a=0; struct thread_ctx tc3; tc3.mutex=mAb3; tc3.ready=&r3a;
    HANDLE thAb3 = CreateThread(NULL,0,abandon_owner_thread,&tc3,0,NULL);
    for(int i=0;i<50 && r3a==0;i++) Sleep(20);
    if(thAb3){ WaitForSingleObject(thAb3,2000); CloseHandle(thAb3); }
    HANDLE arrAbAll[2] = {mAb3, evSig};
    DWORD abWaitAll = WaitForMultipleObjects(2, arrAbAll, TRUE, 0);

    // ---- Timeout accuracy ----
    LARGE_INTEGER startQ, endQ, freq;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&startQ);
    DWORD toRes = WaitForSingleObject(eAuto, 100);
    QueryPerformanceCounter(&endQ);
    double elapsedMs = (double)(endQ.QuadPart - startQ.QuadPart) * 1000.0 / (double)freq.QuadPart;

    printf("  \"events\": {\n");
    printf("    \"auto\": {\"initial_wait\":%lu,\"after_set_wait1\":%lu,\"after_set_wait2\":%lu},\n", (unsigned long)r0, (unsigned long)r1, (unsigned long)r2);
    printf("    \"manual\": {\"initial_wait\":%lu,\"after_set_wait1\":%lu,\"after_set_wait2\":%lu,\"after_reset\":%lu},\n", (unsigned long)r0m, (unsigned long)r1m, (unsigned long)r2m, (unsigned long)r3m);
    printf("    \"pulse_auto\": {\"ntstatus\":%ld,\"ntstatus_hex\":\"0x%08lX\",\"prev\":%ld,\"waiter_result\":%lu,\"after_pulse_wait\":%lu},\n", (long)stPulseAuto, (unsigned long)(uint32_t)stPulseAuto, (long)prevPulse, (unsigned long)waitPulseAuto, (unsigned long)afterPulseAuto);
    printf("    \"pulse_manual\": {\"ntstatus\":%ld,\"ntstatus_hex\":\"0x%08lX\",\"prev\":%ld,\"waiter1\":%lu,\"waiter2\":%lu,\"after_pulse_wait\":%lu}\n", (long)stPulseMan, (unsigned long)(uint32_t)stPulseMan, (long)prevPulseM, (unsigned long)ec1, (unsigned long)ec2, (unsigned long)afterPulseMan);
    printf("  },\n");
    printf("  \"mutant\": {\n");
    printf("    \"recursive_acquire\": [%lu,%lu,%lu],\n", (unsigned long)mr1,(unsigned long)mr2,(unsigned long)mr3);
    printf("    \"recursive_release\": [{\"success\":%s,\"err\":%lu},{\"success\":%s,\"err\":%lu},{\"success\":%s,\"err\":%lu}],\n", rel1?"true":"false",(unsigned long)errRel1, rel2?"true":"false",(unsigned long)errRel2, rel3?"true":"false",(unsigned long)errRel3);
    printf("    \"non_owner_win32_success\": %s,\n", g_nonOwnerSuccess?"true":"false");
    printf("    \"non_owner_win32_err\": %lu,\n", (unsigned long)g_nonOwnerErr);
    printf("    \"non_owner_win32_err_hex\": \"0x%lX\",\n", (unsigned long)g_nonOwnerErr);
    printf("    \"non_owner_ntstatus\": %ld,\n", (long)g_nonOwnerNt);
    printf("    \"non_owner_ntstatus_hex\": \"0x%08lX\",\n", (unsigned long)(uint32_t)g_nonOwnerNt);
    printf("    \"non_owner_prev\": %ld,\n", (long)g_nonOwnerPrev);
    printf("    \"owner_release_after\": {\"success\":%s,\"err\":%lu},\n", relOwnerAfter?"true":"false",(unsigned long)errOwnerAfter);
    printf("    \"extra_release\": {\"success\":%s,\"err\":%lu,\"ntstatus\":%ld,\"ntstatus_hex\":\"0x%08lX\",\"prev\":%ld},\n", relExtra?"true":"false",(unsigned long)errExtra,(long)stNtExtra,(unsigned long)(uint32_t)stNtExtra,(long)prevNtExtra);
    printf("    \"abandon\": {\"thread_exit\":%lu,\"wait_result\":%lu,\"wait_hex\":\"0x%lX\",\"release_after_abandon_success\":%s,\"release_err\":%lu}\n", (unsigned long)thExit,(unsigned long)abandonWait,(unsigned long)abandonWait, relAbandon?"true":"false",(unsigned long)errRelAb);
    printf("  },\n");
    printf("  \"semaphore\": {\n");
    printf("    \"initial1_max3\": [{\"release1_success\":%s,\"prev\":%ld,\"err\":%lu},{\"release2_success\":%s,\"prev\":%ld,\"err\":%lu},{\"over_release_success\":%s,\"prev\":%ld,\"err\":%lu,\"err_hex\":\"0x%lX\"}],\n", relSem1?"true":"false",(long)prevSem,(unsigned long)errSem1, relSem2?"true":"false",(long)prevSem2,(unsigned long)errSem2, relSemOver?"true":"false",(long)prevSemOver,(unsigned long)errSemOver,(unsigned long)errSemOver);
    printf("    \"nt_over_release\": {\"ntstatus\":%ld,\"ntstatus_hex\":\"0x%08lX\",\"prev\":%ld},\n", (long)stNtOver,(unsigned long)(uint32_t)stNtOver,(long)prevNt);
    printf("    \"prev_count_test\": {\"initial0_max5_release2_prev\":%ld,\"success\":%s,\"wait1\":%lu,\"wait2\":%lu,\"wait3\":%lu}\n", (long)prev3, rel3b?"true":"false",(unsigned long)waitSem3_1,(unsigned long)waitSem3_2,(unsigned long)waitSem3_3);
    printf("  },\n");
    printf("  \"wait_multiple\": {\n");
    printf("    \"wait_any_index\": %lu,\n", (unsigned long)anyRes);
    printf("    \"wait_all_2signaled\": %lu,\n", (unsigned long)allRes2);
    printf("    \"wait_all_timeout\": %lu,\n", (unsigned long)allResTimeout);
    printf("    \"wait_any_abandoned\": %lu,\n", (unsigned long)abWaitAny);
    printf("    \"wait_all_abandoned\": %lu\n", (unsigned long)abWaitAll);
    printf("  },\n");
    printf("  \"timeout\": {\"wait_100ms_result\":%lu,\"elapsed_ms\":%.3f}\n", (unsigned long)toRes, elapsedMs);
    printf("}\n");

    CloseHandle(eAuto); CloseHandle(eManual); CloseHandle(ePulseAuto); CloseHandle(ePulseManual);
    CloseHandle(mRec); CloseHandle(mAbandon); CloseHandle(sem); CloseHandle(sem2); CloseHandle(sem3);
    CloseHandle(ev1); CloseHandle(ev2); CloseHandle(ev3); CloseHandle(mAb2); CloseHandle(evSig); CloseHandle(mAb3); CloseHandle(mForNonOwner);
    return 0;
}
