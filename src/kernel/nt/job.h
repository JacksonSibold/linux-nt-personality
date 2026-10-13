/*
 * job.h - NT job objects (kernel/nt/job.c).
 *
 * A job is a named or unnamed object that groups processes. A process joins at
 * most one job (nested jobs are not supported). The job holds a reference on
 * itself for every member, so the job outlives its members. Membership ends
 * when the process object is destroyed (ObDestroyProcessObject).
 */

#ifndef NTJOB_H
#define NTJOB_H

#include "object.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/* Access rights (winnt.h)                                                    */
/* -------------------------------------------------------------------------- */

#define JOB_OBJECT_ASSIGN_PROCESS           0x0001UL
#define JOB_OBJECT_SET_ATTRIBUTES           0x0002UL
#define JOB_OBJECT_QUERY                    0x0004UL
#define JOB_OBJECT_TERMINATE                0x0008UL
#define JOB_OBJECT_SET_SECURITY_ATTRIBUTES  0x0010UL
#define JOB_OBJECT_IMPERSONATE              0x0020UL
/* STANDARD_RIGHTS_REQUIRED | SYNCHRONIZE | 0x3F */
#define JOB_OBJECT_ALL_ACCESS               0x001F003FUL

/* Process access the assign service needs on the process handle (winnt.h). */
#define JOB_PROCESS_TERMINATE               0x0001UL
#define JOB_PROCESS_SET_QUOTA               0x0100UL

/* -------------------------------------------------------------------------- */
/* Information classes                                                        */
/* -------------------------------------------------------------------------- */

typedef enum _JOBOBJECTINFOCLASS {
    JobObjectBasicAccountingInformation = 1,
    JobObjectBasicLimitInformation      = 2,
    JobObjectBasicProcessIdList         = 3,
    JobObjectBasicUIRestrictions        = 4,   /* not supported */
    JobObjectExtendedLimitInformation   = 9,
} JOBOBJECTINFOCLASS;

/* -------------------------------------------------------------------------- */
/* Limit flags (winnt.h)                                                      */
/* -------------------------------------------------------------------------- */

#define JOB_OBJECT_LIMIT_WORKINGSET                 0x00000001UL
#define JOB_OBJECT_LIMIT_PROCESS_TIME               0x00000002UL
#define JOB_OBJECT_LIMIT_JOB_TIME                   0x00000004UL
#define JOB_OBJECT_LIMIT_ACTIVE_PROCESS             0x00000008UL
#define JOB_OBJECT_LIMIT_AFFINITY                   0x00000010UL
#define JOB_OBJECT_LIMIT_PRIORITY_CLASS             0x00000020UL
#define JOB_OBJECT_LIMIT_PRESERVE_JOB_TIME          0x00000040UL
#define JOB_OBJECT_LIMIT_SCHEDULING_CLASS           0x00000080UL
#define JOB_OBJECT_LIMIT_PROCESS_MEMORY             0x00000100UL
#define JOB_OBJECT_LIMIT_JOB_MEMORY                 0x00000200UL
#define JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION 0x00000400UL
#define JOB_OBJECT_LIMIT_BREAKAWAY_OK               0x00000800UL
#define JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK        0x00001000UL
#define JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE          0x00002000UL

/* Every flag bit a set may carry. Any other bit is STATUS_INVALID_PARAMETER. */
#define JOB_OBJECT_LIMIT_VALID_FLAGS \
    (JOB_OBJECT_LIMIT_WORKINGSET | JOB_OBJECT_LIMIT_PROCESS_TIME | \
     JOB_OBJECT_LIMIT_JOB_TIME | JOB_OBJECT_LIMIT_ACTIVE_PROCESS | \
     JOB_OBJECT_LIMIT_AFFINITY | JOB_OBJECT_LIMIT_PRIORITY_CLASS | \
     JOB_OBJECT_LIMIT_PRESERVE_JOB_TIME | JOB_OBJECT_LIMIT_SCHEDULING_CLASS | \
     JOB_OBJECT_LIMIT_PROCESS_MEMORY | JOB_OBJECT_LIMIT_JOB_MEMORY | \
     JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION | JOB_OBJECT_LIMIT_BREAKAWAY_OK | \
     JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK | JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE)

/* -------------------------------------------------------------------------- */
/* Information structures (winnt.h layout)                                    */
/* -------------------------------------------------------------------------- */

