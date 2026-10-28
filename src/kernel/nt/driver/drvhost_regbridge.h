/* drvhost_regbridge.h - the driver host's access to the personality registry (src/kernel/nt/registry.c).
 *
 * Plain C with no WDK types, like drvhost_svcdb.h, so that it compiles against the kernel-side headers. The driver-facing
 * Zw and Rtl registry functions (drvhost_zwreg.c) are written in the WDK type universe and call through this interface.
 *
 * A bridged key remembers its absolute path. That is how a key opened relative to another key is resolved: the personality
 * registry opens write access under \Registry\Machine only through ntreg_open_key_trusted, which takes a full path, and
 * a driver runs in kernel mode and is entitled to that access.
 *
 * Names are UTF-16 code units (unsigned short) with a byte length, not NUL terminated. Return values are NTSTATUS.
 */
#ifndef DRVHOST_REGBRIDGE_H
#define DRVHOST_REGBRIDGE_H

typedef struct dhreg_key dhreg_key_t;

/* Opens, or with create creates, the key `name`. A name that starts with a backslash is absolute; any other name is
 * relative to `root`, which must then be non-NULL. access may contain generic rights and MAXIMUM_ALLOWED; they are mapped
 * to key rights. *disposition is REG_CREATED_NEW_KEY or REG_OPENED_EXISTING_KEY when it is not NULL. */
long dhreg_open(dhreg_key_t *root, const unsigned short *name, unsigned name_bytes, unsigned long access, int create,
                unsigned long *disposition, dhreg_key_t **out);
long dhreg_close(dhreg_key_t *key);

long dhreg_query_value(dhreg_key_t *key, const unsigned short *name, unsigned name_bytes, unsigned long info_class,
                       void *buf, unsigned long len, unsigned long *result_len);
long dhreg_set_value(dhreg_key_t *key, const unsigned short *name, unsigned name_bytes, unsigned long type,
                     const void *data, unsigned long size);
long dhreg_delete_value(dhreg_key_t *key, const unsigned short *name, unsigned name_bytes);
long dhreg_enum_value(dhreg_key_t *key, unsigned long index, unsigned long info_class, void *buf, unsigned long len,
                      unsigned long *result_len);
long dhreg_enum_key(dhreg_key_t *key, unsigned long index, unsigned long info_class, void *buf, unsigned long len,
                    unsigned long *result_len);
long dhreg_query_key(dhreg_key_t *key, unsigned long info_class, void *buf, unsigned long len,
                     unsigned long *result_len);
long dhreg_delete_key(dhreg_key_t *key);
long dhreg_flush(dhreg_key_t *key);

#endif /* DRVHOST_REGBRIDGE_H */
