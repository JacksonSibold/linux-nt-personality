/*
 * test_k32_file_pure.c - host tests for src/ntdll/k32_file_pure.h: CreateFile
 * access, share, flag and attribute mapping, the create result and error
 * rules, Win32-to-NT path conversion (normalisation and the \??\ prefix),
 * GetFileSize splitting, SetFilePointer targets and GetFileAttributes results.
 */
#include "minitest.h"
#include "k32_file_pure.h"

#include <stddef.h>
#include <string.h>

/* Copies an ASCII string to WCHARs and terminates it. Returns the length, or
 * (size_t)-1 when it does not fit in cap. */
static size_t mkw(const char *s, WCHAR *out, size_t cap)
{
    size_t i = 0;
    for (; s[i]; ++i) {
        if (i + 1 >= cap) return (size_t)-1;
        out[i] = (WCHAR)(unsigned char)s[i];
    }
    if (i + 1 > cap) return (size_t)-1;
    out[i] = 0;
    return i;
}

/* Normalises in against cwd and copies the result to res as ASCII. Returns
 * the Win32 error (0 on success). */
static DWORD run_norm(const char *in, const char *cwd, char *res, size_t rescap)
{
    static WCHAR win[600], wcwd[600], wout[600];
    size_t inn = mkw(in, win, 600);
    size_t cwdn = mkw(cwd, wcwd, 600);
    size_t outn = 0;
    res[0] = 0;
    if (inn == (size_t)-1 || cwdn == (size_t)-1) return 0xFFFFFFFFu;
    DWORD err = k32_path_dos_normalise(win, inn, wcwd, cwdn, wout, 600, &outn);
    if (!err) {
        size_t i = 0;
        for (; i < outn && i + 1 < rescap; ++i) res[i] = (char)wout[i];
        res[i] = 0;
    }
    return err;
}

/* Same for the NT form: dos is an absolute DOS path, res receives the NT name. */
static size_t run_nt(const char *dos, char *res, size_t rescap)
{
    static WCHAR win[600], wout[600];
    size_t dn = mkw(dos, win, 600);
    size_t outn = k32_path_to_nt(win, dn, wout, 600);
    size_t i = 0;
    res[0] = 0;
    if (outn == 0) return 0;
    for (; i < outn && i + 1 < rescap; ++i) res[i] = (char)wout[i];
    res[i] = 0;
    return outn;
}

/* K32-19: the access mapping gives the file-specific NT rights. GENERIC_READ,
 * GENERIC_WRITE and GENERIC_EXECUTE expand to FILE_GENERIC_*, and every
 * request includes SYNCHRONIZE for synchronous I/O. */
static void access_maps_generic_rights(void)
{
    ULONG r = k32_file_access_to_nt(0x80000000u); /* GENERIC_READ */
    MT_EXPECT_EQ(r & 0x00120089u, 0x00120089u);
    MT_EXPECT(r & 0x00100000u);
    ULONG w = k32_file_access_to_nt(0x40000000u); /* GENERIC_WRITE */
    MT_EXPECT_EQ(w & 0x00120116u, 0x00120116u);
    ULONG x = k32_file_access_to_nt(0x20000000u); /* GENERIC_EXECUTE */
    MT_EXPECT_EQ(x & 0x001200A0u, 0x001200A0u);
    ULONG all = k32_file_access_to_nt(0x10000000u); /* GENERIC_ALL */
    MT_EXPECT_EQ(all & 0x001F01FFu, 0x001F01FFu);
}

static void access_keeps_specific_and_standard_rights(void)
{
    ULONG d = k32_file_access_to_nt(0x00010000u); /* DELETE */
    MT_EXPECT(d & 0x00010000u);
    ULONG spec = k32_file_access_to_nt(0x00000001u); /* FILE_READ_DATA */
    MT_EXPECT(spec & 0x1);
    MT_EXPECT(k32_file_access_to_nt(0) & 0x00100000u); /* SYNCHRONIZE even with no access */
}

/* Share modes are FILE_SHARE_READ 1, WRITE 2, DELETE 4, and pass through. */
static void share_mode_mapping(void)
{
    MT_EXPECT_EQ(k32_file_share_to_nt(7), 7);
    MT_EXPECT_EQ(k32_file_share_to_nt(0), 0);
    MT_EXPECT_EQ(k32_file_share_to_nt(1), 1);
}