typedef struct _JOBOBJECT_BASIC_ACCOUNTING_INFORMATION {
    LARGE_INTEGER TotalUserTime;
    LARGE_INTEGER TotalKernelTime;
    LARGE_INTEGER ThisPeriodTotalUserTime;
    LARGE_INTEGER ThisPeriodTotalKernelTime;
    ULONG         TotalPageFaultCount;
    ULONG         TotalProcesses;
    ULONG         ActiveProcesses;
    ULONG         TotalTerminatedProcesses;
} JOBOBJECT_BASIC_ACCOUNTING_INFORMATION, *PJOBOBJECT_BASIC_ACCOUNTING_INFORMATION;

typedef struct _JOBOBJECT_BASIC_LIMIT_INFORMATION {
    LARGE_INTEGER PerProcessUserTimeLimit;
    LARGE_INTEGER PerJobUserTimeLimit;
    ULONG         LimitFlags;
    SIZE_T        MinimumWorkingSetSize;
    SIZE_T        MaximumWorkingSetSize;
    ULONG         ActiveProcessLimit;
    ULONG_PTR     Affinity;
    ULONG         PriorityClass;
    ULONG         SchedulingClass;
} JOBOBJECT_BASIC_LIMIT_INFORMATION, *PJOBOBJECT_BASIC_LIMIT_INFORMATION;

typedef struct _JOBOBJECT_IO_COUNTERS_LOCAL {
    ULONGLONG ReadOperationCount;
    ULONGLONG WriteOperationCount;
    ULONGLONG OtherOperationCount;
    ULONGLONG ReadTransferCount;
    ULONGLONG WriteTransferCount;
    ULONGLONG OtherTransferCount;
} JOBOBJECT_IO_COUNTERS_LOCAL;

typedef struct _JOBOBJECT_EXTENDED_LIMIT_INFORMATION {
    JOBOBJECT_BASIC_LIMIT_INFORMATION BasicLimitInformation;
    JOBOBJECT_IO_COUNTERS_LOCAL       IoInfo;
    SIZE_T                            ProcessMemoryLimit;
    SIZE_T                            JobMemoryLimit;
    SIZE_T                            PeakProcessMemoryUsed;
    SIZE_T                            PeakJobMemoryUsed;
} JOBOBJECT_EXTENDED_LIMIT_INFORMATION, *PJOBOBJECT_EXTENDED_LIMIT_INFORMATION;

/* ProcessIdList[] holds NumberOfProcessIdsInList entries; the array is the
 * tail of the structure (offsetof(ProcessIdList) bytes of header). */
typedef struct _JOBOBJECT_BASIC_PROCESS_ID_LIST {
    ULONG     NumberOfAssignedProcesses;
    ULONG     NumberOfProcessIdsInList;
    ULONG_PTR ProcessIdList[1];
} JOBOBJECT_BASIC_PROCESS_ID_LIST, *PJOBOBJECT_BASIC_PROCESS_ID_LIST;

/* -------------------------------------------------------------------------- */
/* Job object body                                                            */
/* -------------------------------------------------------------------------- */

struct nt_job;

/* Object type. Registered by the create and open services. */
extern struct nt_object_type nt_job_type;

/* Drops process from its job (if any): removes the membership node, releases
 * the membership reference on the job. Called by ObDestroyProcessObject before
 * the process is torn down. A process that is not in a job is a no-op. */
void nt_job_remove_process(struct nt_eprocess *proc);

#ifndef __KERNEL__
/* Userspace service entry points (typed). The kernel build takes the ULONG
 * ABI wrappers in job.c instead. */
NTSTATUS nt_svc_NtCreateJobObject(PHANDLE JobHandle, ACCESS_MASK DesiredAccess,
                                  POBJECT_ATTRIBUTES ObjectAttributes);
NTSTATUS nt_svc_NtOpenJobObject(PHANDLE JobHandle, ACCESS_MASK DesiredAccess,
                                POBJECT_ATTRIBUTES ObjectAttributes);
NTSTATUS nt_svc_NtAssignProcessToJobObject(HANDLE JobHandle, HANDLE ProcessHandle);
NTSTATUS nt_svc_NtTerminateJobObject(HANDLE JobHandle, NTSTATUS ExitStatus);
NTSTATUS nt_svc_NtQueryInformationJobObject(HANDLE JobHandle,
                                            JOBOBJECTINFOCLASS JobObjectInformationClass,
                                            PVOID JobObjectInformation,
                                            ULONG JobObjectInformationLength,
                                            PULONG ReturnLength);
NTSTATUS nt_svc_NtSetInformationJobObject(HANDLE JobHandle,
                                          JOBOBJECTINFOCLASS JobObjectInformationClass,
                                          PVOID JobObjectInformation,
                                          ULONG JobObjectInformationLength);
#endif

#ifdef __cplusplus
}
#endif

#endif /* NTJOB_H */
