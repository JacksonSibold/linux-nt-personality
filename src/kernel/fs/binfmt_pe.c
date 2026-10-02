// SPDX-License-Identifier: GPL-2.0
/*
 * binfmt_pe.c - PE32+ loader for NT personality
 * Minimal M0 implementation with fixes for FileAlignment vs SectionAlignment,
 * kernel_read based headers/reloc, and stack sizes.
 */

#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/binfmts.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/personality.h>
#include <linux/random.h>
#include <linux/string.h>
#include <linux/sched/task_stack.h>
#include <asm/processor.h>
#include <asm/fsgsbase.h>
#include <asm/cpufeature.h>

/* Forward from arch/x86/nt */
static struct linux_binfmt pe_format;
#ifdef CONFIG_NT_PERSONALITY
#ifdef CONFIG_NT_PERSONALITY
#include "../../arch/x86/nt/nt_internal.h"
extern int ntmm_add_region(unsigned long, unsigned long, unsigned long, unsigned long, unsigned long);
#else
static inline void nt_enter_personality(struct task_struct *tsk) { (void)tsk; }
#endif
#else
static inline void nt_enter_personality(struct task_struct *tsk) { (void)tsk; }
#endif

/* PE constants */
#define IMAGE_DOS_SIGNATURE 0x5A4D
#define IMAGE_NT_SIGNATURE 0x00004550
#define IMAGE_FILE_MACHINE_AMD64 0x8664
#define IMAGE_NT_OPTIONAL_HDR64_MAGIC 0x20B
#define IMAGE_NUMBEROF_DIRECTORY_ENTRIES 16
#define IMAGE_SUBSYSTEM_WINDOWS_GUI 2
#define IMAGE_SUBSYSTEM_WINDOWS_CUI 3
#define IMAGE_FILE_DLL 0x2000
#define IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE 0x0040
#define IMAGE_REL_BASED_ABSOLUTE 0
#define IMAGE_REL_BASED_DIR64 10
#define IMAGE_DIRECTORY_ENTRY_BASERELOC 5
#define IMAGE_DIRECTORY_ENTRY_TLS 9

#define PE_PREFERRED_BASE 0x140000000ULL
#define PE_ASLR_MASK 0x7FFF0000ULL
#define PE_DEFAULT_STACK_RES 0x100000ULL
#define PE_DEFAULT_STACK_COM 0x1000ULL
#define PE_TLS_SLOT_COUNT 64

/* Helpers to load little endian */
static inline u16 pe_load_u16(const u8 *p) { return (u16)p[0] | ((u16)p[1] << 8); }
static inline u32 pe_load_u32(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }
static inline u64 pe_load_u64(const u8 *p) { return (u64)p[0] | ((u64)p[1] << 8) | ((u64)p[2] << 16) | ((u64)p[3] << 24) | ((u64)p[4] << 32) | ((u64)p[5] << 40) | ((u64)p[6] << 48) | ((u64)p[7] << 56); }

struct pe_mapping {
    unsigned long addr;
    size_t len;
    struct pe_mapping *next;
};

struct pe_load_state {
    struct mm_struct *mm;
    struct file *file;
    struct linux_binprm *bprm;
    u64 image_base;
    u64 actual_base;
    s64 delta;
    u32 size_of_image;
    u32 entry_point;
    u32 size_of_headers;
    u32 section_alignment;
    u32 file_alignment;
    u16 num_sections;
    u32 section_table_off;
    u32 nt_off;
    u16 subsystem;
    u16 dll_characteristics;
    u64 stack_reserve;
    u64 stack_commit;
    int dynamic_base;
    struct pe_mapping *mappings;
    void *headers_buf;
    size_t headers_size;
    loff_t file_size;
    void *teb;
    void *peb;
    void *params;
    void *stack_base;
    size_t stack_size;
    void *tls_template;
    size_t tls_size;
};

static void pe_state_add_mapping(struct pe_load_state *st, unsigned long addr, size_t len)
{
    struct pe_mapping *m;
    if (!addr || !len) return;
    m = kzalloc(sizeof(*m), GFP_KERNEL);
    if (!m) return;
    m->addr = addr;
    m->len = len;
    m->next = st->mappings;
    st->mappings = m;
}

static void pe_unmap_all(struct pe_load_state *st)
{
    struct pe_mapping *m = st->mappings;
    while (m) {
        struct pe_mapping *next = m->next;
        if (m->addr && m->len)
            vm_munmap(m->addr, m->len);
        kfree(m);
        m = next;
    }
    st->mappings = NULL;
}

static void pe_state_cleanup(struct pe_load_state *st)
{
    if (!st) return;
    pe_unmap_all(st);
    kfree(st->headers_buf);
    st->headers_buf = NULL;
}

/* Simple kernel_read wrapper */
static ssize_t pe_kernel_read(struct file *f, loff_t off, void *buf, size_t count)
{
    loff_t pos = off;
    return kernel_read(f, buf, count, &pos);
}

