/*
 * ntdll_main.c - freestanding ntdll entry and minimal runtime shims
 *
 * Compiles only for __MINGW32__ (Windows PE). For Linux test builds this file
 * is not linked; the loader tests use ldr.c directly.
 */
#ifdef __MINGW32__
#ifdef DUMMYUNIONNAME
#undef DUMMYUNIONNAME
#endif
#ifdef DUMMYSTRUCTNAME
#undef DUMMYSTRUCTNAME
#endif
#include "nt_types.h"
#include "ntstatus.h"
#include "nt_syscalls.h"
#include "ldr.h"
#include "unwind.h"
#include "except.h"
#include "../loader/pe_format.h"
#include "pe_bounds.h"
#include "ldr_pure.h"
#undef DUMMYUNIONNAME
#undef DUMMYSTRUCTNAME

/* DllMain - ntdll entry point. */
BOOL NTAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    (void)hinstDLL; (void)lpvReserved;
    switch (fdwReason) {
        case DLL_PROCESS_ATTACH: break;
        case DLL_THREAD_ATTACH: break;
        case DLL_THREAD_DETACH: break;
        case DLL_PROCESS_DETACH: break;
        default: break;
    }
    return 1;
}
BOOL __stdcall _DllMainCRTStartup(HINSTANCE h, DWORD r, LPVOID v) {
    return DllMain(h, r, v);
}
BOOL __stdcall DllMainCRTStartup(HINSTANCE h, DWORD r, LPVOID v) {
    return DllMain(h, r, v);
}
void NTAPI KiUserExceptionDispatcher(PEXCEPTION_RECORD rec, PCONTEXT ctx) {
    RtlDispatchException(rec, ctx);
}
void NTAPI KiUserApcDispatcher(PCONTEXT ctx, PVOID apc1, PVOID apc2, PVOID func) {
    (void)ctx; (void)apc1; (void)apc2; (void)func;
}

/* Helpers for LdrInitializeThunk */
static inline u16 ldr_load_u16(const void *p) { const unsigned char *b=p; return (u16)b[0] | ((u16)b[1]<<8); }
static inline u32 ldr_load_u32(const void *p) { const unsigned char *b=p; return (u32)b[0]|((u32)b[1]<<8)|((u32)b[2]<<16)|((u32)b[3]<<24); }
static inline u64 ldr_load_u64(const void *p) { const unsigned char *b=p; return (u64)b[0]|((u64)b[1]<<8)|((u64)b[2]<<16)|((u64)b[3]<<24)|((u64)b[4]<<32)|((u64)b[5]<<40)|((u64)b[6]<<48)|((u64)b[7]<<56); }

