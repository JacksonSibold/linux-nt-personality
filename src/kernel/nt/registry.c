/*
 * registry.c - NT registry personality (GG4)
 * Implements keys as NT objects (ObCreateObject / ObInsertObject) with handle semantics,
 * backing persisted under guest filesystem, externally parseable.
 */

#include "registry.h"
#include "object.h"
#include "handle.h"
#include "kshim.h"
#include "../../ntdll/nt_types.h"
#include "../../ntdll/ntstatus.h"

#ifndef __KERNEL__
#include <string.h>
#include <wchar.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#else
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/stat.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/namei.h>
#include <linux/fs_struct.h>
extern struct filename *getname_kernel(const char *filename);
extern int do_mkdirat(int dfd, struct filename *name, umode_t mode);
extern int do_unlinkat(int dfd, struct filename *name);
extern int do_rmdir(int dfd, struct filename *name);
#endif

/* -------------------------------------------------------------------- */
/* Value and key structures                                              */
/* -------------------------------------------------------------------- */

struct reg_value {
    struct list_head link;
    UNICODE_STRING name; /* owned copy */
    ULONG type;
    ULONG data_len;
    uint8_t *data; /* malloced, may be 0 len */
};

struct subkey_entry {
    struct list_head link;
    struct nt_object_header *hdr; /* ref held */
    UNICODE_STRING name; /* dup for lookup */
};

struct nt_key {
    rwlock_t lock;
    struct list_head subkeys;
    struct list_head values;
    UNICODE_STRING full_path; /* absolute NT path e.g. \Registry\Machine\Software\Foo */
    UNICODE_STRING key_name; /* leaf component */
    UNICODE_STRING class_name; /* optional class */
    LARGE_INTEGER last_write_time;
    bool deleted;
};

static void registry_delete(struct nt_object_header *hdr)
{
    struct nt_key *k = (struct nt_key *)nt_object_body(hdr);
    struct reg_value *v, *tmpv;
    struct subkey_entry *e, *tmpe;
    /* free values */
    list_for_each_entry_safe(v, tmpv, &k->values, link) {
        list_del(&v->link);
        if (v->name.Buffer) kfree(v->name.Buffer);
        if (v->data) kfree(v->data);
        kfree(v);
    }
    list_for_each_entry_safe(e, tmpe, &k->subkeys, link) {
        list_del(&e->link);
        if (e->name.Buffer) kfree(e->name.Buffer);
        if (e->hdr) ObDereferenceObject(e->hdr);
        kfree(e);
    }
    if (k->full_path.Buffer) kfree(k->full_path.Buffer);
    if (k->key_name.Buffer) kfree(k->key_name.Buffer);
    if (k->class_name.Buffer) kfree(k->class_name.Buffer);
}

static NTSTATUS registry_parse(struct nt_object_header *parse_obj, void *context,
                               PUNICODE_STRING remaining_path, ULONG attributes,
                               struct nt_object_header **found)
{
    (void)context;
    struct nt_key *k = (struct nt_key *)nt_object_body(parse_obj);
    if (!remaining_path || remaining_path->Length==0) {
        ObReferenceObject(parse_obj);
        *found = parse_obj;
        return STATUS_SUCCESS;
    }
    /* remaining_path starts with component (maybe leading slash trimmed by caller?)
       Obj parser passes remaining including current component separated. We need to
       walk components.
    */
    size_t pos=0;
    size_t len = remaining_path->Length / sizeof(WCHAR);
    const WCHAR *buf = remaining_path->Buffer;
    /* skip leading slashes */
    while (pos < len && buf[pos]==(WCHAR)'\\') pos++;
    if (pos>=len) { ObReferenceObject(parse_obj); *found=parse_obj; return STATUS_SUCCESS; }
    size_t start=pos;
    while (pos < len && buf[pos]!=(WCHAR)'\\') pos++;
    size_t comp_len = pos - start;
    /* lookup subkey case-insensitive */
    struct subkey_entry *e = NULL;
    bool hit=false;
    read_lock(&k->lock);
    list_for_each_entry(e, &k->subkeys, link) {
        size_t elen = e->name.Length / sizeof(WCHAR);
        if (elen != comp_len) continue;
        bool eq=true;
        for (size_t i=0;i<elen;i++) {
            WCHAR a = e->name.Buffer[i];
            WCHAR b = buf[start+i];
            if (a>='a' && a<='z') a = a - ('a'-'A');
            if (b>='a' && b<='z') b = b - ('a'-'A');
            if (a!=b) { eq=false; break; }
        }
        if (eq) { hit=true; ObReferenceObject(e->hdr);
#ifndef __KERNEL__
#endif
            break;
        }
    }
    read_unlock(&k->lock);
#ifndef __KERNEL__
#endif
    if (!hit) return STATUS_OBJECT_NAME_NOT_FOUND;
    /* e->hdr is now referenced; need to handle remainder recursively */
    if (pos < len) {
        /* more components: delegate to child parse */
        UNICODE_STRING rem;
        rem.Buffer = (PWSTR)(buf+pos);
        rem.Length = (USHORT)((len-pos)*sizeof(WCHAR));
        rem.MaximumLength = rem.Length;
        struct nt_object_header *next = NULL;
        NTSTATUS st = registry_parse(e->hdr, NULL, &rem, attributes, &next);
        ObDereferenceObject(e->hdr);
        if (!NT_SUCCESS(st)) return st;
        *found = next;
        return STATUS_SUCCESS;
    }
    *found = e->hdr;
    return STATUS_SUCCESS;
}

struct nt_object_type nt_key_type = {
    .name = "Key",
    .ops = { .delete_obj = registry_delete, .parse = registry_parse, },
    .pool_tag = 0,
};

static NTSTATUS registry_find_key(PCUNICODE_STRING path, struct nt_object_header **out);

static bool g_reg_init = false;
static struct nt_object_header *g_registry_root = NULL; /* \Registry directory */

static uint64_t now_100ns(void) {
#ifndef __KERNEL__
    struct timespec ts; clock_gettime(CLOCK_REALTIME,&ts);
    uint64_t t = (uint64_t)ts.tv_sec * 10000000ULL + ts.tv_nsec/100 + 116444736000000000ULL;
    return t;
#else
    return ktime_get_real_ns()/100 + 116444736000000000ULL;
#endif
}

/* helpers for unicode */
static NTSTATUS dup_us(PCUNICODE_STRING src, PUNICODE_STRING dst) {
    if (!src || src->Length==0) { dst->Buffer=NULL; dst->Length=0; dst->MaximumLength=0; return STATUS_SUCCESS; }
    if (src->Length & 1) return STATUS_OBJECT_NAME_INVALID;
    USHORT max = src->Length + sizeof(WCHAR);
    PWSTR b = (PWSTR)kmalloc(max, GFP_KERNEL);
    if (!b) return STATUS_NO_MEMORY;
    memcpy(b, src->Buffer, src->Length);
    b[src->Length/sizeof(WCHAR)]=0;
    dst->Buffer=b; dst->Length=src->Length; dst->MaximumLength=max;
    return STATUS_SUCCESS;
}
static void free_us(PUNICODE_STRING us) {
    if (us->Buffer) kfree(us->Buffer);
    us->Buffer=NULL; us->Length=0; us->MaximumLength=0;
}
static bool us_equal_ci(PCUNICODE_STRING a, PCUNICODE_STRING b) {
    if (a->Length != b->Length) return false;
    size_t n = a->Length/sizeof(WCHAR);
    for (size_t i=0;i<n;i++) {
        WCHAR ca = a->Buffer[i], cb=b->Buffer[i];
        if (ca>='a' && ca<='z') ca-=32;
        if (cb>='a' && cb<='z') cb-=32;
        if (ca!=cb) return false;
    }
    return true;
}


