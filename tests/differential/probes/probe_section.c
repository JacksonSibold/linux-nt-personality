#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winternl.h>
#include <stdio.h>
#include <stdint.h>

typedef NTSTATUS (NTAPI *PFN_NtCreateSection)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PLARGE_INTEGER, ULONG, ULONG, HANDLE);
typedef NTSTATUS (NTAPI *PFN_NtMapViewOfSection)(HANDLE, HANDLE, PVOID*, ULONG_PTR, SIZE_T, PLARGE_INTEGER, PSIZE_T, ULONG, ULONG, ULONG);
typedef NTSTATUS (NTAPI *PFN_NtUnmapViewOfSection)(HANDLE, PVOID);
typedef NTSTATUS (NTAPI *PFN_NtClose)(HANDLE);

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

static void dump_mbi(const char *label, void *addr){
    MEMORY_BASIC_INFORMATION mbi;
    SIZE_T r = VirtualQuery(addr, &mbi, sizeof(mbi));
    if(r==0){
        printf("      {\"label\":\"%s\",\"addr\":\"0x%llX\",\"ret\":0},\n", label, (unsigned long long)(uintptr_t)addr);
    } else {
        const char *stateStr = (mbi.State==MEM_COMMIT)?"COMMIT":(mbi.State==MEM_RESERVE?"RESERVE":(mbi.State==MEM_FREE?"FREE":"OTHER"));
        const char *typeStr = (mbi.Type==MEM_PRIVATE)?"PRIVATE":(mbi.Type==MEM_MAPPED?"MAPPED":(mbi.Type==MEM_IMAGE?"IMAGE":"OTHER"));
        printf("      {\"label\":\"%s\",\"addr\":\"0x%llX\",\"Base\":\"0x%llX\",\"AllocBase\":\"0x%llX\",\"AllocProt\":%lu,\"RegionSize\":%llu,\"State\":%lu,\"StateStr\":\"%s\",\"Protect\":%lu,\"Type\":%lu,\"TypeStr\":\"%s\"},\n",
            label,(unsigned long long)(uintptr_t)addr,(unsigned long long)(uintptr_t)mbi.BaseAddress,(unsigned long long)(uintptr_t)mbi.AllocationBase,(unsigned long)mbi.AllocationProtect,(unsigned long long)mbi.RegionSize,(unsigned long)mbi.State,stateStr,(unsigned long)mbi.Protect,(unsigned long)mbi.Type,typeStr);
    }
}

