/* linux/cred.h - syntax-check stub. Only the credential members the loader reads. */
#ifndef SYNTAX_SHIM_LINUX_CRED_H
#define SYNTAX_SHIM_LINUX_CRED_H

#include <linux/uidgid.h>

struct cred {
	kuid_t uid;
	kgid_t gid;
	kuid_t euid;
	kgid_t egid;
};

#endif
