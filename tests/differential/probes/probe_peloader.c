#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

// Probe: Windows PE loader oracle
// Usage: probe_peloader.exe <pe_path>
// Attempts SEC_IMAGE mapping and LoadLibraryEx, reports Windows accept/reject
// Also supports batch mode: probe_peloader.exe --batch (reads paths from stdin)
// Output: JSON object per file or single JSON for one file

static void print_windows_version(void) {
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    OSVERSIONINFOEXW ovi = { sizeof(ovi) };
    typedef LONG (WINAPI *PFN_RtlGetVersion)(OSVERSIONINFOEXW*);
    PFN_RtlGetVersion pRtlGetVersion = (PFN_RtlGetVersion)GetProcAddress(ntdll, "RtlGetVersion");
    DWORD build=0, ubr=0, major=0, minor=0;
    if (pRtlGetVersion) {
        pRtlGetVersion(&ovi);
        major=ovi.dwMajorVersion; minor=ovi.dwMinorVersion; build=ovi.dwBuildNumber;
    }
    HKEY hkey; DWORD ubrVal=0, cb=sizeof(ubrVal);
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",0,KEY_READ,&hkey)==ERROR_SUCCESS){
        RegQueryValueExA(hkey,"UBR",NULL,NULL,(BYTE*)&ubrVal,&cb); RegCloseKey(hkey); ubr=ubrVal;
    }
    printf("  \"windows_build\": \"%lu.%lu\",\n", (unsigned long)build, (unsigned long)ubr);
    printf("  \"windows_version\": \"%lu.%lu.%lu\",\n", (unsigned long)major, (unsigned long)minor, (unsigned long)build);
    printf("  \"arch\": \"x64\",\n");
}

static void probe_file(const char *path, int is_first, int is_batch) {
    // Attempt SEC_IMAGE mapping
    HANDLE hFile = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD sec_image_error = 0;
    int sec_image_accept = 0;
    DWORD last_err_fileopen = 0;
    if (hFile == INVALID_HANDLE_VALUE) {
        last_err_fileopen = GetLastError();
        sec_image_error = last_err_fileopen;
        sec_image_accept = 0;
    } else {
        HANDLE hMap = CreateFileMappingA(hFile, NULL, PAGE_READONLY | SEC_IMAGE, 0, 0, NULL);
        if (hMap == NULL) {
            sec_image_error = GetLastError();
            sec_image_accept = 0;
        } else {
            sec_image_accept = 1;
            sec_image_error = 0;
            // Try to map view to ensure it's really mappable
            LPVOID view = MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0);
            if (view) UnmapViewOfFile(view);
            CloseHandle(hMap);
        }
        CloseHandle(hFile);
    }

    // Attempt LoadLibraryEx (for DLLs and EXEs)
    HMODULE hMod = LoadLibraryExA(path, NULL, DONT_RESOLVE_DLL_REFERENCES);
    DWORD ll_error = 0;
    int ll_accept = 0;
    if (hMod == NULL) {
        ll_error = GetLastError();
        ll_accept = 0;
    } else {
        ll_accept = 1;
        ll_error = 0;
        FreeLibrary(hMod);
    }

    // Also try CreateProcess for EXE (dry-run): we don't actually create process, but we can check if it's executable via GetBinaryType
    DWORD binType=0;
    BOOL isExe = GetBinaryTypeA(path, &binType);
    // Overall Windows verdict: accept if SEC_IMAGE succeeded (most stringent)
    int windows_accept = sec_image_accept;

    if (!is_batch) {
        // single file mode: output full envelope
        // caller handles envelope
    }

    // Emit JSON entry - escape backslashes for valid JSON
    printf("    {\"path\":\"");
    for (const char *c = path; *c; ++c) {
        if (*c == '\\') printf("\\\\");
        else if (*c == '\"') printf("\\\"");
        else if ((unsigned char)*c < 0x20) printf("\\u%04x", (unsigned char)*c);
        else putchar(*c);
    }
    printf("\",");
    printf("\"sec_image_accept\":%s,", sec_image_accept ? "true" : "false");
    printf("\"sec_image_error\":%lu,", (unsigned long)sec_image_error);
    printf("\"sec_image_error_hex\":\"0x%08lX\",", (unsigned long)sec_image_error);
    printf("\"loadlibrary_accept\":%s,", ll_accept ? "true" : "false");
    printf("\"loadlibrary_error\":%lu,", (unsigned long)ll_error);
    printf("\"loadlibrary_error_hex\":\"0x%08lX\",", (unsigned long)ll_error);
    printf("\"is_exe\":%s,", isExe ? "true" : "false");
    printf("\"bin_type\":%lu,", (unsigned long)binType);
    printf("\"windows_accept\":%s", windows_accept ? "true" : "false");
    if (last_err_fileopen) {
        printf(",\"file_open_error\":%lu", (unsigned long)last_err_fileopen);
    }
    printf("}");
}

int main(int argc, char *argv[]) {
    int batch = 0;
    const char *single_path = NULL;
    if (argc >= 2 && strcmp(argv[1], "--batch")==0) batch = 1;
    else if (argc >= 2) single_path = argv[1];

    // Header for JSON
    printf("{\n");
    printf("  \"probe\": \"probe_peloader\",\n");
    print_windows_version();

    if (batch) {
        // Read paths from stdin line by line
        printf("  \"results\": [\n");
        char line[4096];
        int first=1;
        while (fgets(line, sizeof(line), stdin)) {
            // strip newline
            size_t len = strlen(line);
            while (len>0 && (line[len-1]=='\n' || line[len-1]=='\r')) line[--len]='\0';
            if (len==0) continue;
            if (!first) printf(",\n");
            probe_file(line, first, 1);
            first=0;
        }
        printf("\n  ]\n");
    } else if (single_path) {
        printf("  \"results\": [\n");
        probe_file(single_path, 1, 0);
        printf("\n  ]\n");
    } else {
        // No args: test good corpus automatically
        printf("  \"results\": [\n");
        const char *defaults[] = {
            "C:\\Users\\Jackson\\Desktop\\02 - Personal\\winapi-native-os\\tmp_test_dummy",
            NULL
        };
        // Try to probe default corpus if exists via /tmp/pecorpus mapped to C:\Users\...\AppData\Local\Temp\opencode?
        // We'll probe the actual files if they exist
        const char *candidates[] = {
            "C:\\tmp\\pecorpus\\pe_smoke.exe",
            "C:\\Users\\Jackson\\AppData\\Local\\Temp\\opencode\\pe_smoke.exe",
            NULL
        };
        // Just emit placeholder - caller should pass explicit path
        printf("    {\"error\":\"no path provided, usage: probe_peloader.exe <pe_path> or --batch\"}\n");
        printf("  ]\n");
    }
    printf("}\n");
    return 0;
}