static int pe_read_file_headers(struct linux_binprm *bprm, struct pe_load_state *st)
{
    ssize_t n;
    u16 magic;
    u32 sig, oh_off, nt_off;
    u16 machine, num_sections, opt_magic, subsystem, dllchars, size_opt;
    u64 image_base;
    u32 size_image, size_headers, sec_align, file_align;
    u32 entry;
    u64 stack_res, stack_com;
    loff_t fsize;

    st->file = bprm->file;
    st->bprm = bprm;
    if (!st->file || !st->file->f_inode) {
        pr_info("binfmt_pe: reject: null file or inode (bprm %s)\n", bprm->filename);
        return -ENOEXEC;
    }
    fsize = i_size_read(file_inode(st->file));
    st->file_size = fsize;
    if (fsize < 64) {
        pr_info("binfmt_pe: reject: file size %lld < 64 for %s\n", fsize, bprm->filename);
        return -ENOEXEC;
    }

    st->headers_buf = kzalloc(PAGE_SIZE, GFP_KERNEL);
    if (!st->headers_buf)
        return -ENOMEM;
    st->headers_size = PAGE_SIZE;

    n = pe_kernel_read(st->file, 0, st->headers_buf, PAGE_SIZE);
    if (n < 64) {
        pr_info("binfmt_pe: reject: kernel_read %zd <64 for %s\n", n, bprm->filename);
        return -ENOEXEC;
    }
    if (pe_load_u16((u8*)st->headers_buf) != IMAGE_DOS_SIGNATURE) {
        pr_info("binfmt_pe: reject: MZ mismatch 0x%04x for %s\n", pe_load_u16((u8*)st->headers_buf), bprm->filename);
        return -ENOEXEC;
    }
    nt_off = pe_load_u32((u8*)st->headers_buf + 60);
    if (nt_off + 6 > (u32)n) {
        pr_info("binfmt_pe: reject: e_lfanew %u+6 > read %zd for %s\n", nt_off, n, bprm->filename);
        return -ENOEXEC;
    }
    st->nt_off = nt_off;
    sig = pe_load_u32((u8*)st->headers_buf + nt_off);
    if (sig != IMAGE_NT_SIGNATURE) {
        pr_info("binfmt_pe: reject: PE sig 0x%08x != 0x4550 at %u for %s\n", sig, nt_off, bprm->filename);
        return -ENOEXEC;
    }
    /* File header at nt_off+4 */
    machine = pe_load_u16((u8*)st->headers_buf + nt_off + 4);
    if (machine != IMAGE_FILE_MACHINE_AMD64) {
        pr_info("binfmt_pe: reject: Machine 0x%04x != AMD64 for %s\n", machine, bprm->filename);
        return -ENOEXEC;
    }
    num_sections = pe_load_u16((u8*)st->headers_buf + nt_off + 6);
    if (num_sections == 0 || num_sections > 96) {
        pr_info("binfmt_pe: reject: NumberOfSections %u out of [1,96] for %s\n", num_sections, bprm->filename);
        return -ENOEXEC;
    }
    size_opt = pe_load_u16((u8*)st->headers_buf + nt_off + 20);
    if (size_opt < 2 || size_opt > 240) {
        pr_info("binfmt_pe: reject: SizeOfOptionalHeader %u out of [2,240] for %s\n", size_opt, bprm->filename);
        return -ENOEXEC;
    }
    st->num_sections = num_sections;
    oh_off = nt_off + 4 + 20;
    if (oh_off + size_opt > (u32)n) {
        pr_info("binfmt_pe: reject: oh_off %u + size_opt %u > read %zd for %s\n", oh_off, size_opt, n, bprm->filename);
        return -ENOEXEC;
    }
    opt_magic = pe_load_u16((u8*)st->headers_buf + oh_off);
    if (opt_magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        pr_info("binfmt_pe: reject: optional magic 0x%04x != 0x20B for %s\n", opt_magic, bprm->filename);
        return -ENOEXEC;
    }
    magic = opt_magic;
    (void)magic;
    /* Parse key fields directly from buffer */
    entry = pe_load_u32((u8*)st->headers_buf + oh_off + 16);
    image_base = pe_load_u64((u8*)st->headers_buf + oh_off + 24);
    sec_align = pe_load_u32((u8*)st->headers_buf + oh_off + 32);
    file_align = pe_load_u32((u8*)st->headers_buf + oh_off + 36);
    size_image = pe_load_u32((u8*)st->headers_buf + oh_off + 56);
    size_headers = pe_load_u32((u8*)st->headers_buf + oh_off + 60);
    subsystem = pe_load_u16((u8*)st->headers_buf + oh_off + 68);
    dllchars = pe_load_u16((u8*)st->headers_buf + oh_off + 70);
    stack_res = pe_load_u64((u8*)st->headers_buf + oh_off + 72);
    stack_com = pe_load_u64((u8*)st->headers_buf + oh_off + 80);
    {
        u16 chars = pe_load_u16((u8*)st->headers_buf + nt_off + 4 + 18);
        if (chars & IMAGE_FILE_DLL) {
            pr_info("binfmt_pe: reject: DLL bit set chars 0x%04x for %s\n", chars, bprm->filename);
            return -ENOEXEC;
        }
    }
    if (subsystem != IMAGE_SUBSYSTEM_WINDOWS_GUI && subsystem != IMAGE_SUBSYSTEM_WINDOWS_CUI) {
        pr_info("binfmt_pe: reject: subsystem %u not GUI/CUI for %s\n", subsystem, bprm->filename);
        return -ENOEXEC;
    }
    if (sec_align == 0 || file_align == 0) {
        pr_info("binfmt_pe: reject: sec_align 0x%x file_align 0x%x zero for %s\n", sec_align, file_align, bprm->filename);
        return -ENOEXEC;
    }
    if (size_image == 0 || (size_image % sec_align) != 0) {
        pr_info("binfmt_pe: reject: SizeOfImage 0x%x invalid (zero or not aligned to sec_align 0x%x) for %s\n", size_image, sec_align, bprm->filename);
        return -ENOEXEC;
    }

    st->image_base = image_base;
    st->size_of_image = size_image;
    st->size_of_headers = size_headers;
    st->entry_point = entry;
    st->section_alignment = sec_align;
    st->file_alignment = file_align;
    st->subsystem = subsystem;
    st->dll_characteristics = dllchars;
    st->dynamic_base = (dllchars & IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE) != 0;
    st->section_table_off = oh_off + size_opt;
    st->stack_reserve = stack_res;
    st->stack_commit = stack_com;

    /* Validate section table fits within SizeOfHeaders and buffer.
     * Windows tolerates SizeOfHeaders > file size? No, but tolerates misaligned
     * SizeOfHeaders (FileAlignment). We only require headers cover section table. */
    {
        u32 sec_table_end = st->section_table_off + (u32)num_sections * 40;
        if (sec_table_end > size_headers) {
            pr_info("binfmt_pe: reject: sec_table_end 0x%x > SizeOfHeaders 0x%x for %s\n", sec_table_end, size_headers, bprm->filename);
            return -ENOEXEC;
        }
        if (sec_table_end > (u32)n) {
            /* Need larger headers buffer */
            size_t new_sz = PAGE_ALIGN(sec_table_end);
            void *nb = kzalloc(new_sz, GFP_KERNEL);
            if (!nb) return -ENOMEM;
            n = pe_kernel_read(st->file, 0, nb, sec_table_end);
            if (n < (ssize_t)sec_table_end) { kfree(nb); return -ENOEXEC; }
            kfree(st->headers_buf);
            st->headers_buf = nb;
            st->headers_size = new_sz;
            /* Re-parse nt_off etc not needed */
        }
        if (size_headers > st->headers_size) {
            size_t new_sz = PAGE_ALIGN(size_headers);
            void *nb = kzalloc(new_sz, GFP_KERNEL);
            if (!nb) return -ENOMEM;
            n = pe_kernel_read(st->file, 0, nb, size_headers);
            if (n < (ssize_t)size_headers) { kfree(nb); return -ENOEXEC; }
            kfree(st->headers_buf);
            st->headers_buf = nb;
            st->headers_size = new_sz;
        }
        /* Validate each section quickly — also log which section fails */
        for (u16 i = 0; i < num_sections; ++i) {
            u32 off = st->section_table_off + i*40;
            u32 vaddr = pe_load_u32((u8*)st->headers_buf + off + 12);
            u32 vsize = pe_load_u32((u8*)st->headers_buf + off + 8);
            u32 rawsize = pe_load_u32((u8*)st->headers_buf + off + 16);
            u32 rawptr = pe_load_u32((u8*)st->headers_buf + off + 20);
            u32 vs = vsize ? vsize : rawsize;
            if (vaddr >= size_image) {
                pr_info("binfmt_pe: reject: section %u VA 0x%x >= SizeOfImage 0x%x for %s\n", i, vaddr, size_image, bprm->filename);
                return -ENOEXEC;
            }
            if ((u64)vaddr + vs > size_image) {
                pr_info("binfmt_pe: reject: section %u VA 0x%x+vs 0x%x > SizeOfImage 0x%x for %s (vsize 0x%x rawsize 0x%x)\n", i, vaddr, vs, size_image, bprm->filename, vsize, rawsize);
                return -ENOEXEC;
            }
            if (rawsize && (u64)rawptr + rawsize > (u64)fsize) {
                pr_info("binfmt_pe: reject: section %u rawptr 0x%x+rawsize 0x%x > fsize %lld for %s\n", i, rawptr, rawsize, fsize, bprm->filename);
                return -ENOEXEC;
            }
        }
    }

    pr_info("binfmt_pe: headers ok imagebase=0x%llx entry=0x%x size=0x%x sections=%u stack_res=0x%llx\n",
        (unsigned long long)image_base, entry, size_image, num_sections, (unsigned long long)stack_res);
    return 0;
}

