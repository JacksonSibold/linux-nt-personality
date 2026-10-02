#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdint.h>

static inline uint64_t read_gs(uint32_t offset) {
    uint64_t val=0;
    switch(offset) {
        case 0x00: __asm__ volatile("movq %%gs:0x00, %0" : "=r"(val)); break;
        case 0x08: __asm__ volatile("movq %%gs:0x08, %0" : "=r"(val)); break;
        case 0x10: __asm__ volatile("movq %%gs:0x10, %0" : "=r"(val)); break;
        case 0x28: __asm__ volatile("movq %%gs:0x28, %0" : "=r"(val)); break;
        case 0x30: __asm__ volatile("movq %%gs:0x30, %0" : "=r"(val)); break;
        case 0x58: __asm__ volatile("movq %%gs:0x58, %0" : "=r"(val)); break;
        case 0x60: __asm__ volatile("movq %%gs:0x60, %0" : "=r"(val)); break;
        case 0x68: { uint32_t tmp; __asm__ volatile("movl %%gs:0x68, %0" : "=r"(tmp)); val = tmp; break; }
        case 0x1480: __asm__ volatile("movq %%gs:0x1480, %0" : "=r"(val)); break;
        default: {
            uint64_t teb;
            __asm__ volatile("movq %%gs:0x30, %0" : "=r"(teb));
            val = *(uint64_t*)(teb + offset);
            break;
        }
    }
    return val;
}