/* path helpers */
static void hive_base_path(char *out, size_t outlen) {
#ifndef __KERNEL__
    const char *root = g_kshim_root;
    if (!root || !root[0]) root="/tmp";
    snprintf(out, outlen, "%s/nt_registry", root);
#else
    snprintf(out, outlen, "/tmp/nt_registry");
#endif
}
static void ensure_hive_dir(void) {
    char base[PATH_MAX]; hive_base_path(base,sizeof(base));
#ifndef __KERNEL__
    mkdir(base,0755);
#else
    {
        struct file *f = filp_open(base, O_RDONLY|O_DIRECTORY, 0);
        if (IS_ERR(f)) {
            struct filename *fn = getname_kernel(base);
            if (!IS_ERR(fn)) {
                do_mkdirat(AT_FDCWD, fn, 0755);
                putname(fn);
            }
        } else {
            filp_close(f, NULL);
        }
    }
#endif
}
static NTSTATUS nt_path_to_fs(PCUNICODE_STRING nt, char *out, size_t outlen) {
    char base[PATH_MAX]; hive_base_path(base,sizeof(base));
    if (!nt || nt->Length==0) { snprintf(out,outlen,"%s",base); return STATUS_SUCCESS; }
#ifdef __KERNEL__
    char *tmp = kmalloc(PATH_MAX, GFP_KERNEL);
    if (!tmp) return STATUS_NO_MEMORY;
#else
    char tmp[PATH_MAX];
#endif
    size_t chars = nt->Length/sizeof(WCHAR);
    if (chars >= PATH_MAX) {
#ifdef __KERNEL__
        kfree(tmp);
#endif
        return STATUS_NAME_TOO_LONG;
    }
    for (size_t i=0;i<chars;i++) {
        WCHAR wc = nt->Buffer[i];
        if (wc=='\\') tmp[i]='/';
        else if (wc<0x80) tmp[i]=(char)wc;
        else tmp[i]='?';
    }
    tmp[chars]=0;
    const char *p = tmp;
    if (strncmp(p,"/Registry/",9)==0) p+=9;
    else if (strncmp(p,"/Registry",9)==0) p+=9;
    else if (p[0]=='/') p++;
    if (p[0]=='/') p++;
    NTSTATUS ret = STATUS_SUCCESS;
    if (!p[0]) snprintf(out,outlen,"%s",base);
    else snprintf(out,outlen,"%s/%s",base,p);
#ifdef __KERNEL__
    kfree(tmp);
#endif
    return ret;
}
static NTSTATUS persist_write_value(struct nt_key *k, struct reg_value *v) {
#ifdef __KERNEL__
    char *dir = kmalloc(PATH_MAX, GFP_KERNEL);
    if (!dir) return STATUS_NO_MEMORY;
    NTSTATUS _st = nt_path_to_fs(&k->full_path, dir, PATH_MAX);
    if (!NT_SUCCESS(_st)) { kfree(dir); return _st; }
#else
    char dir[PATH_MAX]; nt_path_to_fs(&k->full_path, dir, sizeof(dir));
#endif
#ifndef __KERNEL__
    ensure_hive_dir();
    char tmp[PATH_MAX]; snprintf(tmp,sizeof(tmp),"%s",dir);
    for (char *c=tmp+1; *c; c++) if (*c=='/') { *c=0; mkdir(tmp,0755); *c='/'; }
    mkdir(tmp,0755);
    char vname[256];
    if (v->name.Length==0) snprintf(vname,sizeof(vname),".val_(default)");
    else {
        size_t n = v->name.Length/sizeof(WCHAR);
        if (n>=200) n=200;
        for (size_t i=0;i<n;i++) {
            WCHAR wc=v->name.Buffer[i];
            if (wc<0x80 && wc!='/' && wc!='\\') vname[i]=(char)wc; else vname[i]='?';
        }
        vname[n]=0;
        for (char *c=vname;*c;c++) if (*c=='/') *c='_';
        char f[PATH_MAX]; snprintf(f,sizeof(f),".val_%s",vname);
        snprintf(vname,sizeof(vname),"%s",f);
    }
    char fpath[PATH_MAX*2]; snprintf(fpath,sizeof(fpath),"%s/%s",dir,vname);
    int fd = open(fpath, O_WRONLY|O_CREAT|O_TRUNC, 0644);
    if (fd<0) return STATUS_ACCESS_DENIED;
    uint32_t hdr[2]; hdr[0]=v->type; hdr[1]=v->data_len;
    if (write(fd,hdr,8)!=8) { close(fd); return STATUS_ACCESS_DENIED; }
    if (v->data_len && write(fd,v->data,v->data_len)!=(ssize_t)v->data_len) { close(fd); return STATUS_ACCESS_DENIED; }
    fsync(fd); close(fd);
    (void)dir;
#else
    {
        char *tmp = kmalloc(PATH_MAX, GFP_KERNEL);
        if (!tmp) { kfree(dir); return STATUS_NO_MEMORY; }
        snprintf(tmp,PATH_MAX,"%s",dir);
        for (char *c=tmp+1; *c; c++) {
            if (*c=='/') {
                *c=0;
                struct filename *fn = getname_kernel(tmp);
                if (!IS_ERR(fn)) { do_mkdirat(AT_FDCWD, fn, 0755); putname(fn); }
                *c='/';
            }
        }
        struct filename *fn = getname_kernel(tmp);
        if (!IS_ERR(fn)) { do_mkdirat(AT_FDCWD, fn, 0755); putname(fn); }
        kfree(tmp);
    }
    {
        char vname[256];
        if (v->name.Length==0) snprintf(vname,sizeof(vname),".val_(default)");
        else {
            size_t n = v->name.Length/sizeof(WCHAR);
            if (n>=200) n=200;
            for (size_t i=0;i<n;i++) { WCHAR wc=v->name.Buffer[i]; vname[i]=(wc<0x80 && wc!='/' && wc!='\\')?(char)wc:'?'; }
            vname[n]=0;
            for (char *c=vname;*c;c++) if (*c=='/') *c='_';
            char f[PATH_MAX]; snprintf(f,sizeof(f),".val_%s",vname);
            snprintf(vname,sizeof(vname),"%s",f);
        }
        char fpath[PATH_MAX*2]; snprintf(fpath,sizeof(fpath),"%s/%s",dir,vname);
        struct file *filp = filp_open(fpath, O_WRONLY|O_CREAT|O_TRUNC, 0644);
        if (!IS_ERR(filp)) {
            loff_t pos=0;
            uint32_t hdr[2]; hdr[0]=v->type; hdr[1]=v->data_len;
            kernel_write(filp, hdr, 8, &pos);
            if (v->data_len && v->data) kernel_write(filp, v->data, v->data_len, &pos);
            vfs_fsync(filp, 0);
            filp_close(filp, NULL);
        }
    }
#endif
#ifdef __KERNEL__
    kfree(dir);
#endif
    return STATUS_SUCCESS;
}
static NTSTATUS persist_delete_value(struct nt_key *k, PCUNICODE_STRING name) {
#ifdef __KERNEL__
    char *dir = kmalloc(PATH_MAX, GFP_KERNEL);
    if (!dir) return STATUS_NO_MEMORY;
    NTSTATUS _st2 = nt_path_to_fs(&k->full_path, dir, PATH_MAX);
    if (!NT_SUCCESS(_st2)) { kfree(dir); return _st2; }
#else
    char dir[PATH_MAX]; nt_path_to_fs(&k->full_path, dir, sizeof(dir));
#endif
#ifndef __KERNEL__
    char vname[256];
    if (!name || name->Length==0) snprintf(vname,sizeof(vname),".val_(default)");
    else {
        size_t n = name->Length/sizeof(WCHAR);
        if (n>=200) n=200;
        for (size_t i=0;i<n;i++) { WCHAR wc=name->Buffer[i]; vname[i]=(wc<0x80)?(char)wc:'?'; } vname[n]=0;
        for (char *c=vname;*c;c++) if (*c=='/') *c='_';
        char f[PATH_MAX]; snprintf(f,sizeof(f),".val_%s",vname); snprintf(vname,sizeof(vname),"%s",f);
    }
    char fpath[PATH_MAX*2]; snprintf(fpath,sizeof(fpath),"%s/%s",dir,vname);
    unlink(fpath);
#else
    {
        char vname[256];
        if (!name || name->Length==0) snprintf(vname,sizeof(vname),".val_(default)");
        else {
            size_t n = name->Length/sizeof(WCHAR);
            if (n>=200) n=200;
            for (size_t i=0;i<n;i++) { WCHAR wc=name->Buffer[i]; vname[i]=(wc<0x80)?(char)wc:'?'; } vname[n]=0;
            for (char *c=vname;*c;c++) if (*c=='/') *c='_';
            char f[PATH_MAX]; snprintf(f,sizeof(f),".val_%s",vname); snprintf(vname,sizeof(vname),"%s",f);
        }
        char fpath[PATH_MAX*2]; snprintf(fpath,sizeof(fpath),"%s/%s",dir,vname);
        struct filename *fn = getname_kernel(fpath);
        if (!IS_ERR(fn)) { do_unlinkat(AT_FDCWD, fn); putname(fn); }
    }
#endif
#ifdef __KERNEL__
    kfree(dir);
#endif
    return STATUS_SUCCESS;
}
static NTSTATUS persist_create_key(struct nt_key *k) {
#ifdef __KERNEL__
    char *dir = kmalloc(PATH_MAX, GFP_KERNEL);
    if (!dir) return STATUS_NO_MEMORY;
    NTSTATUS _st3 = nt_path_to_fs(&k->full_path, dir, PATH_MAX);
    if (!NT_SUCCESS(_st3)) { kfree(dir); return _st3; }
#else
    char dir[PATH_MAX]; nt_path_to_fs(&k->full_path, dir, sizeof(dir));
#endif
#ifndef __KERNEL__
    ensure_hive_dir();
    char tmp[PATH_MAX]; snprintf(tmp,sizeof(tmp),"%s",dir);
    for (char *c=tmp+1; *c; c++) if (*c=='/') { *c=0; mkdir(tmp,0755); *c='/'; }
    mkdir(tmp,0755);
    char base[PATH_MAX]; hive_base_path(base,sizeof(base));
    int dfd = open(base, O_DIRECTORY);
    if (dfd>=0) { fsync(dfd); close(dfd); }
#else
    {
        char *tmp = kmalloc(PATH_MAX, GFP_KERNEL);
        if (!tmp) { kfree(dir); return STATUS_NO_MEMORY; }
        snprintf(tmp,PATH_MAX,"%s",dir);
        for (char *c=tmp+1; *c; c++) {
            if (*c=='/') {
                *c=0;
                struct filename *fn = getname_kernel(tmp);
                if (!IS_ERR(fn)) { do_mkdirat(AT_FDCWD, fn, 0755); putname(fn); }
                *c='/';
            }
        }
        struct filename *fn = getname_kernel(tmp);
        if (!IS_ERR(fn)) { do_mkdirat(AT_FDCWD, fn, 0755); putname(fn); }
        kfree(tmp);
    }
#endif
#ifdef __KERNEL__
    kfree(dir);
#endif
    return STATUS_SUCCESS;
}
static NTSTATUS persist_delete_key(struct nt_key *k) {
#ifdef __KERNEL__
    char *dir = kmalloc(PATH_MAX, GFP_KERNEL);
    if (!dir) return STATUS_NO_MEMORY;
    NTSTATUS _st4 = nt_path_to_fs(&k->full_path, dir, PATH_MAX);
    if (!NT_SUCCESS(_st4)) { kfree(dir); return _st4; }
#else
    char dir[PATH_MAX]; nt_path_to_fs(&k->full_path, dir, sizeof(dir));
#endif
#ifndef __KERNEL__
    rmdir(dir);
#else
    {
        struct filename *fn = getname_kernel(dir);
        if (!IS_ERR(fn)) { do_rmdir(AT_FDCWD, fn); putname(fn); }
    }
#endif
#ifdef __KERNEL__
    kfree(dir);
#endif
    return STATUS_SUCCESS;
}

NTSTATUS ntreg_init(void) {
    if (g_reg_init) return STATUS_SUCCESS;
    NTSTATUS st = ObInitObjectManager();
    if (!NT_SUCCESS(st) && st!=STATUS_OBJECT_NAME_COLLISION) return st;
    ObRegisterObjectType(&nt_key_type);
    /* Ensure \Registry directory exists */
    struct nt_object_header *root = ObGetRootDirectory();
    (void)root;
    /* Create \Registry if not present */
    {
        struct nt_directory *rd = (struct nt_directory *)nt_object_body(root);
        UNICODE_STRING name; WCHAR w[] = {(WCHAR)'R',(WCHAR)'e',(WCHAR)'g',(WCHAR)'i',(WCHAR)'s',(WCHAR)'t',(WCHAR)'r',(WCHAR)'y',0};
        name.Buffer=w; name.Length=8*sizeof(WCHAR); name.MaximumLength=name.Length;
        struct nt_object_header *found = ObpLookupDirectoryEntry(rd, &name);
        if (found) {
            g_registry_root = found;
            ObDereferenceObject(found);
        } else {
            struct nt_object_header *hdr; struct nt_directory *dir;
            OBJECT_ATTRIBUTES oa={0}; oa.Length=sizeof(oa);
            NTSTATUS s = ObCreateObject(&nt_directory_type, &oa, sizeof(*dir), &hdr, (void**)&dir);
            if (NT_SUCCESS(s)) {
                INIT_LIST_HEAD(&dir->entries);
                rwlock_init(&dir->lock);
                UNICODE_STRING us; us.Buffer=w; us.Length=8*sizeof(WCHAR); us.MaximumLength=us.Length;
                /* directly insert using internal helper? Use directory entry insertion via temporary attrs */
                /* Build path \Registry */
                WCHAR pathw[] = {(WCHAR)'\\',(WCHAR)'R',(WCHAR)'e',(WCHAR)'g',(WCHAR)'i',(WCHAR)'s',(WCHAR)'t',(WCHAR)'r',(WCHAR)'y',0};
                UNICODE_STRING path; path.Buffer=pathw; path.Length=9*sizeof(WCHAR); path.MaximumLength=path.Length;
                OBJECT_ATTRIBUTES oa2={0}; oa2.Length=sizeof(oa2); oa2.ObjectName=&path;
                s = ObInsertObject(hdr, &oa2, 0, kshim_current_process(), NULL);
                if (!NT_SUCCESS(s)) ObDereferenceObject(hdr);
                else {
                    g_registry_root = hdr;
                    ObDereferenceObject(hdr); /* keep directory ref via root */
                    hdr = NULL;
                    /* re-lookup to get stable pointer */
                    struct nt_object_header *tmp = ObpLookupDirectoryEntry(rd, &name);
                    if (tmp) { g_registry_root = tmp; ObDereferenceObject(tmp); }
                }
            }
        }
    }
    ensure_hive_dir();
    /* Create well-known hive roots: Machine and User */
    /* Helper to create key object under \Registry */
    struct nt_object_header *reg_dir = NULL;
    {
        WCHAR rp[] = {(WCHAR)'\\',(WCHAR)'R',(WCHAR)'e',(WCHAR)'g',(WCHAR)'i',(WCHAR)'s',(WCHAR)'t',(WCHAR)'r',(WCHAR)'y',0};
        UNICODE_STRING us; us.Buffer=rp; us.Length=9*sizeof(WCHAR); us.MaximumLength=us.Length;
        ObReferenceObjectByName(&us, &nt_directory_type, KernelMode,0,&reg_dir);
    }
    if (reg_dir) {
        const WCHAR *hives[] = {(const WCHAR*)u"Machine", (const WCHAR*)u"User"};
        for (int hi=0;hi<2;hi++) {
            const WCHAR *hname = hives[hi];
            size_t hl = 0; while (hname[hl]) hl++;
            UNICODE_STRING us; us.Buffer=(PWSTR)hname; us.Length=(USHORT)(hl*sizeof(WCHAR)); us.MaximumLength=us.Length;
            struct nt_directory *d = (struct nt_directory*)nt_object_body(reg_dir);
            struct nt_object_header *existing = ObpLookupDirectoryEntry(d,&us);
            if (existing) { ObDereferenceObject(existing); continue; }
            struct nt_object_header *hdr; struct nt_key *k;
            OBJECT_ATTRIBUTES oa={0}; oa.Length=sizeof(oa);
            NTSTATUS s = ObCreateObject(&nt_key_type, &oa, sizeof(*k), &hdr, (void**)&k);
            if (!NT_SUCCESS(s)) continue;
            INIT_LIST_HEAD(&k->subkeys); INIT_LIST_HEAD(&k->values);
            rwlock_init(&k->lock);
            k->deleted=false;
            /* full path \Registry\<hive> */
            WCHAR full[64]; full[0]='\\'; size_t p=1;
            const WCHAR *r = u"Registry"; size_t rl=8;
            for (size_t i=0;i<rl;i++) full[p++]=r[i];
            full[p++]='\\';
            for (size_t i=0;i<hl;i++) full[p++]=hname[i];
            UNICODE_STRING fps; fps.Buffer=full; fps.Length=(USHORT)(p*sizeof(WCHAR)); fps.MaximumLength=fps.Length+2;
            dup_us(&fps,&k->full_path);
            USHORT leafLen = (USHORT)(hl*sizeof(WCHAR));
            WCHAR *leaf = (WCHAR*)kmalloc(leafLen+2,GFP_KERNEL); if (leaf){ memcpy(leaf,hname,leafLen); leaf[hl]=0; k->key_name.Buffer=leaf; k->key_name.Length=leafLen; k->key_name.MaximumLength=leafLen+2; }
            k->class_name.Buffer=NULL; k->class_name.Length=0;
            k->last_write_time.QuadPart = (LONGLONG)now_100ns();
            /* Build OA for insert: \Registry\<hive> */
            WCHAR path[64]; path[0]='\\'; size_t pp=1;
            for (size_t i=0;i<rl;i++) path[pp++]=r[i];
            path[pp++]='\\';
            for (size_t i=0;i<hl;i++) path[pp++]=hname[i];
            UNICODE_STRING pathUs; pathUs.Buffer=path; pathUs.Length=(USHORT)(pp*sizeof(WCHAR)); pathUs.MaximumLength=pathUs.Length;
            OBJECT_ATTRIBUTES oa2={0}; oa2.Length=sizeof(oa2); oa2.ObjectName=&pathUs;
            s = ObInsertObject(hdr, &oa2, 0, kshim_current_process(), NULL);
            if (!NT_SUCCESS(s)) ObDereferenceObject(hdr);
            else { ObDereferenceObject(hdr); persist_create_key(k); }
        }
        ObDereferenceObject(reg_dir);
    }
    g_reg_init=true;
    /* Pre-create Software under Machine and User to avoid auto-create races in tests */
    {
        struct nt_eprocess *saved = kshim_current_process();
        struct nt_eprocess *tmpProc = NULL;
        if (!saved) {
            ObCreateProcessObject(&tmpProc, 1, "init");
            kshim_set_current_process(tmpProc);
            struct nt_ethread *tmpThr=NULL; ObCreateThreadObject(&tmpThr, tmpProc, 1); kshim_set_current_thread(tmpThr);
        }
        const char *soft = "Software";
        for (int hi=0;hi<2;hi++) {
            const WCHAR *hname = hi==0 ? (const WCHAR*)u"Machine" : (const WCHAR*)u"User";
            WCHAR path[64]; size_t p=0; const WCHAR *pre = u"\\Registry\\"; size_t pr=10;
            for(size_t i=0;i<pr;i++) path[p++]=pre[i];
            size_t hl=0; while(hname[hl]) hl++;
            for(size_t i=0;i<hl;i++) path[p++]=hname[i];
            path[p++]='\\';
            for(size_t i=0;soft[i];i++) path[p++]=(WCHAR)soft[i];
            UNICODE_STRING us; us.Buffer=path; us.Length=(USHORT)(p*sizeof(WCHAR)); us.MaximumLength=us.Length;
            struct nt_object_header *tmp=NULL;
            NTSTATUS s = registry_find_key(&us, &tmp);
            if (NT_SUCCESS(s)) { ObDereferenceObject(tmp); continue; }
            OBJECT_ATTRIBUTES oa={0}; oa.Length=sizeof(oa); oa.ObjectName=&us; oa.Attributes=OBJ_CASE_INSENSITIVE;
            HANDLE h=NULL; ULONG disp;
            NTSTATUS cr = NtCreateKey(&h, 0, &oa, 0, NULL, 0, &disp);
            if (NT_SUCCESS(cr)) { struct nt_eprocess *pr=kshim_current_process(); if(pr) ObCloseHandle(pr,h); }
        }
        if (tmpProc) {
            struct nt_ethread *thr=kshim_current_thread();
            if (thr) { kshim_set_current_thread(NULL); ObDestroyThreadObject(thr); }
            kshim_set_current_process(saved);
            ObDestroyProcessObject(tmpProc);
        }
    }
    return STATUS_SUCCESS;
}
void ntreg_cleanup(void) { g_reg_init=false; }