int main(){
    setvbuf(stdout, NULL, _IONBF, 0);
    print_build_header("probe_section");
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    PFN_NtCreateSection pNtCreateSection = (PFN_NtCreateSection)GetProcAddress(ntdll,"NtCreateSection");
    PFN_NtMapViewOfSection pNtMapViewOfSection = (PFN_NtMapViewOfSection)GetProcAddress(ntdll,"NtMapViewOfSection");
    PFN_NtUnmapViewOfSection pNtUnmapViewOfSection = (PFN_NtUnmapViewOfSection)GetProcAddress(ntdll,"NtUnmapViewOfSection");
    PFN_NtClose pNtClose = (PFN_NtClose)GetProcAddress(ntdll,"NtClose");

    printf("  \"page_size\": %lu,\n", (unsigned long)4096);
    printf("  \"alloc_gran\": %lu,\n", (unsigned long)65536);

    // Create temp file for file-backed sections
    char tmpPath[MAX_PATH], tmpFile[MAX_PATH];
    GetTempPathA(sizeof(tmpPath), tmpPath);
    GetTempFileNameA(tmpPath, "sec", 0, tmpFile);
    HANDLE hf = CreateFileA(tmpFile, GENERIC_READ|GENERIC_WRITE, FILE_SHARE_READ|FILE_SHARE_WRITE, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if(hf!=INVALID_HANDLE_VALUE){
        // make it 64K
        SetFilePointer(hf, 65536, NULL, FILE_BEGIN);
        SetEndOfFile(hf);
        CloseHandle(hf);
    }
    HANDLE hFile = CreateFileA(tmpFile, GENERIC_READ|GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    // SEC_COMMIT pagefile section
    HANDLE hSecCommit = NULL;
    LARGE_INTEGER maxSize; maxSize.QuadPart = 65536;
    NTSTATUS stSecCommit = pNtCreateSection ? pNtCreateSection(&hSecCommit, SECTION_MAP_READ|SECTION_MAP_WRITE, NULL, &maxSize, PAGE_READWRITE, SEC_COMMIT, NULL) : -1;
    printf("  \"sec_commit\": {\"ntstatus\":%ld,\"hex\":\"0x%08lX\",\"handle\":\"0x%llX\"},\n", (long)stSecCommit,(unsigned long)(uint32_t)stSecCommit,(unsigned long long)(uintptr_t)hSecCommit);
    // Map view
    PVOID baseCommit = NULL; SIZE_T viewCommit = 0;
    NTSTATUS stMapCommit = -1;
    if(hSecCommit){
        stMapCommit = pNtMapViewOfSection(hSecCommit, GetCurrentProcess(), &baseCommit, 0, 0, NULL, &viewCommit, 1, 0, PAGE_READWRITE);
        printf("  \"map_commit\": {\"ntstatus\":%ld,\"hex\":\"0x%08lX\",\"base\":\"0x%llX\",\"viewSize\":%llu},\n", (long)stMapCommit,(unsigned long)(uint32_t)stMapCommit,(unsigned long long)(uintptr_t)baseCommit,(unsigned long long)viewCommit);
        if(NT_SUCCESS(stMapCommit)){
            // VirtualQuery checks
            printf("  \"vquery_commit\": [\n");
            dump_mbi("commit_base", baseCommit);
            dump_mbi("commit_mid", (char*)baseCommit+4096);
            printf("      {\"sentinel\":true}\n  ],\n");
            // COW test: write to page, check still
            *(volatile int*)baseCommit = 0x12345678;
            printf("  \"cow_write\": {\"value\":%d},\n", *(volatile int*)baseCommit);
            // View offset tests
            // Try map second view with offset 8192 (page aligned but not alloc gran?)
            PVOID base2=NULL; SIZE_T vs2=4096; LARGE_INTEGER off2; off2.QuadPart=8192;
            NTSTATUS stOff2 = pNtMapViewOfSection(hSecCommit, GetCurrentProcess(), &base2, 0, 0, &off2, &vs2, 1, 0, PAGE_READWRITE);
            printf("  \"map_commit_offset_8192\": {\"ntstatus\":%ld,\"hex\":\"0x%08lX\",\"base\":\"0x%llX\"},\n", (long)stOff2,(unsigned long)(uint32_t)stOff2,(unsigned long long)(uintptr_t)base2);
            if(NT_SUCCESS(stOff2)) pNtUnmapViewOfSection(GetCurrentProcess(), base2);
            // Misaligned offset 1 -> should fail STATUS_MAPPED_ALIGNMENT or INVALID_PARAMETER?
            PVOID baseMis=NULL; SIZE_T vsMis=4096; LARGE_INTEGER offMis; offMis.QuadPart=1;
            NTSTATUS stMis = pNtMapViewOfSection(hSecCommit, GetCurrentProcess(), &baseMis, 0, 0, &offMis, &vsMis, 1, 0, PAGE_READWRITE);
            printf("  \"map_misaligned_offset_1\": {\"ntstatus\":%ld,\"hex\":\"0x%08lX\"},\n", (long)stMis,(unsigned long)(uint32_t)stMis);
            // Oversized view (beyond section)
            PVOID baseOver=NULL; SIZE_T vsOver= 131072; LARGE_INTEGER off0; off0.QuadPart=0;
            NTSTATUS stOver = pNtMapViewOfSection(hSecCommit, GetCurrentProcess(), &baseOver, 0, 0, &off0, &vsOver, 1, 0, PAGE_READWRITE);
            printf("  \"map_oversized_128k\": {\"ntstatus\":%ld,\"hex\":\"0x%08lX\"},\n", (long)stOver,(unsigned long)(uint32_t)stOver);
            pNtUnmapViewOfSection(GetCurrentProcess(), baseCommit);
        }
    }

    // SEC_RESERVE
    HANDLE hSecReserve = NULL;
    NTSTATUS stSecReserve = pNtCreateSection ? pNtCreateSection(&hSecReserve, SECTION_MAP_READ|SECTION_MAP_WRITE, NULL, &maxSize, PAGE_READWRITE, SEC_RESERVE, NULL) : -1;
    printf("  \"sec_reserve\": {\"ntstatus\":%ld,\"hex\":\"0x%08lX\",\"handle\":\"0x%llX\"},\n", (long)stSecReserve,(unsigned long)(uint32_t)stSecReserve,(unsigned long long)(uintptr_t)hSecReserve);
    if(hSecReserve){
        PVOID br=NULL; SIZE_T vr=4096;
        NTSTATUS stMapRes = pNtMapViewOfSection(hSecReserve, GetCurrentProcess(), &br, 0, 0, NULL, &vr, 1, 0, PAGE_READWRITE);
        printf("  \"map_reserve\": {\"ntstatus\":%ld,\"hex\":\"0x%08lX\",\"base\":\"0x%llX\",\"viewSize\":%llu},\n", (long)stMapRes,(unsigned long)(uint32_t)stMapRes,(unsigned long long)(uintptr_t)br,(unsigned long long)vr);
        if(NT_SUCCESS(stMapRes)){
            printf("  \"vquery_reserve\": [\n");
            dump_mbi("reserve_base", br);
            printf("      {\"sentinel\":true}\n  ],\n");
            pNtUnmapViewOfSection(GetCurrentProcess(), br);
        }
    }

    // SEC_IMAGE (requires file)
    HANDLE hSecImage = NULL;
    NTSTATUS stSecImage = -1;
    if(hFile && hFile!=INVALID_HANDLE_VALUE){
        // For SEC_IMAGE, MaximumSize must be 0
        stSecImage = pNtCreateSection ? pNtCreateSection(&hSecImage, SECTION_MAP_READ|SECTION_MAP_EXECUTE, NULL, NULL, PAGE_READONLY, SEC_IMAGE, hFile) : -1;
        printf("  \"sec_image\": {\"ntstatus\":%ld,\"hex\":\"0x%08lX\",\"handle\":\"0x%llX\"},\n", (long)stSecImage,(unsigned long)(uint32_t)stSecImage,(unsigned long long)(uintptr_t)hSecImage);
        if(NT_SUCCESS(stSecImage) && hSecImage){
            PVOID bi=NULL; SIZE_T vi=0;
            NTSTATUS stMapImg = pNtMapViewOfSection(hSecImage, GetCurrentProcess(), &bi, 0, 0, NULL, &vi, 1, 0, PAGE_READONLY);
            printf("  \"map_image\": {\"ntstatus\":%ld,\"hex\":\"0x%08lX\",\"base\":\"0x%llX\",\"viewSize\":%llu},\n", (long)stMapImg,(unsigned long)(uint32_t)stMapImg,(unsigned long long)(uintptr_t)bi,(unsigned long long)vi);
            if(NT_SUCCESS(stMapImg)){
                printf("  \"vquery_image\": [\n");
                dump_mbi("image_base", bi);
                printf("      {\"sentinel\":true}\n  ],\n");
                pNtUnmapViewOfSection(GetCurrentProcess(), bi);
            }
        }
    } else {
        printf("  \"sec_image\": {\"ntstatus\":-1,\"hex\":\"0xFFFFFFFF\",\"note\":\"no file\"},\n");
    }

    // File-backed SEC_COMMIT (map file)
    HANDLE hSecFileCommit = NULL;
    NTSTATUS stSecFile = -1;
    if(hFile && hFile!=INVALID_HANDLE_VALUE){
        LARGE_INTEGER fsize; fsize.QuadPart=0;
        stSecFile = pNtCreateSection ? pNtCreateSection(&hSecFileCommit, SECTION_MAP_READ|SECTION_MAP_WRITE, NULL, &fsize, PAGE_READWRITE, SEC_COMMIT, hFile) : -1;
        printf("  \"sec_file_commit\": {\"ntstatus\":%ld,\"hex\":\"0x%08lX\",\"handle\":\"0x%llX\"},\n", (long)stSecFile,(unsigned long)(uint32_t)stSecFile,(unsigned long long)(uintptr_t)hSecFileCommit);
        if(NT_SUCCESS(stSecFile) && hSecFileCommit){
            PVOID bf=NULL; SIZE_T vf=0;
            NTSTATUS stMapF = pNtMapViewOfSection(hSecFileCommit, GetCurrentProcess(), &bf, 0, 0, NULL, &vf, 1, 0, PAGE_READWRITE);
            printf("  \"map_file_commit\": {\"ntstatus\":%ld,\"hex\":\"0x%08lX\",\"base\":\"0x%llX\",\"viewSize\":%llu},\n", (long)stMapF,(unsigned long)(uint32_t)stMapF,(unsigned long long)(uintptr_t)bf,(unsigned long long)vf);
            if(NT_SUCCESS(stMapF)){
                printf("  \"vquery_file_mapped\": [\n");
                dump_mbi("file_base", bf);
                printf("      {\"sentinel\":true}\n  ],\n");
                pNtUnmapViewOfSection(GetCurrentProcess(), bf);
            }
        }
    }

    // SEC_IMAGE with real PE
    HANDLE hRealPe = CreateFileA("C:\\Windows\\System32\\ntdll.dll", GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    HANDLE hSecRealImage = NULL;
    NTSTATUS stRealImage = -1;
    if(hRealPe && hRealPe!=INVALID_HANDLE_VALUE){
        stRealImage = pNtCreateSection ? pNtCreateSection(&hSecRealImage, SECTION_MAP_READ|SECTION_MAP_EXECUTE, NULL, NULL, PAGE_READONLY, SEC_IMAGE, hRealPe) : -1;
        printf("  \"sec_real_image\": {\"ntstatus\":%ld,\"hex\":\"0x%08lX\",\"handle\":\"0x%llX\"},\n", (long)stRealImage,(unsigned long)(uint32_t)stRealImage,(unsigned long long)(uintptr_t)hSecRealImage);
        if(NT_SUCCESS(stRealImage) && hSecRealImage){
            PVOID br=NULL; SIZE_T vr=0;
            NTSTATUS stMapR = pNtMapViewOfSection(hSecRealImage, GetCurrentProcess(), &br, 0, 0, NULL, &vr, 1, 0, PAGE_READONLY);
            printf("  \"map_real_image\": {\"ntstatus\":%ld,\"hex\":\"0x%08lX\",\"base\":\"0x%llX\",\"viewSize\":%llu},\n", (long)stMapR,(unsigned long)(uint32_t)stMapR,(unsigned long long)(uintptr_t)br,(unsigned long long)vr);
            if(NT_SUCCESS(stMapR)){
                printf("  \"vquery_real_image\": [\n");
                dump_mbi("real_image_base", br);
                printf("      {\"sentinel\":true}\n  ],\n");
                pNtUnmapViewOfSection(GetCurrentProcess(), br);
            }
        }
        if(hSecRealImage && pNtClose) pNtClose(hSecRealImage);
        CloseHandle(hRealPe);
    } else {
        printf("  \"sec_real_image\": {\"ntstatus\":-1,\"hex\":\"0xFFFFFFFF\",\"note\":\"cannot open ntdll\"},\n");
    }

    // CreateFileMapping SEC_IMAGE vs SEC_COMMIT comparison via Win32 VirtualQuery types
    // Use CreateFileMappingA directly and MapViewOfFile
    HANDLE hMapCommitWin = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE|SEC_COMMIT, 0, 65536, NULL);
    if(hMapCommitWin){
        void *v = MapViewOfFile(hMapCommitWin, FILE_MAP_WRITE, 0,0,0);
        if(v){
            printf("  \"win32_commit_vquery\": [\n");
            dump_mbi("win32_commit", v);
            printf("      {\"sentinel\":true}\n  ],\n");
            UnmapViewOfFile(v);
        }
        CloseHandle(hMapCommitWin);
    }
    HANDLE hMapReserveWin = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE|SEC_RESERVE, 0, 65536, NULL);
    if(hMapReserveWin){
        void *v = MapViewOfFile(hMapReserveWin, FILE_MAP_WRITE, 0,0,4096);
        if(v){
            printf("  \"win32_reserve_vquery\": [\n");
            dump_mbi("win32_reserve", v);
            printf("      {\"sentinel\":true}\n  ],\n");
            UnmapViewOfFile(v);
        } else {
            printf("  \"win32_reserve_map_failed\": {\"err\":%lu},\n", (unsigned long)GetLastError());
        }
        CloseHandle(hMapReserveWin);
    }

    // Cleanup
    if(hSecCommit && pNtClose) pNtClose(hSecCommit);
    if(hSecReserve && pNtClose) pNtClose(hSecReserve);
    if(hSecImage && pNtClose) pNtClose(hSecImage);
    if(hSecFileCommit && pNtClose) pNtClose(hSecFileCommit);
    if(hFile && hFile!=INVALID_HANDLE_VALUE) CloseHandle(hFile);
    DeleteFileA(tmpFile);
    // remove last comma handling: we printed with commas; ensure valid JSON by adding dummy
    printf("  \"done\": true\n");
    printf("}\n");
    return 0;
}