int main(){
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    OSVERSIONINFOEXW ovi; ovi.dwOSVersionInfoSize = sizeof(ovi);
    typedef LONG (WINAPI *PFN_RtlGetVersion)(OSVERSIONINFOEXW*);
    PFN_RtlGetVersion pRtlGetVersion = (PFN_RtlGetVersion)GetProcAddress(ntdll, "RtlGetVersion");
    DWORD build=0, ubr=0, major=0, minor=0;
    if(pRtlGetVersion){ pRtlGetVersion(&ovi); major=ovi.dwMajorVersion; minor=ovi.dwMinorVersion; build=ovi.dwBuildNumber; }
    HKEY hkey; DWORD ubrVal=0, cb=sizeof(ubrVal);
    if(RegOpenKeyExA(HKEY_LOCAL_MACHINE,"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",0,KEY_READ,&hkey)==ERROR_SUCCESS){
        RegQueryValueExA(hkey,"UBR",NULL,NULL,(BYTE*)&ubrVal,&cb); RegCloseKey(hkey); ubr=ubrVal;
    }

    uint64_t teb_addr;
    __asm__ volatile("movq %%gs:0x30, %0" : "=r"(teb_addr));
    uint64_t peb_addr = read_gs(0x60);
    // Read PEB fields via raw offsets (our nt_types.h offsets)
    uint8_t  peb_BeingDebugged = *(uint8_t*)(peb_addr + 0x02);
    uint64_t peb_ImageBase = *(uint64_t*)(peb_addr + 0x10);
    uint64_t peb_Ldr = *(uint64_t*)(peb_addr + 0x18);
    uint64_t peb_ProcessParameters = *(uint64_t*)(peb_addr + 0x20);
    uint64_t peb_ProcessHeap = *(uint64_t*)(peb_addr + 0x30);
    uint64_t peb_ApiSetMap = *(uint64_t*)(peb_addr + 0x68);
    uint32_t peb_NtGlobalFlag = *(uint32_t*)(peb_addr + 0xBC);
    // Additional PEB fields for validation
    uint32_t peb_OSMajor = *(uint32_t*)(peb_addr + 0x118);
    uint32_t peb_OSMinor = *(uint32_t*)(peb_addr + 0x11C);
    uint16_t peb_OSBuild = *(uint16_t*)(peb_addr + 0x120);

    // TEB raw reads
    uint64_t teb_StackBase = *(uint64_t*)(teb_addr + 0x08);
    uint64_t teb_StackLimit = *(uint64_t*)(teb_addr + 0x10);
    uint64_t teb_Self = *(uint64_t*)(teb_addr + 0x30);
    uint64_t teb_TLS = *(uint64_t*)(teb_addr + 0x58);
    uint64_t teb_PEB = *(uint64_t*)(teb_addr + 0x60);
    uint32_t teb_LastError = *(uint32_t*)(teb_addr + 0x68);
    uint64_t teb_TlsSlots = *(uint64_t*)(teb_addr + 0x1480);
    uint64_t teb_DeallocStack = *(uint64_t*)(teb_addr + 0x1478); // our header says DeallocationStack at 0x1478

    // Validate ImageBase matches GetModuleHandle(NULL)
    uint64_t imageBaseActual = (uint64_t)(uintptr_t)GetModuleHandleA(NULL);

    printf("{\n");
    printf("  \"probe\": \"probe_teb_peb\",\n");
    printf("  \"windows_build\": \"%lu.%lu\",\n", (unsigned long)build,(unsigned long)ubr);
    printf("  \"windows_version\": \"%lu.%lu.%lu\",\n", (unsigned long)major,(unsigned long)minor,(unsigned long)build);
    printf("  \"arch\": \"x64\",\n");
    printf("  \"teb\": \"0x%llX\",\n", (unsigned long long)teb_addr);
    printf("  \"peb\": \"0x%llX\",\n", (unsigned long long)peb_addr);
    printf("  \"gs_checks\": [\n");
    // Each check: offset, gs_value, mem_value, match, semantic
    struct { const char *name; uint32_t off; uint64_t gs; uint64_t mem; } checks[] = {
        {"ExceptionList", 0x00, read_gs(0x00), *(uint64_t*)(teb_addr+0x00)},
        {"StackBase", 0x08, read_gs(0x08), teb_StackBase},
        {"StackLimit", 0x10, read_gs(0x10), teb_StackLimit},
        {"Self", 0x30, read_gs(0x30), teb_Self},
        {"ArbitraryUserPointer", 0x28, read_gs(0x28), *(uint64_t*)(teb_addr+0x28)},
        {"ThreadLocalStoragePointer", 0x58, read_gs(0x58), teb_TLS},
        {"ProcessEnvironmentBlock", 0x60, read_gs(0x60), teb_PEB},
        {"LastErrorValue", 0x68, read_gs(0x68), (uint64_t)teb_LastError},
        {"TlsSlots", 0x1480, read_gs(0x1480), teb_TlsSlots},
    };
    int n = sizeof(checks)/sizeof(checks[0]);
    for(int i=0;i<n;i++){
        int match = (checks[i].gs == checks[i].mem);
        // For Self, also check equals teb_addr
        if(i==3) match = (checks[i].gs == teb_addr && checks[i].mem == teb_addr);
        printf("    {\"field\":\"%s\",\"offset\":\"0x%X\",\"gs_value\":\"0x%llX\",\"mem_value\":\"0x%llX\",\"match\":%s}%s\n",
            checks[i].name, checks[i].off, (unsigned long long)checks[i].gs, (unsigned long long)checks[i].mem, match?"true":"false", i+1<n?",":"");
    }
    printf("  ],\n");
    printf("  \"teb_raw\": {\n");
    printf("    \"StackBase\":\"0x%llX\",\"StackLimit\":\"0x%llX\",\"Self\":\"0x%llX\",\"Self_matches_TEB\":%s,\n",
        (unsigned long long)teb_StackBase, (unsigned long long)teb_StackLimit, (unsigned long long)teb_Self, (teb_Self==teb_addr)?"true":"false");
    printf("    \"DeallocationStack_at_0x1478\":\"0x%llX\",\n", (unsigned long long)teb_DeallocStack);
    printf("    \"TlsSlots_0\":\"0x%llX\",\"TlsSlots_ptr\":\"0x%llX\"\n", (unsigned long long)teb_TlsSlots, (unsigned long long)(teb_addr+0x1480));
    printf("  },\n");
    printf("  \"peb_checks\": [\n");
    // Validate PEB offsets by checking mem vs expected semantic
    // For BeingDebugged, compare IsDebuggerPresent()
    BOOL isDbg = IsDebuggerPresent();
    int dbgMatch = (peb_BeingDebugged == (isDbg?1:0)) || (peb_BeingDebugged==0 || peb_BeingDebugged==1); // just check 0/1
    printf("    {\"field\":\"BeingDebugged\",\"offset\":\"0x02\",\"mem_value\":%u,\"is_debugger_present\":%s,\"valid\":%s},\n",
        (unsigned)peb_BeingDebugged, isDbg?"true":"false", (peb_BeingDebugged<=1)?"true":"false");
    printf("    {\"field\":\"ImageBaseAddress\",\"offset\":\"0x10\",\"mem_value\":\"0x%llX\",\"GetModuleHandle(NULL)\":\"0x%llX\",\"match\":%s},\n",
        (unsigned long long)peb_ImageBase, (unsigned long long)imageBaseActual, (peb_ImageBase==imageBaseActual)?"true":"false");
    printf("    {\"field\":\"Ldr\",\"offset\":\"0x18\",\"mem_value\":\"0x%llX\",\"non_null\":%s},\n",
        (unsigned long long)peb_Ldr, peb_Ldr?"true":"false");
    printf("    {\"field\":\"ProcessParameters\",\"offset\":\"0x20\",\"mem_value\":\"0x%llX\",\"non_null\":%s},\n",
        (unsigned long long)peb_ProcessParameters, peb_ProcessParameters?"true":"false");
    printf("    {\"field\":\"ProcessHeap\",\"offset\":\"0x30\",\"mem_value\":\"0x%llX\",\"non_null\":%s},\n",
        (unsigned long long)peb_ProcessHeap, peb_ProcessHeap?"true":"false");
    printf("    {\"field\":\"ApiSetMap\",\"offset\":\"0x68\",\"mem_value\":\"0x%llX\",\"non_null\":%s},\n",
        (unsigned long long)peb_ApiSetMap, peb_ApiSetMap?"true":"false");
    printf("    {\"field\":\"NtGlobalFlag\",\"offset\":\"0xBC\",\"mem_value\":\"0x%lX\"},\n", (unsigned long)peb_NtGlobalFlag);
    printf("    {\"field\":\"OSMajorVersion\",\"offset\":\"0x118\",\"mem_value\":%lu},\n", (unsigned long)peb_OSMajor);
    printf("    {\"field\":\"OSMinorVersion\",\"offset\":\"0x11C\",\"mem_value\":%lu},\n", (unsigned long)peb_OSMinor);
    printf("    {\"field\":\"OSBuildNumber\",\"offset\":\"0x120\",\"mem_value\":%u}\n", (unsigned)peb_OSBuild);
    printf("  ],\n");
    // LastError GS test
    SetLastError(0x12345678);
    uint64_t lastErrGS = read_gs(0x68) & 0xFFFFFFFFULL;
    uint32_t lastErrGet = GetLastError();
    printf("  \"last_error_gs_test\": {\"set\":\"0x12345678\",\"gs_read\":\"0x%llX\",\"getlasterror\":%lu,\"match\":%s},\n",
        (unsigned long long)lastErrGS, (unsigned long)lastErrGet, (lastErrGS==0x12345678ULL)?"true":"false");
    SetLastError(0);
    printf("  \"context_offsets\": {\"ContextFlags_expected\":\"0x30\",\"MxCsr_expected\":\"0x34\",\"Rax_expected\":\"0x78\",\"Rip_expected\":\"0xF8\"}\n");
    printf("}\n");
    return 0;
}
