/* ucrtbase_path_t.inc - _splitpath and _makepath with their _s forms, written once for char and wide characters.
 * Included twice by ucrtbase_fs.inc with UCT_T (the character type), UCT_P (the export prefix: w__ or w__w) and
 * UCT_LEN, UCT_CPY (the length and copy functions for UCT_T). */
#define UCT_CAT_(a, b) a##b
#define UCT_CAT(a, b) UCT_CAT_(a, b)
#define UCT_FN(name) UCT_CAT(UCT_P, name)

static int UCT_CAT(uct_sep_, UCT_T)(UCT_T c) { return c == '\\' || c == '/'; }

/* The pieces of a path: drive ("C:"), directory (up to and including the last separator), name and extension (from the last
 * dot of the name). Lengths are in characters. */
typedef struct UCT_CAT(uct_parts_, UCT_T) {
    const UCT_T *drive, *dir, *name, *ext;
    size_t drive_n, dir_n, name_n, ext_n;
} UCT_CAT(uct_parts_, UCT_T);

static void UCT_CAT(uct_split_, UCT_T)(const UCT_T *path, UCT_CAT(uct_parts_, UCT_T) *p)
{
    const UCT_T *s = path, *last_sep = NULL, *last_dot = NULL, *q;
    p->drive = s;
    p->drive_n = 0;
    if (s[0] && s[1] == ':') {
        p->drive_n = 2;
        s += 2;
    }
    p->dir = s;
    for (q = s; *q; ++q) {
        if (UCT_CAT(uct_sep_, UCT_T)(*q)) {
            last_sep = q;
            last_dot = NULL;
        } else if (*q == '.') {
            last_dot = q;
        }
    }
    p->dir_n = last_sep ? (size_t)(last_sep - s) + 1 : 0;
    p->name = s + p->dir_n;
    {
        const UCT_T *end = q;
        p->name_n = last_dot ? (size_t)(last_dot - p->name) : (size_t)(end - p->name);
        p->ext = last_dot ? last_dot : end;
        p->ext_n = last_dot ? (size_t)(end - last_dot) : 0;
    }
}

static void UCT_CAT(uct_put_, UCT_T)(UCT_T *dst, const UCT_T *src, size_t n)
{
    if (dst) {
        for (size_t i = 0; i < n; ++i)
            dst[i] = src[i];
        dst[n] = 0;
    }
}

void UCT_FN(splitpath)(const UCT_T *path, UCT_T *drive, UCT_T *dir, UCT_T *fname, UCT_T *ext)
{
    UCT_CAT(uct_parts_, UCT_T) p;
    UCT_CAT(uct_split_, UCT_T)(path, &p);
    UCT_CAT(uct_put_, UCT_T)(drive, p.drive, p.drive_n);
    UCT_CAT(uct_put_, UCT_T)(dir, p.dir, p.dir_n);
    UCT_CAT(uct_put_, UCT_T)(fname, p.name, p.name_n);
    UCT_CAT(uct_put_, UCT_T)(ext, p.ext, p.ext_n);
}

/* The _s form: a buffer and its size are both given or both absent; if any piece does not fit, all outputs are emptied. */
int UCT_FN(splitpath_s)(const UCT_T *path, UCT_T *drive, size_t drive_size, UCT_T *dir, size_t dir_size, UCT_T *fname,
                        size_t fname_size, UCT_T *ext, size_t ext_size)
{
    UCT_CAT(uct_parts_, UCT_T) p;
    if (!path || (drive != NULL) != (drive_size != 0) || (dir != NULL) != (dir_size != 0) || (fname != NULL) != (fname_size != 0) ||
        (ext != NULL) != (ext_size != 0)) {
        if (drive && drive_size)
            drive[0] = 0;
        if (dir && dir_size)
            dir[0] = 0;
        if (fname && fname_size)
            fname[0] = 0;
        if (ext && ext_size)
            ext[0] = 0;
        return ucrt_invalid(UCRT_EINVAL);
    }
    UCT_CAT(uct_split_, UCT_T)(path, &p);
    if ((drive && p.drive_n + 1 > drive_size) || (dir && p.dir_n + 1 > dir_size) || (fname && p.name_n + 1 > fname_size) ||
        (ext && p.ext_n + 1 > ext_size)) {
        if (drive)
            drive[0] = 0;
        if (dir)
            dir[0] = 0;
        if (fname)
            fname[0] = 0;
        if (ext)
            ext[0] = 0;
        return ucrt_invalid(UCRT_ERANGE);
    }
    UCT_CAT(uct_put_, UCT_T)(drive, p.drive, p.drive_n);
    UCT_CAT(uct_put_, UCT_T)(dir, p.dir, p.dir_n);
    UCT_CAT(uct_put_, UCT_T)(fname, p.name, p.name_n);
    UCT_CAT(uct_put_, UCT_T)(ext, p.ext, p.ext_n);
    return 0;
}

/* Composes a path into out (capacity cap, or unlimited when cap is -1); returns the characters needed without the terminator. */
static size_t UCT_CAT(uct_make_, UCT_T)(UCT_T *out, size_t cap, const UCT_T *drive, const UCT_T *dir, const UCT_T *fname, const UCT_T *ext)
{
    size_t n = 0;
#define UCT_OUT(ch) do { if (out && n < cap) out[n] = (UCT_T)(ch); ++n; } while (0)
    if (drive && drive[0]) {
        UCT_OUT(drive[0]);
        UCT_OUT(':');
    }
    if (dir && dir[0]) {
        size_t i = 0;
        for (; dir[i]; ++i)
            UCT_OUT(dir[i]);
        if (!UCT_CAT(uct_sep_, UCT_T)(dir[i - 1]))
            UCT_OUT('\\');
    }
    if (fname)
        for (size_t i = 0; fname[i]; ++i)
            UCT_OUT(fname[i]);
    if (ext && ext[0]) {
        if (ext[0] != '.')
            UCT_OUT('.');
        for (size_t i = 0; ext[i]; ++i)
            UCT_OUT(ext[i]);
    }
#undef UCT_OUT
    if (out && n < cap)
        out[n] = 0;
    return n;
}

void UCT_FN(makepath)(UCT_T *path, const UCT_T *drive, const UCT_T *dir, const UCT_T *fname, const UCT_T *ext)
{
    if (path)
        UCT_CAT(uct_make_, UCT_T)(path, (size_t)-1 >> 1, drive, dir, fname, ext);
}

int UCT_FN(makepath_s)(UCT_T *path, size_t size, const UCT_T *drive, const UCT_T *dir, const UCT_T *fname, const UCT_T *ext)
{
    size_t need;
    if (!path || !size)
        return ucrt_invalid(UCRT_EINVAL);
    need = UCT_CAT(uct_make_, UCT_T)(NULL, 0, drive, dir, fname, ext);
    if (need + 1 > size) {
        path[0] = 0;
        return ucrt_invalid(UCRT_ERANGE);
    }
    UCT_CAT(uct_make_, UCT_T)(path, size, drive, dir, fname, ext);
    return 0;
}

#undef UCT_FN
#undef UCT_CAT
#undef UCT_CAT_
