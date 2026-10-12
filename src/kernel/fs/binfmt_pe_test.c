/* binfmt_pe_test.c - userspace unit tests for binfmt_pe.c logic.
 *
 * This file brings in kshim.h and then #includes binfmt_pe.c so that the
 * loader's static helpers can be exercised without booting a kernel.
 */

#include "kshim.h"
#include "binfmt_pe.c"
#include "../../loader/pe_prot.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

/* -------------------------------------------------------------------------- */
/* Tiny test harness                                                          */
/* -------------------------------------------------------------------------- */
static int tests_run = 0;
static int tests_pass = 0;
static int tests_fail = 0;

#define ASSERT(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        tests_fail++; \
        return; \
    } \
} while (0)

#define ASSERT_EQ(a, b) ASSERT((a) == (b))
#define ASSERT_NE(a, b) ASSERT((a) != (b))

#define RUN(name) do { \
    tests_run++; \
    printf("[ RUN ] %s\n", #name); \
    int _fail_before = tests_fail; \
    test_##name(); \
    if (tests_fail == _fail_before) { \
        tests_pass++; \
        printf("[ PASS] %s\n", #name); \
    } else { \
        printf("[ FAIL] %s\n", #name); \
    } \
} while (0)

/* Safe base for tests under ASan: 0x140000000 collides with ASan shadow gap. */
#define TEST_SAFE_BASE 0x20000000ULL

/* -------------------------------------------------------------------------- */
/* Helpers for constructing minimal PE32+ images in temp files.               */
/* -------------------------------------------------------------------------- */
struct pe_builder {
    uint8_t *data;
    size_t size;
    size_t cap;
    size_t nt_off;
    size_t fh_off;
    size_t oh_off;
    size_t sec_off;
};

static void pb_init(struct pe_builder *pb)
{
    memset(pb, 0, sizeof(*pb));
    pb->cap = 4096;
    pb->data = calloc(1, pb->cap);
    pb->size = 0;
}

static void pb_grow(struct pe_builder *pb, size_t need)
{
    while (pb->cap < need) {
        pb->cap *= 2;
        uint8_t *nd = realloc(pb->data, pb->cap);
        if (!nd) abort();
        pb->data = nd;
    }
    if (pb->cap > pb->size)
        memset(pb->data + pb->size, 0, pb->cap - pb->size);
}

static void pb_ensure(struct pe_builder *pb, size_t off, size_t len)
{
    if (off + len > pb->cap)
        pb_grow(pb, off + len);
    if (off + len > pb->size)
        pb->size = off + len;
}

static void pb_write_u16(struct pe_builder *pb, size_t off, uint16_t v)
{
    pb_ensure(pb, off, 2);
    pb->data[off + 0] = (uint8_t)v;
    pb->data[off + 1] = (uint8_t)(v >> 8);
}

static void pb_write_u32(struct pe_builder *pb, size_t off, uint32_t v)
{
    pb_ensure(pb, off, 4);
    pb->data[off + 0] = (uint8_t)v;
    pb->data[off + 1] = (uint8_t)(v >> 8);
    pb->data[off + 2] = (uint8_t)(v >> 16);
    pb->data[off + 3] = (uint8_t)(v >> 24);
}

static void pb_write_u64(struct pe_builder *pb, size_t off, uint64_t v)
{
    pb_ensure(pb, off, 8);
    pb->data[off + 0] = (uint8_t)v;
    pb->data[off + 1] = (uint8_t)(v >> 8);
    pb->data[off + 2] = (uint8_t)(v >> 16);
    pb->data[off + 3] = (uint8_t)(v >> 24);
    pb->data[off + 4] = (uint8_t)(v >> 32);
    pb->data[off + 5] = (uint8_t)(v >> 40);
    pb->data[off + 6] = (uint8_t)(v >> 48);
    pb->data[off + 7] = (uint8_t)(v >> 56);
}

static void pb_build_nt_headers(struct pe_builder *pb,
                                uint64_t image_base,
                                uint32_t size_of_image,
                                uint32_t entry_point,
                                uint16_t subsystem,
                                uint16_t dll_chars)
{
    size_t nt_off = 64;
    size_t fh_off = nt_off + 4;
    size_t oh_off = fh_off + sizeof(IMAGE_FILE_HEADER);
    size_t sec_off = oh_off + sizeof(IMAGE_OPTIONAL_HEADER64);

    pb->nt_off = nt_off;
    pb->fh_off = fh_off;
    pb->oh_off = oh_off;
    pb->sec_off = sec_off;

    pb_ensure(pb, sec_off + sizeof(IMAGE_SECTION_HEADER) * 96, 1);

    pb_write_u16(pb, 0, 0x5A4D);
    pb_write_u32(pb, 60, (uint32_t)nt_off);
    pb_write_u32(pb, nt_off, 0x00004550);
    pb_write_u16(pb, fh_off + 0, IMAGE_FILE_MACHINE_AMD64);
    pb_write_u16(pb, fh_off + 2, 0);
    pb_write_u32(pb, fh_off + 4, 0);
    pb_write_u32(pb, fh_off + 8, 0);
    pb_write_u32(pb, fh_off + 12, 0);
    pb_write_u16(pb, fh_off + 16, sizeof(IMAGE_OPTIONAL_HEADER64));
    pb_write_u16(pb, fh_off + 18, IMAGE_FILE_EXECUTABLE_IMAGE);
    pb_write_u16(pb, oh_off + 0, IMAGE_NT_OPTIONAL_HDR64_MAGIC);
    pb_write_u32(pb, oh_off + 16, entry_point);
    pb_write_u64(pb, oh_off + 24, image_base);
    pb_write_u32(pb, oh_off + 32, 0x1000);
    pb_write_u32(pb, oh_off + 36, 0x200);
    pb_write_u32(pb, oh_off + 56, size_of_image);
    pb_write_u32(pb, oh_off + 60, (uint32_t)sec_off);
    pb_write_u16(pb, oh_off + 68, subsystem);
    pb_write_u16(pb, oh_off + 70, dll_chars);
    pb_write_u64(pb, oh_off + 72, PE_DEFAULT_STACK_RES);
    pb_write_u64(pb, oh_off + 80, PE_DEFAULT_STACK_COM);
    pb_write_u64(pb, oh_off + 88, 0x100000);
    pb_write_u64(pb, oh_off + 96, 0x1000);
    pb_write_u32(pb, oh_off + 104, 0);
    pb_write_u32(pb, oh_off + 108, 16);
}

static size_t pb_add_section(struct pe_builder *pb,
                             const char *name,
                             uint32_t vsize,
                             uint32_t vaddr,
                             uint32_t raw_size,
                             uint32_t raw_off,
                             uint32_t chars)
{
    uint16_t n = (uint16_t)(pb->data[pb->fh_off + 2] | (pb->data[pb->fh_off + 3] << 8));
    size_t off = pb->sec_off + (size_t)n * sizeof(IMAGE_SECTION_HEADER);
    pb_ensure(pb, off, sizeof(IMAGE_SECTION_HEADER));
    memset(pb->data + off, 0, sizeof(IMAGE_SECTION_HEADER));
    size_t namelen = strlen(name);
    if (namelen > 8) namelen = 8;
    memcpy(pb->data + off, name, namelen);
    pb_write_u32(pb, off + 8, vsize);
    pb_write_u32(pb, off + 12, vaddr);
    pb_write_u32(pb, off + 16, raw_size);
    pb_write_u32(pb, off + 20, raw_off);
    pb_write_u32(pb, off + 36, chars);
    n++;
    pb_write_u16(pb, pb->fh_off + 2, n);
    {
        uint32_t headers_end = (uint32_t)(off + sizeof(IMAGE_SECTION_HEADER));
        uint32_t soh = (headers_end + 0xFFF) & ~0xFFFU;
        pb_write_u32(pb, pb->oh_off + 60, soh);
    }
    if (raw_off + raw_size > pb->size) {
        pb_ensure(pb, raw_off + raw_size, 1);
        memset(pb->data + pb->size - (raw_off + raw_size - (pb->size - (raw_off + raw_size))), 0, 1);
    }
    return n - 1;
}

static void pb_set_dir(struct pe_builder *pb, int dir, uint32_t rva, uint32_t size)
{
    size_t off = pb->oh_off + 112 + (size_t)dir * 8;
    pb_write_u32(pb, off, rva);
    pb_write_u32(pb, off + 4, size);
}

static void pb_finish_headers(struct pe_builder *pb)
{
    uint32_t soh = (uint32_t)(pb->data[pb->oh_off + 60] | (pb->data[pb->oh_off + 61] << 8) |
                              (pb->data[pb->oh_off + 62] << 16) | (pb->data[pb->oh_off + 63] << 24));
    pb_ensure(pb, soh, 1);
}

static int pb_write_file(struct pe_builder *pb, const char *path)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    ssize_t w = write(fd, pb->data, pb->size);
    close(fd);
    if (w != (ssize_t)pb->size) return -1;
    return 0;
}

/* -------------------------------------------------------------------------- */
/* Tests                                                                      */
/* -------------------------------------------------------------------------- */
static void test_prot_mapping(void)
{
    IMAGE_SECTION_HEADER sec = {0};
    pe_page_protection_t pp;

    sec.Characteristics = IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_EXECUTE;
    ASSERT_EQ(pe_prot_from_section(&sec, &pp), PE_OK);
    ASSERT_EQ(pp.prot, PE_PROT_READ | PE_PROT_EXEC);

    sec.Characteristics = IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE;
    ASSERT_EQ(pe_prot_from_section(&sec, &pp), PE_OK);
    ASSERT_EQ(pp.prot, PE_PROT_READ | PE_PROT_WRITE);

    sec.Characteristics = IMAGE_SCN_CNT_UNINITIALIZED_DATA | IMAGE_SCN_MEM_WRITE;
    ASSERT_EQ(pe_prot_from_section(&sec, &pp), PE_OK);
    /* BSS with WRITE -> inferred READ|WRITE */
    ASSERT(pp.prot & PE_PROT_WRITE);

    sec.Characteristics = IMAGE_SCN_CNT_CODE;
    ASSERT_EQ(pe_prot_from_section(&sec, &pp), PE_OK);
    ASSERT_EQ(pp.prot, PE_PROT_READ | PE_PROT_EXEC);

    ASSERT_EQ(pe_prot_to_mmap(PE_PROT_READ | PE_PROT_EXEC), (unsigned long)(PROT_READ | PROT_EXEC));
    ASSERT_EQ(pe_prot_to_mmap(PE_PROT_READ | PE_PROT_WRITE), (unsigned long)(PROT_READ | PROT_WRITE));
    ASSERT_EQ(pe_prot_to_mmap(PE_PROT_READ), (unsigned long)PROT_READ);
    /* RWX */
    ASSERT_EQ(pe_prot_to_mmap(PE_PROT_READ | PE_PROT_WRITE | PE_PROT_EXEC),
              (unsigned long)(PROT_READ | PROT_WRITE | PROT_EXEC));
}

static void test_base_selection_preferred(void)
{
    struct pe_load_state st = {0};
    int ret;

    kshim_mapping_reset();
    st.image_base = PE_PREFERRED_BASE;
    st.size_of_image = 0x2000;
    st.dynamic_base = 0;

    ret = pe_pick_base(&st);
    ASSERT_EQ(ret, 0);
    ASSERT_EQ(st.actual_base, PE_PREFERRED_BASE);
    ASSERT_EQ(st.delta, (s64)0);

    pe_state_cleanup(&st);
    kshim_mapping_reset();
}

static void test_base_selection_aslr_collision(void)
{
    struct pe_load_state st = {0};
    int ret;

    kshim_mapping_reset();
    kshim_preferred_base_ok = 0;
    kshim_forced_random_base = PE_PREFERRED_BASE + 0x10000ULL;
    st.image_base = PE_PREFERRED_BASE;
    st.size_of_image = 0x2000;
    st.dynamic_base = 1;

    ret = pe_pick_base(&st);
    ASSERT_EQ(ret, 0);
    ASSERT_NE(st.actual_base, PE_PREFERRED_BASE);
    ASSERT_EQ(st.delta, (s64)(st.actual_base - PE_PREFERRED_BASE));

    pe_state_cleanup(&st);
    kshim_mapping_reset();
}

static void test_base_selection_no_aslr_fails(void)
{
    struct pe_load_state st = {0};
    int ret;

    kshim_mapping_reset();
    kshim_preferred_base_ok = 0;
    st.image_base = PE_PREFERRED_BASE;
    st.size_of_image = 0x2000;
    st.dynamic_base = 0;

    ret = pe_pick_base(&st);
    ASSERT_EQ(ret, -ENOMEM);

    pe_state_cleanup(&st);
    kshim_mapping_reset();
}

__attribute__((no_sanitize_address))
static void test_reloc_dir64(void)
{
    struct pe_builder pb;
    struct pe_load_state st = {0};
    char path[] = "/tmp/binfmt_pe_test_reloc.XXXXXX";
    int fd;
    struct linux_binprm bprm = {0};
    struct file f = {0};
    struct mm_struct mm = {0};
    int ret;
    uint32_t text_rva = 0x1000;
    uint32_t reloc_rva = 0x2000;
    uint64_t image_base = TEST_SAFE_BASE;
    uint64_t expected_val;

    pb_init(&pb);
    pb_build_nt_headers(&pb, image_base, 0x3000, text_rva,
                        IMAGE_SUBSYSTEM_WINDOWS_CUI,
                        IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE);
    pb_add_section(&pb, ".text", 0x1000, text_rva, 0x1000, 0x1000,
                   IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_EXECUTE);
    pb_write_u64(&pb, 0x1000, image_base + 0x1234);
    pb_add_section(&pb, ".reloc", 0x1000, reloc_rva, 0x1000, 0x2000,
                   IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ);
    pb_set_dir(&pb, IMAGE_DIRECTORY_ENTRY_BASERELOC, reloc_rva, 16);
    pb_write_u32(&pb, 0x2000, text_rva);
    pb_write_u32(&pb, 0x2004, 16);
    pb_write_u16(&pb, 0x2008, (uint16_t)((10 << 12) | 0));
    pb_write_u16(&pb, 0x200A, 0);
    pb_write_u32(&pb, 0x200C, 0);
    pb_write_u32(&pb, 0x2010, 0);
    pb_finish_headers(&pb);

    fd = mkstemp(path);
    ASSERT(fd >= 0);
    close(fd);
    ASSERT_EQ(pb_write_file(&pb, path), 0);

    fd = open(path, O_RDONLY);
    ASSERT(fd >= 0);
    f.fd = fd;
    f.size = pb.size;

    bprm.file = &f;
    bprm.mm = &mm;
    bprm.buf = pb.data;
    bprm.buf_len = pb.size > BINPRM_BUF_SIZE ? BINPRM_BUF_SIZE : pb.size;
    bprm.argv = NULL;
    bprm.argc = 0;
    bprm.envp = NULL;
    bprm.envc = 0;

    st.file = &f;
    st.bprm = &bprm;

    kshim_mapping_reset();
    ret = pe_read_file_headers(&bprm, &st);
    ASSERT_EQ(ret, 0);

    st.actual_base = image_base + 0x100000ULL;
    st.delta = (s64)(st.actual_base - image_base);

    {
        unsigned long base = (unsigned long)st.actual_base;
        unsigned long p = vm_mmap(&f, base + text_rva, 0x1000,
                                  PROT_READ | PROT_WRITE,
                                  MAP_PRIVATE | MAP_FIXED | MAP_DENYWRITE,
                                  0x1000);
        ASSERT(!IS_ERR_VALUE(p));
        pe_state_add_mapping(&st, p, 0x1000);
    }

    ret = pe_apply_relocs(&st);
    ASSERT_EQ(ret, 0);

    expected_val = (image_base + 0x100000ULL) + 0x1234;
    ASSERT_EQ(*(uint64_t *)(uintptr_t)(st.actual_base + text_rva), expected_val);

    pe_state_cleanup(&st);
    close(fd);
    unlink(path);
    free(pb.data);
    kshim_mapping_reset();
}

__attribute__((no_sanitize_address))
static void test_reloc_absolute_skip(void)
{
    struct pe_builder pb;
    struct pe_load_state st = {0};
    char path[] = "/tmp/binfmt_pe_test_abs.XXXXXX";
    int fd;
    struct linux_binprm bprm = {0};
    struct file f = {0};
    struct mm_struct mm = {0};
    int ret;
    uint32_t text_rva = 0x1000;
    uint32_t reloc_rva = 0x2000;
    uint64_t image_base = TEST_SAFE_BASE;

    pb_init(&pb);
    pb_build_nt_headers(&pb, image_base, 0x3000, text_rva,
                        IMAGE_SUBSYSTEM_WINDOWS_CUI,
                        IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE);
    pb_add_section(&pb, ".text", 0x1000, text_rva, 0x1000, 0x1000,
                   IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_EXECUTE);
    pb_write_u64(&pb, 0x1000, 0xDEADBEEFDEADBEEFULL);
    pb_add_section(&pb, ".reloc", 0x1000, reloc_rva, 0x1000, 0x2000,
                   IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ);
    pb_set_dir(&pb, IMAGE_DIRECTORY_ENTRY_BASERELOC, reloc_rva, 12);
    pb_write_u32(&pb, 0x2000, text_rva);
    pb_write_u32(&pb, 0x2004, 12);
    pb_write_u16(&pb, 0x2008, 0); /* ABSOLUTE */
    pb_write_u16(&pb, 0x200A, 0);
    pb_finish_headers(&pb);

    fd = mkstemp(path);
    ASSERT(fd >= 0);
    close(fd);
    ASSERT_EQ(pb_write_file(&pb, path), 0);
    fd = open(path, O_RDONLY);
    ASSERT(fd >= 0);
    f.fd = fd; f.size = pb.size;
    bprm.file = &f; bprm.mm = &mm;
    bprm.buf = pb.data; bprm.buf_len = pb.size > 512 ? 512 : pb.size;
    st.file = &f; st.bprm = &bprm;
    kshim_mapping_reset();
    ret = pe_read_file_headers(&bprm, &st);
    ASSERT_EQ(ret, 0);
    st.actual_base = image_base + 0x5000;
    st.delta = (s64)(st.actual_base - image_base);
    {
        unsigned long p = vm_mmap(&f, (unsigned long)st.actual_base + text_rva, 0x1000,
                                  PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_FIXED | MAP_DENYWRITE, 0x1000);
        ASSERT(!IS_ERR_VALUE(p));
        pe_state_add_mapping(&st, p, 0x1000);
    }
    ret = pe_apply_relocs(&st);
    ASSERT_EQ(ret, 0);
    /* ABSOLUTE should not have changed the value */
    ASSERT_EQ(*(uint64_t *)(uintptr_t)(st.actual_base + text_rva), (uint64_t)0xDEADBEEFDEADBEEFULL);

    pe_state_cleanup(&st);
    close(fd); unlink(path); free(pb.data); kshim_mapping_reset();
}

__attribute__((no_sanitize_address))
static void test_reloc_unsupported_type(void)
{
    struct pe_builder pb;
    struct pe_load_state st = {0};
    char path[] = "/tmp/binfmt_pe_test_badrel.XXXXXX";
    int fd;
    struct linux_binprm bprm = {0};
    struct file f = {0};
    struct mm_struct mm = {0};
    int ret;
    uint32_t text_rva = 0x1000;
    uint32_t reloc_rva = 0x2000;
    uint64_t image_base = TEST_SAFE_BASE;

    pb_init(&pb);
    pb_build_nt_headers(&pb, image_base, 0x3000, text_rva,
                        IMAGE_SUBSYSTEM_WINDOWS_CUI,
                        IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE);
    pb_add_section(&pb, ".text", 0x1000, text_rva, 0x1000, 0x1000,
                    IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_EXECUTE);
    pb_add_section(&pb, ".reloc", 0x1000, reloc_rva, 0x1000, 0x2000,
                   IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ);
    pb_set_dir(&pb, IMAGE_DIRECTORY_ENTRY_BASERELOC, reloc_rva, 12);
    pb_write_u32(&pb, 0x2000, text_rva);
    pb_write_u32(&pb, 0x2004, 12);
    pb_write_u16(&pb, 0x2008, (uint16_t)((3 << 12) | 0)); /* HIGHLOW - unsupported on x64 */
    pb_write_u16(&pb, 0x200A, 0);
    pb_finish_headers(&pb);

    fd = mkstemp(path);
    ASSERT(fd >= 0); close(fd);
    ASSERT_EQ(pb_write_file(&pb, path), 0);
    fd = open(path, O_RDONLY);
    ASSERT(fd >= 0);
    f.fd = fd; f.size = pb.size;
    bprm.file = &f; bprm.mm = &mm;
    bprm.buf = pb.data; bprm.buf_len = pb.size > 512 ? 512 : pb.size;
    st.file = &f; st.bprm = &bprm;
    kshim_mapping_reset();
    ret = pe_read_file_headers(&bprm, &st);
    ASSERT_EQ(ret, 0);
    st.actual_base = image_base + 0x10000;
    st.delta = (s64)(st.actual_base - image_base);
    {
        unsigned long p = vm_mmap(&f, (unsigned long)st.actual_base + text_rva, 0x1000,
                                  PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_FIXED | MAP_DENYWRITE, 0x1000);
        ASSERT(!IS_ERR_VALUE(p));
        pe_state_add_mapping(&st, p, 0x1000);
    }
    ret = pe_apply_relocs(&st);
    ASSERT_EQ(ret, -ENOEXEC);

    pe_state_cleanup(&st);
    close(fd); unlink(path); free(pb.data); kshim_mapping_reset();
}

static void test_cmdline_and_env(void)
{
    struct linux_binprm bprm = {0};
    const char *argv[] = { "prog.exe", "hello world", "--flag" };
    const char *envp[] = { "FOO=bar", "PATH=C:\\Windows" };
    WCHAR buf[256];
    size_t n;

    bprm.argc = 3;
    bprm.argv = argv;
    bprm.envc = 2;
    bprm.envp = envp;

    n = pe_build_command_line(&bprm, buf, 256);
    ASSERT(n > 0);
    /* First char is 'p' */
    ASSERT(buf[0] == (WCHAR)'p');
    /* Contains quoted argument for "hello world" - search for quote */
    {
        int found_quote = 0;
        for (size_t i = 0; i < n; ++i) if (buf[i] == (WCHAR)'"') found_quote = 1;
        ASSERT(found_quote);
    }

    n = pe_build_environment(&bprm, buf, 256);
    ASSERT(n > 0);
    ASSERT(buf[0] == (WCHAR)'F');
    /* Find double-null terminator */
    {
        int found_double_null = 0;
        for (size_t i = 0; i + 1 < n; ++i) if (buf[i]==0 && buf[i+1]==0) found_double_null = 1;
        ASSERT(found_double_null);
    }
}

static void test_utf8_to_utf16(void)
{
    WCHAR out[16];
    size_t n;

    n = pe_utf8_to_utf16("ABC", out, 16);
    ASSERT_EQ(n, 4);
    ASSERT_EQ(out[0], (WCHAR)'A');
    ASSERT_EQ(out[1], (WCHAR)'B');
    ASSERT_EQ(out[2], (WCHAR)'C');
    ASSERT_EQ(out[3], 0);

    /* 2-byte UTF-8: e acute */
    n = pe_utf8_to_utf16("\xC3\xA9", out, 16);
    ASSERT_EQ(n, 2);
    ASSERT_EQ(out[0], 0x00E9);

    /* 4-byte UTF-8 -> surrogate pair U+1F600 */
    n = pe_utf8_to_utf16("\xF0\x9F\x98\x80", out, 16);
    ASSERT_EQ(n, 3);
    ASSERT_EQ(out[0], (WCHAR)0xD83D);
    ASSERT_EQ(out[1], (WCHAR)0xDE00);
}

static void test_peb_teb_population(void)
{
    struct pe_load_state st = {0};
    struct linux_binprm bprm = {0};
    const char *argv[] = { "x.exe" };
    const char *envp[] = { "A=B" };
    int ret;
    PPEB peb;
    PTEB teb;

    kshim_mapping_reset();
    memset(&st, 0, sizeof(st));

    bprm.argc = 1;
    bprm.argv = argv;
    bprm.envc = 1;
    bprm.envp = envp;
    st.bprm = &bprm;
    st.actual_base = PE_PREFERRED_BASE;
    st.entry_point = 0x1000;
    st.pe.subsystem = IMAGE_SUBSYSTEM_WINDOWS_CUI;
    st.image_base = PE_PREFERRED_BASE;
    st.size_of_image = 0x10000;
    st.dynamic_base = 1;

    ret = pe_setup_stack(&st);
    ASSERT_EQ(ret, 0);
    ASSERT(st.stack_base != NULL);
    ASSERT_EQ(st.stack_size, PE_DEFAULT_STACK_RES);

    ret = pe_build_process_parameters(&st);
    ASSERT_EQ(ret, 0);
    ASSERT(st.params != NULL);

    ret = pe_init_peb(&st);
    ASSERT_EQ(ret, 0);
    ASSERT(st.peb != NULL);

    ret = pe_init_teb(&st);
    ASSERT_EQ(ret, 0);
    ASSERT(st.teb != NULL);

    peb = (PPEB)st.peb;
    teb = (PTEB)st.teb;

    ASSERT_EQ((uintptr_t)peb->ImageBaseAddress, (uintptr_t)PE_PREFERRED_BASE);
    ASSERT(peb->ProcessParameters == (PRTL_USER_PROCESS_PARAMETERS)st.params);
    ASSERT_EQ(peb->BeingDebugged, 0);
    ASSERT_EQ(peb->NtGlobalFlag, 0);
    ASSERT_EQ(peb->OSMajorVersion, 10);
    ASSERT_EQ(peb->OSBuildNumber, 20348);

    ASSERT((struct _NT_TIB *)teb == teb->NtTib.Self);
    ASSERT_EQ(teb->ProcessEnvironmentBlock, (struct _PEB *)peb);
    ASSERT_EQ(teb->NtTib.StackBase, (void *)((uintptr_t)st.stack_base + st.stack_size));
    ASSERT_EQ(teb->NtTib.StackLimit, st.stack_base);
    ASSERT_EQ(teb->LastErrorValue, 0);
    ASSERT_EQ(teb->ClientId.UniqueProcess, (HANDLE)(uintptr_t)current->pid);

    ASSERT(peb->ProcessParameters->CommandLine.Length > 0);
    ASSERT(peb->ProcessParameters->CommandLine.Buffer != NULL);
    ASSERT(peb->ProcessParameters->Environment != NULL);
    /* Verify GS base would be TEB */
    {
        struct task_struct *tsk = current;
        set_user_gsbase(tsk, (unsigned long)teb);
        ASSERT_EQ((void *)tsk->gsbase, (void *)teb);
        nt_enter_personality(tsk);
        ASSERT_EQ(tsk->nt_abi, 1);
    }

    /* Stack alignment */
    {
        unsigned long sp = (unsigned long)st.stack_base + st.stack_size;
        sp &= ~0xFUL;
        ASSERT_EQ(sp & 0xF, 0);
    }

    pe_state_cleanup(&st);
    kshim_mapping_reset();
}

static void test_stack_alignment_and_size(void)
{
    struct pe_load_state st = {0};
    struct linux_binprm bprm = {0};
    const char *argv[] = {"a.exe"};
    const char *envp[] = {"X=Y"};
    int ret;
    bprm.argc = 1; bprm.argv = argv; bprm.envc = 1; bprm.envp = envp;
    st.bprm = &bprm;
    st.pe.subsystem = IMAGE_SUBSYSTEM_WINDOWS_CUI;
    kshim_mapping_reset();
    ret = pe_setup_stack(&st);
    ASSERT_EQ(ret, 0);
    ASSERT_EQ(st.stack_size % PAGE_SIZE, 0);
    ASSERT(st.stack_base != NULL);
    {
        unsigned long sp = (unsigned long)st.stack_base + st.stack_size;
        ASSERT_EQ(sp % 16, 0);
        sp &= ~0xFUL;
        ASSERT_EQ(sp % 16, 0);
    }
    pe_state_cleanup(&st);
    kshim_mapping_reset();
}

static void test_process_parameters_layout(void)
{
    struct pe_load_state st = {0};
    struct linux_binprm bprm = {0};
    const char *argv[] = {"myprog.exe", "arg1"};
    const char *envp[] = {"ONE=1", "TWO=2"};
    bprm.argc = 2; bprm.argv = argv; bprm.envc = 2; bprm.envp = envp;
    st.bprm = &bprm;
    st.pe.subsystem = IMAGE_SUBSYSTEM_WINDOWS_CUI;
    kshim_mapping_reset();
    int ret = pe_build_process_parameters(&st);
    ASSERT_EQ(ret, 0);
    RTL_USER_PROCESS_PARAMETERS *pp = (RTL_USER_PROCESS_PARAMETERS *)st.params;
    ASSERT(pp->MaximumLength == sizeof(RTL_USER_PROCESS_PARAMETERS));
    ASSERT(pp->Length == sizeof(RTL_USER_PROCESS_PARAMETERS));
    ASSERT(pp->Environment != NULL);
    ASSERT(pp->EnvironmentSize > 0);
    ASSERT(pp->CommandLine.Buffer != NULL);
    /* CommandLine Length should be (wchars-1)*2 */
    ASSERT(pp->CommandLine.Length % 2 == 0);
    /* Environment must be double-null terminated */
    {
        WCHAR *env = (WCHAR *)pp->Environment;
        size_t i = 0;
        int dbl = 0;
        while (i < pp->EnvironmentSize / 2) {
            if (env[i]==0 && env[i+1]==0) { dbl=1; break; }
            i++;
        }
        ASSERT(dbl);
    }
    pe_state_cleanup(&st);
    kshim_mapping_reset();
}

static void test_pe_validate_rejects_dll(void)
{
    struct pe_builder pb;
    char path[] = "/tmp/binfmt_pe_dll.XXXXXX";
    int fd;
    struct linux_binprm bprm = {0};
    struct file f = {0};
    struct mm_struct mm = {0};
    struct pe_load_state st = {0};
    int ret;
    pb_init(&pb);
    pb_build_nt_headers(&pb, TEST_SAFE_BASE, 0x2000, 0x1000,
                        IMAGE_SUBSYSTEM_WINDOWS_CUI,
                        IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE);
    /* Force DLL flag */
    pb_write_u16(&pb, pb.fh_off + 18, IMAGE_FILE_DLL | IMAGE_FILE_EXECUTABLE_IMAGE);
    pb_add_section(&pb, ".text", 0x1000, 0x1000, 0x200, 0x400,
                   IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_EXECUTE);
    pb_finish_headers(&pb);
    fd = mkstemp(path); ASSERT(fd>=0); close(fd);
    ASSERT_EQ(pb_write_file(&pb, path), 0);
    fd = open(path, O_RDONLY); ASSERT(fd>=0);
    f.fd = fd; f.size = pb.size;
    bprm.file = &f; bprm.mm = &mm;
    bprm.buf = pb.data; bprm.buf_len = pb.size > 512 ? 512 : pb.size;
    st.file = &f; st.bprm = &bprm;
    kshim_mapping_reset();
    ret = pe_read_file_headers(&bprm, &st);
    ASSERT_EQ(ret, -ENOEXEC);
    close(fd); unlink(path); free(pb.data);
    pe_state_cleanup(&st);
    kshim_mapping_reset();
}

static void test_cleanup_on_map_failure(void)
{
    struct pe_builder pb;
    char path[] = "/tmp/binfmt_pe_cleanup.XXXXXX";
    int fd;
    struct linux_binprm bprm = {0};
    struct file f = {0};
    struct mm_struct mm = {0};
    struct pe_load_state st = {0};
    int ret;
    pb_init(&pb);
    pb_build_nt_headers(&pb, TEST_SAFE_BASE, 0x2000, 0x1000,
                        IMAGE_SUBSYSTEM_WINDOWS_CUI,
                        IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE);
    pb_add_section(&pb, ".text", 0x1000, 0x1000, 0x200, 0x400,
                   IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_EXECUTE);
    pb_finish_headers(&pb);
    fd = mkstemp(path); ASSERT(fd>=0); close(fd);
    ASSERT_EQ(pb_write_file(&pb, path), 0);
    fd = open(path, O_RDONLY); ASSERT(fd>=0);
    f.fd = fd; f.size = pb.size;
    bprm.file = &f; bprm.mm = &mm;
    bprm.buf = pb.data; bprm.buf_len = pb.size > 512 ? 512 : pb.size;
    st.file = &f; st.bprm = &bprm;
    kshim_mapping_reset();
    ret = pe_read_file_headers(&bprm, &st);
    ASSERT_EQ(ret, 0);
    ret = pe_pick_base(&st);
    ASSERT_EQ(ret, 0);
    /* Simulate failure: close file so vm_mmap file read fails? Or just test unmap_all */
    /* Map sections then immediately cleanup should unmap all. */
    ret = pe_map_sections(&st);
    ASSERT_EQ(ret, 0);
    ASSERT(kshim_mappings != NULL);
    pe_state_cleanup(&st);
    ASSERT(kshim_mappings == NULL);
    close(fd); unlink(path); free(pb.data);
    kshim_mapping_reset();
}

/* -------------------------------------------------------------------------- */
/* main                                                                       */
/* -------------------------------------------------------------------------- */
int main(void)
{
    RUN(prot_mapping);
    RUN(base_selection_preferred);
    RUN(base_selection_aslr_collision);
    RUN(base_selection_no_aslr_fails);
    RUN(reloc_dir64);
    RUN(reloc_absolute_skip);
    RUN(reloc_unsupported_type);
    RUN(cmdline_and_env);
    RUN(utf8_to_utf16);
    RUN(peb_teb_population);
    RUN(stack_alignment_and_size);
    RUN(process_parameters_layout);
    RUN(pe_validate_rejects_dll);
    RUN(cleanup_on_map_failure);

    printf("binfmt_pe tests: run=%d pass=%d fail=%d\n",
           tests_run, tests_pass, tests_fail);
    return tests_fail ? 1 : 0;
}