/* Dispositions: 1 to 5 are valid; 0 and anything above 5 are not. */
static void disposition_validity(void)
{
    for (DWORD d = 1; d <= 5; ++d) MT_EXPECT(k32_disposition_valid(d));
    MT_EXPECT(!k32_disposition_valid(0));
    MT_EXPECT(!k32_disposition_valid(6));
    MT_EXPECT(!k32_disposition_valid(0xFFFFFFFFu));
}

/* Default open is a synchronous, non-directory open. BACKUP_SEMANTICS allows a
 * directory and sets FILE_OPEN_FOR_BACKUP_INTENT. OVERLAPPED drops the
 * synchronous flag. */
static void options_default_backup_and_overlapped(void)
{
    ULONG o = k32_file_options(0);
    MT_EXPECT(o & 0x40);   /* FILE_NON_DIRECTORY_FILE */
    MT_EXPECT(o & 0x20);   /* FILE_SYNCHRONOUS_IO_NONALERT */
    MT_EXPECT(!(o & 0x4000));
    ULONG b = k32_file_options(0x02000000u); /* BACKUP_SEMANTICS */
    MT_EXPECT(!(b & 0x40));
    MT_EXPECT(b & 0x4000);
    ULONG ov = k32_file_options(0x40000000u); /* OVERLAPPED */
    MT_EXPECT(!(ov & 0x20));
}

static void options_flag_bits(void)
{
    MT_EXPECT(k32_file_options(0x80000000u) & 0x2);    /* WRITE_THROUGH */
    MT_EXPECT(k32_file_options(0x20000000u) & 0x8);    /* NO_BUFFERING */
    MT_EXPECT(k32_file_options(0x04000000u) & 0x1000); /* DELETE_ON_CLOSE */
    MT_EXPECT(k32_file_options(0x00200000u) & 0x200000); /* OPEN_REPARSE_POINT */
    ULONG both = k32_file_options(0x10000000u | 0x08000000u); /* RANDOM and SEQUENTIAL */
    MT_EXPECT(both & 0x800);
    MT_EXPECT(!(both & 0x4));
}

/* FILE_ATTRIBUTE bits are kept; FILE_ATTRIBUTE_NORMAL (0x80) is dropped when
 * another attribute is set. The flag bits in the high word are not attributes. */
static void attributes_mapping(void)
{
    MT_EXPECT_EQ(k32_file_attributes_to_nt(0x80), 0x80);
    MT_EXPECT_EQ(k32_file_attributes_to_nt(0x81), 0x01);
    MT_EXPECT_EQ(k32_file_attributes_to_nt(0x21), 0x21);
    MT_EXPECT_EQ(k32_file_attributes_to_nt(0x40000000u | 0x20), 0x20);
}

/* A successful CREATE_ALWAYS or OPEN_ALWAYS that found the file reports
 * ERROR_ALREADY_EXISTS (183). Creating a new file, or opening with
 * OPEN_EXISTING, reports no error. */
static void create_success_already_exists(void)
{
    MT_EXPECT_EQ(k32_create_success_error(2, 3), 183); /* CREATE_ALWAYS, overwritten */
    MT_EXPECT_EQ(k32_create_success_error(2, 2), 0);   /* CREATE_ALWAYS, created */
    MT_EXPECT_EQ(k32_create_success_error(4, 1), 183); /* OPEN_ALWAYS, opened */
    MT_EXPECT_EQ(k32_create_success_error(4, 2), 0);   /* OPEN_ALWAYS, created */
    MT_EXPECT_EQ(k32_create_success_error(3, 1), 0);   /* OPEN_EXISTING */
    MT_EXPECT_EQ(k32_create_success_error(1, 2), 0);   /* CREATE_NEW */
}

/* CREATE_NEW on an existing file is ERROR_FILE_EXISTS (80), not the
 * ERROR_ALREADY_EXISTS (183) that the default table gives. */
static void create_collision_override(void)
{
    MT_EXPECT_EQ(k32_create_error_override(1, (NTSTATUS)0xC0000035L), 80);
    MT_EXPECT_EQ(k32_create_error_override(2, (NTSTATUS)0xC0000035L), 0);
    MT_EXPECT_EQ(k32_create_error_override(1, (NTSTATUS)0xC0000034L), 0);
}

/* A DOS path with a drive and absolute components normalises: '/' becomes '\',
 * '.' and empty segments drop, '..' pops one segment (never past the root). */