/* resolve full NT path from OA + RootDirectory */
static NTSTATUS build_full_path(POBJECT_ATTRIBUTES oa, PUNICODE_STRING out, WCHAR *buf, size_t bufchars) {
    // out will point into buf
    if (!oa || !oa->ObjectName || oa->ObjectName->Length==0) return STATUS_OBJECT_NAME_INVALID;
    UNICODE_STRING *us = oa->ObjectName;
    if (us->Length & 1) return STATUS_OBJECT_NAME_INVALID;
    if (oa->RootDirectory && oa->RootDirectory!=NULL) {
        struct nt_eprocess *proc = kshim_current_process();
        struct nt_object_header *rhdr=NULL;
        NTSTATUS st = ObReferenceObjectByHandle(proc, oa->RootDirectory, &nt_key_type, UserMode,0,&rhdr,NULL);
        if (!NT_SUCCESS(st)) return st;
        struct nt_key *rk = (struct nt_key*)nt_object_body(rhdr);
        size_t rlen = rk->full_path.Length/sizeof(WCHAR);
        size_t nlen = us->Length/sizeof(WCHAR);
        if (rlen + 1 + nlen + 1 > bufchars) { ObDereferenceObject(rhdr); return STATUS_NAME_TOO_LONG; }
        size_t p=0;
        for (size_t i=0;i<rlen;i++) buf[p++]=rk->full_path.Buffer[i];
        if (buf[p-1]!='\\') buf[p++]='\\';
        /* skip leading slash in us */
        size_t off=0; if (nlen>0 && us->Buffer[0]=='\\') off=1;
        for (size_t i=off;i<nlen;i++) buf[p++]=us->Buffer[i];
        out->Buffer=buf; out->Length=(USHORT)(p*sizeof(WCHAR)); out->MaximumLength=out->Length+2;
        buf[p]=0;
        ObDereferenceObject(rhdr);
        return STATUS_SUCCESS;
    } else {
        if (us->Buffer[0]!='\\') return STATUS_OBJECT_PATH_SYNTAX_BAD;
        size_t n = us->Length/sizeof(WCHAR);
        if (n+1 > bufchars) return STATUS_NAME_TOO_LONG;
        memcpy(buf, us->Buffer, us->Length);
        buf[n]=0;
        out->Buffer=buf; out->Length=us->Length; out->MaximumLength=us->Length+2;
        return STATUS_SUCCESS;
    }
}

static bool is_machine_path(PCUNICODE_STRING p) {
    const WCHAR *prefix = u"\\Registry\\Machine";
    size_t pl = 17; /* chars */
    size_t len = p->Length/sizeof(WCHAR);
    if (len < pl) return false;
    for (size_t i=0;i<pl;i++) {
        WCHAR a=p->Buffer[i], b=prefix[i];
        if (a>='a'&&a<='z') a-=32;
        if (b>='a'&&b<='z') b-=32;
        if (a!=b) return false;
    }
    return true;
}
static ACCESS_MASK strip_wow64(ACCESS_MASK a){ return a & ~KEY_WOW64_RESERVED; }

static NTSTATUS registry_find_key(PCUNICODE_STRING path, struct nt_object_header **out) {
    if (!path || path->Length==0) return STATUS_OBJECT_NAME_INVALID;
    // path is absolute \Registry\...
    // find \Registry directory
    WCHAR reg[] = { (WCHAR)'\\',(WCHAR)'R',(WCHAR)'e',(WCHAR)'g',(WCHAR)'i',(WCHAR)'s',(WCHAR)'t',(WCHAR)'r',(WCHAR)'y',0 };
    UNICODE_STRING regUs; regUs.Buffer=reg; regUs.Length=9*sizeof(WCHAR); regUs.MaximumLength=regUs.Length;
    struct nt_object_header *cur=NULL;
    NTSTATUS st = ObReferenceObjectByName(&regUs, &nt_directory_type, KernelMode,0,&cur);
    if (!NT_SUCCESS(st)) return st;
    // iterate components after \Registry
    size_t len = path->Length/sizeof(WCHAR); const WCHAR *buf=path->Buffer;
    // parse path components
    size_t p=0;
    // skip leading slashes and first component Registry
    p=0; while(p<len && buf[p]=='\\') p++;
    // skip Registry
    while(p<len && buf[p]!='\\') p++;
    while(p<len && buf[p]=='\\') p++;
    struct nt_object_header *parent = cur; // Registry dir
    // if path is just \Registry, return it
    if (p>=len) { *out=cur; return STATUS_SUCCESS; }
    // now walk remaining components
    while (p < len) {
        size_t start=p;
        while(p<len && buf[p]!='\\') p++;
        size_t comp_len = p-start;
        if (comp_len==0) { while(p<len && buf[p]=='\\') p++; continue; }
        UNICODE_STRING compUs; compUs.Buffer=(PWSTR)(buf+start); compUs.Length=(USHORT)(comp_len*sizeof(WCHAR)); compUs.MaximumLength=compUs.Length;
        struct nt_object_header *next=NULL;
        if (parent->type == &nt_directory_type) {
            struct nt_directory *d=(struct nt_directory*)nt_object_body(parent);
            next = ObpLookupDirectoryEntry(d, &compUs);
            if (!next) { ObDereferenceObject(parent); return STATUS_OBJECT_NAME_NOT_FOUND; }
        } else if (parent->type == &nt_key_type) {
            struct nt_key *k=(struct nt_key*)nt_object_body(parent);
            bool found=false;
            struct subkey_entry *e;
            read_lock(&k->lock);
            list_for_each_entry(e, &k->subkeys, link){
                if (e->name.Length==compUs.Length && us_equal_ci(&e->name,&compUs)) { ObReferenceObject(e->hdr); next=e->hdr; found=true; break; }
            }
            read_unlock(&k->lock);
            if (!found) { ObDereferenceObject(parent); return STATUS_OBJECT_NAME_NOT_FOUND; }
        } else { ObDereferenceObject(parent); return STATUS_OBJECT_TYPE_MISMATCH; }
        ObDereferenceObject(parent);
        parent=next;
        while(p<len && buf[p]=='\\') p++;
        if (p>=len) { *out=parent; return STATUS_SUCCESS; }
    }
    *out=parent;
    return STATUS_SUCCESS;
}

