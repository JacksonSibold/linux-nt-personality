/* drvhost_svcdb.h - service database for the driver host, backed by the
 * personality registry (src/kernel/nt/registry.c).
 *
 * Each service is a key under
 *   \Registry\Machine\System\CurrentControlSet\Services\<name>
 * with ImagePath (REG_SZ), Type and Start (REG_DWORD), as on Windows.  The
 * driver host reads its service records here; there is no separate table.
 *
 * Plain C with no WDK types, so it compiles against the kernel-side headers.
 * Return values are NTSTATUS codes.
 */
#ifndef DRVHOST_SVCDB_H
#define DRVHOST_SVCDB_H

#define DRVHOST_SVC_KEY_PREFIX "\\Registry\\Machine\\System\\CurrentControlSet\\Services\\"

typedef struct drvhost_svc_record {
    char          image_path[256];   /* ImagePath, ASCII (values are ASCII-only here) */
    unsigned long type;              /* Type */
    unsigned long start;             /* Start */
} drvhost_svc_record_t;

/* Creates or updates the service key and writes its values. */
long drvhost_svcdb_register(const char *name, const char *image_path,
                            unsigned long type, unsigned long start);

/* Reads the service key.  STATUS_OBJECT_NAME_NOT_FOUND if the key is absent,
 * STATUS_OBJECT_NAME_INVALID if a required value is missing or malformed. */
long drvhost_svcdb_lookup(const char *name, drvhost_svc_record_t *out);

/* Deletes the service key.  STATUS_OBJECT_NAME_NOT_FOUND if absent. */
long drvhost_svcdb_remove(const char *name);

#endif /* DRVHOST_SVCDB_H */
