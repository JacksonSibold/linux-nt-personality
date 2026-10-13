/* ntpers_probe_root.h - scratch-root handling for the Windows build of the
 * direct probes. Included only under #ifndef NTPERS_VM, so the VM / Linux
 * guest build never sees it and its output and binary are unchanged.
 *
 * Root: --root <dir>, else env NTPERS_PROBE_ROOT, else %TEMP%.
 * The root must be a local absolute path (X:\dir). UNC, device and
 * drive-relative roots are refused, because a UNC path can start an SMB
 * connection. Every file a probe creates goes under the root and is deleted
 * before exit. A probe refuses to start if its own scratch file or directory
 * already exists, so it never deletes something it did not create.
 */
#ifndef NTPERS_PROBE_ROOT_H
#define NTPERS_PROBE_ROOT_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

static char g_root[MAX_PATH];

static int resolve_root(int argc, char **argv){
    const char *r = NULL;
    for(int i = 1; i + 1 < argc; i++) if(strcmp(argv[i], "--root") == 0) r = argv[i + 1];
    if(!r) r = getenv("NTPERS_PROBE_ROOT");
    if(r && r[0]) snprintf(g_root, sizeof g_root, "%s", r);
    else if(GetTempPathA(sizeof g_root, g_root) == 0) return 0;
    size_t l = strlen(g_root);
    while(l > 0 && (g_root[l - 1] == '\\' || g_root[l - 1] == '/')) g_root[--l] = 0;
    if(!(((g_root[0] >= 'A' && g_root[0] <= 'Z') || (g_root[0] >= 'a' && g_root[0] <= 'z'))
         && g_root[1] == ':' && g_root[2] == '\\')) return 0;
    return 1;
}

/* Opt-in only: UNC cases in probe_path and probe_path_direct. */
static int allow_unc(void){
    const char *v = getenv("NTPERS_PROBE_ALLOW_UNC");
    return v && strcmp(v, "1") == 0;
}

/* Integer argument "--name N", else def. Clamped to [lo, hi]. */
static long arg_long(int argc, char **argv, const char *name, long def, long lo, long hi){
    long v = def;
    for(int i = 1; i + 1 < argc; i++) if(strcmp(argv[i], name) == 0) v = strtol(argv[i + 1], NULL, 10);
    if(v < lo) v = lo;
    if(v > hi) v = hi;
    return v;
}

/* 1 if the path does not exist (the probe may create it and delete it). */
static int path_absent(const char *win_path){
    return GetFileAttributesA(win_path) == INVALID_FILE_ATTRIBUTES;
}

#endif