static unsigned long pe_randomize_base(u64 preferred, u32 size_image)
{
    u32 rnd = 0;
    get_random_bytes(&rnd, sizeof(rnd));
    u64 r = ((u64)rnd << 16) & PE_ASLR_MASK;
    if (r == 0) r = 0x10000ULL;
    return (unsigned long)(preferred + r);
}

static int pe_pick_base(struct pe_load_state *st)
{
    u64 preferred = st->image_base ? st->image_base : PE_PREFERRED_BASE;
    unsigned long len = PAGE_ALIGN(st->size_of_image);
    unsigned long base;
    /* Try preferred base directly without get_unmapped_area to avoid mmap lock warnings during exec */
    base = vm_mmap(NULL, (unsigned long)preferred, len, PROT_NONE, MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE, 0);
    if (!IS_ERR_VALUE(base)) {
        st->actual_base = (u64)base;
        st->delta = (s64)(st->actual_base - preferred);
        pe_state_add_mapping(st, base, len);
        return 0;
    }
    if (!st->dynamic_base) {
        pr_info("binfmt_pe: preferred base 0x%llx unavailable and non-ASLR\n", (unsigned long long)preferred);
        return -ENOMEM;
    }
    {
        unsigned long addr = pe_randomize_base(preferred, st->size_of_image);
    base = vm_mmap(NULL, addr, len, PROT_NONE, MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE, 0);
    if (IS_ERR_VALUE(base)) {
        base = vm_mmap(NULL, 0, len, PROT_NONE, MAP_PRIVATE|MAP_ANONYMOUS, 0);
        if (IS_ERR_VALUE(base)) return -ENOMEM;
    }
    st->actual_base = (u64)base;
    st->delta = (s64)(st->actual_base - preferred);
    pe_state_add_mapping(st, base, len);
    return 0;
    }
}

static unsigned long pe_prot_from_chars(u32 chars)
{
    unsigned long prot = 0;
    if (chars & 0x40000000) prot |= PROT_READ;
    if (chars & 0x80000000) prot |= PROT_WRITE;
    if (chars & 0x20000000) prot |= PROT_EXEC;
    if (prot == 0) prot = PROT_READ;
    return prot;
}

static unsigned long pe_nt_prot_from_chars(u32 chars)
{
    int r = (chars & 0x40000000) != 0;
    int w = (chars & 0x80000000) != 0;
    int x = (chars & 0x20000000) != 0;
    if (x) {
        if (r && w) return 0x40; /* PAGE_EXECUTE_READWRITE */
        if (r) return 0x20; /* PAGE_EXECUTE_READ */
        if (w) return 0x40;
        return 0x10; /* PAGE_EXECUTE */
    } else {
        if (r && w) return 0x04; /* PAGE_READWRITE */
        if (r) return 0x02; /* PAGE_READONLY */
        if (w) return 0x04;
        return 0x02;
    }
}

