/* pe_host.h - what the two halves of the host PE runner share. */
#ifndef PE_HOST_H
#define PE_HOST_H

struct pehost_start {
    unsigned long ip, sp, cx, teb;
};

extern int pehost_verbose; /* show the loader's kernel log lines */

/* pe_boot.c: load the program with the kernel's PE loader. 0 or a negative errno. */
int pehost_boot(const char *root, const char *exe_path, int argc, const char *const *argv, const char *const *envp, struct pehost_start *out);

/* ntpers_run.c: the loader's hooks into the NT modules. */
int pehost_std_install(int fd, unsigned long long *out_handle);
void pehost_std_release(unsigned long long handle);

#endif
