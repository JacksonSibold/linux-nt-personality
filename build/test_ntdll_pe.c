/*
 * test_ntdll_pe.c - verifies ntdll.dll + kernel32.dll produced by Makefile.ntdll
 */
#include "pe_parse.h"
#include "pe_format.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static int failures = 0;

#define CHECK(cond, fmt, ...) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: " fmt "\n", ##__VA_ARGS__); failures++; } \
    else { printf("PASS: " fmt "\n", ##__VA_ARGS__); } \
} while(0)

static uint8_t *load_file(const char *path, size_t *out_sz) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return NULL; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc(sz);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, sz, f) != (size_t)sz) { fclose(f); free(buf); return NULL; }
    fclose(f);
    *out_sz = (size_t)sz;
    return buf;
}

struct export_check {
    const char **names;
    size_t count;
    int found;
    int *present;
};

static pe_status_t export_cb(void *user, const pe_export_entry_t *ent) {
    struct export_check *st = user;
    if (!ent->name) return PE_OK;
    for (size_t i=0;i<st->count;i++) {
        if (st->names[i] && ent->name_len == strlen(st->names[i]) && memcmp(ent->name, st->names[i], ent->name_len)==0) {
            if (!st->present[i]) {
                st->present[i]=1;
                st->found++;
            }
            break;
        }
    }
    return PE_OK;
}

static int verify_one_stub(pe_context_t *ctx, uint32_t rva, uint32_t expected_ssn, const char *name) {
    uint32_t off;
    pe_status_t s = pe_rva_to_offset(ctx, rva, &off);
    if (s != PE_OK) {
        fprintf(stderr, "FAIL: stub %s rva 0x%x -> offset failed %d\n", name, rva, s);
        failures++;
        return 0;
    }
    if (off + 11 > ctx->size) {
        fprintf(stderr, "FAIL: stub %s offset out of bounds\n", name);
        failures++;
        return 0;
    }
    const uint8_t *p = ctx->data + off;
    uint8_t expect[11];
    expect[0]=0x49; expect[1]=0x89; expect[2]=0xCA;
    expect[3]=0xB8;
    expect[4]= expected_ssn & 0xFF;
    expect[5]= (expected_ssn>>8) & 0xFF;
    expect[6]= (expected_ssn>>16) & 0xFF;
    expect[7]= (expected_ssn>>24) & 0xFF;
    expect[8]=0x0F; expect[9]=0x05; expect[10]=0xC3;
    if (memcmp(p, expect, 11)!=0) {
        fprintf(stderr, "FAIL: stub %s ssn 0x%x bytes mismatch: got ", name, expected_ssn);
        for(int i=0;i<11;i++) fprintf(stderr, "%02X ", p[i]);
        fprintf(stderr, " expected ");
        for(int i=0;i<11;i++) fprintf(stderr, "%02X ", expect[i]);
        fprintf(stderr, "\n");
        failures++;
        return 0;
    }
    return 1;
}

static pe_status_t import_dll_cb(void *user, const char *dll_name, size_t dll_len, uint32_t iat_rva, uint32_t ilt_rva) {
    int *cnt = user;
    (*cnt)++;
    printf("  import DLL: %.*s (iat 0x%x ilt 0x%x)\n", (int)dll_len, dll_name, iat_rva, ilt_rva);
    return PE_OK;
}