static void normalise_absolute_paths(void)
{
    char out[300];
    MT_EXPECT_EQ(run_norm("C:\\a\\..\\b", "D:\\x", out, sizeof out), 0);
    MT_EXPECT(strcmp(out, "C:\\b") == 0);
    MT_EXPECT_EQ(run_norm("C:/a//./b", "D:\\x", out, sizeof out), 0);
    MT_EXPECT(strcmp(out, "C:\\a\\b") == 0);
    MT_EXPECT_EQ(run_norm("C:\\..\\..\\x", "D:\\x", out, sizeof out), 0);
    MT_EXPECT(strcmp(out, "C:\\x") == 0);
}

/* Relative names join the current directory; a rooted name without a drive
 * takes the current drive; drive-relative takes the current directory only
 * when it is on the same drive. */
static void normalise_relative_paths(void)
{
    char out[300];
    MT_EXPECT_EQ(run_norm("a/b", "C:\\x\\", out, sizeof out), 0);
    MT_EXPECT(strcmp(out, "C:\\x\\a\\b") == 0);
    MT_EXPECT_EQ(run_norm("\\foo", "D:\\x", out, sizeof out), 0);
    MT_EXPECT(strcmp(out, "D:\\foo") == 0);
    MT_EXPECT_EQ(run_norm("C:foo", "C:\\x", out, sizeof out), 0);
    MT_EXPECT(strcmp(out, "C:\\x\\foo") == 0);
    MT_EXPECT_EQ(run_norm("C:foo", "D:\\x", out, sizeof out), 0);
    MT_EXPECT(strcmp(out, "C:\\foo") == 0);
}

/* Win32 drops trailing spaces and dots from each segment (not '.' or '..'). */
static void normalise_trailing_spaces_and_dots(void)
{
    char out[300];
    MT_EXPECT_EQ(run_norm("C:\\a. \\b ", "D:\\x", out, sizeof out), 0);
    MT_EXPECT(strcmp(out, "C:\\a\\b") == 0);
}

/* UNC: the server and share are required; the path keeps the \\server\share
 * root. An extended prefix is kept as given. */
static void normalise_unc_and_extended(void)
{
    char out[300];
    MT_EXPECT_EQ(run_norm("\\\\srv\\sh\\a\\b", "C:\\x", out, sizeof out), 0);
    MT_EXPECT(strcmp(out, "\\\\srv\\sh\\a\\b") == 0);
    MT_EXPECT_EQ(run_norm("\\\\srv", "C:\\x", out, sizeof out), 123); /* ERROR_INVALID_NAME */
    MT_EXPECT_EQ(run_norm("\\\\?\\C:\\a/b..\\c", "C:\\x", out, sizeof out), 0);
    MT_EXPECT(strcmp(out, "\\\\?\\C:\\a/b..\\c") == 0); /* not normalised */
}

/* Empty names are ERROR_PATH_NOT_FOUND (3). Wildcards and control characters
 * are ERROR_INVALID_NAME (123). */
static void normalise_errors(void)
{
    char out[300];
    MT_EXPECT_EQ(run_norm("", "C:\\x", out, sizeof out), 3);
    MT_EXPECT_EQ(run_norm("C:\\a*b", "C:\\x", out, sizeof out), 123);
    MT_EXPECT_EQ(run_norm("C:\\a<b", "C:\\x", out, sizeof out), 123);
    MT_EXPECT_EQ(run_norm("C:\\a\x01" "b", "C:\\x", out, sizeof out), 123);
}

/* A relative name with no current directory has no base: ERROR_PATH_NOT_FOUND. */
static void normalise_relative_without_cwd(void)
{
    char out[300];
    MT_EXPECT_EQ(run_norm("a\\b", "", out, sizeof out), 3);
}

/* Output that does not fit the caller's buffer is ERROR_FILENAME_EXCED_RANGE
 * (206). The 32,767-character limit is the Win32 long-path maximum. */
static void normalise_length_limit(void)
{
    static WCHAR in[600], wout[16];
    WCHAR wcwd[8];
    size_t outn = 0;
    size_t n = 0;
    in[n++] = 'C'; in[n++] = ':'; in[n++] = '\\';
    for (int i = 0; i < 100; ++i) in[n++] = 'a';
    size_t cwdn = mkw("C:\\", wcwd, 8);
    DWORD err = k32_path_dos_normalise(in, n, wcwd, cwdn, wout, 16, &outn);
    MT_EXPECT_EQ(err, 206);
}

/* The NT form adds the \??\ object prefix; UNC and extended names get their
 * own prefixes. */