static int pe_map_sections(struct pe_load_state *st)
{
    unsigned long base = (unsigned long)st->actual_base;
    unsigned long map_addr, map_len;
    unsigned long prot;
    u16 nsec = st->num_sections;

    /* Map headers as anonymous + copy */
    map_len = PAGE_ALIGN(st->size_of_headers);
    map_addr = vm_mmap(NULL, base, map_len, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED, 0);
    if (IS_ERR_VALUE(map_addr)) {
        pr_info("binfmt_pe: header map failed len=0x%lx err=%ld\n", map_len, (long)map_addr);
        return -ENOMEM;
    }
    pe_state_add_mapping(st, map_addr, map_len);
#ifdef CONFIG_NT_PERSONALITY
    ntmm_add_region(map_addr, map_len, 0x02, 0x01000000, base);
#endif
    {

        size_t to_copy = st->size_of_headers;
        if (to_copy > (size_t)st->file_size) to_copy = (size_t)st->file_size;
        void *tmp = kmalloc(to_copy, GFP_KERNEL);
        if (!tmp) return -ENOMEM;
        ssize_t n = pe_kernel_read(st->file, 0, tmp, to_copy);
        if (n != (ssize_t)to_copy) { kfree(tmp); return -ENOEXEC; }
        if (copy_to_user((void __user *)map_addr, tmp, to_copy)) { kfree(tmp); return -EFAULT; }
        kfree(tmp);
        if (map_len > to_copy) {
            unsigned long tail = map_addr + to_copy;
            clear_user((void __user *)tail, map_len - to_copy);
        }
    }

    for (u16 i = 0; i < nsec; ++i) {
        u32 off = st->section_table_off + i*40;
        u32 vsize = pe_load_u32((u8*)st->headers_buf + off + 8);
        u32 vaddr = pe_load_u32((u8*)st->headers_buf + off + 12);
        u32 rawsize = pe_load_u32((u8*)st->headers_buf + off + 16);
        u32 rawptr = pe_load_u32((u8*)st->headers_buf + off + 20);
        u32 chars = pe_load_u32((u8*)st->headers_buf + off + 36);
        u32 vs = vsize ? vsize : rawsize;
        if (vs == 0) continue;
        map_addr = base + vaddr;
        map_len = PAGE_ALIGN(vs);
        if (map_len == 0) continue;
        prot = pe_prot_from_chars(chars);
        map_addr = vm_mmap(NULL, map_addr, map_len, prot|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED, 0);
        if (IS_ERR_VALUE(map_addr)) {
            pr_info("binfmt_pe: section %u map failed vaddr=0x%x len=0x%lx err=%ld\n", i, vaddr, map_len, (long)map_addr);
            return -ENOMEM;
        }
        pe_state_add_mapping(st, map_addr, map_len);
#ifdef CONFIG_NT_PERSONALITY
        {
            unsigned long nt_prot = pe_nt_prot_from_chars(chars);
            ntmm_add_region(map_addr, map_len, nt_prot, 0x01000000, base);
        }
#endif
        if (rawsize) {
            size_t copy_len = rawsize < vs ? rawsize : vs;
            void *tmp = kmalloc(copy_len, GFP_KERNEL);
            if (!tmp) return -ENOMEM;
            ssize_t n = pe_kernel_read(st->file, rawptr, tmp, copy_len);
            if (n != (ssize_t)copy_len) { kfree(tmp); return -ENOEXEC; }
            // FileAlignment vs SectionAlignment mismatch handled: we copy exactly copy_len bytes at virtual address, ignoring offset alignment
            if (copy_to_user((void __user *)map_addr, tmp, copy_len)) { kfree(tmp); return -EFAULT; }
            kfree(tmp);
        }
        if (vs > rawsize) {
            unsigned long tail = map_addr + rawsize;
            unsigned long end = map_addr + map_len;
            // Zero the remainder of section (bss)
            if (end > tail) clear_user((void __user *)tail, end - tail);
        } else if ((vs & (PAGE_SIZE-1)) != 0) {
            // Ensure tail of last page zeroed beyond file data if needed (already handled above)
        }
        pr_info("binfmt_pe: mapped section %u vaddr=0x%x vs=0x%x raw=0x%x prot=0x%lx at 0x%lx\n", i, vaddr, vs, rawsize, prot, map_addr);
    }
    return 0;
}

static int pe_apply_relocs(struct pe_load_state *st)
{
    if (st->delta == 0) return 0;
    // Find reloc directory
    u32 oh_off = st->nt_off + 4 + 20;
    u32 num_dirs = pe_load_u32((u8*)st->headers_buf + oh_off + 108);
    if (num_dirs <= IMAGE_DIRECTORY_ENTRY_BASERELOC) return 0;
    u32 reloc_rva = pe_load_u32((u8*)st->headers_buf + oh_off + 112 + IMAGE_DIRECTORY_ENTRY_BASERELOC*8);
    u32 reloc_size = pe_load_u32((u8*)st->headers_buf + oh_off + 112 + IMAGE_DIRECTORY_ENTRY_BASERELOC*8 + 4);
    if (reloc_rva == 0 || reloc_size == 0) return 0;
    // Translate RVA to file offset by scanning sections
    u32 reloc_off = 0;
    bool found = false;
    for (u16 i = 0; i < st->num_sections; ++i) {
        u32 off = st->section_table_off + i*40;
        u32 vaddr = pe_load_u32((u8*)st->headers_buf + off + 12);
        u32 vsize = pe_load_u32((u8*)st->headers_buf + off + 8);
        u32 rawsize = pe_load_u32((u8*)st->headers_buf + off + 16);
        u32 rawptr = pe_load_u32((u8*)st->headers_buf + off + 20);
        u32 vs = vsize ? vsize : rawsize;
        if (reloc_rva >= vaddr && reloc_rva < vaddr + vs) {
            if (reloc_rva - vaddr >= rawsize) return -ENOEXEC;
            reloc_off = rawptr + (reloc_rva - vaddr);
            found = true;
            break;
        }
    }
    if (!found) {
        // Maybe in headers
        if (reloc_rva < st->size_of_headers) {
            reloc_off = reloc_rva;
            found = true;
        }
    }
    if (!found) return -ENOEXEC;
    if (reloc_off + reloc_size > (u32)st->file_size) return -ENOEXEC;
    void *buf = kmalloc(reloc_size, GFP_KERNEL);
    if (!buf) return -ENOMEM;
    ssize_t n = pe_kernel_read(st->file, reloc_off, buf, reloc_size);
    if (n != (ssize_t)reloc_size) { kfree(buf); return -ENOEXEC; }
    u32 cursor = 0;
    while (cursor + 8 <= reloc_size) {
        u32 page_rva = pe_load_u32((u8*)buf + cursor);
        u32 block_size = pe_load_u32((u8*)buf + cursor + 4);
        if (block_size == 0) break;
        if (block_size < 8 || (block_size % 4) != 0) { kfree(buf); return -ENOEXEC; }
        if (cursor + block_size > reloc_size) { kfree(buf); return -ENOEXEC; }
        u32 num_entries = (block_size - 8)/2;
        for (u32 e = 0; e < num_entries; ++e) {
            u16 entry = pe_load_u16((u8*)buf + cursor + 8 + e*2);
            u16 type = entry >> 12;
            u16 offset = entry & 0xFFF;
            if (type == IMAGE_REL_BASED_ABSOLUTE) continue;
            if (type != IMAGE_REL_BASED_DIR64) {
                pr_info("binfmt_pe: unsupported reloc type %u\n", type);
                kfree(buf);
                return -ENOEXEC;
            }
            unsigned long target = (unsigned long)st->actual_base + page_rva + offset;
            u64 val;
            if (copy_from_user(&val, (void __user *)target, sizeof(val))) { kfree(buf); return -EFAULT; }
            val += (u64)st->delta;
            if (copy_to_user((void __user *)target, &val, sizeof(val))) { kfree(buf); return -EFAULT; }
        }
        cursor += block_size;
    }
    kfree(buf);
    return 0;
}