/* internal find-or-create for NtCreateKey */
NTSTATUS NtCreateKey(PHANDLE KeyHandle, ACCESS_MASK DesiredAccess,
                     POBJECT_ATTRIBUTES ObjectAttributes, ULONG TitleIndex,
                     PUNICODE_STRING Class, ULONG CreateOptions, PULONG Disposition)
{
    (void)TitleIndex; (void)CreateOptions;
    if (!KeyHandle) return STATUS_INVALID_PARAMETER;
    ntreg_init();
    DesiredAccess = strip_wow64(DesiredAccess);
    /* HKLM write deny */
    WCHAR buf[512];
    UNICODE_STRING full;
    NTSTATUS st = build_full_path(ObjectAttributes, &full, buf, 512);
    if (!NT_SUCCESS(st)) return st;
    if (is_machine_path(&full) && (DesiredAccess & (KEY_SET_VALUE|KEY_CREATE_SUB_KEY|0x00020000UL)) ) {
        /* if has write bits, deny unless KEY_READ only? Check if write bits present */
        ACCESS_MASK writeMask = KEY_SET_VALUE|KEY_CREATE_SUB_KEY;
        if (DesiredAccess & writeMask) {
            /* allow read-only? if DesiredAccess is exactly KEY_READ or contains only read bits, allow */
            /* For probe: ro_lasterror 0 with KEY_READ, rw_lasterror 5 with KEY_WRITE.
               KEY_WRITE includes writeMask so deny. */
            return STATUS_ACCESS_DENIED;
        }
    }
    /* try open existing via direct registry walk */
    struct nt_object_header *existing=NULL;
    st = registry_find_key(&full, &existing);
#ifndef __KERNEL__
#endif
    if (NT_SUCCESS(st)) {
        /* exists */
        struct nt_eprocess *proc = kshim_current_process();
        if (!proc) { ObDereferenceObject(existing); return STATUS_INVALID_PARAMETER; }
        if (!proc->handle_table) { ObDereferenceObject(existing); return STATUS_INVALID_HANDLE; }
        ULONG hattr=0;
        if (ObjectAttributes && (ObjectAttributes->Attributes & OBJ_INHERIT)) hattr|=OBJ_INHERIT;
        HANDLE h;
        st = ObCreateHandle(proc, existing, DesiredAccess, hattr, &h);
        ObDereferenceObject(existing);
        if (!NT_SUCCESS(st)) return st;
        *KeyHandle = h;
        if (Disposition) *Disposition = REG_OPENED_EXISTING_KEY;
        return STATUS_SUCCESS;
    }
    /* need to create: walk parent path */
    /* find parent directory/key */
    size_t len = full.Length/sizeof(WCHAR);
    ssize_t slash=-1;
    for (ssize_t i=(ssize_t)len-1;i>=0;i--) if (full.Buffer[i]=='\\') { slash=i; break; }
    if (slash<0) return STATUS_OBJECT_PATH_SYNTAX_BAD;
    // parent path = full[0..slash-1]
    UNICODE_STRING parent; parent.Buffer=full.Buffer; parent.Length=(USHORT)(slash*sizeof(WCHAR)); parent.MaximumLength=parent.Length;
    WCHAR leafbuf[256];
    size_t leafChars = len - slash -1;
    if (leafChars==0) return STATUS_OBJECT_NAME_INVALID;
    if (leafChars* sizeof(WCHAR) >= sizeof(leafbuf)) return STATUS_NAME_TOO_LONG;
    for (size_t i=0;i<leafChars;i++) leafbuf[i]=full.Buffer[slash+1+i];
    leafbuf[leafChars]=0;
    UNICODE_STRING leaf; leaf.Buffer=leafbuf; leaf.Length=(USHORT)(leafChars*sizeof(WCHAR)); leaf.MaximumLength=leaf.Length+2;
    struct nt_object_header *parentHdr=NULL;
    st = registry_find_key(&parent, &parentHdr);
    if (!NT_SUCCESS(st)) {
        /* auto-create intermediate keys recursively */
        // parent not found: try to create parent path first
        // split parent into its parent + leaf
        size_t plen = parent.Length/sizeof(WCHAR);
        if (plen>1) {
            ssize_t pslash=-1;
            for (ssize_t i=(ssize_t)plen-1;i>=0;i--) if(parent.Buffer[i]=='\\'){ pslash=i; break; }
            if (pslash>0) {
                UNICODE_STRING gparent; gparent.Buffer=parent.Buffer; gparent.Length=(USHORT)(pslash*sizeof(WCHAR)); gparent.MaximumLength=gparent.Length;
                WCHAR pleafBuf[256];
                size_t pleafLen = plen - pslash -1;
                for (size_t i=0;i<pleafLen;i++) pleafBuf[i]=parent.Buffer[pslash+1+i];
                UNICODE_STRING pleaf; pleaf.Buffer=pleafBuf; pleaf.Length=(USHORT)(pleafLen*sizeof(WCHAR)); pleaf.MaximumLength=pleaf.Length;
                // create gparent recursively if needed
                struct nt_object_header *gpHdr=NULL;
                NTSTATUS gst = registry_find_key(&gparent, &gpHdr);
                if (!NT_SUCCESS(gst)) {
                    // try to create gparent via recursive NtCreateKey
                    OBJECT_ATTRIBUTES goa={0}; goa.Length=sizeof(goa); goa.ObjectName=&gparent; goa.Attributes=OBJ_CASE_INSENSITIVE;
                    HANDLE gh=NULL; ULONG gd;
                    NTSTATUS cr = NtCreateKey(&gh, KEY_ALL_ACCESS, &goa, 0, NULL, 0, &gd);
                    if (NT_SUCCESS(cr)) {
                        struct nt_eprocess *pr=kshim_current_process();
                        if (pr) ObCloseHandle(pr, gh);
                        gst = registry_find_key(&gparent, &gpHdr);
                    }
                }
                if (NT_SUCCESS(gst)) {
                    // now create parent leaf under gpHdr
                    struct nt_object_header *nhdr; struct nt_key *nk;
                    OBJECT_ATTRIBUTES tmp2={0}; tmp2.Length=sizeof(tmp2);
                    NTSTATUS ns = ObCreateObject(&nt_key_type,&tmp2,sizeof(*nk),&nhdr,(void**)&nk);
                    if (NT_SUCCESS(ns)) {
                        INIT_LIST_HEAD(&nk->subkeys); INIT_LIST_HEAD(&nk->values);
                        rwlock_init(&nk->lock); nk->deleted=false; nk->last_write_time.QuadPart=(LONGLONG)now_100ns();
                        WCHAR pfull[512]; size_t pp= (size_t)gparent.Length/sizeof(WCHAR);
                        for(size_t i=0;i<pp;i++) pfull[i]=gparent.Buffer[i];
                        pfull[pp]='\\'; for(size_t i=0;i<pleafLen;i++) pfull[pp+1+i]=pleaf.Buffer[i];
                        UNICODE_STRING fu; fu.Buffer=pfull; fu.Length=(USHORT)((pp+1+pleafLen)*sizeof(WCHAR)); fu.MaximumLength=fu.Length;
                        dup_us(&fu,&nk->full_path); dup_us(&pleaf,&nk->key_name); nk->class_name.Buffer=NULL; nk->class_name.Length=0;
                        if (gpHdr->type==&nt_key_type) {
                            struct nt_key *gpk=(struct nt_key*)nt_object_body(gpHdr);
                            struct nt_eprocess *pr=kshim_current_process();
                            HANDLE nh; NTSTATUS is = ObCreateHandle(pr, nhdr, 0, 0, &nh);
                            if (NT_SUCCESS(is)) {
                                struct subkey_entry *en=(struct subkey_entry*)kmalloc(sizeof(*en),GFP_KERNEL);
                                if (en){ INIT_LIST_HEAD(&en->link); en->hdr=nhdr; ObReferenceObject(nhdr); dup_us(&pleaf,&en->name);
                                    write_lock(&gpk->lock); list_add_tail(&en->link,&gpk->subkeys); write_unlock(&gpk->lock);
                                }
                                ObDereferenceObject(nhdr);
                                ObCloseHandle(pr,nh);
                                ObDereferenceObject(gpHdr);
                                st = registry_find_key(&parent, &parentHdr);
                            } else { ObDereferenceObject(nhdr); ObDereferenceObject(gpHdr); return is; }
                        } else {
                            OBJECT_ATTRIBUTES oa2={0}; oa2.Length=sizeof(oa2); oa2.ObjectName=&fu;
                            struct nt_eprocess *pr=kshim_current_process();
                            HANDLE nh; NTSTATUS is = ObInsertObject(nhdr,&oa2,0,pr,&nh);
                            if (NT_SUCCESS(is)) {
                                ObDereferenceObject(nhdr);
                                ObCloseHandle(pr,nh);
                                ObDereferenceObject(gpHdr);
                                st = registry_find_key(&parent, &parentHdr);
                            } else { ObDereferenceObject(nhdr); ObDereferenceObject(gpHdr); return is; }
                        }
                    } else { if(gpHdr) ObDereferenceObject(gpHdr); return ns; }
                } else {
                    if(gpHdr) ObDereferenceObject(gpHdr);
                    return st;
                }
            } else return STATUS_OBJECT_NAME_NOT_FOUND;
        } else return STATUS_OBJECT_NAME_NOT_FOUND;
        if (!NT_SUCCESS(st)) return st;
    }
    // parent can be directory (\Registry) or key (\Registry\Machine ...)
    struct nt_key *parentKey=NULL;
    bool parentIsKey = (parentHdr->type == &nt_key_type);
    if (parentIsKey) parentKey = (struct nt_key*)nt_object_body(parentHdr);
    else if (parentHdr->type != &nt_directory_type) { ObDereferenceObject(parentHdr); return STATUS_OBJECT_TYPE_MISMATCH; }

    struct nt_object_header *hdr; struct nt_key *k;
    OBJECT_ATTRIBUTES tmp={0}; tmp.Length=sizeof(tmp);
    st = ObCreateObject(&nt_key_type, &tmp, sizeof(*k), &hdr, (void**)&k);
    if (!NT_SUCCESS(st)) { ObDereferenceObject(parentHdr); return st; }
    INIT_LIST_HEAD(&k->subkeys); INIT_LIST_HEAD(&k->values);
    rwlock_init(&k->lock);
    k->deleted=false;
    k->last_write_time.QuadPart = (LONGLONG)now_100ns();
    dup_us(&full,&k->full_path);
    dup_us(&leaf,&k->key_name);
    if (Class && Class->Length) dup_us(Class,&k->class_name);
    else { k->class_name.Buffer=NULL; k->class_name.Length=0; k->class_name.MaximumLength=0; }

    // insert into parent
    if (parentIsKey) {
        // registry subkey: do not use ObInsertObject directory insertion; manage via parent subkey list and handle table
        struct nt_eprocess *proc = kshim_current_process();
        if (!proc || !proc->handle_table) { ObDereferenceObject(hdr); ObDereferenceObject(parentHdr); return STATUS_INVALID_HANDLE; }
        // handle creation
        ULONG hattr=0;
        if (ObjectAttributes && (ObjectAttributes->Attributes & OBJ_INHERIT)) hattr|=OBJ_INHERIT;
        HANDLE h;
        st = ObCreateHandle(proc, hdr, DesiredAccess, hattr, &h);
        if (!NT_SUCCESS(st)) { ObDereferenceObject(hdr); ObDereferenceObject(parentHdr); return st; }
        // add to parent list
        struct subkey_entry *e = (struct subkey_entry*)kmalloc(sizeof(*e),GFP_KERNEL);
        if (!e) { ObCloseHandle(proc,h); ObDereferenceObject(hdr); ObDereferenceObject(parentHdr); return STATUS_NO_MEMORY; }
        INIT_LIST_HEAD(&e->link);
        e->hdr = hdr; ObReferenceObject(hdr);
        dup_us(&leaf,&e->name);
        write_lock(&parentKey->lock);
        list_add_tail(&e->link,&parentKey->subkeys);
        write_unlock(&parentKey->lock);
        parentKey->last_write_time.QuadPart = (LONGLONG)now_100ns();
        ObDereferenceObject(hdr); // drop creator ref; handle + parent list hold refs
        *KeyHandle = h;
        if (Disposition) *Disposition = REG_CREATED_NEW_KEY;
        persist_create_key(k);
        ObDereferenceObject(parentHdr);
        return STATUS_SUCCESS;
    } else {
        // parent is \Registry directory: direct insert
        OBJECT_ATTRIBUTES oa2={0}; oa2.Length=sizeof(oa2); oa2.ObjectName=&full;
        if (ObjectAttributes) oa2.Attributes = ObjectAttributes->Attributes & OBJ_VALID_ATTRIBUTES;
        struct nt_eprocess *proc = kshim_current_process();
        HANDLE h;
        st = ObInsertObject(hdr, &oa2, DesiredAccess, proc, &h);
        if (!NT_SUCCESS(st)) { ObDereferenceObject(hdr); ObDereferenceObject(parentHdr); return st; }
        ObDereferenceObject(hdr);
        *KeyHandle = h;
        if (Disposition) *Disposition = REG_CREATED_NEW_KEY;
        persist_create_key(k);
        ObDereferenceObject(parentHdr);
        return STATUS_SUCCESS;
    }
}

NTSTATUS NtOpenKey(PHANDLE KeyHandle, ACCESS_MASK DesiredAccess, POBJECT_ATTRIBUTES ObjectAttributes) {
    return NtOpenKeyEx(KeyHandle, DesiredAccess, ObjectAttributes, 0);
}
NTSTATUS NtOpenKeyEx(PHANDLE KeyHandle, ACCESS_MASK DesiredAccess, POBJECT_ATTRIBUTES ObjectAttributes, ULONG OpenOptions) {
    (void)OpenOptions;
    if (!KeyHandle || !ObjectAttributes || !ObjectAttributes->ObjectName) return STATUS_INVALID_PARAMETER;
    ntreg_init();
    DesiredAccess = strip_wow64(DesiredAccess);
    WCHAR buf[512];
    UNICODE_STRING full;
    NTSTATUS st = build_full_path(ObjectAttributes, &full, buf, 512);
    if (!NT_SUCCESS(st)) return st;
    if (is_machine_path(&full) && (DesiredAccess & (KEY_SET_VALUE|KEY_CREATE_SUB_KEY))) {
        ACCESS_MASK writeMask = KEY_SET_VALUE|KEY_CREATE_SUB_KEY;
        if (DesiredAccess & writeMask) return STATUS_ACCESS_DENIED;
    }
    struct nt_object_header *hdr=NULL;
    st = registry_find_key(&full, &hdr);
    if (!NT_SUCCESS(st)) return STATUS_OBJECT_NAME_NOT_FOUND;
    struct nt_eprocess *proc = kshim_current_process();
    if (!proc || !proc->handle_table) { ObDereferenceObject(hdr); return STATUS_INVALID_HANDLE; }
    ULONG hattr=0;
    if (ObjectAttributes->Attributes & OBJ_INHERIT) hattr|=OBJ_INHERIT;
    HANDLE h;
    st = ObCreateHandle(proc, hdr, DesiredAccess, hattr, &h);
    ObDereferenceObject(hdr);
    if (!NT_SUCCESS(st)) return st;
    *KeyHandle = h;
    return STATUS_SUCCESS;
}

NTSTATUS NtDeleteKey(HANDLE KeyHandle) {
    ntreg_init();
    struct nt_eprocess *proc = kshim_current_process();
    struct nt_object_header *hdr=NULL;
    NTSTATUS st = ObReferenceObjectByHandle(proc, KeyHandle, &nt_key_type, UserMode, 0, &hdr, NULL);
    if (!NT_SUCCESS(st)) return st;
    struct nt_key *k = (struct nt_key*)nt_object_body(hdr);
    read_lock(&k->lock);
    bool hasSubs = !list_empty(&k->subkeys);
    read_unlock(&k->lock);
    if (hasSubs) { ObDereferenceObject(hdr); return STATUS_ACCESS_DENIED; }
    /* remove from parent's list and namespace */
    // find parent path
    size_t len = k->full_path.Length/sizeof(WCHAR);
    ssize_t slash=-1;
    for (ssize_t i=(ssize_t)len-1;i>=0;i--) if (k->full_path.Buffer[i]=='\\'){ slash=i; break; }
    if (slash>0) {
        UNICODE_STRING parent; parent.Buffer=k->full_path.Buffer; parent.Length=(USHORT)(slash*sizeof(WCHAR)); parent.MaximumLength=parent.Length;
        struct nt_object_header *phdr=NULL;
        if (NT_SUCCESS(ObReferenceObjectByName(&parent, NULL, UserMode,0,&phdr))) {
            if (phdr->type==&nt_key_type) {
                struct nt_key *pk=(struct nt_key*)nt_object_body(phdr);
                write_lock(&pk->lock);
                struct subkey_entry *e,*tmp;
                list_for_each_entry_safe(e,tmp,&pk->subkeys,link){
                    if (e->hdr==hdr){ list_del(&e->link); if(e->name.Buffer) kfree(e->name.Buffer); ObDereferenceObject(e->hdr); kfree(e); break; }
                }
                write_unlock(&pk->lock);
            }
            ObDereferenceObject(phdr);
        }
    }
    persist_delete_key(k);
    ObMakeTemporaryObject(hdr);
    // mark deleted
    write_lock(&k->lock); k->deleted=true; write_unlock(&k->lock);
    ObDereferenceObject(hdr);
    /* close handle? Caller will NtClose */
    // Also need to close the handle that was used? No, just deref
    // But per NT, deleting key doesn't close handle until NtClose
    return STATUS_SUCCESS;
}
NTSTATUS NtFlushKey(HANDLE KeyHandle) {
    ntreg_init();
    struct nt_eprocess *proc=kshim_current_process();
    struct nt_object_header *hdr=NULL;
    NTSTATUS st=ObReferenceObjectByHandle(proc,KeyHandle,&nt_key_type,UserMode,0,&hdr,NULL);
    if (!NT_SUCCESS(st)) return st;
    /* persist snapshot */
#ifdef __KERNEL__
    char *snap = kmalloc(PATH_MAX, GFP_KERNEL);
    char *base = kmalloc(PATH_MAX, GFP_KERNEL);
    if (!snap || !base) { if (snap) kfree(snap); if (base) kfree(base); ObDereferenceObject(hdr); return STATUS_NO_MEMORY; }
    hive_base_path(base,PATH_MAX);
    snprintf(snap,PATH_MAX,"%s/hive_snapshot.bin",base);
#else
    char snap[PATH_MAX]; char base[PATH_MAX]; hive_base_path(base,sizeof(base));
    snprintf(snap,sizeof(snap),"%s/hive_snapshot.bin",base);
#endif
#ifndef __KERNEL__
    ensure_hive_dir();
    int fd=open(snap,O_WRONLY|O_CREAT|O_TRUNC,0644);
    if (fd>=0){
        const char *magic="NTREG\x01";
        (void)write(fd,magic,6);
        uint32_t cnt=0;
        (void)write(fd,&cnt,4);
        fsync(fd); close(fd);
        int dfd=open(base,O_DIRECTORY);
        if(dfd>=0){ fsync(dfd); close(dfd); }
    }
#else
    {
        struct file *filp = filp_open(snap, O_WRONLY|O_CREAT|O_TRUNC, 0644);
        if (!IS_ERR(filp)) {
            loff_t pos=0;
            const char magic[6]="NTREG\x01";
            kernel_write(filp, magic, 6, &pos);
            uint32_t cnt=0; kernel_write(filp, &cnt, 4, &pos);
            vfs_fsync(filp, 0);
            filp_close(filp, NULL);
        }
        ensure_hive_dir();
    }
#endif
#ifdef __KERNEL__
    kfree(snap);
    kfree(base);
#endif
    ObDereferenceObject(hdr);
    return STATUS_SUCCESS;
}

