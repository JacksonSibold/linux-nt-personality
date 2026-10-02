#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winternl.h>
#include <stdio.h>
#include <stdint.h>

typedef NTSTATUS (NTAPI *PFN_NtQueryInformationProcess)(HANDLE, PROCESSINFOCLASS, PVOID, ULONG, PULONG);
typedef NTSTATUS (NTAPI *PFN_NtQueryInformationThread)(HANDLE, THREADINFOCLASS, PVOID, ULONG, PULONG);
typedef NTSTATUS (NTAPI *PFN_NtSuspendThread)(HANDLE, PULONG);
typedef NTSTATUS (NTAPI *PFN_NtResumeThread)(HANDLE, PULONG);

static void json_escape(const char *s){ while(*s){ if(*s=='\\') printf("\\\\"); else if(*s=='"') printf("\\\""); else if(*s=='\n') printf("\\n"); else if(*s=='\r') printf("\\r"); else if(*s=='\t') printf("\\t"); else putchar(*s); s++; } }
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

static DWORD WINAPI sleeper_thread(LPVOID p){ Sleep(5000); return 12345; }
static DWORD WINAPI exit_thread_func(LPVOID p){ return 777; }

int main(){
    setvbuf(stdout, NULL, _IONBF, 0);
    print_build_header("probe_process_thread");
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    PFN_NtQueryInformationProcess pNtQIP = (PFN_NtQueryInformationProcess)GetProcAddress(ntdll,"NtQueryInformationProcess");
    PFN_NtQueryInformationThread pNtQIT = (PFN_NtQueryInformationThread)GetProcAddress(ntdll,"NtQueryInformationThread");
    PFN_NtSuspendThread pNtSuspendThread = (PFN_NtSuspendThread)GetProcAddress(ntdll,"NtSuspendThread");
    PFN_NtResumeThread pNtResumeThread = (PFN_NtResumeThread)GetProcAddress(ntdll,"NtResumeThread");

    // PEB/TEB linkage via GS
    uint64_t teb_addr=0, peb_addr=0;
    __asm__ volatile("movq %%gs:0x30, %0" : "=r"(teb_addr));
    __asm__ volatile("movq %%gs:0x60, %0" : "=r"(peb_addr));
    uint64_t teb_peb = *(uint64_t*)(teb_addr + 0x60);
    uint32_t peb_pid = *(uint32_t*)(peb_addr + 0x20); // not accurate, but we check linkage later via NtQIP
    // Get TEB fields: ClientId etc via NtCurrentTeb ?
    // Use NtQueryInformationProcess/Thread
    printf("  \"teb_peb\": {\"teb\":\"0x%llX\",\"peb\":\"0x%llX\",\"teb_peb\":\"0x%llX\",\"teb_peb_match\":%s},\n",
        (unsigned long long)teb_addr,(unsigned long long)peb_addr,(unsigned long long)teb_peb, (teb_peb==peb_addr)?"true":"false");

    // Process classes
    printf("  \"process_info\": [\n");
    struct { ULONG cls; const char *name; } pcls[] = {
        {0, "ProcessBasicInformation"},
        {1, "ProcessQuotaLimits"},
        {2, "ProcessIoCounters"},
        {6, "ProcessDebugPort"},
        {7, "ProcessExceptionPort"},
        {8, "ProcessAccessToken"},
        {10, "ProcessWow64Information"},
        {25, "ProcessTimes"},
        {26, "ProcessWow64Information_dup"},
        {27, "ProcessImageFileName"},
        {29, "ProcessBreakOnTermination"},
        {30, "ProcessDebugObjectHandle"},
        {9999, "BogusClass"}
    };
    int first=1;
    for(int i=0;i< (int)(sizeof(pcls)/sizeof(pcls[0])); i++){
        char buf[1024]; ULONG retLen=0xDEADBEEF;
        NTSTATUS st = pNtQIP ? pNtQIP(GetCurrentProcess(), pcls[i].cls, buf, sizeof(buf), &retLen) : -1;
        if(!first) printf(",\n");
        first=0;
        printf("    {\"class\":%lu,\"name\":\"%s\",\"ntstatus\":%ld,\"ntstatus_hex\":\"0x%08lX\",\"retLen\":%lu",
            (unsigned long)pcls[i].cls, pcls[i].name, (long)st, (unsigned long)(uint32_t)st, (unsigned long)retLen);
        if(pcls[i].cls==0 && NT_SUCCESS(st)){
            PROCESS_BASIC_INFORMATION *pbi = (PROCESS_BASIC_INFORMATION*)buf;
            printf(",\"pbi\":{\"ExitStatus\":%ld,\"PebBase\":\"0x%llX\",\"AffinityMask\":\"0x%llX\",\"BasePriority\":%ld,\"UniqueProcessId\":\"0x%llX\",\"InheritedFromUniqueProcessId\":\"0x%llX\"}",
                (long)pbi->ExitStatus, (unsigned long long)(uintptr_t)pbi->PebBaseAddress, (unsigned long long)pbi->AffinityMask, (long)pbi->BasePriority, (unsigned long long)(uintptr_t)pbi->UniqueProcessId, (unsigned long long)(uintptr_t)pbi->InheritedFromUniqueProcessId);
        }
        if(pcls[i].cls==27 && NT_SUCCESS(st)){
            UNICODE_STRING *us = (UNICODE_STRING*)buf;
            char nameA[260]={0};
            int len = us->Length/2; if(len>259) len=259;
            for(int k=0;k<len;k++) nameA[k]=(char)us->Buffer[k];
            printf(",\"image_name\":\""); json_escape(nameA); printf("\"");
        }
        // small buffer test for ProcessBasicInformation
        if(pcls[i].cls==0){
            char small[4]; ULONG rl2=0;
            NTSTATUS st2 = pNtQIP ? pNtQIP(GetCurrentProcess(), pcls[i].cls, small, 4, &rl2) : -1;
            printf(",\"small_buffer_4\":{\"ntstatus\":%ld,\"hex\":\"0x%08lX\",\"retLen\":%lu}", (long)st2,(unsigned long)(uint32_t)st2,(unsigned long)rl2);
        }
        printf("}");
    }
    printf("\n  ],\n");
    printf("  \"process_basic_size_probe\": [\n");
    {
        int lens[] = {0,4, 48, 49, 1024};
        for(int li=0; li<5; li++){
            int L = lens[li];
            char b[1024]; ULONG rl=0;
            NTSTATUS s = pNtQIP ? pNtQIP(GetCurrentProcess(), 0, b, L, &rl) : -1;
            printf("    {\"len\":%d,\"ntstatus\":%ld,\"hex\":\"0x%08lX\",\"retLen\":%lu}%s\n", L,(long)s,(unsigned long)(uint32_t)s,(unsigned long)rl, li<4?",":"");
        }
    }
    printf("  ],\n");

    // Thread classes
    printf("  \"thread_info\": [\n");
    struct { ULONG cls; const char *name; } tcls[] = {
        {0, "ThreadBasicInformation"},
        {1, "ThreadTimes"},
        {2, "ThreadPriority"},
        {5, "ThreadQuerySetWin32StartAddress"},
        {9, "ThreadHideFromDebugger"},
        {9999, "Bogus"}
    };
    first=1;
    for(int i=0;i<(int)(sizeof(tcls)/sizeof(tcls[0])); i++){
        char buf[512]; ULONG retLen=0;
        NTSTATUS st = pNtQIT ? pNtQIT(GetCurrentThread(), tcls[i].cls, buf, sizeof(buf), &retLen) : -1;
        if(!first) printf(",\n");
        first=0;
        printf("    {\"class\":%lu,\"name\":\"%s\",\"ntstatus\":%ld,\"hex\":\"0x%08lX\",\"retLen\":%lu", (unsigned long)tcls[i].cls, tcls[i].name, (long)st, (unsigned long)(uint32_t)st, (unsigned long)retLen);
        if(tcls[i].cls==0 && NT_SUCCESS(st)){
            struct my_tbi { NTSTATUS ExitStatus; PVOID TebBaseAddress; CLIENT_ID ClientId; ULONG_PTR AffinityMask; KPRIORITY Priority; KPRIORITY BasePriority; } *tbi = (struct my_tbi*)buf;
            printf(",\"tbi\":{\"ExitStatus\":%ld,\"TebBase\":\"0x%llX\",\"ClientId_UniqueProcess\":\"0x%llX\",\"ClientId_UniqueThread\":\"0x%llX\",\"AffinityMask\":\"0x%llX\",\"Priority\":%ld,\"BasePriority\":%ld}",
                (long)tbi->ExitStatus,(unsigned long long)(uintptr_t)tbi->TebBaseAddress,(unsigned long long)(uintptr_t)tbi->ClientId.UniqueProcess,(unsigned long long)(uintptr_t)tbi->ClientId.UniqueThread,(unsigned long long)tbi->AffinityMask,(long)tbi->Priority,(long)tbi->BasePriority);
        }
        if(tcls[i].cls==0){
            char small[4]; ULONG rl2=0;
            NTSTATUS st2 = pNtQIT ? pNtQIT(GetCurrentThread(), tcls[i].cls, small,4,&rl2):-1;
            printf(",\"small_buffer_4\":{\"ntstatus\":%ld,\"hex\":\"0x%08lX\",\"retLen\":%lu}", (long)st2,(unsigned long)(uint32_t)st2,(unsigned long)rl2);
        }
        printf("}");
    }
    printf("\n  ],\n");
    printf("  \"thread_basic_size_probe\": [\n");
    {
        int lens[] = {0,4, 48, 49, 512};
        for(int li=0; li<5; li++){
            int L = lens[li];
            char b[512]; ULONG rl=0;
            NTSTATUS s = pNtQIT ? pNtQIT(GetCurrentThread(), 0, b, L, &rl) : -1;
            printf("    {\"len\":%d,\"ntstatus\":%ld,\"hex\":\"0x%08lX\",\"retLen\":%lu}%s\n", L,(long)s,(unsigned long)(uint32_t)s,(unsigned long)rl, li<4?",":"");
        }
    }
    printf("  ],\n");

    // Suspend / Resume counts
    HANDLE th = CreateThread(NULL,0,sleeper_thread,NULL,0,NULL);
    Sleep(100);
    ULONG prevSuspend=0xDEAD;
    NTSTATUS stSus1 = pNtSuspendThread ? pNtSuspendThread(th, &prevSuspend) : -1;
    ULONG prevSus2=0xDEAD;
    NTSTATUS stSus2 = pNtSuspendThread ? pNtSuspendThread(th, &prevSus2) : -1;
    DWORD winSus1 = 0, winSus2=0;
    // Also test Win32 SuspendThread
    // Need fresh thread for Win32 test
    HANDLE th2 = CreateThread(NULL,0,sleeper_thread,NULL,0,NULL);
    Sleep(100);
    DWORD wPrev1 = SuspendThread(th2);
    DWORD wPrev2 = SuspendThread(th2);
    ULONG prevRes1=0xDEAD, prevRes2=0xDEAD;
    NTSTATUS stRes1 = pNtResumeThread ? pNtResumeThread(th, &prevRes1) : -1;
    NTSTATUS stRes2 = pNtResumeThread ? pNtResumeThread(th, &prevRes2) : -1;
    DWORD wRes1 = ResumeThread(th2);
    DWORD wRes2 = ResumeThread(th2);
    // Clean up suspended threads: ensure resumed
    ResumeThread(th); ResumeThread(th); ResumeThread(th2); ResumeThread(th2);
    TerminateThread(th,0); TerminateThread(th2,0);
    CloseHandle(th); CloseHandle(th2);
    // Suspend current thread via pseudo would deadlock, so we skip suspending self.
    DWORD selfSus = 0xDEAD; DWORD selfSusErr = 0;
    // Test via real handle but not actually suspending: we query suspend count via NtQueryInformationThread instead
    // We will attempt NtSuspendThread on a duplicate of current thread but immediately resume from another thread? For now report as skipped.
    // To get ground truth for suspend count semantics we already covered via th/th2 above.
    printf("  \"suspend_resume\": {\n");
    printf("    \"nt_suspend1\":{\"ntstatus\":%ld,\"hex\":\"0x%08lX\",\"prev\":%lu},\n", (long)stSus1,(unsigned long)(uint32_t)stSus1,(unsigned long)prevSuspend);
    printf("    \"nt_suspend2\":{\"ntstatus\":%ld,\"hex\":\"0x%08lX\",\"prev\":%lu},\n", (long)stSus2,(unsigned long)(uint32_t)stSus2,(unsigned long)prevSus2);
    printf("    \"win32_suspend1\":%lu,\"win32_suspend2\":%lu,\n", (unsigned long)wPrev1,(unsigned long)wPrev2);
    printf("    \"nt_resume1\":{\"ntstatus\":%ld,\"hex\":\"0x%08lX\",\"prev\":%lu},\n", (long)stRes1,(unsigned long)(uint32_t)stRes1,(unsigned long)prevRes1);
    printf("    \"nt_resume2\":{\"ntstatus\":%ld,\"hex\":\"0x%08lX\",\"prev\":%lu},\n", (long)stRes2,(unsigned long)(uint32_t)stRes2,(unsigned long)prevRes2);
    printf("    \"win32_resume1\":%lu,\"win32_resume2\":%lu,\n", (unsigned long)wRes1,(unsigned long)wRes2);
    printf("    \"self_suspend_result\":%lu,\"self_suspend_err\":%lu\n", (unsigned long)selfSus,(unsigned long)selfSusErr);
    printf("  },\n");

    // Priorities
    int pri = GetThreadPriority(GetCurrentThread());
    BOOL setHigh = SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    int priHigh = GetThreadPriority(GetCurrentThread());
    BOOL setNorm = SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);
    int priNorm = GetThreadPriority(GetCurrentThread());
    DWORD procPri = GetPriorityClass(GetCurrentProcess());
    printf("  \"priorities\": {\"initial\":%d,\"after_high\":%d,\"after_normal\":%d,\"proc_class\":%lu,\"set_high_success\":%s,\"set_normal_success\":%s},\n",
        pri, priHigh, priNorm, (unsigned long)procPri, setHigh?"true":"false", setNorm?"true":"false");

    // Exit codes
    HANDLE thExit = CreateThread(NULL,0,exit_thread_func,NULL,0,NULL);
    DWORD ecBefore=0xDEAD; GetExitCodeThread(thExit, &ecBefore);
    WaitForSingleObject(thExit, INFINITE);
    DWORD ecAfter=0; GetExitCodeThread(thExit, &ecAfter);
    CloseHandle(thExit);
    // Process exit codes: spawn child
    char tmpPath[MAX_PATH]; GetTempPathA(sizeof(tmpPath), tmpPath);
    char tmpExe[MAX_PATH]; GetTempFileNameA(tmpPath, "prc", 0, tmpExe);
    // Use current probe as child with arg? Simpler: spawn cmd /c exit 42
    STARTUPINFOA si={sizeof(si)}; PROCESS_INFORMATION pi={0};
    char cmdLine[] = "cmd.exe /c exit 42";
    BOOL cr = CreateProcessA(NULL, cmdLine, NULL,NULL,FALSE,0,NULL,NULL,&si,&pi);
    DWORD procEcBefore=0xDEAD, procEcAfter=0xDEAD;
    NTSTATUS stProcInfoBefore=-1, stProcInfoAfter=-1;
    if(cr){
        GetExitCodeProcess(pi.hProcess, &procEcBefore);
        if(pNtQIP){
            char buf[64]; ULONG rl=0;
            stProcInfoBefore = pNtQIP(pi.hProcess, 0, buf, sizeof(buf), &rl);
        }
        WaitForSingleObject(pi.hProcess, INFINITE);
        GetExitCodeProcess(pi.hProcess, &procEcAfter);
        if(pNtQIP){
            char buf[64]; ULONG rl=0;
            stProcInfoAfter = pNtQIP(pi.hProcess, 0, buf, sizeof(buf), &rl);
        }
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    }
    DWORD curProcExit=0; GetExitCodeProcess(GetCurrentProcess(), &curProcExit);
    printf("  \"exit_codes\": {\n");
    printf("    \"thread_before_wait\":%lu,\"thread_after\":%lu,\n", (unsigned long)ecBefore,(unsigned long)ecAfter);
    printf("    \"process_child_before\":%lu,\"process_child_after\":%lu,\"child_create_success\":%s,\n", (unsigned long)procEcBefore,(unsigned long)procEcAfter, cr?"true":"false");
    printf("    \"current_process_exit\":%lu,\n", (unsigned long)curProcExit);
    printf("    \"ntqip_before_hex\":\"0x%08lX\",\"ntqip_after_hex\":\"0x%08lX\"\n", (unsigned long)(uint32_t)stProcInfoBefore,(unsigned long)(uint32_t)stProcInfoAfter);
    printf("  }\n");
    printf("}\n");
    DeleteFileA(tmpExe);
    return 0;
}