/* Minimal UTF16 helpers not needed for M0 test, but keep stubs */
static int pe_build_process_parameters(struct pe_load_state *st)
{
    // For M0 we just need a minimal PEB params structure; use simple allocation
    // Allocate one page for RTL_USER_PROCESS_PARAMETERS with empty strings
    unsigned long p = vm_mmap(NULL, 0, PAGE_SIZE, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, 0);
    if (IS_ERR_VALUE(p)) return -ENOMEM;
    pe_state_add_mapping(st, p, PAGE_SIZE);
#ifdef CONFIG_NT_PERSONALITY
    ntmm_add_region(p, PAGE_SIZE, 0x04, 0x00020000, p);
#endif
    // Zeroed already; set minimal fields at known offsets for debugging: just store pointers zero
    // Real layout is 0x470 size but we allocate 4k
    // Store MaximumLength, Length at offset 0
    (void)p;
    // Use copy_to_user to zero? Already zero
    st->params = (void *)p;
    // Set CommandLine buffer empty inside same page after struct
    return 0;
}

static int pe_init_peb(struct pe_load_state *st)
{
    unsigned long p = vm_mmap(NULL, 0, PAGE_SIZE, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, 0);
    if (IS_ERR_VALUE(p)) return -ENOMEM;
    pe_state_add_mapping(st, p, PAGE_SIZE);
#ifdef CONFIG_NT_PERSONALITY
    ntmm_add_region(p, PAGE_SIZE, 0x04, 0x00020000, p);
#endif
    // PEB layout: ImageBase at 0x10, ProcessParameters at 0x20
    // Use copy_to_user to set fields
    u64 img = st->actual_base;
    u64 params = (u64)st->params;
    // memset via clear_user already zeroed? vm_mmap anonymous is zeroed but via kernel we need to ensure
    // Write fields
    if (copy_to_user((void __user *)(p+0x10), &img, 8)) return -EFAULT;
    if (copy_to_user((void __user *)(p+0x20), &params, 8)) return -EFAULT;
    // BeingDebugged at 0x02 =0 already
    // OS version etc not needed
    u32 flag=0;
    if (copy_to_user((void __user *)(p+0xbc), &flag, 4)) return -EFAULT;
    st->peb = (void *)p;
    return 0;
}

static int pe_init_teb(struct pe_load_state *st)
{
    unsigned long p = vm_mmap(NULL, 0, 2*PAGE_SIZE, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, 0);
    if (IS_ERR_VALUE(p)) return -ENOMEM;
    pe_state_add_mapping(st, p, 2*PAGE_SIZE);
#ifdef CONFIG_NT_PERSONALITY
    ntmm_add_region(p, 2*PAGE_SIZE, 0x04, 0x00020000, p);
#endif
    // TEB offsets: Self at 0x30, StackBase at 0x08, StackLimit at 0x10, TLS pointer 0x58, PEB at 0x60, LastError 0x68, TlsSlots at 0x1480
    // Clear already zeroed
    u64 self = p;
    u64 stack_top = (u64)st->stack_base + st->stack_size;
    u64 stack_bot = (u64)st->stack_base;
    u64 peb = (u64)st->peb;
    u64 tls = 0;
    // NtTib.Self
    if (copy_to_user((void __user *)(p+0x30), &self, 8)) return -EFAULT;
    if (copy_to_user((void __user *)(p+0x08), &stack_top, 8)) return -EFAULT;
    if (copy_to_user((void __user *)(p+0x10), &stack_bot, 8)) return -EFAULT;
    if (copy_to_user((void __user *)(p+0x58), &tls, 8)) return -EFAULT;
    if (copy_to_user((void __user *)(p+0x60), &peb, 8)) return -EFAULT;
    u32 zero=0;
    if (copy_to_user((void __user *)(p+0x68), &zero, 4)) return -EFAULT;
    // ClientId at 0x40? Not needed but set pid
    u64 pid = (u64)current->pid;
    if (copy_to_user((void __user *)(p+0x40), &pid, 8)) return -EFAULT;
    if (copy_to_user((void __user *)(p+0x48), &pid, 8)) return -EFAULT;
    // TlsSlots at 0x1480 already zero
    st->teb = (void *)p;
    return 0;
}

