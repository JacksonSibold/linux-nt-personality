#!/usr/bin/env python3
"""Generates the export wrappers and module definition of ucrtbase.dll.

Each entry of the tables below is one exported function: the Windows name, the C return type, the parameter types, and
the C function of src/crt that implements it. The generator writes
  src/ntdll/ucrtbase_gen.inc   one wrapper `w_<name>` per entry (included by ucrtbase.c)
  src/ntdll/ucrtbase.def       the export list, mapping each Windows name to its wrapper
Entries with the implementation "@manual" have their wrapper written by hand in ucrtbase.c; they are listed here so the
export list stays in one place. Run: python3 build/gen_ucrtbase.py
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
OUT_INC = ROOT / "src/ntdll/ucrtbase_gen.inc"
OUT_DEF = ROOT / "src/ntdll/ucrtbase.def"

W = "ucrt_wchar"
F = "ucrt_file"
CS = "const char *"
S = "char *"
CW = "const ucrt_wchar *"
WS = "ucrt_wchar *"
VP = "void *"
CVP = "const void *"
SZ = "size_t"
I = "int"
U = "unsigned"
L = "long"
UL = "unsigned long"
LL = "long long"
ULL = "unsigned long long"
D = "double"
FL = "float"

entries = []  # (name, ret, [params], impl)


def add(name, ret, params, impl):
    entries.append((name, ret, params, impl))


def manual(*names):
    for n in names:
        entries.append((n, None, None, "@manual"))


# ---- memory and strings ------------------------------------------------------------------------------------------
add("memcpy", VP, [VP, CVP, SZ], "ucrt_memcpy")
add("memmove", VP, [VP, CVP, SZ], "ucrt_memmove")
add("memset", VP, [VP, I, SZ], "ucrt_memset")
add("memcmp", I, [CVP, CVP, SZ], "ucrt_memcmp")
add("memchr", VP, [CVP, I, SZ], "ucrt_memchr")
add("_memicmp", I, [CVP, CVP, SZ], "ucrt_memicmp")
add("_memccpy", VP, [VP, CVP, I, SZ], "ucrt_memccpy")
add("memcpy_s", I, [VP, SZ, CVP, SZ], "ucrt_memcpy_s")
add("memmove_s", I, [VP, SZ, CVP, SZ], "ucrt_memmove_s")
for n, r, p in [
    ("strlen", SZ, [CS]), ("strnlen", SZ, [CS, SZ]), ("strcpy", S, [S, CS]), ("strncpy", S, [S, CS, SZ]), ("strcat", S, [S, CS]),
    ("strncat", S, [S, CS, SZ]), ("strcmp", I, [CS, CS]), ("strncmp", I, [CS, CS, SZ]), ("strchr", S, [CS, I]), ("strrchr", S, [CS, I]),
    ("strstr", S, [CS, CS]), ("strspn", SZ, [CS, CS]), ("strcspn", SZ, [CS, CS]), ("strpbrk", S, [CS, CS]),
]:
    add(n, r, p, "ucrt_" + n)
add("_stricmp", I, [CS, CS], "ucrt_stricmp")
add("_strcmpi", I, [CS, CS], "ucrt_stricmp")
add("_strnicmp", I, [CS, CS, SZ], "ucrt_strnicmp")
add("_strlwr", S, [S], "ucrt_strlwr")
add("_strupr", S, [S], "ucrt_strupr")
add("_strrev", S, [S], "ucrt_strrev")
add("_strset", S, [S, I], "ucrt_strset")
add("_strnset", S, [S, I, SZ], "ucrt_strnset")
add("strcpy_s", I, [S, SZ, CS], "ucrt_strcpy_s")
add("strcat_s", I, [S, SZ, CS], "ucrt_strcat_s")
add("strncpy_s", I, [S, SZ, CS, SZ], "ucrt_strncpy_s")
add("strncat_s", I, [S, SZ, CS, SZ], "ucrt_strncat_s")
add("strtok_s", S, [S, CS, "char **"], "ucrt_strtok_r")
add("_strtok_s_l", S, [S, CS, "char **", VP], "ucrt_strtok_r_l")
manual("strtok", "_strdup", "strerror", "_strerror", "strerror_s", "_wcserror", "__strncnt", "_mbscmp")
for n, r, p in [
    ("wcslen", SZ, [CW]), ("wcsnlen", SZ, [CW, SZ]), ("wcscpy", WS, [WS, CW]), ("wcsncpy", WS, [WS, CW, SZ]), ("wcscat", WS, [WS, CW]),
    ("wcsncat", WS, [WS, CW, SZ]), ("wcscmp", I, [CW, CW]), ("wcsncmp", I, [CW, CW, SZ]), ("wcschr", WS, [CW, W]), ("wcsrchr", WS, [CW, W]),
    ("wcsstr", WS, [CW, CW]), ("wcsspn", SZ, [CW, CW]), ("wcscspn", SZ, [CW, CW]), ("wcspbrk", WS, [CW, CW]),
]:
    add(n, r, p, "ucrt_" + n)
add("_wcsicmp", I, [CW, CW], "ucrt_wcsicmp")
add("_wcsnicmp", I, [CW, CW, SZ], "ucrt_wcsnicmp")
add("_wcslwr", WS, [WS], "ucrt_wcslwr")
add("_wcsupr", WS, [WS], "ucrt_wcsupr")
add("_wcsrev", WS, [WS], "ucrt_wcsrev")
add("_wcsset", WS, [WS, W], "ucrt_wcsset")
add("_wcsnset", WS, [WS, W, SZ], "ucrt_wcsnset")
add("wcscpy_s", I, [WS, SZ, CW], "ucrt_wcscpy_s")
add("wcscat_s", I, [WS, SZ, CW], "ucrt_wcscat_s")
add("wcsncpy_s", I, [WS, SZ, CW, SZ], "ucrt_wcsncpy_s")
add("wcsncat_s", I, [WS, SZ, CW, SZ], "ucrt_wcsncat_s")
add("wcstok_s", WS, [WS, CW, "ucrt_wchar **"], "ucrt_wcstok_r")
manual("wcstok", "_wcsdup", "wmemcpy", "wmemmove", "wmemset", "wmemcmp", "wmemchr", "wmemcpy_s", "wmemmove_s")

# ---- character classes -----------------------------------------------------------------------------------------------
for n in ["alnum", "alpha", "blank", "cntrl", "graph", "lower", "print", "punct", "space", "upper"]:
    add("is" + n, I, [I], "ucrt_loc_is" + n)
add("isdigit", I, [I], "ucrt_isdigit")
add("isxdigit", I, [I], "ucrt_isxdigit")
add("__isascii", I, [I], "ucrt_isascii")
add("__toascii", I, [I], "ucrt_toascii")
add("toupper", I, [I], "ucrt_loc_toupper")
add("tolower", I, [I], "ucrt_loc_tolower")
add("_toupper", I, [I], "ucrt_loc_toupper")
add("_tolower", I, [I], "ucrt_loc_tolower")
for n in ["alnum", "alpha", "blank", "cntrl", "digit", "graph", "lower", "print", "punct", "space", "upper", "xdigit"]:
    add("isw" + n, I, [W], "ucrt_isw" + n)
add("towupper", W, [W], "ucrt_towupper")
add("towlower", W, [W], "ucrt_towlower")
manual("_isctype", "iswctype", "_iswctype", "__pctype_func", "__pwctype_func", "___mb_cur_max_func", "___lc_codepage_func",
       "___lc_locale_name_func", "__p___mb_cur_max", "_isleadbyte", "___lc_handle_func", "_get_current_locale", "_create_locale",
       "_free_locale", "__acrt_locale_get_ctype_array_value")

# ---- conversions ----------------------------------------------------------------------------------------------------------
add("atoi", I, [CS], "ucrt_atoi")
add("atol", L, [CS], "ucrt_atoi")
add("atoll", LL, [CS], "ucrt_atoi64")
add("_atoi64", LL, [CS], "ucrt_atoi64")
add("atof", D, [CS], "ucrt_atof")
add("strtol", L, [CS, "char **", I], "ucrt_strtol_x")
add("strtoul", UL, [CS, "char **", I], "ucrt_strtoul_x")
add("strtoll", LL, [CS, "char **", I], "ucrt_strtoi64_x")
add("_strtoi64", LL, [CS, "char **", I], "ucrt_strtoi64_x")
add("strtoull", ULL, [CS, "char **", I], "ucrt_strtoui64_x")
add("_strtoui64", ULL, [CS, "char **", I], "ucrt_strtoui64_x")
add("strtod", D, [CS, "char **"], "ucrt_strtod_x")
add("strtof", FL, [CS, "char **"], "ucrt_strtof_x")
add("_wtoi", I, [CW], "ucrt_wcsatoi")
add("_wtol", L, [CW], "ucrt_wcsatoi")
add("_wtoi64", LL, [CW], "ucrt_wcsatoi64")
add("wcstol", L, [CW, "ucrt_wchar **", I], "ucrt_wcstol_x")
add("wcstoul", UL, [CW, "ucrt_wchar **", I], "ucrt_wcstoul_x")
add("wcstoll", LL, [CW, "ucrt_wchar **", I], "ucrt_wcstoi64_x")
add("_wcstoi64", LL, [CW, "ucrt_wchar **", I], "ucrt_wcstoi64_x")
add("wcstoull", ULL, [CW, "ucrt_wchar **", I], "ucrt_wcstoui64_x")
add("_wcstoui64", ULL, [CW, "ucrt_wchar **", I], "ucrt_wcstoui64_x")
add("wcstod", D, [CW, "ucrt_wchar **"], "ucrt_wcstod_x")
add("wcstof", FL, [CW, "ucrt_wchar **"], "ucrt_wcstof_x")
manual("_wtof", "_itoa", "_ltoa", "_ultoa", "_i64toa", "_ui64toa", "_itoa_s", "_ltoa_s", "_ultoa_s", "_i64toa_s", "_ui64toa_s",
       "_itow", "_ltow", "_ultow", "_i64tow", "_ui64tow", "_itow_s", "_ltow_s", "_ultow_s", "_i64tow_s", "_ui64tow_s",
       "_fcvt", "_ecvt", "_gcvt", "_fcvt_s", "_ecvt_s", "_gcvt_s")

# ---- stdlib ----------------------------------------------------------------------------------------------------------------
add("abs", I, [I], "ucrt_abs")
add("labs", L, [L], "ucrt_labs")
add("llabs", LL, [LL], "ucrt_llabs")
add("_abs64", LL, [LL], "ucrt_llabs")
add("div", "ucrt_div_t", [I, I], "ucrt_div")
add("ldiv", "ucrt_ldiv_t", [L, L], "ucrt_ldiv")
add("lldiv", "ucrt_lldiv_t", [LL, LL], "ucrt_lldiv")
add("rand", I, [], "ucrt_rand")
add("srand", None, [U], "ucrt_srand")
add("_byteswap_ushort", "unsigned short", ["unsigned short"], "ucrt_byteswap_ushort")
add("_byteswap_ulong", UL, [UL], "ucrt_byteswap_ulong")
add("_byteswap_uint64", ULL, [ULL], "ucrt_byteswap_uint64")
add("_swab", None, [S, S, I], "ucrt_swab")
add("qsort", None, [VP, SZ, SZ, "ucrt_cmp_fn"], "ucrt_qsort")
add("qsort_s", None, [VP, SZ, SZ, "ucrt_cmp_s_fn", VP], "ucrt_qsort_s")
add("bsearch", VP, [CVP, CVP, SZ, SZ, "ucrt_cmp_fn"], "ucrt_bsearch")
add("bsearch_s", VP, [CVP, CVP, SZ, SZ, "ucrt_cmp_s_fn", VP], "ucrt_bsearch_s")
add("_lfind", VP, [CVP, CVP, U + " *", U, "ucrt_cmp_fn"], "ucrt_lfind")
add("_lsearch", VP, [CVP, VP, U + " *", U, "ucrt_cmp_fn"], "ucrt_lsearch")
manual("rand_s", "_rotl", "_rotr", "_rotl64", "_rotr64", "_lrotl", "_lrotr", "_rotl8", "_rotr8", "_rotl16", "_rotr16", "_set_errno",
       "_get_errno", "_errno", "__doserrno", "_get_doserrno", "_set_doserrno")

# ---- math (the extended-precision ones through their guarded entry points) ---------------------------------------------------
for n in ["sin", "cos", "tan", "asin", "acos", "atan", "sinh", "cosh", "tanh", "asinh", "acosh", "atanh", "exp", "exp2", "expm1", "log",
          "log2", "log10", "log1p", "cbrt", "erf", "erfc", "tgamma", "lgamma"]:
    add(n, D, [D], "ucrt_%s_g" % n)
    add(n + "f", FL, [FL], "ucrt_%sf_g" % n)
for n in ["pow", "atan2", "hypot"]:
    add(n, D, [D, D], "ucrt_%s_g" % n)
    add(n + "f", FL, [FL, FL], "ucrt_%sf_g" % n)
add("_hypot", D, [D, D], "ucrt_hypot_g")
add("sqrt", D, [D], "ucrt_sqrt")
add("sqrtf", FL, [FL], "ucrt_sqrtf")
for n in ["fabs", "floor", "ceil", "trunc", "round", "rint", "logb"]:
    add(n, D, [D], "ucrt_" + n)
    add(n + "f", FL, [FL], "ucrt_%sf" % n)
add("nearbyint", D, [D], "ucrt_rint")
add("nearbyintf", FL, [FL], "ucrt_rintf")
for n in ["fmod", "remainder", "copysign", "fmin", "fmax", "fdim", "nextafter"]:
    add(n, D, [D, D], "ucrt_" + n)
for n in ["fmod", "remainder", "copysign", "fmin", "fmax", "fdim"]:
    add(n + "f", FL, [FL, FL], "ucrt_%sf" % n)
add("_copysign", D, [D, D], "ucrt_copysign")
add("_nextafter", D, [D, D], "ucrt_nextafter")
add("_logb", D, [D], "ucrt_logb")
add("fma", D, [D, D, D], "ucrt_fma")
add("fmaf", FL, [FL, FL, FL], "ucrt_fmaf")
add("ldexp", D, [D, I], "ucrt_ldexp")
add("ldexpf", FL, [FL, I], "ucrt_ldexpf")
add("scalbn", D, [D, I], "ucrt_scalbn")
add("scalbln", D, [D, L], "ucrt_scalbn")
add("_scalb", D, [D, L], "ucrt_scalbn")
add("frexp", D, [D, I + " *"], "ucrt_frexp")
add("frexpf", FL, [FL, I + " *"], "ucrt_frexpf")
add("modf", D, [D, D + " *"], "ucrt_modf")
add("modff", FL, [FL, FL + " *"], "ucrt_modff")
add("ilogb", I, [D], "ucrt_ilogb")
add("lrint", L, [D], "ucrt_lrint")
add("llrint", LL, [D], "ucrt_llrint")
add("lround", L, [D], "ucrt_lround")
add("llround", LL, [D], "ucrt_llround")
add("nan", D, [CS], "ucrt_nan")
add("_finite", I, [D], "ucrt_isfinite")
add("_isnan", I, [D], "ucrt_isnan")
add("_chgsign", D, [D], "ucrt_chgsign")
manual("_fpclass", "_dclass", "_fdclass", "_dsign", "_fdsign", "_controlfp_s", "_controlfp", "_control87", "_clearfp", "_statusfp", "_fpreset",
       "__setusermatherr", "_set_FMA3_enable", "_get_FMA3_enable")

# ---- stdio ---------------------------------------------------------------------------------------------------------------------
add("fopen", F + " *", [CS, CS], "ucrt_fopen")
add("_wfopen", F + " *", [CW, CW], "ucrt_wfopen")
add("_fsopen", F + " *", [CS, CS, I], "ucrt_fsopen")
add("_wfsopen", F + " *", [CW, CS, I], "ucrt_wfsopen_w")
add("fopen_s", I, [F + " **", CS, CS], "ucrt_fopen_s")
add("_fdopen", F + " *", [I, CS], "ucrt_fdopen")
add("freopen", F + " *", [CS, CS, F + " *"], "ucrt_freopen")
add("fclose", I, [F + " *"], "ucrt_fclose")
add("_fcloseall", I, [], "ucrt_fcloseall")
add("fflush", I, [F + " *"], "ucrt_fflush")
add("_flushall", I, [], "ucrt_flushall")
add("fread", SZ, [VP, SZ, SZ, F + " *"], "ucrt_fread")
add("fwrite", SZ, [CVP, SZ, SZ, F + " *"], "ucrt_fwrite")
add("fgetc", I, [F + " *"], "ucrt_fgetc")
add("getc", I, [F + " *"], "ucrt_fgetc")
add("fputc", I, [I, F + " *"], "ucrt_fputc")
add("putc", I, [I, F + " *"], "ucrt_fputc")
add("ungetc", I, [I, F + " *"], "ucrt_ungetc")
add("fgets", S, [S, I, F + " *"], "ucrt_fgets")
add("fputs", I, [CS, F + " *"], "ucrt_fputs")
add("puts", I, [CS], "ucrt_puts")
add("getchar", I, [], "ucrt_getchar")
add("putchar", I, [I], "ucrt_putchar")
add("gets_s", S, [S, SZ], "ucrt_gets_s")
add("fseek", I, [F + " *", L, I], "ucrt_fseek")
add("_fseeki64", I, [F + " *", LL, I], "ucrt_fseeki64")
add("ftell", L, [F + " *"], "ucrt_ftell")
add("_ftelli64", LL, [F + " *"], "ucrt_ftelli64")
add("fgetpos", I, [F + " *", "long long *"], "ucrt_fgetpos")
add("fsetpos", I, [F + " *", "const long long *"], "ucrt_fsetpos")
add("rewind", None, [F + " *"], "ucrt_rewind")
add("clearerr", None, [F + " *"], "ucrt_clearerr")
add("feof", I, [F + " *"], "ucrt_feof")
add("ferror", I, [F + " *"], "ucrt_ferror")
add("_fileno", I, [F + " *"], "ucrt_fileno")
add("setvbuf", I, [F + " *", S, I, SZ], "ucrt_setvbuf")
add("setbuf", None, [F + " *", S], "ucrt_setbuf")
add("remove", I, [CS], "ucrt_unlink")
add("_wremove", I, [CW], "ucrt_wunlink")
add("rename", I, [CS, CS], "ucrt_rename")
add("_wrename", I, [CW, CW], "ucrt_wrename")
add("__acrt_iob_func", F + " *", [U], "ucrt_iob_func")
add("__iob_func", F + " *", [], "ucrt_iob_base")
add("fgetwc", I, [F + " *"], "ucrt_fgetwc")
add("getwc", I, [F + " *"], "ucrt_fgetwc")
add("fputwc", I, [W, F + " *"], "ucrt_fputwc")
add("putwc", I, [W, F + " *"], "ucrt_fputwc")
add("ungetwc", I, [W, F + " *"], "ucrt_ungetwc")
add("fputws", I, [CW, F + " *"], "ucrt_fputws")
add("fgetws", WS, [WS, I, F + " *"], "ucrt_fgetws")
add("getwchar", I, [], "ucrt_getwchar")
add("putwchar", I, [W], "ucrt_putwchar")
manual("__stdio_common_vfprintf", "__stdio_common_vfprintf_s", "__stdio_common_vfprintf_p", "__stdio_common_vsprintf", "__stdio_common_vsprintf_s",
       "__stdio_common_vsprintf_p", "__stdio_common_vsnprintf_s", "__stdio_common_vfwprintf", "__stdio_common_vfwprintf_s",
       "__stdio_common_vswprintf", "__stdio_common_vswprintf_s", "__stdio_common_vsnwprintf_s", "__stdio_common_vfscanf",
       "__stdio_common_vsscanf", "__stdio_common_vfwscanf", "__stdio_common_vswscanf", "printf", "fprintf", "sprintf", "snprintf",
       "_snprintf", "vprintf", "vfprintf", "vsprintf", "vsnprintf", "_vsnprintf", "swprintf", "_snwprintf", "wprintf", "fwprintf",
       "vfwprintf", "vswprintf", "_vsnwprintf", "sscanf", "fscanf", "scanf", "swscanf", "fwscanf", "wscanf", "sprintf_s", "printf_s",
       "fprintf_s", "sscanf_s", "perror", "_set_printf_count_output", "_get_printf_count_output", "_lock_file", "_unlock_file",
       "_fgetwchar", "_fputwchar", "_getw", "_putw", "tmpfile", "tmpnam", "_tempnam", "_wtempnam", "_vscprintf", "_scprintf", "_vscwprintf",
       "_scwprintf", "_vsprintf_p", "_getch", "_getche", "_putch", "_kbhit", "_ungetch")

# ---- low-level I/O ----------------------------------------------------------------------------------------------------------------
add("_open", I, [CS, I, I], "ucrt_open")
add("_wopen", I, [CW, I, I], "ucrt_wopen")
add("_creat", I, [CS, I], "ucrt_creat")
add("_close", I, [I], "ucrt_close")
add("_read", I, [I, VP, U], "ucrt_read")
add("_write", I, [I, CVP, U], "ucrt_write")
add("_lseek", L, [I, L, I], "ucrt_lseek")
add("_lseeki64", LL, [I, LL, I], "ucrt_lseeki64")
add("_tell", L, [I], "ucrt_tell")
add("_telli64", LL, [I], "ucrt_telli64")
add("_eof", I, [I], "ucrt_eof")
add("_dup", I, [I], "ucrt_dup")
add("_dup2", I, [I, I], "ucrt_dup2")
add("_isatty", I, [I], "ucrt_isatty")
add("_setmode", I, [I, I], "ucrt_setmode")
add("_filelength", L, [I], "ucrt_filelength")
add("_filelengthi64", LL, [I], "ucrt_filelengthi64")
add("_chsize", I, [I, L], "ucrt_chsize")
add("_chsize_s", I, [I, LL], "ucrt_chsize_s")
add("_commit", I, [I], "ucrt_commit")
add("_get_osfhandle", "intptr_t", [I], "ucrt_get_osfhandle_i")
add("_open_osfhandle", I, ["intptr_t", I], "ucrt_open_osfhandle_i")
add("_unlink", I, [CS], "ucrt_unlink")
add("_wunlink", I, [CW], "ucrt_wunlink")
add("_sopen_s", I, [I + " *", CS, I, I, I], "ucrt_sopen_s")
add("_wsopen_s", I, [I + " *", CW, I, I, I], "ucrt_wsopen_s")
manual("_sopen", "_wsopen", "_pipe", "_access", "_waccess", "_access_s", "_stat64", "_stat64i32", "_stat32", "_fstat64", "_fstat64i32",
       "_wstat64", "_wstat64i32", "_getcwd", "_wgetcwd", "_chdir", "_wchdir", "_mkdir", "_wmkdir", "_rmdir", "_wrmdir", "_fullpath",
       "_wfullpath", "_makepath", "_splitpath", "_wmakepath", "_wsplitpath", "_findfirst64", "_findnext64", "_findclose",
       "_findfirst64i32", "_findnext64i32", "_wfindfirst64", "_wfindnext64", "_wfindfirst64i32", "_wfindnext64i32", "_chmod", "_wchmod",
       "_getdrive", "_chdrive", "_getdcwd", "_wgetdcwd", "_utime64", "_futime64", "_mktemp", "_mktemp_s", "_umask", "system", "_wsystem",
       "_spawnl", "_spawnv", "_execv", "_cwait", "_getpid", "_get_pgmptr", "_get_wpgmptr", "_set_fmode_dummy")

# ---- time ----------------------------------------------------------------------------------------------------------------------------
add("_time64", "long long", ["long long *"], "ucrt_time64")
add("time", "long long", ["long long *"], "ucrt_time64")
add("_time32", "int", ["int *"], "ucrt_time32")
add("_gmtime64_s", I, ["ucrt_tm *", "const long long *"], "ucrt_gmtime64_s")
add("_localtime64_s", I, ["ucrt_tm *", "const long long *"], "ucrt_localtime64_s")
add("_mktime64", "long long", ["ucrt_tm *"], "ucrt_mktime64")
add("mktime", "long long", ["ucrt_tm *"], "ucrt_mktime64")
add("_mkgmtime64", "long long", ["ucrt_tm *"], "ucrt_mkgmtime64")
add("_mkgmtime", "long long", ["ucrt_tm *"], "ucrt_mkgmtime64")
add("asctime_s", I, [S, SZ, "const ucrt_tm *"], "ucrt_asctime_s")
add("_ctime64_s", I, [S, SZ, "const long long *"], "ucrt_ctime64_s")
add("strftime", SZ, [S, SZ, CS, "const ucrt_tm *"], "ucrt_strftime")
add("wcsftime", SZ, [WS, SZ, CW, "const ucrt_tm *"], "ucrt_wcsftime")
add("clock", "int", [], "ucrt_clock")
add("_difftime64", D, ["long long", "long long"], "ucrt_difftime64")
add("difftime", D, ["long long", "long long"], "ucrt_difftime64")
add("_tzset", None, [], "ucrt_tzset")
manual("_localtime64", "_gmtime64", "localtime", "gmtime", "_ctime64", "ctime", "asctime", "_ftime64_s", "_ftime64", "__p__timezone",
       "__p__daylight", "__p__dstbias", "__p__tzname", "_get_timezone", "_get_daylight", "_get_dstbias", "_get_tzname", "_strdate",
       "_strtime", "_strdate_s", "_strtime_s", "_Strftime", "_Getdays", "_Getmonths", "_Gettnames", "_wasctime", "_wctime64",
       "_localtime32_s", "_gmtime32_s", "_mktime32", "_ctime32_s")

# ---- locale and multibyte -----------------------------------------------------------------------------------------------------------------
add("setlocale", S, [I, CS], "ucrt_setlocale_x")
add("localeconv", "ucrt_lconv *", [], "ucrt_localeconv")
add("mblen", I, [CS, SZ], "ucrt_mblen")
add("mbtowc", I, [WS, CS, SZ], "ucrt_mbtowc")
add("wctomb", I, [S, W], "ucrt_wctomb")
add("wctomb_s", I, [I + " *", S, SZ, W], "ucrt_wctomb_s")
add("btowc", I, [I], "ucrt_btowc")
add("wctob", I, [W], "ucrt_wctob")
add("mbstowcs", SZ, [WS, CS, SZ], "ucrt_mbstowcs")
add("wcstombs", SZ, [S, CW, SZ], "ucrt_wcstombs")
add("mbstowcs_s", I, [SZ + " *", WS, SZ, CS, SZ], "ucrt_mbstowcs_s")
add("wcstombs_s", I, [SZ + " *", S, SZ, CW, SZ], "ucrt_wcstombs_s")
add("mbrtowc", SZ, [WS, CS, SZ, "ucrt_mbstate *"], "ucrt_mbrtowc")
add("wcrtomb", SZ, [S, W, "ucrt_mbstate *"], "ucrt_wcrtomb")
add("mbrlen", SZ, [CS, SZ, "ucrt_mbstate *"], "ucrt_mbrlen")
add("mbsinit", I, ["const ucrt_mbstate *"], "ucrt_mbsinit")
add("_mbstrlen", SZ, [CS], "ucrt_mbstrlen")
manual("mbsrtowcs", "wcsrtombs", "_mbslen", "_setmbcp", "_getmbcp", "_ismbblead", "_ismbtrail", "_mbsinc", "_mbsdec", "_mbsnbcnt")

# ---- process runtime -------------------------------------------------------------------------------------------------------------------------
add("getenv", S, [CS], "ucrt_getenv")
add("_putenv", I, [CS], "ucrt_putenv")
add("_putenv_s", I, [CS, CS], "ucrt_putenv_s")
add("getenv_s", I, [SZ + " *", S, SZ, CS], "ucrt_getenv_s")
add("_dupenv_s", I, [S + "*", SZ + " *", CS], "ucrt_dupenv_s")
add("_wgetenv", WS, [CW], "ucrt_wgetenv")
add("atexit", I, ["ucrt_onexit_fn"], "ucrt_atexit")
add("_onexit", "ucrt_onexit_fn", ["ucrt_onexit_fn"], "ucrt_onexit")
add("exit", None, [I], "ucrt_exit")
add("_exit", None, [I], "ucrt_exit_now")
add("_Exit", None, [I], "ucrt_exit_now")
add("quick_exit", None, [I], "ucrt_exit")
add("abort", None, [], "ucrt_abort")
add("_cexit", None, [], "ucrt_cexit")
add("_c_exit", None, [], "ucrt_cexit")
add("raise", I, [I], "ucrt_raise")
add("signal", "ucrt_signal_fn", [I, "ucrt_signal_fn"], "ucrt_signal")
add("_initterm", None, ["ucrt_initterm_fn *", "ucrt_initterm_fn *"], "ucrt_initterm")
add("_initterm_e", I, ["ucrt_inittermE_fn *", "ucrt_inittermE_fn *"], "ucrt_initterm_e")
add("_set_abort_behavior", U, [U, U], "ucrt_set_abort_behavior")
add("_set_invalid_parameter_handler", "ucrt_invalid_param_fn", ["ucrt_invalid_param_fn"], "ucrt_set_invalid_parameter_handler")
add("_set_purecall_handler", "ucrt_purecall_fn", ["ucrt_purecall_fn"], "ucrt_set_purecall_handler")
add("_purecall", None, [], "ucrt_purecall")
manual("_get_invalid_parameter_handler", "_set_thread_local_invalid_parameter_handler", "_get_thread_local_invalid_parameter_handler",
       "_invalid_parameter", "_invalid_parameter_noinfo", "_invalid_parameter_noinfo_noreturn", "_invoke_watson", "_get_purecall_handler",
       "_crt_atexit", "_crt_at_quick_exit", "_register_onexit_function", "_register_thread_local_exe_atexit_callback",
       "_initialize_onexit_table", "_execute_onexit_table", "at_quick_exit", "__p___argc", "__p___argv", "__p___wargv", "__p__acmdln",
       "__p__wcmdln", "__p__environ", "__p__wenviron", "__p__pgmptr", "__p__wpgmptr", "_configure_narrow_argv", "_configure_wide_argv",
       "_initialize_narrow_environment", "_initialize_wide_environment", "_get_initial_narrow_environment", "_get_initial_wide_environment",
       "__getmainargs", "__wgetmainargs", "_set_app_type", "_get_app_type", "_set_fmode", "_get_fmode", "__p__fmode", "__p__commode", "_set_commode",
       "_set_new_mode", "_query_new_mode", "_set_error_mode", "_seh_filter_exe", "_seh_filter_dll", "_crt_debugger_hook", "__acrt_initialize",
       "_exit_dummy", "terminate", "_set_se_translator", "_set_doserrno_dummy", "_cexit_dummy", "_lock", "_unlock", "_amsg_exit",
       "__acrt_errno_map_os_error", "_set_controlfp_dummy", "_beginthread", "_beginthreadex", "_endthread", "_endthreadex", "_threadid",
       "__threadid", "__threadhandle", "_get_thread_dummy", "__sys_errlist_dummy", "__p__sys_nerr_dummy", "_CIpow", "_get_osplatform",
       "_get_osver", "_get_winver", "_get_winmajor", "_get_winminor", "_get_sbh_threshold", "_set_sbh_threshold", "__crtGetShowWindowMode",
       "__crtSetUnhandledExceptionFilter", "__crtUnhandledException", "__crtTerminateProcess", "_set_output_format", "_get_output_format")

# ---- heap ---------------------------------------------------------------------------------------------------------------------------------------
manual("malloc", "free", "calloc", "realloc", "_msize", "_expand", "_recalloc", "_aligned_malloc", "_aligned_free", "_aligned_realloc",
       "_aligned_msize", "_aligned_offset_malloc", "_aligned_recalloc", "_callnewh", "_set_new_handler", "_get_heap_handle", "_heapchk",
       "_heapmin", "_heapwalk", "_freea_dummy", "_msize_base", "_malloc_base", "_free_base", "_calloc_base", "_realloc_base",
       "_set_malloc_dummy")



def add_locale_variants():
    base = {}
    for e in entries:
        if e[3] != "@manual":
            base[e[0]] = e
    names = [n for n in ["isalnum", "isalpha", "isblank", "iscntrl", "isdigit", "isgraph", "islower", "isprint", "ispunct", "isspace",
                         "isupper", "isxdigit", "tolower", "toupper", "iswalnum", "iswalpha", "iswblank", "iswcntrl", "iswdigit",
                         "iswgraph", "iswlower", "iswprint", "iswpunct", "iswspace", "iswupper", "iswxdigit", "towlower", "towupper",
                         "atoi", "atol", "atof", "_atoi64", "strtol", "strtoul", "strtod", "strtof", "_strtoi64", "_strtoui64",
                         "strtoll", "strtoull", "wcstol", "wcstoul", "wcstod", "wcstof", "_wcstoi64", "_wcstoui64", "wcstoll", "wcstoull",
                         "_wtoi", "_wtol", "_wtoi64", "_stricmp", "_strnicmp", "_strlwr", "_strupr", "_wcsicmp", "_wcsnicmp", "_wcslwr",
                         "_wcsupr", "mblen", "mbtowc", "mbstowcs", "wcstombs", "wctomb", "btowc", "wctob", "mbrtowc", "wcrtomb",
                         "mbrlen", "mbstowcs_s", "wcstombs_s", "wctomb_s", "_mbstrlen", "strftime", "wcsftime", "_strtok_s"]
             if n in base]
    for n in names:
        _, ret, params, impl = base[n]
        if params is None:
            continue
        # mbrtowc, wcrtomb, mbrlen and the _s forms keep their natural order; the locale is always the last parameter
        newname = n + "_l" if n.startswith("_") else "_" + n + "_l"
        if any(e[0] == newname for e in entries):
            continue
        entries.append((newname, ret, list(params) + [VP], "@locale:" + impl))


def main():
    implemented = set()
    for f in sorted((ROOT / "src/ntdll").glob("ucrtbase*")):
        if f.name in ("ucrtbase_gen.inc", "ucrtbase.def") or f.suffix not in (".c", ".inc"):
            continue
        for m in re.finditer(r"^[A-Za-z_][^;(){}\n]*\bw_(\w+)\s*\(", f.read_text(), re.M):
            implemented.add(m.group(1))
    add_locale_variants()
    seen = {}
    unimplemented = []
    inc = ["/* Generated by build/gen_ucrtbase.py; do not edit. */", ""]
    defs = ["; Generated by build/gen_ucrtbase.py; do not edit.", "LIBRARY ucrtbase", "EXPORTS"]
    for name, ret, params, impl in entries:
        if name in seen:
            sys.exit("duplicate export: " + name)
        seen[name] = 1
        if impl == "@manual":
            if name in implemented:
                defs.append("    %s=w_%s" % (name, name))
            else:
                unimplemented.append(name)
            continue
        call_impl = impl
        if impl.startswith("@locale:"):
            call_impl = impl[len("@locale:"):]
            args = ", ".join("%s a%d" % (t, i) for i, t in enumerate(params)) or "void"
            call = ", ".join("a%d" % i for i in range(len(params) - 1))  # the locale argument is dropped
        else:
            args = ", ".join("%s a%d" % (t, i) for i, t in enumerate(params)) or "void"
            call = ", ".join("a%d" % i for i in range(len(params)))
        rt = ret if ret is not None else "void"
        unused = "(void)a%d; " % (len(params) - 1) if impl.startswith("@locale:") else ""
        if ret is None:
            inc.append("%s w_%s(%s) { %s%s(%s); }" % (rt, name, args, unused, call_impl, call))
        else:
            inc.append("%s w_%s(%s) { %sreturn %s(%s); }" % (rt, name, args, unused, call_impl, call))
        defs.append("    %s=w_%s" % (name, name))
    OUT_INC.write_text("\n".join(inc) + "\n")
    OUT_DEF.write_text("\n".join(defs) + "\n")
    (ROOT / "build/ucrtbase_unimplemented.txt").write_text("\n".join(unimplemented) + "\n")
    print("exports: %d, unimplemented manual entries: %d (build/ucrtbase_unimplemented.txt)" % (len(defs) - 3, len(unimplemented)))


main()