/* Globals for stub verification - used to avoid nested functions */
static pe_context_t *g_ctx;
static struct { char name[64]; uint32_t ssn; } g_map[256];
static size_t g_map_cnt;
static int g_ok, g_total;

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s ntdll.dll kernel32.dll\n", argv[0]);
        return 1;
    }
    const char *ntdll_path = argv[1];
    const char *k32_path = argv[2];

    size_t ntdll_sz;
    uint8_t *ntdll_data = load_file(ntdll_path, &ntdll_sz);
    if (!ntdll_data) return 1;
    pe_context_t nctx;
    pe_context_init(&nctx, ntdll_data, ntdll_sz);
    pe_status_t s = pe_validate(&nctx);
    CHECK(s==PE_OK, "ntdll.dll pe_validate => %d (expected 0)", s);
    if (s==PE_OK) {
        CHECK(nctx.optional_magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC, "ntdll optional magic 0x%x == 0x20B", nctx.optional_magic);
        CHECK(nctx.num_sections >0 && nctx.num_sections <=96, "ntdll sections %u in range", nctx.num_sections);
        printf("ntdll.dll: ImageBase 0x%llx SizeOfImage 0x%x Entry 0x%x Sections %u\n",
            (unsigned long long)nctx.image_base, nctx.size_of_image, nctx.entry_point, nctx.num_sections);
    }

    char exp_names_buf[512][64];
    const char *exp_ptrs[512];
    size_t exp_count=0;
    FILE *def = fopen("src/ntdll/syscall_table.def","r");
    if (!def) def = fopen("/path/to/ntpers-research/src/ntdll/syscall_table.def","r");
    if (def) {
        char line[512];
        while (fgets(line, sizeof(line), def)) {
            char name[64]; char ssn[16];
            if (sscanf(line, " NT_SYSCALL ( %63[^,], %15[^,]", name, ssn)==2) {
                char *p=name; while(*p==' '||*p=='\t') p++; char *e=p+strlen(p)-1; while(e>p && (*e==' '||*e=='\t')) *e--=0;
                strncpy(exp_names_buf[exp_count], p, 63);
                exp_names_buf[exp_count][63]=0;
                exp_ptrs[exp_count]=exp_names_buf[exp_count];
                exp_count++;
                char zw[64];
                if (strncmp(p,"Nt",2)==0) snprintf(zw,sizeof(zw),"Zw%s",p+2);
                else snprintf(zw,sizeof(zw),"Zw%s",p);
                strncpy(exp_names_buf[exp_count], zw, 63);
                exp_names_buf[exp_count][63]=0;
                exp_ptrs[exp_count]=exp_names_buf[exp_count];
                exp_count++;
            }
        }
        fclose(def);
    } else {
        fprintf(stderr, "WARN: could not open syscall_table.def for export list\n");
    }
    const char *extra[] = {
        "LdrLoadDll","LdrUnloadDll","LdrGetDllHandle","LdrGetProcedureAddress",
        "RtlNtStatusToDosError","RtlVirtualUnwind","RtlLookupFunctionEntry","RtlCaptureContext","RtlUnwindEx","RtlRaiseException",
        "AddVectoredExceptionHandler","RemoveVectoredExceptionHandler","KiUserExceptionDispatcher",NULL
    };
    for(int i=0;extra[i];i++){
        strncpy(exp_names_buf[exp_count], extra[i], 63);
        exp_ptrs[exp_count]=exp_names_buf[exp_count];
        exp_count++;
    }

    int *present = calloc(exp_count, sizeof(int));
    struct export_check ec = { exp_ptrs, exp_count, 0, present };
    s = pe_exports_walk(&nctx, export_cb, &ec);
    CHECK(s==PE_OK, "ntdll exports walk status %d", s);
    for(size_t i=0;i<exp_count;i++){
        if (!present[i]) {
            fprintf(stderr, "FAIL: ntdll missing export %s\n", exp_ptrs[i]);
            failures++;
        }
    }
    if (failures==0) printf("PASS: all %zu expected ntdll exports present (found %d)\n", exp_count, ec.found);

    // Build map name->SSN for stub verification
    g_map_cnt=0;
    def = fopen("src/ntdll/syscall_table.def","r");
    if (!def) def = fopen("/path/to/ntpers-research/src/ntdll/syscall_table.def","r");
    if (def) {
        char line[512];
        while (fgets(line, sizeof(line), def)) {
            char name[64], ssn_str[16];
            if (sscanf(line, " NT_SYSCALL ( %63[^,], %15[^,]", name, ssn_str)==2) {
                char *p=name; while(*p==' '||*p=='\t') p++;
                char *e=p+strlen(p)-1; while(e>p && (*e==' '||*e=='\t')) *e--=0;
                char *ss=ssn_str; while(*ss==' '||*ss=='\t') ss++;
                uint32_t ssn = (uint32_t)strtoul(ss, NULL, 0);
                strncpy(g_map[g_map_cnt].name, p, 63);
                g_map[g_map_cnt].ssn = ssn;
                g_map_cnt++;
            }
        }
        fclose(def);
    }
    g_ctx = &nctx;
    g_ok=0; g_total=0;
    // Walk export directory manually to verify stubs
    pe_data_directory_t dd;
    s = pe_directory_get(&nctx, IMAGE_DIRECTORY_ENTRY_EXPORT, &dd);
    if (s==PE_OK && dd.rva!=0) {
        uint32_t off;
        s = pe_rva_to_offset(&nctx, dd.rva, &off);
        if (s==PE_OK) {
            IMAGE_EXPORT_DIRECTORY ed;
            s = pe_read_export_directory(&nctx, off, &ed);
            if (s==PE_OK) {
                uint32_t names_off, ords_off, funcs_off;
                pe_rva_to_offset(&nctx, ed.AddressOfNames, &names_off);
                pe_rva_to_offset(&nctx, ed.AddressOfNameOrdinals, &ords_off);
                pe_rva_to_offset(&nctx, ed.AddressOfFunctions, &funcs_off);
                for(uint32_t i=0;i<ed.NumberOfNames;i++){
                    uint32_t name_rva = pe_file_u32(&nctx, names_off + i*4);
                    uint32_t name_off;
                    if (pe_rva_to_offset(&nctx, name_rva, &name_off)!=PE_OK) continue;
                    const char *sym; size_t sym_len;
                    if (pe_read_asciiz(&nctx, name_off, &sym, &sym_len)!=PE_OK) continue;
                    for(size_t m=0;m<g_map_cnt;m++){
                        if (sym_len==strlen(g_map[m].name) && memcmp(sym,g_map[m].name,sym_len)==0){
                            uint16_t ord = pe_file_u16(&nctx, ords_off + i*2);
                            uint32_t rva = pe_file_u32(&nctx, funcs_off + ord*4);
                            if (rva >= dd.rva && rva < dd.rva+dd.size) {
                                continue;
                            }
                            g_total++;
                            if (verify_one_stub(g_ctx, rva, g_map[m].ssn, g_map[m].name)) g_ok++;
                            break;
                        }
                    }
                }
            }
        }
    }
    CHECK(g_total== (int)g_map_cnt, "stub count found %d expected %zu", g_total, g_map_cnt);
    CHECK(g_ok==g_total && g_total>0, "stub byte-pattern verification %d/%d passed", g_ok, g_total);

    int import_cnt=0;
    s = pe_imports_walk(&nctx, import_dll_cb, NULL, &import_cnt);
    CHECK(s==PE_OK, "ntdll imports walk status %d", s);
    CHECK(import_cnt==0, "ntdll has zero imports (found %d)", import_cnt);

    size_t k32_sz;
    uint8_t *k32_data = load_file(k32_path, &k32_sz);
    if (!k32_data) { failures++; goto done; }
    pe_context_t kctx;
    pe_context_init(&kctx, k32_data, k32_sz);
    s = pe_validate(&kctx);
    CHECK(s==PE_OK, "kernel32.dll pe_validate %d", s);
    if (s==PE_OK) {
        CHECK(kctx.optional_magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC, "kernel32 magic 0x%x", kctx.optional_magic);
    }
    const char *k32_exps[] = {"GetStdHandle","WriteFile","ReadFile","CreateFileW","CloseHandle","ExitProcess","GetLastError","SetLastError","VirtualAlloc","VirtualFree","VirtualProtect","GetProcAddress","LoadLibraryW","HeapAlloc","HeapFree","GetProcessHeap",NULL};
    size_t k32_cnt=0; while(k32_exps[k32_cnt])k32_cnt++;
    int *kpresent = calloc(k32_cnt,sizeof(int));
    struct export_check kec = { k32_exps, k32_cnt, 0, kpresent };
    s = pe_exports_walk(&kctx, export_cb, &kec);
    CHECK(s==PE_OK, "kernel32 exports walk %d", s);
    for(size_t i=0;i<k32_cnt;i++) if(!kpresent[i]) { fprintf(stderr,"FAIL: kernel32 missing %s\n", k32_exps[i]); failures++; }
    if(failures==0) printf("PASS: kernel32 expected exports present %d/%zu\n", kec.found, k32_cnt);
    free(kpresent);
    int k_import_cnt=0;
    s = pe_imports_walk(&kctx, import_dll_cb, NULL, &k_import_cnt);
    CHECK(s==PE_OK, "kernel32 imports walk %d", s);
    CHECK(k_import_cnt>0, "kernel32 has imports (found %d, expected >0)", k_import_cnt);
    free(k32_data);

done:
    free(ntdll_data);
    free(present);
    if (failures==0) {
        printf("\n=== ALL CHECKS PASSED ===\n");
        return 0;
    } else {
        printf("\n=== %d FAILURES ===\n", failures);
        return 1;
    }
}
