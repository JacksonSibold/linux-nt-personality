#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winternl.h>
#include <stdio.h>
#include <stdint.h>
#include <setjmp.h>

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

static EXCEPTION_RECORD g_rec;
static CONTEXT g_ctx;
static int g_has_rec = 0;
static int g_order_idx = 0;
static char g_order[16][32];
static jmp_buf g_jb;
static int g_in_test = 0;

static void log_order(const char *s){
    if(g_order_idx < 16){ strncpy(g_order[g_order_idx], s, 31); g_order[g_order_idx][31]=0; g_order_idx++; }
}

static LONG CALLBACK veh_handler(PEXCEPTION_POINTERS ep){
    g_rec = *ep->ExceptionRecord;
    g_ctx = *ep->ContextRecord;
    g_has_rec = 1;
    log_order("veh");
    if(g_in_test){
        longjmp(g_jb, 1);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
static LONG CALLBACK veh_continue_handler(PEXCEPTION_POINTERS ep){
    (void)ep;
    log_order("veh_continue");
    return EXCEPTION_CONTINUE_SEARCH;
}

// Test functions using setjmp/longjmp
static void test_av_read(){
    g_has_rec=0; g_order_idx=0; g_in_test=1;
    if(setjmp(g_jb)==0){
        volatile int *p = (int*)0;
        volatile int x = *p; (void)x;
    } else {
        log_order("after_longjmp");
    }
    g_in_test=0;
}
static void test_av_write(){
    g_has_rec=0; g_order_idx=0; g_in_test=1;
    if(setjmp(g_jb)==0){
        volatile int *p = (int*)0;
        *p = 123;
    } else {
        log_order("after_longjmp");
    }
    g_in_test=0;
}
static void test_av_execute(){
    g_has_rec=0; g_order_idx=0; g_in_test=1;
    if(setjmp(g_jb)==0){
        void (*fn)(void) = (void(*)(void))0;
        fn();
    } else {
        log_order("after_longjmp");
    }
    g_in_test=0;
}
static void test_div0(){
    g_has_rec=0; g_order_idx=0; g_in_test=1;
    if(setjmp(g_jb)==0){
        volatile int a=10, b=0; volatile int c = a / b; (void)c;
    } else {
        log_order("after_longjmp");
    }
    g_in_test=0;
}
static void test_breakpoint(){
    g_has_rec=0; g_order_idx=0; g_in_test=1;
    if(setjmp(g_jb)==0){
        __debugbreak();
    } else {
        log_order("after_longjmp");
    }
    g_in_test=0;
}
static void test_guard_page(){
    g_has_rec=0; g_order_idx=0; g_in_test=1;
    void *p = VirtualAlloc(NULL, 8192, MEM_COMMIT|MEM_RESERVE, PAGE_READWRITE);
    if(p){
        DWORD old;
        VirtualProtect((char*)p+4096, 4096, PAGE_READWRITE|PAGE_GUARD, &old);
        if(setjmp(g_jb)==0){
            volatile char *q = (char*)p+4096;
            *q = 1;
        } else {
            log_order("after_longjmp");
        }
        VirtualFree(p, 0, MEM_RELEASE);
    }
    g_in_test=0;
}
static void test_stack_overflow(){
    // Use guard page as stack overflow proxy (similar to guard)
    g_has_rec=0; g_order_idx=0; g_in_test=1;
    if(setjmp(g_jb)==0){
        // Try to cause stack overflow via recursion with guard? Instead use large alloca that hits guard
        // Allocate a page as guard and write beyond?
        // We'll just test guard again as stack overflow is same STATUS_STACK_OVERFLOW
        // For real stack overflow we would need to overflow actual stack guard page, but we can simulate via VirtualAlloc guard
        volatile char *big = (char*)_alloca(8192*4);
        for(int i=0;i<8192*4;i++) big[i]=0; // may hit guard
    } else {
        log_order("after_longjmp");
    }
    g_in_test=0;
}

int main(){
    setvbuf(stdout, NULL, _IONBF, 0);
    print_build_header("probe_exception");
    PVOID hVeh = AddVectoredExceptionHandler(1, veh_handler);
    PVOID hVch = AddVectoredContinueHandler(1, veh_continue_handler);

    printf("  \"tests\": [\n");
    struct { const char *name; void (*fn)(void); } cases[] = {
        {"av_read", test_av_read},
        {"av_write", test_av_write},
        {"av_execute", test_av_execute},
        {"div0", test_div0},
        {"breakpoint", test_breakpoint},
        {"guard_page", test_guard_page},
        {"stack_overflow_sim", test_stack_overflow},
    };
    for(int i=0;i<(int)(sizeof(cases)/sizeof(cases[0])); i++){
        cases[i].fn();
        printf("    {\"name\":\"%s\",\"exception_code\":%lu,\"hex\":\"0x%08lX\",\"num_params\":%lu,\"info0\":%llu,\"info1\":%llu,\"rip\":\"0x%llX\",\"order\":[",
            cases[i].name, (unsigned long)g_rec.ExceptionCode, (unsigned long)(uint32_t)g_rec.ExceptionCode, (unsigned long)g_rec.NumberParameters,
            (unsigned long long)(g_has_rec? g_rec.ExceptionInformation[0]:0),
            (unsigned long long)(g_has_rec? g_rec.ExceptionInformation[1]:0),
            (unsigned long long)(g_has_rec? (uintptr_t)g_rec.ExceptionAddress:0));
        for(int j=0;j<g_order_idx;j++){
            printf("\"%s\"%s", g_order[j], j+1<g_order_idx?",":"");
        }
        printf("],\"has_rec\":%s}", g_has_rec?"true":"false");
        if(i+1<(int)(sizeof(cases)/sizeof(cases[0]))) printf(",\n"); else printf("\n");
        memset(&g_rec,0,sizeof(g_rec)); g_has_rec=0;
        g_order_idx=0;
    }
    printf("  ],\n");
    printf("  \"veh_order_note\": \"veh should be first before any frame handlers (no SEH frames in this probe, so order is veh -> veh_continue only on ContinueExecution)\",\n");
    printf("  \"context_check\": {\"MxCsr\":\"0x%lX\",\"Rip\":\"0x%llX\"},\n", (unsigned long)g_ctx.MxCsr, (unsigned long long)g_ctx.Rip);
    printf("  \"done\": true\n");
    printf("}\n");
    RemoveVectoredExceptionHandler(hVeh);
    RemoveVectoredContinueHandler(hVch);
    return 0;
}