static void nt_prefix_forms(void)
{
    char out[300];
    MT_EXPECT_EQ(run_nt("C:\\a", out, sizeof out), 8);
    MT_EXPECT(strcmp(out, "\\??\\C:\\a") == 0);
    MT_EXPECT(run_nt("\\\\srv\\sh\\a", out, sizeof out) > 0);
    MT_EXPECT(strcmp(out, "\\??\\UNC\\srv\\sh\\a") == 0);
    MT_EXPECT(run_nt("\\\\?\\C:\\a", out, sizeof out) > 0);
    MT_EXPECT(strcmp(out, "\\??\\C:\\a") == 0);
    MT_EXPECT(run_nt("\\\\.\\CON", out, sizeof out) > 0);
    MT_EXPECT(strcmp(out, "\\??\\CON") == 0);
}

/* The NT name must fit a USHORT byte length and the caller's buffer. */
static void nt_path_capacity(void)
{
    static WCHAR win[8], wout[4];
    size_t dn = mkw("C:\\a", win, 8);
    MT_EXPECT_EQ(k32_path_to_nt(win, dn, wout, 4), 0);
}

/* GetFileSize: the low DWORD is returned, the high DWORD goes to lpFileSizeHigh. */
static void file_size_split(void)
{
    DWORD lo = 0, hi = 0;
    k32_file_size_split((LONGLONG)0x100000010LL, &lo, &hi);
    MT_EXPECT_EQ(lo, 0x10u);
    MT_EXPECT_EQ(hi, 1u);
    k32_file_size_split(42, &lo, &hi);
    MT_EXPECT_EQ(lo, 42u);
    MT_EXPECT_EQ(hi, 0u);
}

/* SetFilePointer targets: FILE_BEGIN (0), FILE_CURRENT (1), FILE_END (2).
 * A negative target is ERROR_NEGATIVE_SEEK (131); an unknown method is
 * ERROR_INVALID_PARAMETER (87). */
static void seek_target_rules(void)
{
    LONGLONG t = -1;
    MT_EXPECT_EQ(k32_seek_target(0, 0, 100, 5, &t), 0);
    MT_EXPECT(t == 5);
    MT_EXPECT_EQ(k32_seek_target(1, 10, 100, -3, &t), 0);
    MT_EXPECT(t == 7);
    MT_EXPECT_EQ(k32_seek_target(2, 0, 100, -1, &t), 0);
    MT_EXPECT(t == 99);
    MT_EXPECT_EQ(k32_seek_target(0, 0, 0, -1, &t), 131);
    MT_EXPECT_EQ(k32_seek_target(7, 0, 0, 0, &t), 87);
}

/* GetFileAttributes: the NT attributes are masked to the defined bits, and a
 * file with none reports FILE_ATTRIBUTE_NORMAL. */
static void attributes_from_nt_result(void)
{
    MT_EXPECT_EQ(k32_attributes_from_nt(0), 0x80u);
    MT_EXPECT_EQ(k32_attributes_from_nt(0x10), 0x10u);       /* DIRECTORY */
    MT_EXPECT_EQ(k32_attributes_from_nt(0xFFFF0021u), 0x21u); /* high bits masked */
}

static const mt_case_t cases[] = {
    { "access_maps_generic_rights", access_maps_generic_rights },
    { "access_keeps_specific_and_standard_rights", access_keeps_specific_and_standard_rights },
    { "share_mode_mapping", share_mode_mapping },
    { "disposition_validity", disposition_validity },
    { "options_default_backup_and_overlapped", options_default_backup_and_overlapped },
    { "options_flag_bits", options_flag_bits },
    { "attributes_mapping", attributes_mapping },
    { "create_success_already_exists", create_success_already_exists },
    { "create_collision_override", create_collision_override },
    { "normalise_absolute_paths", normalise_absolute_paths },
    { "normalise_relative_paths", normalise_relative_paths },
    { "normalise_trailing_spaces_and_dots", normalise_trailing_spaces_and_dots },
    { "normalise_unc_and_extended", normalise_unc_and_extended },
    { "normalise_errors", normalise_errors },
    { "normalise_relative_without_cwd", normalise_relative_without_cwd },
    { "normalise_length_limit", normalise_length_limit },
    { "nt_prefix_forms", nt_prefix_forms },
    { "nt_path_capacity", nt_path_capacity },
    { "file_size_split", file_size_split },
    { "seek_target_rules", seek_target_rules },
    { "attributes_from_nt_result", attributes_from_nt_result },
};

int main(void)
{
    return MT_RUN("k32_file_pure", cases);
}
