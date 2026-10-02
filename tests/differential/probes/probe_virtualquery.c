#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdint.h>

void dump_region(const char *label, uintptr_t base, uintptr_t size) {
    // dump helper - not used
    (void)label;
}

int main(){
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
    printf("  \"probe\": \"probe_virtualquery\",\n");
    printf("  \"windows_build\": \"%lu.%lu\",\n", (unsigned long)build,(unsigned long)ubr);
    printf("  \"windows_version\": \"%lu.%lu.%lu\",\n", (unsigned long)major,(unsigned long)minor,(unsigned long)build);
    printf("  \"arch\": \"x64\",\n");
    printf("  \"page_size\": %lu,\n", (unsigned long)4096);
    printf("  \"alloc_granularity\": %lu,\n", (unsigned long)65536);

    // Scripted allocation pattern
    // Step 1: Reserve 64KB
    SIZE_T reserveSize = 0x10000; // 64KB = alloc granularity
    void *base = VirtualAlloc(NULL, reserveSize, MEM_RESERVE, PAGE_READWRITE);
    if(!base){
        printf("  \"error\": \"VirtualAlloc reserve failed %lu\",\n", GetLastError());
        printf("}\n");
        return 1;
    }
    printf("  \"reserve_base\": \"0x%llX\",\n", (unsigned long long)(uintptr_t)base);
    printf("  \"reserve_size\": %llu,\n", (unsigned long long)reserveSize);

    // Step 2: Commit first 4KB RW, second 4KB RO, 4th 4KB RX
    void *c1 = VirtualAlloc(base, 0x1000, MEM_COMMIT, PAGE_READWRITE);
    void *c2 = VirtualAlloc((char*)base+0x1000, 0x1000, MEM_COMMIT, PAGE_READONLY);
    void *c3 = VirtualAlloc((char*)base+0x4000, 0x1000, MEM_COMMIT, PAGE_EXECUTE_READ);
    // Leave 0x2000-0x3000 and 0x5000-0x10000 as RESERVE
    printf("  \"commits\": [\n");
    printf("    {\"addr\":\"0x%llX\",\"size\":4096,\"protect\":4,\"result\":\"%s\"},\n", (unsigned long long)(uintptr_t)base, c1?"success":"fail");
    printf("    {\"addr\":\"0x%llX\",\"size\":4096,\"protect\":2,\"result\":\"%s\"},\n", (unsigned long long)((uintptr_t)base+0x1000), c2?"success":"fail");
    printf("    {\"addr\":\"0x%llX\",\"size\":4096,\"protect\":32,\"result\":\"%s\"}\n", (unsigned long long)((uintptr_t)base+0x4000), c3?"success":"fail");
    printf("  ],\n");

    // Step 3: VirtualProtect subrange of first commit to PAGE_READONLY -> should split
    DWORD oldProt;
    BOOL vp = VirtualProtect(base, 0x1000, PAGE_READONLY, &oldProt);
    printf("  \"virtualprotect\": {\"addr\":\"0x%llX\",\"size\":4096,\"new_protect\":2,\"old_protect\":%lu,\"success\":%s},\n",
        (unsigned long long)(uintptr_t)base, (unsigned long)oldProt, vp?"true":"false");

    // Dump MEMORY_BASIC_INFORMATION for every region in [base, base+0x10000]
    printf("  \"query_before_free\": [\n");
    uintptr_t cur = (uintptr_t)base;
    uintptr_t end = (uintptr_t)base + reserveSize;
    int first=1;
    while(cur < end){
        MEMORY_BASIC_INFORMATION mbi;
        SIZE_T ret = VirtualQuery((void*)cur, &mbi, sizeof(mbi));
        if(ret==0) break;
        if(!first) printf(",\n");
        first=0;
        printf("    {\"BaseAddress\":\"0x%llX\",\"AllocationBase\":\"0x%llX\",\"AllocationProtect\":%lu,\"RegionSize\":%llu,\"State\":%lu,\"Protect\":%lu,\"Type\":%lu}",
            (unsigned long long)(uintptr_t)mbi.BaseAddress,
            (unsigned long long)(uintptr_t)mbi.AllocationBase,
            (unsigned long)mbi.AllocationProtect,
            (unsigned long long)mbi.RegionSize,
            (unsigned long)mbi.State,
            (unsigned long)mbi.Protect,
            (unsigned long)mbi.Type);
        if(mbi.RegionSize==0) break;
        cur = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        if(cur <= (uintptr_t)mbi.BaseAddress) break; // overflow
    }
    printf("\n  ],\n");

    // Step 4: Query a free address (just beyond reserve) to see MEM_FREE coalescing
    {
        MEMORY_BASIC_INFORMATION mbi;
        void *freeAddr = (char*)base + reserveSize + 0x1000;
        // Find a free region by querying high address
        SIZE_T r = VirtualQuery(freeAddr, &mbi, sizeof(mbi));
        printf("  \"free_query\": {\"query_addr\":\"0x%llX\",\"BaseAddress\":\"0x%llX\",\"AllocationBase\":\"0x%llX\",\"RegionSize\":%llu,\"State\":%lu,\"Protect\":%lu,\"Type\":%lu,\"ret\":%llu},\n",
            (unsigned long long)(uintptr_t)freeAddr,
            (unsigned long long)(uintptr_t)mbi.BaseAddress,
            (unsigned long long)(uintptr_t)mbi.AllocationBase,
            (unsigned long long)mbi.RegionSize,
            (unsigned long)mbi.State,
            (unsigned long)mbi.Protect,
            (unsigned long)mbi.Type,
            (unsigned long long)r);
    }

    // Step 5: Decommit middle committed page (0x1000)
    SIZE_T decommitSize = 0x1000;
    void *decommitBase = (char*)base + 0x1000;
    BOOL vd = VirtualFree(decommitBase, decommitSize, MEM_DECOMMIT);
    printf("  \"decommit\": {\"addr\":\"0x%llX\",\"size\":%llu,\"success\":%s},\n",
        (unsigned long long)(uintptr_t)decommitBase, (unsigned long long)decommitSize, vd?"true":"false");

    printf("  \"query_after_decommit\": [\n");
    cur = (uintptr_t)base;
    first=1;
    while(cur < end){
        MEMORY_BASIC_INFORMATION mbi;
        SIZE_T ret = VirtualQuery((void*)cur, &mbi, sizeof(mbi));
        if(ret==0) break;
        if(!first) printf(",\n");
        first=0;
        printf("    {\"BaseAddress\":\"0x%llX\",\"AllocationBase\":\"0x%llX\",\"AllocationProtect\":%lu,\"RegionSize\":%llu,\"State\":%lu,\"Protect\":%lu,\"Type\":%lu}",
            (unsigned long long)(uintptr_t)mbi.BaseAddress,
            (unsigned long long)(uintptr_t)mbi.AllocationBase,
            (unsigned long)mbi.AllocationProtect,
            (unsigned long long)mbi.RegionSize,
            (unsigned long)mbi.State,
            (unsigned long)mbi.Protect,
            (unsigned long)mbi.Type);
        if(mbi.RegionSize==0) break;
        cur = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        if(cur <= (uintptr_t)mbi.BaseAddress) break;
    }
    printf("\n  ],\n");

    // Step 6: Re-commit same page with different protect to test coalescing
    void *c4 = VirtualAlloc(decommitBase, 0x1000, MEM_COMMIT, PAGE_READWRITE);
    printf("  \"recommit\": {\"addr\":\"0x%llX\",\"protect\":4,\"result\":\"%s\"},\n",
        (unsigned long long)(uintptr_t)decommitBase, c4?"success":"fail");

    printf("  \"query_after_recommit\": [\n");
    cur = (uintptr_t)base;
    first=1;
    while(cur < end){
        MEMORY_BASIC_INFORMATION mbi;
        SIZE_T ret = VirtualQuery((void*)cur, &mbi, sizeof(mbi));
        if(ret==0) break;
        if(!first) printf(",\n");
        first=0;
        printf("    {\"BaseAddress\":\"0x%llX\",\"AllocationBase\":\"0x%llX\",\"AllocationProtect\":%lu,\"RegionSize\":%llu,\"State\":%lu,\"Protect\":%lu,\"Type\":%lu}",
            (unsigned long long)(uintptr_t)mbi.BaseAddress,
            (unsigned long long)(uintptr_t)mbi.AllocationBase,
            (unsigned long)mbi.AllocationProtect,
            (unsigned long long)mbi.RegionSize,
            (unsigned long)mbi.State,
            (unsigned long)mbi.Protect,
            (unsigned long)mbi.Type);
        if(mbi.RegionSize==0) break;
        cur = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        if(cur <= (uintptr_t)mbi.BaseAddress) break;
    }
    printf("\n  ],\n");

    // Step 7: Test MEM_RELEASE requires base == alloc base and size 0 semantics via VirtualFree
    // We keep reserve for later release test
    // Query AllocationProtect vs Protect distinction
    {
        MEMORY_BASIC_INFORMATION mbi;
        VirtualQuery(base, &mbi, sizeof(mbi));
        printf("  \"alloc_protect_check\": {\"AllocationProtect\":%lu,\"Protect\":%lu,\"note\":\"AllocationProtect should be initial reserve protect, Protect is current commit protect\"},\n",
            (unsigned long)mbi.AllocationProtect, (unsigned long)mbi.Protect);
    }

    // Cleanup: release entire reservation
    BOOL rel = VirtualFree(base, 0, MEM_RELEASE);
    printf("  \"release\": {\"addr\":\"0x%llX\",\"success\":%s, \"lasterror\":%lu},\n",
        (unsigned long long)(uintptr_t)base, rel?"true":"false", (unsigned long)GetLastError());
    // Verify free after release
    {
        MEMORY_BASIC_INFORMATION mbi;
        VirtualQuery(base, &mbi, sizeof(mbi));
        printf("  \"after_release\": {\"BaseAddress\":\"0x%llX\",\"State\":%lu,\"RegionSize\":%llu}\n",
            (unsigned long long)(uintptr_t)mbi.BaseAddress, (unsigned long)mbi.State, (unsigned long long)mbi.RegionSize);
    }
    printf("}\n");
    return 0;
}