static void __attribute__((unused)) ldr_ascii_to_wcs(const char *in, WCHAR *out, size_t outsz) {
    size_t i=0; for (; i+1<outsz && in[i]; ++i) out[i]=(WCHAR)(unsigned char)in[i]; out[i]=0;
}
static int ldr_strcasecmp(const char *a, const char *b) {
    while (*a && *b) {
        char ca=*a, cb=*b;
        if (ca>='A'&&ca<='Z') ca=ca-'A'+'a';
        if (cb>='A'&&cb<='Z') cb=cb-'A'+'a';
        if (ca!=cb) return (unsigned char)ca-(unsigned char)cb;
        a++; b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}
static int ldr_strncasecmp(const char *a, const char *b, size_t n) {
    for (size_t i=0;i<n;i++) {
        char ca=a[i], cb=b[i];
        if (ca>='A'&&ca<='Z') ca=ca-'A'+'a';
        if (cb>='A'&&cb<='Z') cb=cb-'A'+'a';
        if (ca!=cb) return (unsigned char)ca-(unsigned char)cb;
        if (ca==0) return 0;
    }
    return 0;
}

static void dbg_print_early(const char *msg) {
    HANDLE h = (HANDLE)(intptr_t)-11;
    IO_STATUS_BLOCK ios;
    SIZE_T len=0; while(msg && msg[len]) len++;
    if (len) NtWriteFile(h, NULL, NULL, NULL, &ios, (PVOID)msg, (ULONG)len, NULL, NULL);
}
static void dbg_print_hex32(const char *prefix, u32 v) {
    char tmp[32]; tmp[0]='0'; tmp[1]='x'; int p=2;
    for (int i=7;i>=0;i--) { unsigned d=(v>> (i*4)) & 0xF; tmp[p++]= d<10?'0'+d:'a'+d-10; }
    tmp[p]='\0';
    dbg_print_early(prefix); dbg_print_early(tmp); dbg_print_early("\n");
}
static void dbg_print_hex64(const char *prefix, u64 v) {
    char tmp[32]; tmp[0]='0'; tmp[1]='x'; int p=2;
    for (int i=15;i>=0;i--) { unsigned d=(v>> (i*4)) & 0xF; tmp[p++]= d<10?'0'+d:'a'+d-10; }
    tmp[p]='\0';
    dbg_print_early(prefix); dbg_print_early(tmp); dbg_print_early("\n");
}
static ULONG ldr_nt_prot_from_chars_early(u32 chars) {
    int r = (chars & 0x40000000) != 0;
    int w = (chars & 0x80000000) != 0;
    int x = (chars & 0x20000000) != 0;
    if (x) {
        if (r && w) return 0x40;
        if (r) return 0x20;
        if (w) return 0x40;
        return 0x10;
    } else {
        if (r && w) return 0x04;
        if (r) return 0x02;
        if (w) return 0x04;
        return 0x02;
    }
}

/* Minimal file mapping via NT syscalls - extended to handle full PE mapping */
static NTSTATUS ldr_nt_map_dll(const char *path, void **out_base, size_t *out_size) {
    WCHAR wpath[1024];
    size_t plen = 0; while (path[plen] && plen < 1023) { wpath[plen]=(WCHAR)(unsigned char)path[plen]; plen++; } wpath[plen]=0;
    for (size_t i=0; wpath[i]; i++) if (wpath[i]=='/') wpath[i]='\\';
    WCHAR ntpath[1024];
    const WCHAR prefix[] = { '\\','?','?','\\',0 };
    size_t pre=4;
    for (size_t i=0;i<pre;i++) ntpath[i]=prefix[i];
    for (size_t i=0;i<=plen;i++) ntpath[pre+i]=wpath[i];
    size_t total_len = pre+plen;
    UNICODE_STRING us;
    us.Length = (USHORT)(total_len*sizeof(WCHAR));
    us.MaximumLength = us.Length+sizeof(WCHAR);
    us.Buffer = ntpath;
    OBJECT_ATTRIBUTES oa;
    oa.Length = sizeof(oa);
    oa.RootDirectory = NULL;
    oa.ObjectName = &us;
    oa.Attributes = 0x40; // OBJ_CASE_INSENSITIVE
    oa.SecurityDescriptor = NULL;
    oa.SecurityQualityOfService = NULL;
    HANDLE h=NULL;
    IO_STATUS_BLOCK ios;
    NTSTATUS st = NtCreateFile(&h, 0x80000000|0x00100080, &oa, &ios, NULL, 0, 1|2, 1, 0x40|0x20, NULL, 0);
    if (!NT_SUCCESS(st) || !h) {
        us.Length = (USHORT)(plen*sizeof(WCHAR));
        us.Buffer = wpath;
        oa.ObjectName = &us;
        st = NtCreateFile(&h, 0x80000000|0x00100080, &oa, &ios, NULL, 0, 1|2, 1, 0x40|0x20, NULL, 0);
        if (!NT_SUCCESS(st) || !h) return STATUS_NO_SUCH_FILE;
    }
    struct { LARGE_INTEGER AllocationSize; LARGE_INTEGER EndOfFile; ULONG NumberOfLinks; BOOLEAN DeletePending; BOOLEAN Directory; } finfo;
    st = NtQueryInformationFile(h, &ios, &finfo, sizeof(finfo), 5);
    /* Raw reads are bounded by the file size (MAIN-1). If the query failed,
     * no bound is known here and NtReadFile still reports short reads. */
    unsigned long long file_limit = ~0ULL;
    if (NT_SUCCESS(st) && finfo.EndOfFile.QuadPart > 0)
        file_limit = (unsigned long long)finfo.EndOfFile.QuadPart;
    void *hdr = NULL;
    SIZE_T hdr_size = 0x1000;
    PVOID base = NULL;
    SIZE_T sz = hdr_size;
    st = NtAllocateVirtualMemory((HANDLE)-1, &base, 0, &sz, 0x2000|0x1000, 0x04);
    if (!NT_SUCCESS(st) || !base) { NtClose(h); return STATUS_NO_MEMORY; }
    hdr = base;
    LARGE_INTEGER off; off.QuadPart=0;
    IO_STATUS_BLOCK rios;
    st = NtReadFile(h, NULL, NULL, NULL, &rios, hdr, 0x1000, &off, NULL);
    if (!NT_SUCCESS(st)) {
        dbg_print_early("ldr_nt_map_dll: NtReadFile header failed path=");
        dbg_print_early(path);
        dbg_print_early(" st=");
        dbg_print_hex32("", (u32)st);
        dbg_print_hex32(" ios.Status ", (u32)rios.DUMMYUNIONNAME.Status);
        dbg_print_hex32(" ios.Info ", (u32)rios.Information);
        NtClose(h);
        return STATUS_INVALID_IMAGE_FORMAT;
    }
    if (rios.Information < 0x40) {
        dbg_print_early("ldr_nt_map_dll: short header read path=");
        dbg_print_early(path);
        dbg_print_hex32(" info ", (u32)rios.Information);
        dbg_print_hex32(" hdr[0] ", ldr_load_u32((unsigned char*)hdr));
        dbg_print_hex32(" hdr[0x3C] ", ldr_load_u32((unsigned char*)hdr+0x3C));
        NtClose(h);
        return STATUS_INVALID_IMAGE_FORMAT;
    }
    u32 nt_off = ldr_load_u32((unsigned char*)hdr + 0x3C);
    if (nt_off >= 0x1000 || nt_off < 0x40) {
        dbg_print_early("ldr_nt_map_dll: bad nt_off path=");
        dbg_print_early(path);
        dbg_print_hex32(" nt_off ", nt_off);
        NtClose(h);
        return STATUS_INVALID_IMAGE_FORMAT;
    }
    u32 sig = ldr_load_u32((unsigned char*)hdr + nt_off);
    if (sig != 0x4550) {
        dbg_print_early("ldr_nt_map_dll: bad sig path=");
        dbg_print_early(path);
        dbg_print_hex32(" sig ", sig);
        dbg_print_hex32(" nt_off ", nt_off);
        dbg_print_hex32(" hdr0 ", ldr_load_u32((unsigned char*)hdr));
        dbg_print_hex32(" e_lfanew ", nt_off);
        // dump first 16 bytes
        for (int i=0;i<16;i++) { char tmpb[8]; tmpb[0]=' '; tmpb[1]=' '; unsigned char b=((unsigned char*)hdr)[i]; unsigned hi=(b>>4)&0xF; unsigned lo=b&0xF; tmpb[2]= hi<10?'0'+hi:'a'+hi-10; tmpb[3]= lo<10?'0'+lo:'a'+lo-10; tmpb[4]='\0'; dbg_print_early(tmpb); }
        dbg_print_early("\n");
        NtClose(h);
        return STATUS_INVALID_IMAGE_FORMAT;
    }
    u16 num_sections = ldr_load_u16((unsigned char*)hdr + nt_off + 6);
    u16 size_opt = ldr_load_u16((unsigned char*)hdr + nt_off + 20);
    u32 oh_off = nt_off + 4 + 20;
    u64 image_base = ldr_load_u64((unsigned char*)hdr + oh_off + 24);
    u32 size_image = ldr_load_u32((unsigned char*)hdr + oh_off + 56);
    u32 size_headers = ldr_load_u32((unsigned char*)hdr + oh_off + 60);
    // Allocate image
    PVOID img_base = NULL;
    SIZE_T img_size = size_image;
    img_base = (PVOID)(ULONG_PTR)image_base;
    SIZE_T try_sz = img_size;
    st = NtAllocateVirtualMemory((HANDLE)-1, &img_base, 0, &try_sz, 0x2000|0x1000, 0x04);
    if (!NT_SUCCESS(st) || !img_base) {
        img_base = NULL;
        try_sz = img_size;
        st = NtAllocateVirtualMemory((HANDLE)-1, &img_base, 0, &try_sz, 0x2000|0x1000, 0x04);
        if (!NT_SUCCESS(st)) { NtClose(h); return STATUS_NO_MEMORY; }
    }
    {
        size_t to_copy = size_headers < 0x1000 ? size_headers : 0x1000;
        for (size_t i=0;i<to_copy;i++) ((unsigned char*)img_base)[i] = ((unsigned char*)hdr)[i];
        if (size_headers > 0x1000) {
            off.QuadPart = 0x1000;
            size_t rem = size_headers - 0x1000;
            st = NtReadFile(h, NULL, NULL, NULL, &rios, (unsigned char*)img_base+0x1000, (ULONG)rem, &off, NULL);
            if (!NT_SUCCESS(st)) { NtClose(h); return STATUS_INVALID_IMAGE_FORMAT; }
        }
    }
    u32 section_table_off = oh_off + size_opt;
    /* MAIN-1: the section table must lie inside the header buffer. */
    if (!pe_range_fits(section_table_off, (unsigned long long)num_sections * 40, 0x1000)) {
        NtClose(h);
        return STATUS_INVALID_IMAGE_FORMAT;
    }
    for (u16 i=0;i<num_sections;i++) {
        u32 sec_off = section_table_off + i*40;
        u32 vsize = ldr_load_u32((unsigned char*)hdr + sec_off + 8);
        u32 vaddr = ldr_load_u32((unsigned char*)hdr + sec_off + 12);
        u32 rawsize = ldr_load_u32((unsigned char*)hdr + sec_off + 16);
        u32 rawptr = ldr_load_u32((unsigned char*)hdr + sec_off + 20);
        u32 vs = vsize ? vsize : rawsize;
        if (vs==0) continue;
        /* MAIN-1: the section must lie inside the image, and its raw data
         * inside the file, before anything is read into it. */
        if (!pe_range_fits(vaddr, vs, size_image)) { NtClose(h); return STATUS_INVALID_IMAGE_FORMAT; }
        if (rawsize && !pe_range_fits(rawptr, rawsize, file_limit)) { NtClose(h); return STATUS_INVALID_IMAGE_FORMAT; }
        void *dst = (unsigned char*)img_base + vaddr;
        if (rawsize) {
            off.QuadPart = rawptr;
            st = NtReadFile(h, NULL, NULL, NULL, &rios, dst, rawsize, &off, NULL);
            if (!NT_SUCCESS(st)) { NtClose(h); return STATUS_INVALID_IMAGE_FORMAT; }
        }
        if (vs > rawsize) {
            for (u32 j=rawsize;j<vs;j++) ((unsigned char*)dst)[j]=0;
        }
    }
    {
        u64 delta = (u64)(ULONG_PTR)img_base - image_base;
        if (delta != 0) {
            u32 num_dirs = ldr_load_u32((unsigned char*)hdr + oh_off + 108);
            if (num_dirs > 5) {
                u32 reloc_rva = ldr_load_u32((unsigned char*)hdr + oh_off + 112 + 5*8);
                u32 reloc_size = ldr_load_u32((unsigned char*)hdr + oh_off + 112 + 5*8 + 4);
                if (reloc_rva && reloc_size) {
                    /* MAIN-2: the directory must lie inside the image. */
                    if (!pe_range_fits(reloc_rva, reloc_size, size_image)) { NtClose(h); return STATUS_INVALID_IMAGE_FORMAT; }
                    unsigned char *reloc = (unsigned char*)img_base + reloc_rva;
                    u32 cursor=0;
                    while (cursor+8 <= reloc_size) {
                        u32 page_rva = ldr_load_u32(reloc+cursor);
                        u32 block_size = ldr_load_u32(reloc+cursor+4);
                        if (block_size==0) break;
                        if (block_size<8 || (block_size%4)!=0) break;
                        if (cursor+block_size > reloc_size) break;
                        u32 nent = (block_size-8)/2;
                        for (u32 e=0;e<nent;e++) {
                            u16 entry = ldr_load_u16(reloc+cursor+8+e*2);
                            u16 type = entry>>12;
                            u16 off2 = entry & 0xFFF;
                            if (type==0) continue;
                            if (type!=10) { NtClose(h); return STATUS_INVALID_IMAGE_FORMAT; }
                            /* MAIN-2: the 8-byte target must lie inside the image. */
                            if (!pe_range_fits((unsigned long long)page_rva + off2, 8, size_image)) { NtClose(h); return STATUS_INVALID_IMAGE_FORMAT; }
                            u64 *p = (u64*)((unsigned char*)img_base + page_rva + off2);
                            *p += delta;
                        }
                        cursor += block_size;
                    }
                }
            }
        }
    }
    // Apply per-section protections (Windows-accurate: .text EXECUTE_READ, .idata READONLY, etc.)
    // Headers: READONLY
    {
        PVOID p = img_base;
        SIZE_T s = (size_headers + 0xFFF) & ~0xFFF;
        if (s > 0 && s <= img_size) {
            ULONG old;
            NtProtectVirtualMemory((HANDLE)-1, &p, &s, 0x02, &old);
        }
    }
    // Sections: protect per Characteristics
    for (u16 si=0; si<num_sections; si++) {
        u32 sec_off2 = section_table_off + si*40;
        u32 vs2 = ldr_load_u32((unsigned char*)hdr + sec_off2 + 8);
        u32 vaddr2 = ldr_load_u32((unsigned char*)hdr + sec_off2 + 12);
        u32 rawsize2 = ldr_load_u32((unsigned char*)hdr + sec_off2 + 16);
        u32 vs2a = vs2 ? vs2 : rawsize2;
        if (vs2a==0) continue;
        u32 chars2 = ldr_load_u32((unsigned char*)hdr + sec_off2 + 36);
        ULONG prot = ldr_nt_prot_from_chars_early(chars2);
        ULONG_PTR va = (ULONG_PTR)img_base + vaddr2;
        ULONG_PTR aligned = va & ~0xFFFULL;
        ULONG_PTR end = (va + vs2a + 0xFFFULL) & ~0xFFFULL;
        SIZE_T sz2 = end - aligned;
        PVOID pp = (PVOID)aligned;
        ULONG old2;
        NTSTATUS pr = NtProtectVirtualMemory((HANDLE)-1, &pp, &sz2, prot, &old2);
        if (!NT_SUCCESS(pr)) {
            // Non-fatal: log but continue (kernel may have coalesced)
            dbg_print_early("ldr_nt_map_dll: NtProtect section failed ");
            dbg_print_hex32("", (u32)pr);
        }
    }
    NtClose(h);
    PVOID tmp = hdr;
    SIZE_T tmp_sz = 0; /* MEM_RELEASE requires RegionSize 0 (MAIN-8) */
    NtFreeVirtualMemory((HANDLE)-1, &tmp, &tmp_sz, 0x8000);
    *out_base = img_base;
    *out_size = img_size;
    return STATUS_SUCCESS;
}

/* Forward declarations for forwarder resolution */
static void* ldr_find_export(void *dll_base, const char *name);
static void* ldr_find_export_by_ordinal(void *dll_base, WORD ordinal);
static void* find_loaded(const char *name);
static int is_already_loaded_ntdll(const char *name);
static void* get_ntdll_base(void);
static NTSTATUS ldr_nt_map_dll(const char *path, void **out_base, size_t *out_size);

static void* ldr_resolve_forwarder_body(const char *fwd_str) {
    if (!fwd_str || !*fwd_str) return NULL;
    const char *p = fwd_str;
    if (*p == '!') p++;
    const char *dot = NULL;
    for (const char *q = p; *q; q++) if (*q == '.') dot = q;
    if (!dot) return NULL;
    char dllname[64];
    size_t dlen = (size_t)(dot - p);
    if (dlen >= sizeof(dllname)-5) return NULL;
    for (size_t i=0;i<dlen;i++) dllname[i]=p[i];
    dllname[dlen]='\0';
    // Append .dll if missing
    size_t dl = dlen;
    int has_dll = 0;
    if (dl >= 4) {
        if ((dllname[dl-4]=='.' || dllname[dl-4]=='.') &&
            (dllname[dl-3]=='d'||dllname[dl-3]=='D') &&
            (dllname[dl-2]=='l'||dllname[dl-2]=='L') &&
            (dllname[dl-1]=='l'||dllname[dl-1]=='L')) has_dll=1;
    }
    if (!has_dll) {
        if (dlen+4 >= sizeof(dllname)) return NULL;
        dllname[dlen]='.'; dllname[dlen+1]='d'; dllname[dlen+2]='l'; dllname[dlen+3]='l'; dllname[dlen+4]='\0';
    }
    void *host = find_loaded(dllname);
    if (!host && is_already_loaded_ntdll(dllname)) host = get_ntdll_base();
    if (!host) {
        // Try to map dependency on demand (handles kernelbase -> kernel32 forwarder before kernel32 mapped)
        size_t sz=0; void *b=NULL;
        /* MAIN-5: only the system directory; no root, /tmp or bare-name fallback. */
        const char *cands[] = {"/windows/system32/", NULL};
        for (int c=0; cands[c]; c++) {
            char full[512]; size_t n=0;
            const char *pre=cands[c];
            while (*pre && n<500) full[n++]=*pre++;
            const char *pp = dllname;
            while (*pp && n<500) full[n++]=*pp++;
            full[n]='\0';
            if (ldr_nt_map_dll(full,&b,&sz)==STATUS_SUCCESS) { host=b; break; }
        }
        // Host found but not in loaded list – transient mapping is enough for forwarder; do not cache here to avoid needing add_loaded (static).
        (void)sz;
    }
    if (!host) return NULL;
    const char *target = dot+1;
    if (*target == '#') {
        WORD ord = 0;
        const char *pp = target+1;
        while (*pp >= '0' && *pp <= '9') { ord = (WORD)(ord*10 + (*pp - '0')); pp++; }
        return ldr_find_export_by_ordinal(host, ord);
    } else {
        return ldr_find_export(host, target);
    }
}

/* MAIN-4: forwarder chains can loop (A -> B -> A). Each resolution goes through
 * here, so a depth cap stops the mutual recursion with ldr_find_export. */
#define LDR_FORWARDER_MAX_DEPTH 16
static int g_forwarder_depth = 0;
static void* ldr_resolve_forwarder(const char *fwd_str) {
    if (g_forwarder_depth >= LDR_FORWARDER_MAX_DEPTH) return NULL;
    g_forwarder_depth++;
    void *resolved = ldr_resolve_forwarder_body(fwd_str);
    g_forwarder_depth--;
    return resolved;
}

/* Helper to find export by name (case-insensitive) with forwarder support */
static void* ldr_find_export(void *dll_base, const char *name) {
    if (!dll_base || !name) return NULL;
    unsigned char *base = (unsigned char*)dll_base;
    if (ldr_load_u16(base) != 0x5A4D) return NULL;
    u32 nt_off = ldr_load_u32(base + 0x3C);
    if (ldr_load_u32(base + nt_off) != 0x4550) return NULL;
    u32 oh_off = nt_off + 4 + 20;
    u32 export_rva = ldr_load_u32(base + oh_off + 112);
    if (!export_rva) return NULL;
    unsigned char *ed = base + export_rva;
    u32 num_names = ldr_load_u32(ed + 24);
    u32 addr_names = ldr_load_u32(ed + 32);
    u32 addr_ords = ldr_load_u32(ed + 36);
    u32 addr_funcs = ldr_load_u32(ed + 28);
    u32 export_size = ldr_load_u32(base + oh_off + 116);
    for (u32 i=0;i<num_names;i++) {
        u32 name_rva = ldr_load_u32(base + addr_names + i*4);
        const char *n = (const char*)(base + name_rva);
        if (ldr_strcasecmp(n, name)==0) {
            u16 ord = ldr_load_u16(base + addr_ords + i*2);
            u32 func_rva = ldr_load_u32(base + addr_funcs + ord*4);
            if (!func_rva) return NULL;
            if (func_rva >= export_rva && func_rva < export_rva + export_size) {
                const char *fwd = (const char*)(base + func_rva);
                void *resolved = ldr_resolve_forwarder(fwd);
                if (resolved) return resolved;
                return NULL;
            }
            return base + func_rva;
        }
    }
    return NULL;
}
static void* ldr_find_export_by_ordinal(void *dll_base, WORD ordinal) {
    if (!dll_base) return NULL;
    unsigned char *base = (unsigned char*)dll_base;
    if (ldr_load_u16(base) != 0x5A4D) return NULL;
    u32 nt_off = ldr_load_u32(base + 0x3C);
    if (ldr_load_u32(base + nt_off) != 0x4550) return NULL;
    u32 oh_off = nt_off + 4 + 20;
    u32 export_rva = ldr_load_u32(base + oh_off + 112);
    if (!export_rva) return NULL;
    unsigned char *ed = base + export_rva;
    u32 base_ord = ldr_load_u32(ed + 16);
    u32 num_funcs = ldr_load_u32(ed + 20);
    u32 addr_funcs = ldr_load_u32(ed + 28);
    if (ordinal < base_ord) return NULL;
    u32 idx = ordinal - base_ord;
    if (idx >= num_funcs) return NULL;
    u32 func_rva = ldr_load_u32(base + addr_funcs + idx*4);
    if (!func_rva) return NULL;
    u32 export_size = ldr_load_u32(base + oh_off + 116);
    if (func_rva >= export_rva && func_rva < export_rva + export_size) {
        const char *fwd = (const char*)(base + func_rva);
        void *resolved = ldr_resolve_forwarder(fwd);
        if (resolved) return resolved;
        return NULL;
    }
    return base + func_rva;
}
/*
 * The base of this ntdll.dll. binfmt_pe maps ntdll at an ASLR address, so the
 * old probe of the preferred base 0x180000000 found nothing (and the loaded table
 * then held a bogus base, S4 of the console milestone). ntdll finds itself from
 * its own code: walk down from LdrInitializeThunk one page at a time until an MZ
 * header with a PE signature. The image starts with its header page, so the walk
 * stops inside the mapping.
 */
void NTAPI LdrInitializeThunk(void);
static void* get_ntdll_base(void) {
    unsigned char *p = (unsigned char*)((ULONG_PTR)(void *)LdrInitializeThunk & ~0xFFFULL);
    for (int i = 0; i < 0x400 && p; i++, p -= 0x1000) {
        if (ldr_load_u16(p) == 0x5A4D) {
            u32 nt_off = ldr_load_u32(p + 0x3C);
            if (nt_off < 0x1000 && ldr_load_u32(p + nt_off) == 0x4550)
                return p;
        }
    }
    return NULL;
}
// Simple loaded list
#define MAX_LOADED 32
struct loaded_entry { char name[64]; void *base; size_t size; int has_init; };
static struct loaded_entry g_loaded[MAX_LOADED];
static int g_nloaded = 0;

/* The process loader context (ldr.h), set when LdrInitializeThunk publishes it.
 * Each startup module the binder maps enters its module list
 * (winldr_register_mapped), so GetModuleHandle and GetProcAddress find it
 * (docs/status/DIFFERENTIAL-RESULTS.md, non-VM probes, cause 1). */
static winldr_ctx_t *g_ldr_ctx = NULL;

static void ldr_register_startup_name(const char *name, void *base)
{
    char path[512];
    size_t n = 0;
    const char *pre = "C:/Windows/System32/";
    if (!g_ldr_ctx || !base || !name) return;
    while (*pre && n < 300) path[n++] = *pre++;
    for (const char *p = name; *p && n < 500; p++) path[n++] = *p;
    path[n] = 0;
    winldr_register_mapped(g_ldr_ctx, path, base);
}

/* The exe itself is the first entry of the load list. Its path is the image
 * path from the process parameters, with '/' separators. */
static void ldr_register_exe(PPEB peb, void *image_base)
{
    char path[512];
    size_t k = 0;
    if (!g_ldr_ctx || !image_base) return;
    if (peb->ProcessParameters && peb->ProcessParameters->ImagePathName.Buffer) {
        const UNICODE_STRING *ip = &peb->ProcessParameters->ImagePathName;
        size_t n = ip->Length / sizeof(WCHAR);
        for (size_t i = 0; i < n && k + 1 < sizeof(path); ++i) {
            WCHAR c = ip->Buffer[i];
            path[k++] = (c == '\\') ? '/' : (c <= 0x7F ? (char)c : '?');
        }
    }
    if (k == 0) {
        const char *dflt = "C:/image.exe";
        while (dflt[k]) { path[k] = dflt[k]; k++; }
    }
    path[k] = 0;
    winldr_register_mapped(g_ldr_ctx, path, image_base);
}
static void* find_loaded(const char *name) {
    char lower[64]; size_t i=0; for (; name[i] && i<63; i++) { char c=name[i]; if (c>='A'&&c<='Z') c+='a'-'A'; lower[i]=c; } lower[i]=0;
    for (int j=0;j<g_nloaded;j++) {
        char l2[64]; size_t k=0; for (; g_loaded[j].name[k] && k<63;k++){char c=g_loaded[j].name[k]; if(c>='A'&&c<='Z') c+='a'-'A'; l2[k]=c;} l2[k]=0;
        if (ldr_strcasecmp(lower, l2)==0) return g_loaded[j].base;
    }
    return NULL;
}
static int add_loaded(const char *name, void *base, size_t size) {
    if (g_nloaded >= MAX_LOADED) return -1;
    size_t i=0; for (; name[i] && i<63; i++) g_loaded[g_nloaded].name[i]=name[i]; g_loaded[g_nloaded].name[i]=0;
    g_loaded[g_nloaded].base = base;
    g_loaded[g_nloaded].size = size;
    g_loaded[g_nloaded].has_init = 0;
    g_nloaded++;
    ldr_register_startup_name(name, base);
    return 0;
}
static int is_already_loaded_ntdll(const char *name) {
    char lower[64]; size_t i=0; for(; name[i]&&i<63;i++){char c=name[i]; if(c>='A'&&c<='Z') c+='a'-'A'; lower[i]=c;} lower[i]=0;
    return (ldr_strcasecmp(lower,"ntdll")==0 || ldr_strcasecmp(lower,"ntdll.dll")==0);
}

// Call DllMain if entry point exists
static void call_dll_main(void *dll_base) {
    unsigned char *base = (unsigned char*)dll_base;
    u32 nt_off = ldr_load_u32(base + 0x3C);
    u32 oh_off = nt_off + 4 + 20;
    u32 entry_rva = ldr_load_u32(base + oh_off + 16);
    if (!entry_rva) return;
    // Check characteristics: DLL?
    // Call DllMain(base, DLL_PROCESS_ATTACH, NULL)
    typedef BOOL (__stdcall *DllMain_t)(HINSTANCE, DWORD, LPVOID);
    DllMain_t fn = (DllMain_t)(base + entry_rva);
    fn((HINSTANCE)dll_base, 1, NULL);
}
// Call TLS callbacks
static void call_tls_callbacks(void *dll_base) {
    unsigned char *base = (unsigned char*)dll_base;
    u32 nt_off = ldr_load_u32(base + 0x3C);
    u32 oh_off = nt_off + 4 + 20;
    u32 num_dirs = ldr_load_u32(base + oh_off + 108);
    if (num_dirs <= 9) return;
    u32 tls_rva = ldr_load_u32(base + oh_off + 112 + 9*8);
    //u32 tls_size = ldr_load_u32(base + oh_off + 112 + 9*8 + 4);
    if (!tls_rva) return;
    // TLS directory layout for PE64: StartRawData 8, EndRawData 8, AddressOfIndex 8, AddressOfCallbacks 8, SizeOfZeroFill 4, Characteristics 4
    unsigned char *tls = base + tls_rva;
    u64 callbacks_va = ldr_load_u64(tls + 24);
    if (!callbacks_va) return;
    // callbacks_va is absolute VA after relocs; should point into image. Convert to pointer.
    // In mapped image, VA == base + (old_va - preferred). After reloc, callbacks_va should be base + rva_of_callbacks? But if relocs applied, callbacks_va is already actual pointer.
    // We can treat it as pointer directly if it lies within image; otherwise try as RVA.
    ULONG_PTR *table = NULL;
    // Heuristic: if callbacks_va is within image range (base to base+size_image), use as ptr; else treat as RVA.
    // For simplicity, assume VA is already relocated to actual address.
    if (callbacks_va >= (u64)(ULONG_PTR)base && callbacks_va < (u64)(ULONG_PTR)base + 0x3000000) {
        table = (ULONG_PTR*)(ULONG_PTR)callbacks_va;
    } else {
        // Fallback: treat as RVA
        u64 rva = callbacks_va - ldr_load_u64(base + oh_off + 24); // delta?
        table = (ULONG_PTR*)(base + rva);
    }
    if (!table) return;
    for (size_t i=0; table[i]!=0; i++) {
        typedef void (*TlsCb_t)(void*, DWORD, void*);
        TlsCb_t cb = (TlsCb_t)(ULONG_PTR)table[i];
        cb(dll_base, 1, NULL);
        // Avoid infinite loop: break after 32
        if (i>32) break;
    }
}

static void dbg_print(const char *msg) {
    HANDLE h = (HANDLE)(intptr_t)-11;
    IO_STATUS_BLOCK ios;
    SIZE_T len=0; while(msg && msg[len]) len++;
    if (len) NtWriteFile(h, NULL, NULL, NULL, &ios, (PVOID)msg, (ULONG)len, NULL, NULL);
}

/* K32-4: one loader context per process, created before any module call and
 * published in the PEB (ldr.h, LDR_PEB_CONTEXT_SLOT). The application directory
 * is the image path's directory. System32 is the fixed personality location. */
static void ldr_publish_process_context(PPEB peb)
{
    winldr_ctx_t *ctx = winldr_ctx_create();
    if (!ctx) { NtTerminateProcess((HANDLE)(intptr_t)-1, STATUS_NO_MEMORY); while (1) {} }
    winldr_set_system32_dir(ctx, "C:/Windows/System32");
    if (peb->ProcessParameters && peb->ProcessParameters->ImagePathName.Buffer) {
        const UNICODE_STRING *ip = &peb->ProcessParameters->ImagePathName;
        size_t n = ip->Length / sizeof(WCHAR), sep = 0;
        for (size_t i = 0; i < n; ++i)
            if (ip->Buffer[i] == '\\' || ip->Buffer[i] == '/') sep = i;
        char dirbuf[512];
        size_t k = 0;
        for (size_t i = 0; i < sep && k + 1 < sizeof(dirbuf); ++i) {
            WCHAR c = ip->Buffer[i];
            dirbuf[k++] = (c == '\\') ? '/' : (c <= 0x7F ? (char)c : '?');
        }
        dirbuf[k] = 0;
        if (k) winldr_set_exe_dir(ctx, dirbuf);
    }
    peb->Ldr = winldr_peb_ldr(ctx);
    LDR_PEB_CONTEXT_SLOT(peb) = ctx;
    g_ldr_ctx = ctx;
}

/*
 * Binds the IAT of a loaded module (not the exe) and of every dependency it maps
 * for the first time, recursively. msvcrt.dll imports kernel32.dll, which imports
 * ntdll.dll (S4 of the console milestone): the top-level loop only binds the exe
 * and one level below it, so a DLL two levels down was never patched.
 */
static void *g_bound_mods[MAX_LOADED];
static int g_nbound = 0;

/* Strict binding (Windows loader). An import that cannot be bound ends the
 * process: the loader writes one line to stderr (handle -12) and to the serial
 * console (handle -11), then terminates with STATUS_DLL_NOT_FOUND (0xC0000135)
 * or STATUS_ENTRYPOINT_NOT_FOUND (0xC0000139). The IAT slot is never left with
 * a raw RVA. Pure status logic: ldr_pure.h (ldr_unbound_import_status). */
static void ldr_err_write(HANDLE h, const char *msg)
{
    IO_STATUS_BLOCK ios;
    SIZE_T len = 0;
    while (msg && msg[len]) len++;
    if (len) NtWriteFile(h, NULL, NULL, NULL, &ios, (PVOID)msg, (ULONG)len, NULL, NULL);
}

static void ldr_fail_unbound_import(const char *importer, const char *dll,
                                    const char *sym, unsigned long status)
{
    const char *what = (status == LDR_PURE_STATUS_DLL_NOT_FOUND)
                           ? "STATUS_DLL_NOT_FOUND"
                           : "STATUS_ENTRYPOINT_NOT_FOUND";
    HANDLE herr = (HANDLE)(intptr_t)-12;
    HANDLE hout = (HANDLE)(intptr_t)-11;
    ldr_err_write(herr, "LdrInitializeThunk: cannot bind import ");
    ldr_err_write(herr, importer ? importer : "?");
    ldr_err_write(herr, " -> ");
    ldr_err_write(herr, dll ? dll : "?");
    if (sym) { ldr_err_write(herr, "!"); ldr_err_write(herr, sym); }
    ldr_err_write(herr, ": ");
    ldr_err_write(herr, what);
    ldr_err_write(herr, "\n");
    /* Same line on serial, so a run without a stderr handle still shows it. */
    ldr_err_write(hout, "Ldr: unbound import ");
    ldr_err_write(hout, dll ? dll : "?");
    ldr_err_write(hout, ": ");
    ldr_err_write(hout, what);
    ldr_err_write(hout, "\n");
    NtTerminateProcess((HANDLE)(intptr_t)-1, (NTSTATUS)status);
    while (1) {}
}

static void ldr_bind_imports(unsigned char *mod, int depth)
{
    if (!mod || depth > 8) return;
    /* Each module is bound once. A dependency that an earlier pass already
     * mapped (and did not bind) still needs its own imports bound. */
    for (int i = 0; i < g_nbound; i++)
        if (g_bound_mods[i] == (void *)mod) return;
    if (g_nbound < MAX_LOADED) g_bound_mods[g_nbound++] = (void *)mod;
    u32 nt_off = ldr_load_u32(mod + 0x3C);
    u32 oh_off = nt_off + 4 + 20;
    u32 imp_rva = ldr_load_u32(mod + oh_off + 112 + 1*8);
    u32 imp_size = ldr_load_u32(mod + oh_off + 112 + 1*8 + 4);
    if (!imp_rva || !imp_size) return;
    for (u32 off = 0; off + 20 <= imp_size; off += 20) {
        unsigned char *d = mod + imp_rva + off;
        u32 orig_first = ldr_load_u32(d + 0);
        u32 time_stamp = ldr_load_u32(d + 4);
        u32 forwarder = ldr_load_u32(d + 8);
        u32 name_rva = ldr_load_u32(d + 12);
        u32 first_thunk = ldr_load_u32(d + 16);
        if (!orig_first && !time_stamp && !forwarder && !name_rva && !first_thunk) break;
        if (!name_rva || !first_thunk) continue;
        const char *dep_name = (const char *)(mod + name_rva);
        void *dep = find_loaded(dep_name);
        if (!dep && is_already_loaded_ntdll(dep_name)) dep = get_ntdll_base();
        int fresh = 0;
        if (!dep) {
            char full[512];
            size_t n = 0;
            const char *pre = "/windows/system32/"; /* MAIN-5: system directory only */
            while (*pre && n < 300) full[n++] = *pre++;
            for (const char *p = dep_name; *p && n < 500; p++) full[n++] = *p;
            full[n] = 0;
            size_t sz = 0;
            if (ldr_nt_map_dll(full, &dep, &sz) != STATUS_SUCCESS)
                ldr_fail_unbound_import("dependency", dep_name, NULL, LDR_PURE_STATUS_DLL_NOT_FOUND);
            add_loaded(dep_name, dep, sz);
            fresh = 1;
            dbg_print("Ldr: bind dependency ");
            dbg_print(dep_name);
            dbg_print("\n");
        }
        ldr_bind_imports((unsigned char *)dep, depth + 1);
        if (fresh) {
            call_tls_callbacks(dep);
            call_dll_main(dep);
            if (g_ldr_ctx) winldr_mark_initialized(g_ldr_ctx, dep);
        }
        u32 cnt = 0;
        for (u32 idx = 0; idx < 512; idx++) {
            u64 v = 0;
            for (int b = 0; b < 8; b++) v |= (u64)mod[(orig_first ? orig_first : first_thunk) + idx*8 + b] << (b*8);
            if (v == 0) break;
            cnt++;
        }
        if (!cnt) continue;
        PVOID pp = (PVOID)(((ULONG_PTR)(mod + first_thunk)) & ~0xFFFULL);
        SIZE_T ssz = ((ULONG_PTR)(mod + first_thunk) + cnt*8 + 0xFFFULL) & ~0xFFFULL;
        ssz -= (ULONG_PTR)pp;
        ULONG old;
        NtProtectVirtualMemory((HANDLE)-1, &pp, &ssz, 0x04, &old);
        for (u32 idx = 0; idx < cnt; idx++) {
            u64 tv = 0;
            u32 src = (orig_first ? orig_first : first_thunk) + idx*8;
            for (int b = 0; b < 8; b++) tv |= (u64)mod[src + b] << (b*8);
            void *proc = NULL;
            if (tv & 0x8000000000000000ULL) {
                proc = ldr_find_export_by_ordinal(dep, (WORD)(tv & 0xFFFF));
            } else {
                u32 ibn_rva = (u32)tv;
                if (ibn_rva < ldr_load_u32(mod + oh_off + 56))
                    proc = ldr_find_export(dep, (const char *)(mod + ibn_rva + 2));
            }
            if (!proc)
                ldr_fail_unbound_import("dependency", (const char *)(mod + name_rva), NULL,
                                        LDR_PURE_STATUS_ENTRYPOINT_NOT_FOUND);
            *(u64 *)(mod + first_thunk + idx*8) = (u64)(ULONG_PTR)proc;
        }
    }
}

void NTAPI LdrInitializeThunk(void) {
    dbg_print("LdrInitializeThunk: enter\n");
    PTEB teb;
    __asm__ volatile("movq %%gs:0x30, %0" : "=r"(teb));
    PPEB peb = teb->ProcessEnvironmentBlock;
    void *image_base = peb->ImageBaseAddress;
    dbg_print("LdrInitializeThunk: got PEB\n");
    ldr_publish_process_context(peb);
    ldr_register_exe(peb, image_base);
    if (!image_base) { NtTerminateProcess((HANDLE)-1, 1); while(1){} }
    unsigned char *base = (unsigned char*)image_base;
    // Validate EXE
    if (ldr_load_u16(base) != 0x5A4D) { NtTerminateProcess((HANDLE)-1, 2); while(1){} }
    u32 nt_off = ldr_load_u32(base + 0x3C);
    if (ldr_load_u32(base + nt_off) != 0x4550) { NtTerminateProcess((HANDLE)-1, 3); while(1){} }
    u32 oh_off = nt_off + 4 + 20;
    u32 import_rva = ldr_load_u32(base + oh_off + 112 + 1*8);
    u32 import_size = ldr_load_u32(base + oh_off + 112 + 1*8 + 4);
    // Init loaded list with ntdll if present
    void *ntdll = get_ntdll_base();
    if (ntdll) {
        add_loaded("ntdll.dll", ntdll, 0);
        /* ntdll is initialized by the process start, before the exe runs. */
        if (g_ldr_ctx) winldr_mark_initialized(g_ldr_ctx, ntdll);
    }
    if (import_rva && import_size) {
        unsigned char *imp = base + import_rva;
        for (u32 off=0; off+20 <= import_size; off+=20) {
            u32 orig_first = ldr_load_u32(imp+off+0);
            u32 time_stamp = ldr_load_u32(imp+off+4);
            u32 forwarder = ldr_load_u32(imp+off+8);
            u32 name_rva = ldr_load_u32(imp+off+12);
            u32 first_thunk = ldr_load_u32(imp+off+16);
            (void)time_stamp; (void)forwarder;
            if (!orig_first && !time_stamp && !forwarder && !name_rva && !first_thunk) break;
            if (!name_rva || !first_thunk) continue;
            const char *dll_name = (const char*)(base + name_rva);
            dbg_print("Ldr: import dll_name=");
            dbg_print(dll_name);
            dbg_print("\n");
            // Find or load DLL
            void *dll_base = find_loaded(dll_name);
            size_t dll_size = 0;
            if (!dll_base) {
                if (is_already_loaded_ntdll(dll_name)) {
                    dll_base = ntdll;
                    if (!dll_base) {
                        // Try to map ntdll from disk as fallback
                        /* MAIN-5: system directory only. */
                        if (ldr_nt_map_dll("/windows/system32/ntdll.dll", &dll_base, &dll_size)!=STATUS_SUCCESS) {
                            ldr_fail_unbound_import("image", dll_name, NULL, LDR_PURE_STATUS_DLL_NOT_FOUND);
                        }
                        add_loaded(dll_name, dll_base, dll_size);
                    }
                } else {
                    // MAIN-5: load from the system directory only (no bare name or root).
                    const char *candidates[] = { "/windows/system32/", NULL };
                    int found=0;
                    for (int c=0; candidates[c]; c++) {
                        char full[512];
                        size_t n=0;
                        const char *pre=candidates[c];
                        while (*pre && n<500) full[n++]=*pre++;
                        const char *p=dll_name;
                        while (*p && n<500) full[n++]=*p++;
                        full[n]=0;
                        if (ldr_nt_map_dll(full, &dll_base, &dll_size)==STATUS_SUCCESS) { found=1; break; }
                    }
                    if (!found)
                        ldr_fail_unbound_import("image", dll_name, NULL, LDR_PURE_STATUS_DLL_NOT_FOUND);
                    add_loaded(dll_name, dll_base, dll_size);
                    dbg_print("Ldr: mapped ");
                    dbg_print(dll_name);
                    dbg_print("\n");
                    // Recursively resolve this DLL's imports (e.g., kernel32 -> ntdll)
                    // Simple: if dll_base has import directory, resolve it
                    {
                        unsigned char *db = (unsigned char*)dll_base;
                        u32 d_nt_off = ldr_load_u32(db + 0x3C);
                        u32 d_oh_off = d_nt_off + 4 + 20;
                        u32 d_import_rva = ldr_load_u32(db + d_oh_off + 112 + 1*8);
                        u32 d_import_size = ldr_load_u32(db + d_oh_off + 112 + 1*8 + 4);
                        if (d_import_rva && d_import_size) {
                            unsigned char *d_imp = db + d_import_rva;
                            for (u32 doff=0; doff+20 <= d_import_size; doff+=20) {
                                u32 d_orig = ldr_load_u32(d_imp+doff+0);
                                u32 d_ts = ldr_load_u32(d_imp+doff+4);
                                u32 d_fw = ldr_load_u32(d_imp+doff+8);
                                u32 d_name_rva = ldr_load_u32(d_imp+doff+12);
                                u32 d_first = ldr_load_u32(d_imp+doff+16);
                                if (!d_orig && !d_ts && !d_fw && !d_name_rva && !d_first) break;
                                if (!d_name_rva || !d_first) continue;
                                const char *d_dll_name = (const char*)(db + d_name_rva);
                                void *dep_base = find_loaded(d_dll_name);
                                if (!dep_base && is_already_loaded_ntdll(d_dll_name)) dep_base = ntdll;
                                if (!dep_base) {
                                    // Try to map dependency
                                    size_t dep_size=0;
                                    const char *cands[] = { "/windows/system32/", NULL }; /* MAIN-5 */
                                    int dfound=0;
                                    for (int cc=0;cands[cc];cc++){
                                        char full[512]; size_t nn=0; const char *pre=cands[cc]; while(*pre&&nn<500) full[nn++]=*pre++; const char *p=d_dll_name; while(*p&&nn<500) full[nn++]=*p++; full[nn]=0;
                                        if (ldr_nt_map_dll(full,&dep_base,&dep_size)==STATUS_SUCCESS){dfound=1;break;}
                                    }
                                    if (!dfound || !dep_base)
                                        ldr_fail_unbound_import(dll_name, d_dll_name, NULL,
                                                                LDR_PURE_STATUS_DLL_NOT_FOUND);
                                    add_loaded(d_dll_name, dep_base, dep_size);
                                }
                                // Resolve IAT for this dependency import
                                if (dep_base && d_first) {
                                    u32 d_thunk_rva = d_first;
                                    u32 d_orig_rva = d_orig ? d_orig : d_first;
                                    // Make writable
                                    // Count thunks
                                    u32 cnt=0; for(u32 idx=0;;idx++){ u64 v=ldr_load_u64(db+d_orig_rva+idx*8); if(!v) break; cnt++; if(cnt>256) break; }
                                    if (cnt) {
                                        PVOID p = (PVOID)(db + d_thunk_rva);
                                        SIZE_T sz = cnt*8;
                                        // Align
                                        ULONG_PTR pa = (ULONG_PTR)p & ~0xFFFULL;
                                        SIZE_T adj = ((ULONG_PTR)p + sz + 0xFFF) & ~0xFFFULL;
                                        adj -= pa; PVOID pp=(PVOID)pa; SIZE_T ssz=adj; ULONG old; NtProtectVirtualMemory((HANDLE)-1,&pp,&ssz,0x04,&old);
                                        for(u32 idx=0; idx<cnt; idx++){
                                            u64 thunk_val=ldr_load_u64(db + d_orig_rva + idx*8);
                                            if(!thunk_val) break;
                                            void *proc=NULL;
                                            if (thunk_val & 0x8000000000000000ULL) {
                                                WORD ord = (WORD)(thunk_val & 0xFFFF);
                                                proc = ldr_find_export_by_ordinal(dep_base, ord);
                                            } else {
                                                u32 ibn_rva=(u32)thunk_val;
                                                const char *sym_name=(const char*)(db+ibn_rva+2);
                                                proc = ldr_find_export(dep_base, sym_name);
                                                if(!proc){ WORD o=(WORD)(*(u16*)(db+ibn_rva)); proc=ldr_find_export_by_ordinal(dep_base,o); }
                                            }
                                            if(!proc)
                                                ldr_fail_unbound_import(dll_name, d_dll_name, NULL,
                                                                        LDR_PURE_STATUS_ENTRYPOINT_NOT_FOUND);
                                            { u64 *iat=(u64*)(db+d_thunk_rva+idx*8); *iat=(u64)(ULONG_PTR)proc; }
                                        }
                                        // Restore
                                        PVOID rp=(PVOID)pa; SIZE_T rsz=adj; ULONG old2; NtProtectVirtualMemory((HANDLE)-1,&rp,&rsz,0x02,&old2);
                                    }
                                }
                            }
                        }
                    }
                    // Call TLS and DllMain for this newly loaded dll (kernel32)
                    call_tls_callbacks(dll_base);
                    call_dll_main(dll_base);
                    if (g_ldr_ctx) winldr_mark_initialized(g_ldr_ctx, dll_base);
                }
            }
            // The DLL's own imports (and any dependency it maps): S4.
            ldr_bind_imports((unsigned char *)dll_base, 1);
            // Now resolve EXE's IAT for this descriptor
            u32 thunk_rva = first_thunk;
            u32 orig_rva = orig_first ? orig_first : first_thunk;
            // Count thunks
            u32 cnt=0; for(u32 idx=0;;idx++){ u64 v=0; for(int b=0;b<8;b++) v |= (u64)base[orig_rva+idx*8+b] << (b*8); if(v==0) break; cnt++; if(cnt>512) break; }
            if (cnt==0) continue;
            dbg_print("Ldr: thunk_rva "); { char tmp[16]; unsigned int v=thunk_rva; int pi=0; char rev[16]; int r=0; if(v==0) rev[r++]='0'; else while(v>0){ unsigned int d=v%16; rev[r++]= d<10?'0'+d:'a'+d-10; v/=16; } tmp[pi++]='0'; tmp[pi++]='x'; while(r>0) tmp[pi++]=rev[--r]; tmp[pi]=0; dbg_print(tmp);} dbg_print(" cnt "); { char tmp[16]; unsigned int v=cnt; int pi=0; char rev[16]; int r=0; if(v==0) rev[r++]='0'; else while(v>0){rev[r++]='0'+(v%10); v/=10;} while(r>0) tmp[pi++]=rev[--r]; tmp[pi]=0; dbg_print(tmp);} dbg_print("\n");
            // Make IAT writable: page align
            {
                PVOID p = (PVOID)(base + thunk_rva);
                SIZE_T sz = cnt * 8;
                ULONG_PTR pa = (ULONG_PTR)p & ~0xFFFULL;
                ULONG_PTR end = ((ULONG_PTR)p + sz + 0xFFFULL) & ~0xFFFULL;
                SIZE_T adj = end - pa;
                PVOID pp = (PVOID)pa;
                SIZE_T ssz = adj;
                ULONG old;
                NTSTATUS pr = NtProtectVirtualMemory((HANDLE)-1, &pp, &ssz, 0x04, &old);
                (void)pr;
                // Regardless of pr, patch
                for (u32 idx=0; idx<cnt; idx++) {
                    u64 thunk_val=0; for(int b=0;b<8;b++) thunk_val |= (u64)base[orig_rva+idx*8+b] << (b*8);
                    if (thunk_val==0) break;
                    void *proc=NULL;
                    if (thunk_val & 0x8000000000000000ULL) {
                        WORD ord = (WORD)(thunk_val & 0xFFFF);
                        proc = ldr_find_export_by_ordinal(dll_base, ord);
                    } else {
                        u32 ibn_rva = (u32)thunk_val;
                        if (ibn_rva >= ldr_load_u32(base+oh_off+56)) { proc=NULL; } else {
                            const char *sym_name = (const char*)(base + ibn_rva + 2);
                            proc = ldr_find_export(dll_base, sym_name);
                            if (!proc) {
                                // Try ordinal hint
                                u16 hint = ldr_load_u16(base + ibn_rva);
                                (void)hint;
                            }
                        }
                    }
                    if (proc) {
                        u64 *iat = (u64*)(base + thunk_rva + idx*8);
                        { dbg_print("Ldr: patch IAT "); { char tmp[16]; unsigned long v=(unsigned long)iat; int pi=0; char hb[32]; int r=0; if(v==0) hb[r++]='0'; else while(v>0){ unsigned long d=v%16; hb[r++]= d<10?'0'+d:'a'+d-10; v/=16; } tmp[pi++]='0'; tmp[pi++]='x'; while(r>0) tmp[pi++]=hb[--r]; tmp[pi]=0; dbg_print(tmp);} dbg_print(" -> "); { char tmp[16]; unsigned long v=(unsigned long)proc; int pi=0; char hb[32]; int r=0; if(v==0) hb[r++]='0'; else while(v>0){ unsigned long d=v%16; hb[r++]= d<10?'0'+d:'a'+d-10; v/=16; } tmp[pi++]='0'; tmp[pi++]='x'; while(r>0) tmp[pi++]=hb[--r]; tmp[pi]=0; dbg_print(tmp);} dbg_print("\n"); }
                        *iat = (u64)(ULONG_PTR)proc;
                    } else {
                        /* Strict (Windows): never leave the raw RVA in the IAT. */
                        const char *miss = (thunk_val & 0x8000000000000000ULL)
                                               ? "ordinal"
                                               : ((u32)thunk_val < ldr_load_u32(base+oh_off+56))
                                                     ? (const char *)(base + (u32)thunk_val + 2)
                                                     : "?";
                        ldr_fail_unbound_import("image", dll_name, miss,
                                                LDR_PURE_STATUS_ENTRYPOINT_NOT_FOUND);
                    }
                }
                // Restore protection to READONLY (0x02)
                PVOID rp = (PVOID)pa;
                SIZE_T rsz = adj;
                ULONG old2;
                NTSTATUS pr2 = NtProtectVirtualMemory((HANDLE)-1, &rp, &rsz, 0x02, &old2);
                (void)pr2;
                dbg_print("Ldr: patched IAT for ");
                dbg_print(dll_name);
                dbg_print("\n");
            }
        }
    }
    dbg_print("Ldr: calling TLS and jumping to entry\n");
    // Handle EXE TLS callbacks
    call_tls_callbacks(image_base);
    // Now jump to EXE entry
    u32 entry_rva = ldr_load_u32(base + oh_off + 16);
    void (*entry)(void) = (void(*)(void))(base + entry_rva);
    entry();
    NtTerminateProcess((HANDLE)-1, 0);
    while(1) {}
}

typedef struct _OSVERSIONINFOEXW {
    ULONG dwOSVersionInfoSize;
    ULONG dwMajorVersion;
    ULONG dwMinorVersion;
    ULONG dwBuildNumber;
    ULONG dwPlatformId;
    WCHAR szCSDVersion[128];
    USHORT wServicePackMajor;
    USHORT wServicePackMinor;
    USHORT wSuiteMask;
    UCHAR wProductType;
    UCHAR wReserved;
} OSVERSIONINFOEXW;

NTSTATUS NTAPI RtlGetVersion(OSVERSIONINFOEXW *info) {
    if (!info || info->dwOSVersionInfoSize < sizeof(*info)) return STATUS_INVALID_PARAMETER;
    info->dwMajorVersion = 10;
    info->dwMinorVersion = 0;
    info->dwBuildNumber = 26200;
    info->dwPlatformId = 2;
    info->wProductType = 1;
    for (int i=0;i<128;i++) info->szCSDVersion[i]=0;
    return STATUS_SUCCESS;
}
PTEB NTAPI NtCurrentTeb(void) {
    PTEB teb;
    __asm__ volatile("movq %%gs:0x30, %0" : "=r"(teb) :: "memory");
    return teb;
}
BOOLEAN NTAPI RtlAddFunctionTable(IMAGE_RUNTIME_FUNCTION_ENTRY *tbl, ULONG count, ULONG64 base) {
    return unwind_register_module(base, 0, tbl, count, (uint8_t*)(uintptr_t)base) == 0 ? 1 : 0;
}
BOOLEAN NTAPI RtlDeleteFunctionTable(IMAGE_RUNTIME_FUNCTION_ENTRY *tbl) {
    /* Unregister so the table is no longer searched (MAIN-7). */
    return unwind_unregister_table(tbl) == 0 ? 1 : 0;
}
#endif /* __MINGW32__ */
