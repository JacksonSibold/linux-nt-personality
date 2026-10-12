/*
 * check_ntstatus.c - Regression checker for NTSTATUS->DOS mapping
 *
 * Loads tests/differential/golden/latest/probe_ntstatus.json and asserts
 * that RtlNtStatusToDosError matches every entry. This prevents silent
 * regressions where the table is modified without updating goldens.
 *
 * Build: gcc -I../../src/ntdll -o check_ntstatus check_ntstatus.c ../../src/ntdll/ntstatus_to_dos.c
 * Run: ./check_ntstatus [path_to_json]
 * Exit 0 on success, 1 on mismatch.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "nt_types.h"
#include "ntstatus.h"

ULONG RtlNtStatusToDosError(NTSTATUS Status);

static int failures = 0;
static int total = 0;

static void check_mapping(uint32_t hex, ULONG expected, const char *name) {
    NTSTATUS st = (NTSTATUS)(int32_t)hex;
    ULONG got = RtlNtStatusToDosError(st);
    total++;
    if (got != expected) {
        failures++;
        fprintf(stderr, "MISMATCH %s 0x%08X: expected %lu got %lu\n",
                name ? name : "?", (unsigned)hex, (unsigned long)expected, (unsigned long)got);
    }
}

// Very small JSON scanner: looks for "ntstatus_hex":"0x........","dos":123
static void scan_golden(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "cannot open %s\n", path);
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = (char*)malloc(sz+1);
    if (!buf) { fclose(f); exit(1); }
    size_t n = fread(buf, 1, sz, f);
    buf[n]='\0';
    fclose(f);
    // Skip BOM
    char *p = buf;
    if (n >=3 && (unsigned char)p[0]==0xEF && (unsigned char)p[1]==0xBB && (unsigned char)p[2]==0xBF) p+=3;

    // Find rtl_mappings section
    char *rtl_start = strstr(p, "\"rtl_mappings\"");
    char *fallback_start = strstr(p, "\"fallback_tests\"");
    char *size_probe_start = strstr(p, "\"size_probe\"");
    if (rtl_start) {
        // find the array start '[' after rtl_mappings
        char *arr = strchr(rtl_start, '[');
        if (arr) {
            // find matching ']' before fallback or size_probe
            char *section_end = fallback_start ? fallback_start : (size_probe_start ? size_probe_start : p+n);
            // scan within [arr, section_end)
            char *cur = arr;
            while ((cur = strstr(cur, "\"ntstatus_hex\"")) != NULL && cur < section_end) {
                char *colon = strchr(cur, ':');
                if (!colon || colon > section_end) break;
                char *q1 = strchr(colon, '"');
                if (!q1 || q1 > section_end) break;
                char *q2 = strchr(q1+1, '"');
                if (!q2 || q2 > section_end) break;
                char hexstr[32];
                size_t hlen = q2 - q1 -1;
                if (hlen >= sizeof(hexstr)) hlen = sizeof(hexstr)-1;
                memcpy(hexstr, q1+1, hlen);
                hexstr[hlen]='\0';
                uint32_t hex = 0;
                sscanf(hexstr, "0x%x", &hex);
                // find dos after this but before next ntstatus_hex or section_end
                char *next_hex = strstr(q2+1, "\"ntstatus_hex\"");
                char *dos_key = strstr(q2, "\"dos\"");
                if (!dos_key || dos_key > section_end || (next_hex && dos_key > next_hex)) { cur = q2+1; continue; }
                char *dcolon = strchr(dos_key, ':');
                if (!dcolon) { cur = q2+1; continue; }
                char *dstart = dcolon+1;
                while (*dstart && (*dstart==' '||*dstart=='\t')) dstart++;
                ULONG dos = (ULONG)strtoul(dstart, NULL, 10);
                // find name
                char *name = NULL;
                char namebuf[128] = {0};
                char *search_start = cur - 500;
                if (search_start < arr) search_start = arr;
                char *last_name = NULL;
                char *tmp = search_start;
                while ((tmp = strstr(tmp, "\"name\"")) != NULL && tmp < cur) {
                    last_name = tmp;
                    tmp += 6;
                }
                if (last_name) {
                    char *nc = strchr(last_name, ':');
                    if (nc) {
                        char *nq1 = strchr(nc, '"');
                        if (nq1) {
                            char *nq2 = strchr(nq1+1, '"');
                            if (nq2 && nq2 < cur) {
                                size_t nlen = nq2 - nq1 -1;
                                if (nlen >= sizeof(namebuf)) nlen = sizeof(namebuf)-1;
                                memcpy(namebuf, nq1+1, nlen);
                                namebuf[nlen]='\0';
                                name = namebuf;
                            }
                        }
                    }
                }
                check_mapping(hex, dos, name);
                cur = q2+1;
            }
        }
    }
    if (fallback_start) {
        char *arr = strchr(fallback_start, '[');
        if (arr) {
            char *section_end = p+n;
            char *cur = arr;
            while ((cur = strstr(cur, "\"ntstatus_hex\"")) != NULL && cur < section_end) {
                char *colon = strchr(cur, ':');
                if (!colon) break;
                char *q1 = strchr(colon, '"');
                if (!q1) break;
                char *q2 = strchr(q1+1, '"');
                if (!q2) break;
                char hexstr[32];
                size_t hlen = q2 - q1 -1;
                if (hlen >= sizeof(hexstr)) hlen = sizeof(hexstr)-1;
                memcpy(hexstr, q1+1, hlen);
                hexstr[hlen]='\0';
                uint32_t hex = 0;
                sscanf(hexstr, "0x%x", &hex);
                char *next_hex = strstr(q2+1, "\"ntstatus_hex\"");
                char *dos_key = strstr(q2, "\"dos\"");
                if (!dos_key || (next_hex && dos_key > next_hex)) { cur = q2+1; continue; }
                char *dcolon = strchr(dos_key, ':');
                if (!dcolon) { cur = q2+1; continue; }
                char *dstart = dcolon+1;
                while (*dstart && (*dstart==' '||*dstart=='\t')) dstart++;
                ULONG dos = (ULONG)strtoul(dstart, NULL, 10);
                check_mapping(hex, dos, "fallback");
                cur = q2+1;
                // stop after fallback array end
                if (cur > section_end) break;
                // break if we passed the closing ]
                char *arr_end = strchr(arr, ']');
                if (arr_end && cur > arr_end) break;
            }
        }
    }
    free(buf);
}

int main(int argc, char **argv) {
    const char *path = "tests/differential/golden/latest/probe_ntstatus.json";
    if (argc > 1) path = argv[1];
    // Try also default relative to project root if not found
    FILE *test = fopen(path, "rb");
    if (!test) {
        // try alternative path from build dir
        path = "../../tests/differential/golden/latest/probe_ntstatus.json";
        test = fopen(path, "rb");
        if (!test) {
            if(getenv("NTPERS_ROOT")){ static char alt[1024]; snprintf(alt,sizeof alt,"%s/tests/differential/golden/latest/probe_ntstatus.json",getenv("NTPERS_ROOT")); path=alt; }
            test = fopen(path, "rb");
        }
    }
    if (test) fclose(test);
    else {
        fprintf(stderr, "golden file not found, tried %s\n", path);
        return 1;
    }
    scan_golden(path);
    printf("check_ntstatus: %d/%d mappings OK, %d failures\n", total-failures, total, failures);
    if (failures) {
        fprintf(stderr, "FAIL: %d mismatches\n", failures);
        return 1;
    }
    printf("PASS: all mappings match golden\n");
    return 0;
}