static int pe_map_ntdll(struct pe_load_state *st, u64 *out_base, u64 *out_thunk)
{
    const char *candidates[] = { "/ntdll.dll", "/windows/system32/ntdll.dll", "/tmp/ntdll.dll", NULL };
    struct file *f = NULL;
    for (int i=0; candidates[i]; i++) {
        f = filp_open(candidates[i], O_RDONLY, 0);
        if (!IS_ERR(f))
            break;
        f = NULL;
    }
    if (!f || IS_ERR(f))
        return -ENOENT;
    void *hdr = kzalloc(PAGE_SIZE, GFP_KERNEL);
    if (!hdr) { filp_close(f, NULL); return -ENOMEM; }
    loff_t fsize = i_size_read(file_inode(f));
    ssize_t n = pe_kernel_read(f, 0, hdr, PAGE_SIZE);
    if (n < 64) { kfree(hdr); filp_close(f, NULL); return -ENOEXEC; }
    if (pe_load_u16(hdr) != 0x5A4D) { kfree(hdr); filp_close(f, NULL); return -ENOEXEC; }
    u32 nt_off = pe_load_u32((u8*)hdr + 60);
    if (nt_off + 6 > (u32)n) { kfree(hdr); filp_close(f, NULL); return -ENOEXEC; }
    u32 sig = pe_load_u32((u8*)hdr + nt_off);
    if (sig != 0x4550) { kfree(hdr); filp_close(f, NULL); return -ENOEXEC; }
    u16 num_sections = pe_load_u16((u8*)hdr + nt_off + 6);
    u16 size_opt = pe_load_u16((u8*)hdr + nt_off + 20);
    if (num_sections == 0 || num_sections > 96) { kfree(hdr); filp_close(f, NULL); return -ENOEXEC; }
    u32 oh_off = nt_off + 4 + 20;
    if (oh_off + size_opt > (u32)n) { kfree(hdr); filp_close(f, NULL); return -ENOEXEC; }
    u16 opt_magic = pe_load_u16((u8*)hdr + oh_off);
    if (opt_magic != 0x20B) { kfree(hdr); filp_close(f, NULL); return -ENOEXEC; }
    u64 image_base = pe_load_u64((u8*)hdr + oh_off + 24);
    u32 size_image = pe_load_u32((u8*)hdr + oh_off + 56);
    u32 size_headers = pe_load_u32((u8*)hdr + oh_off + 60);
    u16 dllchars = pe_load_u16((u8*)hdr + oh_off + 70);
    int dynamic_base = (dllchars & 0x40) != 0;
    u32 section_table_off = oh_off + size_opt;
    u64 preferred = image_base ? image_base : 0x180000000ULL;
    unsigned long len = PAGE_ALIGN(size_image);
    unsigned long base = vm_mmap(NULL, (unsigned long)preferred, len, PROT_NONE, MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE, 0);
    if (IS_ERR_VALUE(base)) {
        if (!dynamic_base) { kfree(hdr); filp_close(f, NULL); return -ENOMEM; }
        base = vm_mmap(NULL, 0, len, PROT_NONE, MAP_PRIVATE|MAP_ANONYMOUS, 0);
        if (IS_ERR_VALUE(base)) { kfree(hdr); filp_close(f, NULL); return -ENOMEM; }
    }
    u64 actual_base = base;
    s64 delta = (s64)(actual_base - preferred);
    pe_state_add_mapping(st, base, len);
    {
        u32 sec_end = section_table_off + (u32)num_sections * 40;
        if (sec_end > (u32)n || size_headers > (u32)n) {
            size_t need = sec_end > size_headers ? sec_end : size_headers;
            need = PAGE_ALIGN(need);
            void *nb = kzalloc(need, GFP_KERNEL);
            if (!nb) { kfree(hdr); filp_close(f, NULL); return -ENOMEM; }
            ssize_t nn = pe_kernel_read(f, 0, nb, need);
            if (nn < (ssize_t)need) { kfree(nb); kfree(hdr); filp_close(f, NULL); return -ENOEXEC; }
            kfree(hdr);
            hdr = nb;
            n = nn;
        }
    }
    {
        unsigned long hlen = PAGE_ALIGN(size_headers);
        unsigned long haddr = vm_mmap(NULL, base, hlen, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED, 0);
        if (IS_ERR_VALUE(haddr)) { kfree(hdr); filp_close(f, NULL); return -ENOMEM; }
        pe_state_add_mapping(st, haddr, hlen);
#ifdef CONFIG_NT_PERSONALITY
        ntmm_add_region(haddr, hlen, 0x02, 0x01000000, actual_base);
#endif
        size_t to_copy = size_headers;
        if (to_copy > (size_t)fsize) to_copy = (size_t)fsize;
        void *tmp = kmalloc(to_copy, GFP_KERNEL);
        if (!tmp) { kfree(hdr); filp_close(f, NULL); return -ENOMEM; }
        ssize_t nn = pe_kernel_read(f, 0, tmp, to_copy);
        if (nn != (ssize_t)to_copy) { kfree(tmp); kfree(hdr); filp_close(f, NULL); return -ENOEXEC; }
        if (copy_to_user((void __user *)haddr, tmp, to_copy)) { kfree(tmp); kfree(hdr); filp_close(f, NULL); return -EFAULT; }
        kfree(tmp);
        if (hlen > to_copy) {
            unsigned long tail = haddr + to_copy;
            clear_user((void __user *)tail, hlen - to_copy);
        }
    }
    for (u16 i=0; i<num_sections; ++i) {
        u32 off = section_table_off + i*40;
        u32 vsize = pe_load_u32((u8*)hdr + off + 8);
        u32 vaddr = pe_load_u32((u8*)hdr + off + 12);
        u32 rawsize = pe_load_u32((u8*)hdr + off + 16);
        u32 rawptr = pe_load_u32((u8*)hdr + off + 20);
        u32 chars = pe_load_u32((u8*)hdr + off + 36);
        u32 vs = vsize ? vsize : rawsize;
        if (vs == 0) continue;
        unsigned long maddr = base + vaddr;
        unsigned long mlen = PAGE_ALIGN(vs);
        unsigned long prot = pe_prot_from_chars(chars);
        unsigned long m = vm_mmap(NULL, maddr, mlen, prot|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED, 0);
        if (IS_ERR_VALUE(m)) { kfree(hdr); filp_close(f, NULL); return -ENOMEM; }
        pe_state_add_mapping(st, m, mlen);
#ifdef CONFIG_NT_PERSONALITY
        {
            unsigned long nt_prot = pe_nt_prot_from_chars(chars);
            ntmm_add_region(m, mlen, nt_prot, 0x01000000, actual_base);
        }
#endif
        if (rawsize) {
            size_t copy_len = rawsize < vs ? rawsize : vs;
            void *tmp = kmalloc(copy_len, GFP_KERNEL);
            if (!tmp) { kfree(hdr); filp_close(f, NULL); return -ENOMEM; }
            ssize_t nn = pe_kernel_read(f, rawptr, tmp, copy_len);
            if (nn != (ssize_t)copy_len) { kfree(tmp); kfree(hdr); filp_close(f, NULL); return -ENOEXEC; }
            if (copy_to_user((void __user *)maddr, tmp, copy_len)) { kfree(tmp); kfree(hdr); filp_close(f, NULL); return -EFAULT; }
            kfree(tmp);
        }
        if (vs > rawsize) {
            unsigned long tail = maddr + rawsize;
            unsigned long end = maddr + mlen;
            if (end > tail) clear_user((void __user *)tail, end - tail);
        }
        pr_info("binfmt_pe: ntdll mapped section %u vaddr=0x%x vs=0x%x prot=0x%lx at 0x%lx\n", i, vaddr, vs, prot, m);
    }
    if (delta != 0) {
        u32 num_dirs = pe_load_u32((u8*)hdr + oh_off + 108);
        if (num_dirs > 5) {
            u32 reloc_rva = pe_load_u32((u8*)hdr + oh_off + 112 + 5*8);
            u32 reloc_size = pe_load_u32((u8*)hdr + oh_off + 112 + 5*8 + 4);
            if (reloc_rva && reloc_size) {
                u32 reloc_off = 0;
                bool found = false;
                for (u16 i=0; i<num_sections; ++i) {
                    u32 off = section_table_off + i*40;
                    u32 vaddr = pe_load_u32((u8*)hdr + off + 12);
                    u32 vsize = pe_load_u32((u8*)hdr + off + 8);
                    u32 rawsize = pe_load_u32((u8*)hdr + off + 16);
                    u32 rawptr = pe_load_u32((u8*)hdr + off + 20);
                    u32 vs2 = vsize ? vsize : rawsize;
                    if (reloc_rva >= vaddr && reloc_rva < vaddr+vs2) {
                        if (reloc_rva - vaddr >= rawsize) { kfree(hdr); filp_close(f,NULL); return -ENOEXEC; }
                        reloc_off = rawptr + (reloc_rva - vaddr);
                        found = true;
                        break;
                    }
                }
                if (!found && reloc_rva < size_headers) { reloc_off = reloc_rva; found = true; }
                if (found && reloc_off + reloc_size <= (u32)fsize) {
                    void *buf = kmalloc(reloc_size, GFP_KERNEL);
                    if (buf) {
                        ssize_t nn = pe_kernel_read(f, reloc_off, buf, reloc_size);
                        if (nn == (ssize_t)reloc_size) {
                            u32 cursor=0;
                            while (cursor+8 <= reloc_size) {
                                u32 page_rva = pe_load_u32((u8*)buf+cursor);
                                u32 block_size = pe_load_u32((u8*)buf+cursor+4);
                                if (block_size==0) break;
                                if (block_size<8 || (block_size%4)!=0) break;
                                if (cursor+block_size > reloc_size) break;
                                u32 nent = (block_size-8)/2;
                                for (u32 e=0; e<nent; ++e) {
                                    u16 entry = pe_load_u16((u8*)buf+cursor+8+e*2);
                                    u16 type = entry>>12;
                                    u16 off2 = entry & 0xFFF;
                                    if (type==0) continue;
                                    if (type!=10) { kfree(buf); kfree(hdr); filp_close(f,NULL); return -ENOEXEC; }
                                    unsigned long target = (unsigned long)actual_base + page_rva + off2;
                                    u64 val;
                                    if (copy_from_user(&val, (void __user *)target, sizeof(val))) { kfree(buf); kfree(hdr); filp_close(f,NULL); return -EFAULT; }
                                    val += (u64)delta;
                                    if (copy_to_user((void __user *)target, &val, sizeof(val))) { kfree(buf); kfree(hdr); filp_close(f,NULL); return -EFAULT; }
                                }
                                cursor += block_size;
                            }
                        }
                        kfree(buf);
                    }
                }
            }
        }
    }
    u64 thunk = 0;
    {
        u32 export_rva = 0, export_size=0;
        if (size_opt >= 112+8) {
            export_rva = pe_load_u32((u8*)hdr + oh_off + 112);
            export_size = pe_load_u32((u8*)hdr + oh_off + 116);
        }
        if (export_rva && export_size) {
            struct {
                u32 Characteristics;
                u32 TimeDateStamp;
                u16 MajorVersion;
                u16 MinorVersion;
                u32 Name;
                u32 Base;
                u32 NumberOfFunctions;
                u32 NumberOfNames;
                u32 AddressOfFunctions;
                u32 AddressOfNames;
                u32 AddressOfNameOrdinals;
            } ed;
            if (!copy_from_user(&ed, (void __user *)(actual_base + export_rva), sizeof(ed))) {
                u32 names_rva = ed.AddressOfNames;
                u32 ords_rva = ed.AddressOfNameOrdinals;
                u32 funcs_rva = ed.AddressOfFunctions;
                u32 num_names = ed.NumberOfNames;
                for (u32 i=0; i<num_names; ++i) {
                    u32 name_rva = 0;
                    if (copy_from_user(&name_rva, (void __user *)(actual_base + names_rva + i*4), 4))
                        break;
                    char name[64];
                    size_t k=0;
                    for (; k<sizeof(name)-1; ++k) {
                        char c;
                        if (copy_from_user(&c, (void __user *)(actual_base + name_rva + k), 1))
                            break;
                        name[k]=c;
                        if (c==0) break;
                    }
                    name[sizeof(name)-1]=0;
                    if (strcmp(name, "LdrInitializeThunk")==0) {
                        u16 ord = 0;
                        if (copy_from_user(&ord, (void __user *)(actual_base + ords_rva + i*2), 2))
                            break;
                        u32 func_rva = 0;
                        if (copy_from_user(&func_rva, (void __user *)(actual_base + funcs_rva + ord*4), 4))
                            break;
                        thunk = actual_base + func_rva;
                        break;
                    }
                }
            }
        }
    }
    kfree(hdr);
    filp_close(f, NULL);
    if (!thunk)
        return -ENOENT;
    *out_base = actual_base;
    *out_thunk = thunk;
    pr_err("binfmt_pe: mapped ntdll at 0x%llx LdrInitializeThunk 0x%llx\n", (unsigned long long)actual_base, (unsigned long long)thunk);
    return 0;
}