NTSTATUS NtSetValueKey(HANDLE KeyHandle, PUNICODE_STRING ValueName, ULONG TitleIndex, ULONG Type, PVOID Data, ULONG DataSize) {
    (void)TitleIndex;
    ntreg_init();
    struct nt_eprocess *proc=kshim_current_process();
    struct nt_object_header *hdr=NULL;
    ACCESS_MASK granted;
    NTSTATUS st=ObReferenceObjectByHandle(proc,KeyHandle,&nt_key_type,UserMode,KEY_SET_VALUE,&hdr,&granted);
    if (!NT_SUCCESS(st)) return st;
    if ((granted & KEY_SET_VALUE)==0 && (granted!=0xFFFFFFFFUL)) { ObDereferenceObject(hdr); return STATUS_ACCESS_DENIED; }
    struct nt_key *k=(struct nt_key*)nt_object_body(hdr);
    UNICODE_STRING vname={0};
    if (ValueName && ValueName->Length) {
        st=dup_us(ValueName,&vname);
        if (!NT_SUCCESS(st)) { ObDereferenceObject(hdr); return st; }
    } else { vname.Buffer=NULL; vname.Length=0; vname.MaximumLength=0; }
    write_lock(&k->lock);
    struct reg_value *iter=NULL;
    struct reg_value *target=NULL;
    list_for_each_entry(iter,&k->values,link){
        bool eq=false;
        if (iter->name.Length==vname.Length) eq=us_equal_ci(&iter->name,&vname);
        else if (iter->name.Length==0 && vname.Length==0) eq=true;
        if (eq){ target=iter; break; }
    }
    if (target) {
        // replace
        if (target->data) kfree(target->data);
        target->data=NULL;
        target->data_len=DataSize;
        target->type=Type;
        if (DataSize) {
            target->data=(uint8_t*)kmalloc(DataSize,GFP_KERNEL);
            if (!target->data){ write_unlock(&k->lock); free_us(&vname); ObDereferenceObject(hdr); return STATUS_NO_MEMORY; }
            if (Data && DataSize) memcpy(target->data,Data,DataSize);
        }
        k->last_write_time.QuadPart=(LONGLONG)now_100ns();
        write_unlock(&k->lock);
        persist_write_value(k,target);
        free_us(&vname);
        ObDereferenceObject(hdr);
        return STATUS_SUCCESS;
    } else {
        struct reg_value *nv=(struct reg_value*)kmalloc(sizeof(*nv),GFP_KERNEL);
        if (!nv){ write_unlock(&k->lock); free_us(&vname); ObDereferenceObject(hdr); return STATUS_NO_MEMORY; }
        INIT_LIST_HEAD(&nv->link);
        nv->name=vname; // take ownership
        nv->type=Type;
        nv->data_len=DataSize;
        if (DataSize){
            nv->data=(uint8_t*)kmalloc(DataSize,GFP_KERNEL);
            if (!nv->data){ kfree(nv); write_unlock(&k->lock); /* vname already moved, avoid double free */ ObDereferenceObject(hdr); return STATUS_NO_MEMORY; }
            if (Data) memcpy(nv->data,Data,DataSize);
        } else nv->data=NULL;
        list_add_tail(&nv->link,&k->values);
        k->last_write_time.QuadPart=(LONGLONG)now_100ns();
        write_unlock(&k->lock);
        persist_write_value(k,nv);
        ObDereferenceObject(hdr);
        return STATUS_SUCCESS;
    }
}
NTSTATUS NtQueryValueKey(HANDLE KeyHandle, PUNICODE_STRING ValueName, ULONG KeyValueInformationClass, PVOID KeyValueInformation, ULONG Length, PULONG ResultLength) {
    ntreg_init();
    struct nt_eprocess *proc=kshim_current_process();
    struct nt_object_header *hdr=NULL;
    NTSTATUS st=ObReferenceObjectByHandle(proc,KeyHandle,&nt_key_type,UserMode,KEY_QUERY_VALUE,&hdr,NULL);
    if (!NT_SUCCESS(st)) return st;
    if (!ValueName) { ObDereferenceObject(hdr); return STATUS_INVALID_PARAMETER; }
    // ValueName is already kernel-copied in svc path; for direct shim calls Buffer is valid kernel pointer
    if (!ValueName->Buffer && ValueName->Length!=0) { ObDereferenceObject(hdr); return STATUS_INVALID_PARAMETER; }
    // allow KeyValueInformation==NULL when Length==0 (probe for size)
    UNICODE_STRING vname; vname.Buffer=ValueName->Buffer; vname.Length=ValueName->Length; vname.MaximumLength=ValueName->MaximumLength;
    struct nt_key *k=(struct nt_key*)nt_object_body(hdr);
    struct reg_value *target=NULL;
    struct reg_value *iter=NULL;
    read_lock(&k->lock);
    list_for_each_entry(iter,&k->values,link){
        if (us_equal_ci(&iter->name,&vname)){ target=iter; break; }
    }
    if (!target) {
        if (vname.Length==0) {
            list_for_each_entry(iter,&k->values,link){ if (iter->name.Length==0) { target=iter; break; } }
        }
    }
    if (!target) {
        read_unlock(&k->lock);
        if (ResultLength) *ResultLength=0;
        ObDereferenceObject(hdr);
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }
    ULONG needed=0;
    if (KeyValueInformationClass==KeyValuePartialInformation) {
        needed = (ULONG)offsetof(KEY_VALUE_PARTIAL_INFORMATION, Data) + target->data_len;
    } else if (KeyValueInformationClass==KeyValueBasicInformation) {
        needed = (ULONG)offsetof(KEY_VALUE_BASIC_INFORMATION, Name) + target->name.Length;
    } else if (KeyValueInformationClass==KeyValueFullInformation) {
        needed = (ULONG)offsetof(KEY_VALUE_FULL_INFORMATION, Name) + target->name.Length + target->data_len;
    } else {
        read_unlock(&k->lock); ObDereferenceObject(hdr); return STATUS_INVALID_INFO_CLASS;
    }
    if (ResultLength) *ResultLength = needed;
    if (Length < needed) { read_unlock(&k->lock); ObDereferenceObject(hdr); return STATUS_BUFFER_TOO_SMALL; }
    if (KeyValueInformationClass==KeyValuePartialInformation) {
        PKEY_VALUE_PARTIAL_INFORMATION p=(PKEY_VALUE_PARTIAL_INFORMATION)KeyValueInformation;
        p->TitleIndex=0;
        p->Type=target->type;
        p->DataLength=target->data_len;
        if (target->data_len && target->data) memcpy(p->Data, target->data, target->data_len);
    } else if (KeyValueInformationClass==KeyValueBasicInformation) {
        PKEY_VALUE_BASIC_INFORMATION p=(PKEY_VALUE_BASIC_INFORMATION)KeyValueInformation;
        p->TitleIndex=0; p->Type=target->type; p->NameLength=target->name.Length;
        if (target->name.Length) memcpy(p->Name, target->name.Buffer, target->name.Length);
    } else {
        PKEY_VALUE_FULL_INFORMATION p=(PKEY_VALUE_FULL_INFORMATION)KeyValueInformation;
        p->TitleIndex=0; p->Type=target->type; p->DataOffset = (ULONG)offsetof(KEY_VALUE_FULL_INFORMATION, Name) + target->name.Length;
        p->DataLength=target->data_len; p->NameLength=target->name.Length;
        if (target->name.Length) memcpy(p->Name, target->name.Buffer, target->name.Length);
        if (target->data_len) memcpy((char*)p + p->DataOffset, target->data, target->data_len);
    }
    read_unlock(&k->lock);
    ObDereferenceObject(hdr);
    return STATUS_SUCCESS;
}
NTSTATUS NtDeleteValueKey(HANDLE KeyHandle, PUNICODE_STRING ValueName) {
    ntreg_init();
    struct nt_eprocess *proc=kshim_current_process();
    struct nt_object_header *hdr=NULL;
    NTSTATUS st=ObReferenceObjectByHandle(proc,KeyHandle,&nt_key_type,UserMode,KEY_SET_VALUE,&hdr,NULL);
    if (!NT_SUCCESS(st)) return st;
    struct nt_key *k=(struct nt_key*)nt_object_body(hdr);
    write_lock(&k->lock);
    struct reg_value *target=NULL, *iter;
    list_for_each_entry(iter,&k->values,link){
        UNICODE_STRING vn={0}; if (ValueName){ vn.Buffer=ValueName->Buffer; vn.Length=ValueName->Length; vn.MaximumLength=ValueName->MaximumLength; }
        if (us_equal_ci(&iter->name,&vn)){ target=iter; break; }
    }
    if (!target) { write_unlock(&k->lock); ObDereferenceObject(hdr); return STATUS_OBJECT_NAME_NOT_FOUND; }
    list_del(&target->link);
    write_unlock(&k->lock);
    persist_delete_value(k, ValueName);
    if (target->name.Buffer) kfree(target->name.Buffer);
    if (target->data) kfree(target->data);
    kfree(target);
    k->last_write_time.QuadPart=(LONGLONG)now_100ns();
    ObDereferenceObject(hdr);
    return STATUS_SUCCESS;
}
NTSTATUS NtEnumerateKey(HANDLE KeyHandle, ULONG Index, ULONG KeyInformationClass, PVOID KeyInformation, ULONG Length, PULONG ResultLength) {
    ntreg_init();
    struct nt_eprocess *proc=kshim_current_process();
    struct nt_object_header *hdr=NULL;
    NTSTATUS st=ObReferenceObjectByHandle(proc,KeyHandle,&nt_key_type,UserMode,KEY_ENUMERATE_SUB_KEYS,&hdr,NULL);
    if (!NT_SUCCESS(st)) return st;
    struct nt_key *k=(struct nt_key*)nt_object_body(hdr);
    read_lock(&k->lock);
    size_t count=0; struct subkey_entry *e;
    list_for_each_entry(e,&k->subkeys,link) count++;
    if (Index >= count) { read_unlock(&k->lock); ObDereferenceObject(hdr); if (ResultLength) *ResultLength=0; return STATUS_NO_MORE_ENTRIES; }
    e=NULL; size_t idx=0;
    list_for_each_entry(e,&k->subkeys,link){ if (idx==Index) break; idx++; }
    if (!e){ read_unlock(&k->lock); ObDereferenceObject(hdr); return STATUS_NO_MORE_ENTRIES; }
    // build needed
    ULONG nameLen = e->name.Length;
    ULONG needed=0;
    if (KeyInformationClass==KeyBasicInformation) {
        needed = (ULONG)offsetof(KEY_BASIC_INFORMATION, Name) + nameLen;
        if (ResultLength) *ResultLength = needed;
        if (Length < needed) { read_unlock(&k->lock); ObDereferenceObject(hdr); return STATUS_BUFFER_TOO_SMALL; }
        PKEY_BASIC_INFORMATION p=(PKEY_BASIC_INFORMATION)KeyInformation;
        p->LastWriteTime.QuadPart = ((struct nt_key*)nt_object_body(e->hdr))->last_write_time.QuadPart;
        p->TitleIndex=0;
        p->NameLength=nameLen;
        if (nameLen) memcpy(p->Name, e->name.Buffer, nameLen);
    } else {
        read_unlock(&k->lock); ObDereferenceObject(hdr); return STATUS_INVALID_INFO_CLASS;
    }
    read_unlock(&k->lock);
    ObDereferenceObject(hdr);
    return STATUS_SUCCESS;
}
NTSTATUS NtEnumerateValueKey(HANDLE KeyHandle, ULONG Index, ULONG KeyValueInformationClass, PVOID KeyValueInformation, ULONG Length, PULONG ResultLength) {
    ntreg_init();
    struct nt_eprocess *proc=kshim_current_process();
    struct nt_object_header *hdr=NULL;
    NTSTATUS st=ObReferenceObjectByHandle(proc,KeyHandle,&nt_key_type,UserMode,KEY_QUERY_VALUE,&hdr,NULL);
    if (!NT_SUCCESS(st)) return st;
    struct nt_key *k=(struct nt_key*)nt_object_body(hdr);
    read_lock(&k->lock);
    size_t count=0; struct reg_value *v;
    list_for_each_entry(v,&k->values,link) count++;
    if (Index >= count){ read_unlock(&k->lock); ObDereferenceObject(hdr); if(ResultLength) *ResultLength=0; return STATUS_NO_MORE_ENTRIES; }
    size_t idx=0; v=NULL;
    list_for_each_entry(v,&k->values,link){ if(idx==Index) break; idx++; }
    if (!v){ read_unlock(&k->lock); ObDereferenceObject(hdr); return STATUS_NO_MORE_ENTRIES; }
    ULONG needed=0;
    if (KeyValueInformationClass==KeyValueBasicInformation) {
        needed = (ULONG)offsetof(KEY_VALUE_BASIC_INFORMATION, Name)+v->name.Length;
    } else if (KeyValueInformationClass==KeyValueFullInformation) {
        needed = (ULONG)offsetof(KEY_VALUE_FULL_INFORMATION, Name)+v->name.Length+v->data_len;
    } else if (KeyValueInformationClass==KeyValuePartialInformation) {
        needed = (ULONG)offsetof(KEY_VALUE_PARTIAL_INFORMATION, Data)+v->data_len;
    } else { read_unlock(&k->lock); ObDereferenceObject(hdr); return STATUS_INVALID_INFO_CLASS; }
    if (ResultLength) *ResultLength=needed;
    if (Length < needed){ read_unlock(&k->lock); ObDereferenceObject(hdr); return STATUS_BUFFER_TOO_SMALL; }
    if (KeyValueInformationClass==KeyValueBasicInformation){
        PKEY_VALUE_BASIC_INFORMATION p=(PKEY_VALUE_BASIC_INFORMATION)KeyValueInformation;
        p->TitleIndex=0; p->Type=v->type; p->NameLength=v->name.Length;
        if(v->name.Length) memcpy(p->Name,v->name.Buffer,v->name.Length);
    } else if (KeyValueInformationClass==KeyValueFullInformation){
        PKEY_VALUE_FULL_INFORMATION p=(PKEY_VALUE_FULL_INFORMATION)KeyValueInformation;
        p->TitleIndex=0; p->Type=v->type; p->NameLength=v->name.Length; p->DataLength=v->data_len;
        p->DataOffset= (ULONG)offsetof(KEY_VALUE_FULL_INFORMATION, Name)+v->name.Length;
        if(v->name.Length) memcpy(p->Name,v->name.Buffer,v->name.Length);
        if(v->data_len) memcpy((char*)p + p->DataOffset, v->data, v->data_len);
    } else {
        PKEY_VALUE_PARTIAL_INFORMATION p=(PKEY_VALUE_PARTIAL_INFORMATION)KeyValueInformation;
        p->TitleIndex=0; p->Type=v->type; p->DataLength=v->data_len;
        if(v->data_len) memcpy(p->Data,v->data,v->data_len);
    }
    read_unlock(&k->lock);
    ObDereferenceObject(hdr);
    return STATUS_SUCCESS;
}
NTSTATUS NtQueryKey(HANDLE KeyHandle, ULONG KeyInformationClass, PVOID KeyInformation, ULONG Length, PULONG ResultLength) {
    ntreg_init();
    struct nt_eprocess *proc=kshim_current_process();
    struct nt_object_header *hdr=NULL;
    NTSTATUS st=ObReferenceObjectByHandle(proc,KeyHandle,&nt_key_type,UserMode,0,&hdr,NULL);
    if (!NT_SUCCESS(st)) return st;
    struct nt_key *k=(struct nt_key*)nt_object_body(hdr);
    ULONG needed=0;
    if (KeyInformationClass==KeyBasicInformation){
        needed = (ULONG)offsetof(KEY_BASIC_INFORMATION, Name) + k->key_name.Length;
        if (ResultLength) *ResultLength=needed;
        if (Length < needed){ ObDereferenceObject(hdr); return STATUS_BUFFER_TOO_SMALL; }
        PKEY_BASIC_INFORMATION p=(PKEY_BASIC_INFORMATION)KeyInformation;
        p->LastWriteTime.QuadPart=k->last_write_time.QuadPart;
        p->TitleIndex=0; p->NameLength=k->key_name.Length;
        if(k->key_name.Length) memcpy(p->Name,k->key_name.Buffer,k->key_name.Length);
    } else if (KeyInformationClass==KeyNodeInformation){
        needed = (ULONG)offsetof(KEY_NODE_INFORMATION, Name) + k->key_name.Length + k->class_name.Length;
        if (needed < sizeof(KEY_NODE_INFORMATION)) needed = sizeof(KEY_NODE_INFORMATION);
        if (ResultLength) *ResultLength=needed;
        if (Length < needed){ ObDereferenceObject(hdr); return STATUS_BUFFER_TOO_SMALL; }
        PKEY_NODE_INFORMATION p=(PKEY_NODE_INFORMATION)KeyInformation;
        p->LastWriteTime.QuadPart=k->last_write_time.QuadPart;
        p->TitleIndex=0;
        p->ClassOffset = (ULONG)offsetof(KEY_NODE_INFORMATION, Name) + k->key_name.Length;
        p->ClassLength=k->class_name.Length;
        p->NameLength=k->key_name.Length;
        if(k->key_name.Length) memcpy(p->Name,k->key_name.Buffer,k->key_name.Length);
        if(k->class_name.Length) memcpy((char*)p + p->ClassOffset, k->class_name.Buffer, k->class_name.Length);
    } else if (KeyInformationClass==KeyFullInformation){
        needed = (ULONG)offsetof(KEY_FULL_INFORMATION, Class) + k->class_name.Length;
        if (ResultLength) *ResultLength=needed;
        if (Length < needed){ ObDereferenceObject(hdr); return STATUS_BUFFER_TOO_SMALL; }
        PKEY_FULL_INFORMATION p=(PKEY_FULL_INFORMATION)KeyInformation;
        p->LastWriteTime.QuadPart=k->last_write_time.QuadPart;
        p->TitleIndex=0;
        p->ClassOffset= (ULONG)offsetof(KEY_FULL_INFORMATION, Class);
        p->ClassLength=k->class_name.Length;
        read_lock(&k->lock);
        size_t sub=0; struct subkey_entry *e; list_for_each_entry(e,&k->subkeys,link) sub++;
        size_t vals=0; struct reg_value *v; list_for_each_entry(v,&k->values,link) vals++;
        ULONG maxName=0, maxValName=0, maxValData=0;
        list_for_each_entry(e,&k->subkeys,link){ if(e->name.Length>maxName) maxName=e->name.Length; }
        list_for_each_entry(v,&k->values,link){ if(v->name.Length>maxValName) maxValName=v->name.Length; if(v->data_len>maxValData) maxValData=v->data_len; }
        read_unlock(&k->lock);
        p->SubKeys=(ULONG)sub; p->MaxNameLen=maxName; p->MaxClassLen=k->class_name.Length;
        p->Values=(ULONG)vals; p->MaxValueNameLen=maxValName; p->MaxValueDataLen=maxValData;
        if(k->class_name.Length) memcpy(p->Class,k->class_name.Buffer,k->class_name.Length);
    } else if (KeyInformationClass==KeyNameInformation){
        needed = (ULONG)offsetof(KEY_NAME_INFORMATION, Name) + k->full_path.Length;
        if (ResultLength) *ResultLength=needed;
        if (Length < needed){ ObDereferenceObject(hdr); return STATUS_BUFFER_OVERFLOW; }
        PKEY_NAME_INFORMATION p=(PKEY_NAME_INFORMATION)KeyInformation;
        p->NameLength=k->full_path.Length;
        if(k->full_path.Length) memcpy(p->Name,k->full_path.Buffer,k->full_path.Length);
    } else {
        ObDereferenceObject(hdr);
        if (ResultLength) *ResultLength=0;
        return STATUS_INVALID_INFO_CLASS;
    }
    ObDereferenceObject(hdr);
    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* svc wrappers                                                        */
/* ------------------------------------------------------------------ */
#ifndef __KERNEL__
/* userspace shim: with user-pointer checks (mirrors kernel path) */
static NTSTATUS k_copy_oa_us(POBJECT_ATTRIBUTES uptr, OBJECT_ATTRIBUTES *koa, UNICODE_STRING *kname, WCHAR *kbuf, size_t kbuf_bytes, POBJECT_ATTRIBUTES *out)
{
    if (!uptr){ memset(koa,0,sizeof(*koa)); koa->Length=sizeof(OBJECT_ATTRIBUTES); *out=NULL; return STATUS_SUCCESS; }
    if (!access_ok(uptr,sizeof(OBJECT_ATTRIBUTES))) return STATUS_ACCESS_VIOLATION;
    if (copy_from_user(koa,uptr,sizeof(OBJECT_ATTRIBUTES))) return STATUS_ACCESS_VIOLATION;
    if (koa->Length != sizeof(OBJECT_ATTRIBUTES)) return STATUS_INVALID_PARAMETER;
    if (koa->ObjectName){
        if (!access_ok(koa->ObjectName,sizeof(UNICODE_STRING))) return STATUS_ACCESS_VIOLATION;
        UNICODE_STRING tmp;
        if (copy_from_user(&tmp,koa->ObjectName,sizeof(tmp))) return STATUS_ACCESS_VIOLATION;
        if (tmp.Length){
            if (!access_ok(tmp.Buffer,tmp.Length)) return STATUS_ACCESS_VIOLATION;
            if ((size_t)tmp.Length > kbuf_bytes) return STATUS_NAME_TOO_LONG;
            if (copy_from_user(kbuf,tmp.Buffer,tmp.Length)) return STATUS_ACCESS_VIOLATION;
            kname->Buffer=kbuf; kname->Length=tmp.Length; kname->MaximumLength=tmp.Length;
            koa->ObjectName=kname;
        } else koa->ObjectName=NULL;
    }
    *out=koa;
    return STATUS_SUCCESS;
}
static NTSTATUS k_copy_us_us(PUNICODE_STRING uptr, UNICODE_STRING *k, WCHAR *buf, size_t buf_bytes)
{
    if (!uptr) { k->Buffer=NULL; k->Length=0; k->MaximumLength=0; return STATUS_SUCCESS; }
    if (!access_ok(uptr,sizeof(UNICODE_STRING))) return STATUS_ACCESS_VIOLATION;
    UNICODE_STRING tmp;
    if (copy_from_user(&tmp,uptr,sizeof(tmp))) return STATUS_ACCESS_VIOLATION;
    if (tmp.Length){
        if (!access_ok(tmp.Buffer,tmp.Length)) return STATUS_ACCESS_VIOLATION;
        if ((size_t)tmp.Length > buf_bytes) return STATUS_NAME_TOO_LONG;
        if (copy_from_user(buf,tmp.Buffer,tmp.Length)) return STATUS_ACCESS_VIOLATION;
        k->Buffer=buf; k->Length=tmp.Length; k->MaximumLength=tmp.Length;
    } else { k->Buffer=NULL; k->Length=0; k->MaximumLength=0; }
    return STATUS_SUCCESS;
}
NTSTATUS nt_svc_NtCreateKey(unsigned long a0, unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5, unsigned long a6, unsigned long a7, unsigned long a8, unsigned long a9, unsigned long a10, unsigned long a11)
{
    PHANDLE uh=(PHANDLE)a0; ACCESS_MASK da=(ACCESS_MASK)a1; POBJECT_ATTRIBUTES uoa=(POBJECT_ATTRIBUTES)a2; ULONG ti=(ULONG)a3; PUNICODE_STRING uclass=(PUNICODE_STRING)a4; ULONG co=(ULONG)a5; PULONG udisp=(PULONG)a6;
    (void)a7;(void)a8;(void)a9;(void)a10;(void)a11;
    if (!uh || !access_ok(uh,sizeof(HANDLE))) return STATUS_ACCESS_VIOLATION;
    if (udisp && !access_ok(udisp,sizeof(ULONG))) return STATUS_ACCESS_VIOLATION;
    OBJECT_ATTRIBUTES koa; UNICODE_STRING kname; WCHAR kbuf[512]; POBJECT_ATTRIBUTES poa=NULL;
    NTSTATUS st=k_copy_oa_us(uoa,&koa,&kname,kbuf,sizeof(kbuf),&poa); if (!NT_SUCCESS(st)) return st;
    UNICODE_STRING kclass={0}; WCHAR cbuf[256]; PUNICODE_STRING pcl=NULL;
    if (uclass){ st=k_copy_us_us(uclass,&kclass,cbuf,sizeof(cbuf)); if (!NT_SUCCESS(st)) return st; pcl=&kclass; }
    HANDLE kh; ULONG kdisp=0;
    st = NtCreateKey(&kh, da, poa, ti, pcl, co, (udisp?&kdisp:NULL));
    if (NT_SUCCESS(st)){
        if (copy_to_user(uh,&kh,sizeof(HANDLE))) return STATUS_ACCESS_VIOLATION;
        if (udisp && copy_to_user(udisp,&kdisp,sizeof(ULONG))) return STATUS_ACCESS_VIOLATION;
    }
    return st;
}
NTSTATUS nt_svc_NtOpenKey(unsigned long a0, unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5, unsigned long a6, unsigned long a7, unsigned long a8, unsigned long a9, unsigned long a10, unsigned long a11)
{
    PHANDLE uh=(PHANDLE)a0; ACCESS_MASK da=(ACCESS_MASK)a1; POBJECT_ATTRIBUTES uoa=(POBJECT_ATTRIBUTES)a2;
    (void)a3;(void)a4;(void)a5;(void)a6;(void)a7;(void)a8;(void)a9;(void)a10;(void)a11;
    if (!uh || !access_ok(uh,sizeof(HANDLE))) return STATUS_ACCESS_VIOLATION;
    OBJECT_ATTRIBUTES koa; UNICODE_STRING kname; WCHAR kbuf[512]; POBJECT_ATTRIBUTES poa=NULL;
    NTSTATUS st=k_copy_oa_us(uoa,&koa,&kname,kbuf,sizeof(kbuf),&poa); if(!NT_SUCCESS(st)) return st;
    if (!poa||!poa->ObjectName) return STATUS_INVALID_PARAMETER;
    HANDLE kh; st=NtOpenKey(&kh,da,poa);
    if (NT_SUCCESS(st)){ if(copy_to_user(uh,&kh,sizeof(HANDLE))) return STATUS_ACCESS_VIOLATION; }
    return st;
}
NTSTATUS nt_svc_NtOpenKeyEx(unsigned long a0, unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5, unsigned long a6, unsigned long a7, unsigned long a8, unsigned long a9, unsigned long a10, unsigned long a11)
{
    PHANDLE uh=(PHANDLE)a0; ACCESS_MASK da=(ACCESS_MASK)a1; POBJECT_ATTRIBUTES uoa=(POBJECT_ATTRIBUTES)a2; ULONG oo=(ULONG)a3;
    (void)a4;(void)a5;(void)a6;(void)a7;(void)a8;(void)a9;(void)a10;(void)a11;
    if (!uh || !access_ok(uh,sizeof(HANDLE))) return STATUS_ACCESS_VIOLATION;
    OBJECT_ATTRIBUTES koa; UNICODE_STRING kname; WCHAR kbuf[512]; POBJECT_ATTRIBUTES poa=NULL;
    NTSTATUS st=k_copy_oa_us(uoa,&koa,&kname,kbuf,sizeof(kbuf),&poa); if(!NT_SUCCESS(st)) return st;
    if (!poa||!poa->ObjectName) return STATUS_INVALID_PARAMETER;
    HANDLE kh; st=NtOpenKeyEx(&kh,da,poa,oo);
    if (NT_SUCCESS(st)){ if(copy_to_user(uh,&kh,sizeof(HANDLE))) return STATUS_ACCESS_VIOLATION; }
    return st;
}
NTSTATUS nt_svc_NtDeleteKey(unsigned long a0, unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5, unsigned long a6, unsigned long a7, unsigned long a8, unsigned long a9, unsigned long a10, unsigned long a11)
{
    HANDLE h=(HANDLE)a0; (void)a1;(void)a2;(void)a3;(void)a4;(void)a5;(void)a6;(void)a7;(void)a8;(void)a9;(void)a10;(void)a11;
    return NtDeleteKey(h);
}
NTSTATUS nt_svc_NtFlushKey(unsigned long a0, unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5, unsigned long a6, unsigned long a7, unsigned long a8, unsigned long a9, unsigned long a10, unsigned long a11)
{
    HANDLE h=(HANDLE)a0; (void)a1;(void)a2;(void)a3;(void)a4;(void)a5;(void)a6;(void)a7;(void)a8;(void)a9;(void)a10;(void)a11;
    return NtFlushKey(h);
}
NTSTATUS nt_svc_NtEnumerateKey(unsigned long a0, unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5, unsigned long a6, unsigned long a7, unsigned long a8, unsigned long a9, unsigned long a10, unsigned long a11)
{
    HANDLE h=(HANDLE)a0; ULONG idx=(ULONG)a1; ULONG cls=(ULONG)a2; PVOID info=(PVOID)a3; ULONG len=(ULONG)a4; PULONG rlen=(PULONG)a5;
    (void)a6;(void)a7;(void)a8;(void)a9;(void)a10;(void)a11;
    if (rlen && !access_ok(rlen,sizeof(ULONG))) return STATUS_ACCESS_VIOLATION;
    if (info && len && !access_ok(info,len)) return STATUS_ACCESS_VIOLATION;
    char *kbuf=NULL; if(len){ kbuf=kzalloc(len,GFP_KERNEL); if(!kbuf) return STATUS_NO_MEMORY; }
    ULONG klen=0;
    NTSTATUS st=NtEnumerateKey(h,idx,cls,kbuf,len,&klen);
    if (rlen){ if(copy_to_user(rlen,&klen,sizeof(ULONG))) {kfree(kbuf); return STATUS_ACCESS_VIOLATION;}}
    if (NT_SUCCESS(st) && kbuf && copy_to_user(info,kbuf,len)) st=STATUS_ACCESS_VIOLATION;
    kfree(kbuf);
    return st;
}
NTSTATUS nt_svc_NtEnumerateValueKey(unsigned long a0, unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5, unsigned long a6, unsigned long a7, unsigned long a8, unsigned long a9, unsigned long a10, unsigned long a11)
{
    HANDLE h=(HANDLE)a0; ULONG idx=(ULONG)a1; ULONG cls=(ULONG)a2; PVOID info=(PVOID)a3; ULONG len=(ULONG)a4; PULONG rlen=(PULONG)a5;
    (void)a6;(void)a7;(void)a8;(void)a9;(void)a10;(void)a11;
    if (rlen && !access_ok(rlen,sizeof(ULONG))) return STATUS_ACCESS_VIOLATION;
    if (info && len && !access_ok(info,len)) return STATUS_ACCESS_VIOLATION;
    char *kbuf=NULL; if(len){ kbuf=kzalloc(len,GFP_KERNEL); if(!kbuf) return STATUS_NO_MEMORY; }
    ULONG klen=0;
    NTSTATUS st=NtEnumerateValueKey(h,idx,cls,kbuf,len,&klen);
    if (rlen){ if(copy_to_user(rlen,&klen,sizeof(ULONG))) {kfree(kbuf); return STATUS_ACCESS_VIOLATION;}}
    if (NT_SUCCESS(st) && kbuf && copy_to_user(info,kbuf,len)) st=STATUS_ACCESS_VIOLATION;
    kfree(kbuf);
    return st;
}
NTSTATUS nt_svc_NtQueryKey(unsigned long a0, unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5, unsigned long a6, unsigned long a7, unsigned long a8, unsigned long a9, unsigned long a10, unsigned long a11)
{
    HANDLE h=(HANDLE)a0; ULONG cls=(ULONG)a1; PVOID info=(PVOID)a2; ULONG len=(ULONG)a3; PULONG rlen=(PULONG)a4;
    (void)a5;(void)a6;(void)a7;(void)a8;(void)a9;(void)a10;(void)a11;
    if (rlen && !access_ok(rlen,sizeof(ULONG))) return STATUS_ACCESS_VIOLATION;
    if (info && len && !access_ok(info,len)) return STATUS_ACCESS_VIOLATION;
    char *kbuf=NULL; if(len){ kbuf=kzalloc(len,GFP_KERNEL); if(!kbuf) return STATUS_NO_MEMORY; }
    ULONG klen=0;
    NTSTATUS st=NtQueryKey(h,cls,kbuf,len,&klen);
    if (rlen){ if(copy_to_user(rlen,&klen,sizeof(ULONG))) {kfree(kbuf); return STATUS_ACCESS_VIOLATION;}}
    if (NT_SUCCESS(st) && kbuf && copy_to_user(info,kbuf,len)) st=STATUS_ACCESS_VIOLATION;
    kfree(kbuf);
    return st;
}
NTSTATUS nt_svc_NtQueryValueKey(unsigned long a0, unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5, unsigned long a6, unsigned long a7, unsigned long a8, unsigned long a9, unsigned long a10, unsigned long a11)
{
    PUNICODE_STRING uvn=(PUNICODE_STRING)a1; ULONG cls=(ULONG)a2; PVOID info=(PVOID)a3; ULONG len=(ULONG)a4; PULONG rlen=(PULONG)a5;
    HANDLE h=(HANDLE)a0;
    (void)a6;(void)a7;(void)a8;(void)a9;(void)a10;(void)a11;
    if (rlen && !access_ok(rlen,sizeof(ULONG))) return STATUS_ACCESS_VIOLATION;
    if (info && len && !access_ok(info,len)) return STATUS_ACCESS_VIOLATION;
    UNICODE_STRING kname={0}; WCHAR kbuf[256];
    NTSTATUS st=k_copy_us_us(uvn,&kname,kbuf,sizeof(kbuf)); if(!NT_SUCCESS(st)) return st;
    char *kout=NULL; if(len){ kout=kzalloc(len,GFP_KERNEL); if(!kout) return STATUS_NO_MEMORY; }
    ULONG klen=0;
    st=NtQueryValueKey(h,&kname,cls,kout,len,&klen);
    if (rlen){ if(copy_to_user(rlen,&klen,sizeof(ULONG))) {kfree(kout); return STATUS_ACCESS_VIOLATION;}}
    if (NT_SUCCESS(st) && kout && copy_to_user(info,kout,len)) st=STATUS_ACCESS_VIOLATION;
    kfree(kout);
    return st;
}
NTSTATUS nt_svc_NtSetValueKey(unsigned long a0, unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5, unsigned long a6, unsigned long a7, unsigned long a8, unsigned long a9, unsigned long a10, unsigned long a11)
{
    HANDLE h=(HANDLE)a0; PUNICODE_STRING uvn=(PUNICODE_STRING)a1; ULONG ti=(ULONG)a2; ULONG type=(ULONG)a3; PVOID data=(PVOID)a4; ULONG dlen=(ULONG)a5;
    (void)a6;(void)a7;(void)a8;(void)a9;(void)a10;(void)a11;
    UNICODE_STRING kname={0}; WCHAR kbuf[256];
    NTSTATUS st=k_copy_us_us(uvn,&kname,kbuf,sizeof(kbuf)); if(!NT_SUCCESS(st)) return st;
    void *kdata=NULL;
    if (dlen){
        if (!access_ok(data,dlen)) return STATUS_ACCESS_VIOLATION;
        kdata=kmalloc(dlen,GFP_KERNEL); if(!kdata) return STATUS_NO_MEMORY;
        if(copy_from_user(kdata,data,dlen)){kfree(kdata); return STATUS_ACCESS_VIOLATION;}
    }
    st=NtSetValueKey(h,&kname,ti,type,kdata,dlen);
    kfree(kdata);
    return st;
}
NTSTATUS nt_svc_NtDeleteValueKey(unsigned long a0, unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5, unsigned long a6, unsigned long a7, unsigned long a8, unsigned long a9, unsigned long a10, unsigned long a11)
{
    HANDLE h=(HANDLE)a0; PUNICODE_STRING uvn=(PUNICODE_STRING)a1;
    (void)a2;(void)a3;(void)a4;(void)a5;(void)a6;(void)a7;(void)a8;(void)a9;(void)a10;(void)a11;
    UNICODE_STRING kname={0}; WCHAR kbuf[256];
    NTSTATUS st=k_copy_us_us(uvn,&kname,kbuf,sizeof(kbuf)); if(!NT_SUCCESS(st)) return st;
    return NtDeleteValueKey(h,&kname);
}

#else /* __KERNEL__ */
static NTSTATUS k_copy_oa(POBJECT_ATTRIBUTES uptr, OBJECT_ATTRIBUTES *koa, UNICODE_STRING *kname, WCHAR *kbuf, size_t kbuf_bytes, POBJECT_ATTRIBUTES *out)
{
    if (!uptr){ memset(koa,0,sizeof(*koa)); koa->Length=sizeof(OBJECT_ATTRIBUTES); *out=NULL; return STATUS_SUCCESS; }
    if (!access_ok(uptr,sizeof(OBJECT_ATTRIBUTES))) return STATUS_ACCESS_VIOLATION;
    if (copy_from_user(koa,uptr,sizeof(OBJECT_ATTRIBUTES))) return STATUS_ACCESS_VIOLATION;
    if (koa->Length != sizeof(OBJECT_ATTRIBUTES)) return STATUS_INVALID_PARAMETER;
    if (koa->ObjectName){
        if (!access_ok(koa->ObjectName,sizeof(UNICODE_STRING))) return STATUS_ACCESS_VIOLATION;
        UNICODE_STRING tmp;
        if (copy_from_user(&tmp,koa->ObjectName,sizeof(tmp))) return STATUS_ACCESS_VIOLATION;
        if (tmp.Length){
            if (!access_ok(tmp.Buffer,tmp.Length)) return STATUS_ACCESS_VIOLATION;
            if ((size_t)tmp.Length > kbuf_bytes) return STATUS_NAME_TOO_LONG;
            if (copy_from_user(kbuf,tmp.Buffer,tmp.Length)) return STATUS_ACCESS_VIOLATION;
            kname->Buffer=kbuf; kname->Length=tmp.Length; kname->MaximumLength=tmp.Length;
            koa->ObjectName=kname;
        } else koa->ObjectName=NULL;
    }
    *out=koa;
    return STATUS_SUCCESS;
}
static NTSTATUS k_copy_us(PUNICODE_STRING uptr, UNICODE_STRING *k, WCHAR *buf, size_t buf_bytes)
{
    if (!uptr) { k->Buffer=NULL; k->Length=0; k->MaximumLength=0; return STATUS_SUCCESS; }
    if (!access_ok(uptr,sizeof(UNICODE_STRING))) return STATUS_ACCESS_VIOLATION;
    UNICODE_STRING tmp;
    if (copy_from_user(&tmp,uptr,sizeof(tmp))) return STATUS_ACCESS_VIOLATION;
    if (tmp.Length){
        if (!access_ok(tmp.Buffer,tmp.Length)) return STATUS_ACCESS_VIOLATION;
        if ((size_t)tmp.Length > buf_bytes) return STATUS_NAME_TOO_LONG;
        if (copy_from_user(buf,tmp.Buffer,tmp.Length)) return STATUS_ACCESS_VIOLATION;
        k->Buffer=buf; k->Length=tmp.Length; k->MaximumLength=tmp.Length;
    } else { k->Buffer=NULL; k->Length=0; k->MaximumLength=0; }
    return STATUS_SUCCESS;
}
NTSTATUS nt_svc_NtCreateKey(unsigned long a0, unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5, unsigned long a6, unsigned long a7, unsigned long a8, unsigned long a9, unsigned long a10, unsigned long a11)
{
    PHANDLE uh=(PHANDLE)a0; ACCESS_MASK da=(ACCESS_MASK)a1; POBJECT_ATTRIBUTES uoa=(POBJECT_ATTRIBUTES)a2; ULONG ti=(ULONG)a3; PUNICODE_STRING uclass=(PUNICODE_STRING)a4; ULONG co=(ULONG)a5; PULONG udisp=(PULONG)a6;
    (void)a7;(void)a8;(void)a9;(void)a10;(void)a11;
    if (!uh || !access_ok(uh,sizeof(PHANDLE))) return STATUS_ACCESS_VIOLATION;
    if (udisp && !access_ok(udisp,sizeof(ULONG))) return STATUS_ACCESS_VIOLATION;
    OBJECT_ATTRIBUTES koa; UNICODE_STRING kname; WCHAR kbuf[512]; POBJECT_ATTRIBUTES poa=NULL;
    NTSTATUS st=k_copy_oa(uoa,&koa,&kname,kbuf,sizeof(kbuf),&poa); if (!NT_SUCCESS(st)) return st;
    UNICODE_STRING kclass={0}; WCHAR cbuf[256]; PUNICODE_STRING pcl=NULL;
    if (uclass){ st=k_copy_us(uclass,&kclass,cbuf,sizeof(cbuf)); if (!NT_SUCCESS(st)) return st; pcl=&kclass; }
    HANDLE kh; ULONG kdisp=0;
    st = NtCreateKey(&kh, da, poa, ti, pcl, co, (udisp?&kdisp:NULL));
    if (NT_SUCCESS(st) || st==STATUS_OBJECT_NAME_COLLISION){
        if (copy_to_user(uh,&kh,sizeof(kh))) return STATUS_ACCESS_VIOLATION;
        if (udisp && copy_to_user(udisp,&kdisp,sizeof(kdisp))) return STATUS_ACCESS_VIOLATION;
    }
    return st;
}
NTSTATUS nt_svc_NtOpenKey(unsigned long a0, unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5, unsigned long a6, unsigned long a7, unsigned long a8, unsigned long a9, unsigned long a10, unsigned long a11)
{
    PHANDLE uh=(PHANDLE)a0; ACCESS_MASK da=(ACCESS_MASK)a1; POBJECT_ATTRIBUTES uoa=(POBJECT_ATTRIBUTES)a2;
    (void)a3;(void)a4;(void)a5;(void)a6;(void)a7;(void)a8;(void)a9;(void)a10;(void)a11;
    if (!uh || !access_ok(uh,sizeof(HANDLE))) return STATUS_ACCESS_VIOLATION;
    OBJECT_ATTRIBUTES koa; UNICODE_STRING kname; WCHAR kbuf[512]; POBJECT_ATTRIBUTES poa=NULL;
    NTSTATUS st=k_copy_oa(uoa,&koa,&kname,kbuf,sizeof(kbuf),&poa); if(!NT_SUCCESS(st)) return st;
    if (!poa||!poa->ObjectName) return STATUS_INVALID_PARAMETER;
    HANDLE kh; st=NtOpenKey(&kh,da,poa);
    if (NT_SUCCESS(st)){ if(copy_to_user(uh,&kh,sizeof(kh))) return STATUS_ACCESS_VIOLATION; }
    return st;
}
NTSTATUS nt_svc_NtOpenKeyEx(unsigned long a0, unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5, unsigned long a6, unsigned long a7, unsigned long a8, unsigned long a9, unsigned long a10, unsigned long a11)
{
    PHANDLE uh=(PHANDLE)a0; ACCESS_MASK da=(ACCESS_MASK)a1; POBJECT_ATTRIBUTES uoa=(POBJECT_ATTRIBUTES)a2; ULONG oo=(ULONG)a3;
    (void)a4;(void)a5;(void)a6;(void)a7;(void)a8;(void)a9;(void)a10;(void)a11;
    if (!uh || !access_ok(uh,sizeof(HANDLE))) return STATUS_ACCESS_VIOLATION;
    OBJECT_ATTRIBUTES koa; UNICODE_STRING kname; WCHAR kbuf[512]; POBJECT_ATTRIBUTES poa=NULL;
    NTSTATUS st=k_copy_oa(uoa,&koa,&kname,kbuf,sizeof(kbuf),&poa); if(!NT_SUCCESS(st)) return st;
    if (!poa||!poa->ObjectName) return STATUS_INVALID_PARAMETER;
    HANDLE kh; st=NtOpenKeyEx(&kh,da,poa,oo);
    if (NT_SUCCESS(st)){ if(copy_to_user(uh,&kh,sizeof(kh))) return STATUS_ACCESS_VIOLATION; }
    return st;
}
NTSTATUS nt_svc_NtDeleteKey(unsigned long a0, unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5, unsigned long a6, unsigned long a7, unsigned long a8, unsigned long a9, unsigned long a10, unsigned long a11)
{
    HANDLE h=(HANDLE)a0; (void)a1;(void)a2;(void)a3;(void)a4;(void)a5;(void)a6;(void)a7;(void)a8;(void)a9;(void)a10;(void)a11;
    return NtDeleteKey(h);
}
NTSTATUS nt_svc_NtFlushKey(unsigned long a0, unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5, unsigned long a6, unsigned long a7, unsigned long a8, unsigned long a9, unsigned long a10, unsigned long a11)
{
    HANDLE h=(HANDLE)a0; (void)a1;(void)a2;(void)a3;(void)a4;(void)a5;(void)a6;(void)a7;(void)a8;(void)a9;(void)a10;(void)a11;
    return NtFlushKey(h);
}
NTSTATUS nt_svc_NtEnumerateKey(unsigned long a0, unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5, unsigned long a6, unsigned long a7, unsigned long a8, unsigned long a9, unsigned long a10, unsigned long a11)
{
    HANDLE h=(HANDLE)a0; ULONG idx=(ULONG)a1; ULONG cls=(ULONG)a2; PVOID info=(PVOID)a3; ULONG len=(ULONG)a4; PULONG rlen=(PULONG)a5;
    (void)a6;(void)a7;(void)a8;(void)a9;(void)a10;(void)a11;
    if (rlen && !access_ok(rlen,sizeof(ULONG))) return STATUS_ACCESS_VIOLATION;
    if (info && len && !access_ok(info,len)) return STATUS_ACCESS_VIOLATION;
    char *kbuf=NULL; if(len){ kbuf=kzalloc(len,GFP_KERNEL); if(!kbuf) return STATUS_NO_MEMORY; }
    ULONG klen=0;
    NTSTATUS st=NtEnumerateKey(h,idx,cls,kbuf,len,&klen);
    if (rlen){ if(copy_to_user(rlen,&klen,sizeof(klen))) {kfree(kbuf); return STATUS_ACCESS_VIOLATION;}}
    if (NT_SUCCESS(st) && kbuf && copy_to_user(info,kbuf,len)) st=STATUS_ACCESS_VIOLATION;
    kfree(kbuf);
    return st;
}
NTSTATUS nt_svc_NtEnumerateValueKey(unsigned long a0, unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5, unsigned long a6, unsigned long a7, unsigned long a8, unsigned long a9, unsigned long a10, unsigned long a11)
{
    HANDLE h=(HANDLE)a0; ULONG idx=(ULONG)a1; ULONG cls=(ULONG)a2; PVOID info=(PVOID)a3; ULONG len=(ULONG)a4; PULONG rlen=(PULONG)a5;
    (void)a6;(void)a7;(void)a8;(void)a9;(void)a10;(void)a11;
    if (rlen && !access_ok(rlen,sizeof(ULONG))) return STATUS_ACCESS_VIOLATION;
    if (info && len && !access_ok(info,len)) return STATUS_ACCESS_VIOLATION;
    char *kbuf=NULL; if(len){ kbuf=kzalloc(len,GFP_KERNEL); if(!kbuf) return STATUS_NO_MEMORY; }
    ULONG klen=0;
    NTSTATUS st=NtEnumerateValueKey(h,idx,cls,kbuf,len,&klen);
    if (rlen){ if(copy_to_user(rlen,&klen,sizeof(klen))) {kfree(kbuf); return STATUS_ACCESS_VIOLATION;}}
    if (NT_SUCCESS(st) && kbuf && copy_to_user(info,kbuf,len)) st=STATUS_ACCESS_VIOLATION;
    kfree(kbuf);
    return st;
}
NTSTATUS nt_svc_NtQueryKey(unsigned long a0, unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5, unsigned long a6, unsigned long a7, unsigned long a8, unsigned long a9, unsigned long a10, unsigned long a11)
{
    HANDLE h=(HANDLE)a0; ULONG cls=(ULONG)a1; PVOID info=(PVOID)a2; ULONG len=(ULONG)a3; PULONG rlen=(PULONG)a4;
    (void)a5;(void)a6;(void)a7;(void)a8;(void)a9;(void)a10;(void)a11;
    if (rlen && !access_ok(rlen,sizeof(ULONG))) return STATUS_ACCESS_VIOLATION;
    if (info && len && !access_ok(info,len)) return STATUS_ACCESS_VIOLATION;
    char *kbuf=NULL; if(len){ kbuf=kzalloc(len,GFP_KERNEL); if(!kbuf) return STATUS_NO_MEMORY; }
    ULONG klen=0;
    NTSTATUS st=NtQueryKey(h,cls,kbuf,len,&klen);
    if (rlen){ if(copy_to_user(rlen,&klen,sizeof(klen))) {kfree(kbuf); return STATUS_ACCESS_VIOLATION;}}
    if (NT_SUCCESS(st) && kbuf && copy_to_user(info,kbuf,len)) st=STATUS_ACCESS_VIOLATION;
    else if (st==STATUS_BUFFER_OVERFLOW && rlen) { /* still need to copy? No */ }
    kfree(kbuf);
    return st;
}
NTSTATUS nt_svc_NtQueryValueKey(unsigned long a0, unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5, unsigned long a6, unsigned long a7, unsigned long a8, unsigned long a9, unsigned long a10, unsigned long a11)
{
    PUNICODE_STRING uvn=(PUNICODE_STRING)a1; ULONG cls=(ULONG)a2; PVOID info=(PVOID)a3; ULONG len=(ULONG)a4; PULONG rlen=(PULONG)a5;
    HANDLE h=(HANDLE)a0;
    (void)a6;(void)a7;(void)a8;(void)a9;(void)a10;(void)a11;
    if (rlen && !access_ok(rlen,sizeof(ULONG))) return STATUS_ACCESS_VIOLATION;
    if (info && len && !access_ok(info,len)) return STATUS_ACCESS_VIOLATION;
    UNICODE_STRING kname={0}; WCHAR kbuf[256];
    NTSTATUS st=k_copy_us(uvn,&kname,kbuf,sizeof(kbuf)); if(!NT_SUCCESS(st)) return st;
    char *kout=NULL; if(len){ kout=kzalloc(len,GFP_KERNEL); if(!kout) return STATUS_NO_MEMORY; }
    ULONG klen=0;
    st=NtQueryValueKey(h,&kname,cls,kout,len,&klen);
    if (rlen){ if(copy_to_user(rlen,&klen,sizeof(klen))) {kfree(kout); return STATUS_ACCESS_VIOLATION;}}
    if (NT_SUCCESS(st) && kout && copy_to_user(info,kout,len)) st=STATUS_ACCESS_VIOLATION;
    kfree(kout);
    return st;
}
NTSTATUS nt_svc_NtSetValueKey(unsigned long a0, unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5, unsigned long a6, unsigned long a7, unsigned long a8, unsigned long a9, unsigned long a10, unsigned long a11)
{
    HANDLE h=(HANDLE)a0; PUNICODE_STRING uvn=(PUNICODE_STRING)a1; ULONG ti=(ULONG)a2; ULONG type=(ULONG)a3; PVOID data=(PVOID)a4; ULONG dlen=(ULONG)a5;
    (void)a6;(void)a7;(void)a8;(void)a9;(void)a10;(void)a11;
    UNICODE_STRING kname={0}; WCHAR kbuf[256];
    NTSTATUS st=k_copy_us(uvn,&kname,kbuf,sizeof(kbuf)); if(!NT_SUCCESS(st)) return st;
    void *kdata=NULL;
    if (dlen){
        if (!access_ok(data,dlen)) return STATUS_ACCESS_VIOLATION;
        kdata=kmalloc(dlen,GFP_KERNEL); if(!kdata) return STATUS_NO_MEMORY;
        if(copy_from_user(kdata,data,dlen)){kfree(kdata); return STATUS_ACCESS_VIOLATION;}
    }
    st=NtSetValueKey(h,&kname,ti,type,kdata,dlen);
    kfree(kdata);
    return st;
}
NTSTATUS nt_svc_NtDeleteValueKey(unsigned long a0, unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5, unsigned long a6, unsigned long a7, unsigned long a8, unsigned long a9, unsigned long a10, unsigned long a11)
{
    HANDLE h=(HANDLE)a0; PUNICODE_STRING uvn=(PUNICODE_STRING)a1;
    (void)a2;(void)a3;(void)a4;(void)a5;(void)a6;(void)a7;(void)a8;(void)a9;(void)a10;(void)a11;
    UNICODE_STRING kname={0}; WCHAR kbuf[256];
    NTSTATUS st=k_copy_us(uvn,&kname,kbuf,sizeof(kbuf)); if(!NT_SUCCESS(st)) return st;
    return NtDeleteValueKey(h,&kname);
}
#endif