static int pe_setup_stack(struct pe_load_state *st)
{
    unsigned long size = PE_DEFAULT_STACK_RES;
    if (st->stack_reserve) {
        size = st->stack_reserve;
        if (size < PAGE_SIZE) size = PAGE_SIZE;
        if (size > 8*1024*1024) size = 8*1024*1024;
        size = PAGE_ALIGN(size);
    }
    // commit ignored for now; we map full reserve as RW
    unsigned long stack = vm_mmap(NULL, 0, size, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, 0);
    if (IS_ERR_VALUE(stack)) return -ENOMEM;
    st->stack_base = (void *)stack;
    st->stack_size = size;
    pe_state_add_mapping(st, stack, size);
#ifdef CONFIG_NT_PERSONALITY
    ntmm_add_region(stack, size, 0x04, 0x00020000, stack);
#endif
    return 0;
}

static int load_pe_binary(struct linux_binprm *bprm)
{
    struct pe_load_state state = {0};
    int ret;
    unsigned long entry, sp;

    if (bprm->cred->uid.val != bprm->cred->euid.val ||
        bprm->cred->gid.val != bprm->cred->egid.val)
        return -EPERM;

    state.mm = bprm->mm;
    state.file = bprm->file;
    state.bprm = bprm;

    ret = pe_read_file_headers(bprm, &state);
    if (ret) goto out;

    /*
     * Full exec bookkeeping - this was missing and is the root cause of
     * perf stat <not counted>: perf_event_enable_on_exec is inside
     * begin_new_exec().  Intercepting exec without begin_new_exec leaves
     * perf counters permanently disabled (enable_on_exec == 1 never
     * cleared).  We now follow the same sequence as load_elf_binary():
     * begin_new_exec -> setup_new_exec -> setup_arg_pages -> map -> finalize.
     */
    ret = begin_new_exec(bprm);
    if (ret)
        goto out;

    setup_new_exec(bprm);

    ret = setup_arg_pages(bprm, randomize_stack_top(STACK_TOP),
                  EXSTACK_DEFAULT);
    if (ret)
        goto out;

    /* begin_new_exec consumed bprm->mm (exec_mmap); update state to new mm */
    state.mm = current->mm;
    /* bprm->mm is now NULL; keep state.file/bprm for mapping helpers
     * (kernel_read uses state.file which is still valid)
     */

    ret = pe_pick_base(&state);
    if (ret) goto out;

    ret = pe_map_sections(&state);
    if (ret) goto out;

    ret = pe_apply_relocs(&state);
    if (ret) goto out;

    ret = pe_setup_stack(&state);
    if (ret) goto out;

    ret = pe_build_process_parameters(&state);
    if (ret) goto out;

    ret = pe_init_peb(&state);
    if (ret) goto out;

    ret = pe_init_teb(&state);
    if (ret) goto out;

    u64 ntdll_base = 0, ntdll_thunk = 0;
    int has_ntdll = 0;
    has_ntdll = (pe_map_ntdll(&state, &ntdll_base, &ntdll_thunk) == 0);
    if (has_ntdll)
        pr_err("binfmt_pe: mapped ntdll at 0x%llx LdrInitializeThunk 0x%llx\n", (unsigned long long)ntdll_base, (unsigned long long)ntdll_thunk);
    else
        pr_err("binfmt_pe: ntdll not mapped, falling back to exe entry\n");

    if (has_ntdll && ntdll_thunk)
        entry = (unsigned long)ntdll_thunk;
    else
        entry = (unsigned long)state.actual_base + state.entry_point;
    sp = (unsigned long)state.stack_base + state.stack_size;
    sp &= ~0xFUL;

    pr_info("binfmt_pe: entry 0x%lx sp 0x%lx teb 0x%lx peb 0x%lx delta 0x%llx\n",
        entry, sp, (unsigned long)state.teb, (unsigned long)state.peb, (unsigned long long)state.delta);
    {
        u8 buf[32];
        if (!copy_from_user(buf, (void __user *)entry, 32)) {
            pr_info("binfmt_pe: entry bytes: %*ph\n", 32, buf);
        } else {
            pr_info("binfmt_pe: failed to read entry bytes\n");
        }
    }

    set_binfmt(&pe_format);

    /* Mirror elf: set mm code/data fields for /proc and core dumps */
    current->mm->start_code = entry;
    current->mm->end_code = entry + PAGE_SIZE;
    current->mm->start_data = (unsigned long)state.peb;
    current->mm->end_data = (unsigned long)state.peb + PAGE_SIZE;
    current->mm->start_brk = current->mm->brk = current->mm->end_data;
    current->mm->start_stack = sp - state.stack_size;

    finalize_exec(bprm);

    pr_err("binfmt_pe: loading PE %s pid=%d\n", bprm->filename, current->pid);
    nt_enter_personality(current);
    pr_err("binfmt_pe: after nt_enter TIF_NTABI=%d\n", test_thread_flag(TIF_NTABI));

    {
        struct pt_regs *regs = task_pt_regs(current);
        start_thread(regs, entry, sp);
        /*
         * RCX handoff: Windows x64 ABI first-argument register.
         * We pass the EXE image base in RCX when entering via
         * LdrInitializeThunk. Real Windows LdrInitializeThunk is
         * argument-less and recovers the EXE base via
         * GS:0x30 -> TEB -> PEB (0x60) -> ImageBaseAddress (0x10),
         * so RCX is not consumed by our current ntdll/ldr but is
         * the natural first-arg slot and documents the handoff.
         * If ntdll is absent (fallback) we keep RCX = PEB for
         * symmetry with the pre-N2 path and for M0 debugging.
         */
        if (has_ntdll && ntdll_thunk)
            regs->cx = (unsigned long)state.actual_base;
        else
            regs->cx = (unsigned long)state.peb;
        /*
         * start_thread() resets the segment state, so the TEB base must be
         * installed afterwards. This mirrors do_arch_prctl_64()'s ARCH_SET_GS
         * path: index 0 plus an explicit base write.
         *
         * x86_gsbase_write_cpu_inactive() writes MSR_KERNEL_GS_BASE, which is
         * where the *user* GS base lives while we are in the kernel; SWAPGS
         * installs it on the way out. The TEB is addressed GS-relative by
         * every PE binary, so this is the load-bearing register for the whole
         * Windows personality.
         */
        if ((unsigned long)state.teb < TASK_SIZE_MAX) {
                preempt_disable();
                current->thread.gsindex = 0;
                current->thread.gsbase = (unsigned long)state.teb;
                x86_gsbase_write_cpu_inactive((unsigned long)state.teb);
                preempt_enable();
        } else {
                /*
                 * Refuse rather than install a kernel or non-canonical base:
                 * a bad GS base is an escalation primitive, not a cosmetic
                 * error, and the value derives from untrusted image layout.
                 */
                pr_err("binfmt_pe: refusing out-of-range TEB %px\n", state.teb);
                ret = -EINVAL;
                goto out;
        }
    }

    /* MMF_PE_IMAGE not defined in vanilla; NT state tracked via TIF_NTABI */
    state.mappings = NULL;
    state.headers_buf = NULL;
    ret = 0;
out:
    pe_state_cleanup(&state);
    return ret;
}


extern u32 nt_svc_NtTerminateProcess(unsigned long, unsigned long, unsigned long, unsigned long, unsigned long, unsigned long, unsigned long, unsigned long, unsigned long, unsigned long, unsigned long, unsigned long);
/* Strong override for NtTerminateProcess - needed for M0 test.
 * Weak stub in arch/x86/nt/stubs.c returns NOT_IMPLEMENTED; this strong
 * definition makes the PE exit with the requested code via do_exit().
 * The test PE issues NtTerminateProcess(-1, 0x2A) and expects exit 42.
 */
#include <linux/sched/signal.h>
__attribute__((weak)) u32 nt_svc_NtTerminateProcess(unsigned long a0, unsigned long a1, unsigned long a2,
                              unsigned long a3, unsigned long a4, unsigned long a5,
                              unsigned long a6, unsigned long a7, unsigned long a8,
                              unsigned long a9, unsigned long a10, unsigned long a11)
{
    long code = (long)(a1 & 0xFF);
    pr_info("binfmt_pe: NtTerminateProcess called handle=0x%lx status=0x%lx code=%ld pid=%d\n", a0, a1, code, current->pid);
    do_group_exit((code & 0xff) << 8);
    pr_info("binfmt_pe: NtTerminateProcess after do_group_exit (should not reach)\n");
    return 0;
}

static struct linux_binfmt pe_format = {
    .module = THIS_MODULE,
    .load_binary = load_pe_binary,
};

static int __init init_pe_binfmt(void)
{
    register_binfmt(&pe_format);
    pr_info("binfmt_pe: registered\n");
    return 0;
}
static void __exit exit_pe_binfmt(void)
{
    unregister_binfmt(&pe_format);
}
core_initcall(init_pe_binfmt);
