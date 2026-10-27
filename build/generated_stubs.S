/* auto-generated syscall stubs - do not edit */
    .text
    .align 16
    .globl NtCreateUserProcess
NtCreateUserProcess:
    mov %rcx, %r10
    mov $0x00cd, %eax
    syscall
    ret
    .globl ZwCreateUserProcess
    .set ZwCreateUserProcess, NtCreateUserProcess
    .globl NtCreateProcess
NtCreateProcess:
    mov %rcx, %r10
    mov $0x00bc, %eax
    syscall
    ret
    .globl ZwCreateProcess
    .set ZwCreateProcess, NtCreateProcess
    .globl NtCreateProcessEx
NtCreateProcessEx:
    mov %rcx, %r10
    mov $0x004d, %eax
    syscall
    ret
    .globl ZwCreateProcessEx
    .set ZwCreateProcessEx, NtCreateProcessEx
    .globl NtTerminateProcess
NtTerminateProcess:
    mov %rcx, %r10
    mov $0x002c, %eax
    syscall
    ret
    .globl ZwTerminateProcess
    .set ZwTerminateProcess, NtTerminateProcess
    .globl NtCreateThread
NtCreateThread:
    mov %rcx, %r10
    mov $0x004e, %eax
    syscall
    ret
    .globl ZwCreateThread
    .set ZwCreateThread, NtCreateThread
    .globl NtCreateThreadEx
NtCreateThreadEx:
    mov %rcx, %r10
    mov $0x00c5, %eax
    syscall
    ret
    .globl ZwCreateThreadEx
    .set ZwCreateThreadEx, NtCreateThreadEx
    .globl NtTerminateThread
NtTerminateThread:
    mov %rcx, %r10
    mov $0x0053, %eax
    syscall
    ret
    .globl ZwTerminateThread
    .set ZwTerminateThread, NtTerminateThread
    .globl NtSuspendThread
NtSuspendThread:
    mov %rcx, %r10
    mov $0x01c4, %eax
    syscall
    ret
    .globl ZwSuspendThread
    .set ZwSuspendThread, NtSuspendThread
    .globl NtResumeThread
NtResumeThread:
    mov %rcx, %r10
    mov $0x0052, %eax
    syscall
    ret
    .globl ZwResumeThread
    .set ZwResumeThread, NtResumeThread
    .globl NtSuspendProcess
NtSuspendProcess:
    mov %rcx, %r10
    mov $0x01c3, %eax
    syscall
    ret
    .globl ZwSuspendProcess
    .set ZwSuspendProcess, NtSuspendProcess
    .globl NtResumeProcess
NtResumeProcess:
    mov %rcx, %r10
    mov $0x0183, %eax
    syscall
    ret
    .globl ZwResumeProcess
    .set ZwResumeProcess, NtResumeProcess
    .globl NtGetContextThread
NtGetContextThread:
    mov %rcx, %r10
    mov $0x00f7, %eax
    syscall
    ret
    .globl ZwGetContextThread
    .set ZwGetContextThread, NtGetContextThread
    .globl NtSetContextThread
NtSetContextThread:
    mov %rcx, %r10
    mov $0x0193, %eax
    syscall
    ret
    .globl ZwSetContextThread
    .set ZwSetContextThread, NtSetContextThread
    .globl NtAlertThread
NtAlertThread:
    mov %rcx, %r10
    mov $0x006f, %eax
    syscall
    ret
    .globl ZwAlertThread
    .set ZwAlertThread, NtAlertThread
    .globl NtAlertThreadByThreadId
NtAlertThreadByThreadId:
    mov %rcx, %r10
    mov $0x0070, %eax
    syscall
    ret
    .globl ZwAlertThreadByThreadId
    .set ZwAlertThreadByThreadId, NtAlertThreadByThreadId
    .globl NtAlertResumeThread
NtAlertResumeThread:
    mov %rcx, %r10
    mov $0x006e, %eax
    syscall
    ret
    .globl ZwAlertResumeThread
    .set ZwAlertResumeThread, NtAlertResumeThread
    .globl NtTestAlert
NtTestAlert:
    mov %rcx, %r10
    mov $0x01c8, %eax
    syscall
    ret
    .globl ZwTestAlert
    .set ZwTestAlert, NtTestAlert
    .globl NtQueueApcThread
NtQueueApcThread:
    mov %rcx, %r10
    mov $0x0045, %eax
    syscall
    ret
    .globl ZwQueueApcThread
    .set ZwQueueApcThread, NtQueueApcThread
    .globl NtQueueApcThreadEx
NtQueueApcThreadEx:
    mov %rcx, %r10
    mov $0x016b, %eax
    syscall
    ret
    .globl ZwQueueApcThreadEx
    .set ZwQueueApcThreadEx, NtQueueApcThreadEx
    .globl NtWaitForAlertByThreadId
NtWaitForAlertByThreadId:
    mov %rcx, %r10
    mov $0x01d8, %eax
    syscall
    ret
    .globl ZwWaitForAlertByThreadId
    .set ZwWaitForAlertByThreadId, NtWaitForAlertByThreadId
    .globl NtAllocateVirtualMemory
NtAllocateVirtualMemory:
    mov %rcx, %r10
    mov $0x0018, %eax
    syscall
    ret
    .globl ZwAllocateVirtualMemory
    .set ZwAllocateVirtualMemory, NtAllocateVirtualMemory
    .globl NtAllocateVirtualMemoryEx
NtAllocateVirtualMemoryEx:
    mov %rcx, %r10
    mov $0x0076, %eax
    syscall
    ret
    .globl ZwAllocateVirtualMemoryEx
    .set ZwAllocateVirtualMemoryEx, NtAllocateVirtualMemoryEx
    .globl NtFreeVirtualMemory
NtFreeVirtualMemory:
    mov %rcx, %r10
    mov $0x001e, %eax
    syscall
    ret
    .globl ZwFreeVirtualMemory
    .set ZwFreeVirtualMemory, NtFreeVirtualMemory
    .globl NtProtectVirtualMemory
NtProtectVirtualMemory:
    mov %rcx, %r10
    mov $0x0050, %eax
    syscall
    ret
    .globl ZwProtectVirtualMemory
    .set ZwProtectVirtualMemory, NtProtectVirtualMemory
    .globl NtQueryVirtualMemory
NtQueryVirtualMemory:
    mov %rcx, %r10
    mov $0x0023, %eax
    syscall
    ret
    .globl ZwQueryVirtualMemory
    .set ZwQueryVirtualMemory, NtQueryVirtualMemory
    .globl NtReadVirtualMemory
NtReadVirtualMemory:
    mov %rcx, %r10
    mov $0x003f, %eax
    syscall
    ret
    .globl ZwReadVirtualMemory
    .set ZwReadVirtualMemory, NtReadVirtualMemory
    .globl NtWriteVirtualMemory
NtWriteVirtualMemory:
    mov %rcx, %r10
    mov $0x003a, %eax
    syscall
    ret
    .globl ZwWriteVirtualMemory
    .set ZwWriteVirtualMemory, NtWriteVirtualMemory
    .globl NtFlushVirtualMemory
NtFlushVirtualMemory:
    mov %rcx, %r10
    mov $0x00f0, %eax
    syscall
    ret
    .globl ZwFlushVirtualMemory
    .set ZwFlushVirtualMemory, NtFlushVirtualMemory
    .globl NtLockVirtualMemory
NtLockVirtualMemory:
    mov %rcx, %r10
    mov $0x0113, %eax
    syscall
    ret
    .globl ZwLockVirtualMemory
    .set ZwLockVirtualMemory, NtLockVirtualMemory
    .globl NtUnlockVirtualMemory
NtUnlockVirtualMemory:
    mov %rcx, %r10
    mov $0x01d3, %eax
    syscall
    ret
    .globl ZwUnlockVirtualMemory
    .set ZwUnlockVirtualMemory, NtUnlockVirtualMemory
    .globl NtCreateSection
NtCreateSection:
    mov %rcx, %r10
    mov $0x004a, %eax
    syscall
    ret
    .globl ZwCreateSection
    .set ZwCreateSection, NtCreateSection
    .globl NtCreateSectionEx
NtCreateSectionEx:
    mov %rcx, %r10
    mov $0x00c2, %eax
    syscall
    ret
    .globl ZwCreateSectionEx
    .set ZwCreateSectionEx, NtCreateSectionEx
    .globl NtOpenSection
NtOpenSection:
    mov %rcx, %r10
    mov $0x0037, %eax
    syscall
    ret
    .globl ZwOpenSection
    .set ZwOpenSection, NtOpenSection
    .globl NtMapViewOfSection
NtMapViewOfSection:
    mov %rcx, %r10
    mov $0x0028, %eax
    syscall
    ret
    .globl ZwMapViewOfSection
    .set ZwMapViewOfSection, NtMapViewOfSection
    .globl NtMapViewOfSectionEx
NtMapViewOfSectionEx:
    mov %rcx, %r10
    mov $0x011a, %eax
    syscall
    ret
    .globl ZwMapViewOfSectionEx
    .set ZwMapViewOfSectionEx, NtMapViewOfSectionEx
    .globl NtUnmapViewOfSection
NtUnmapViewOfSection:
    mov %rcx, %r10
    mov $0x002a, %eax
    syscall
    ret
    .globl ZwUnmapViewOfSection
    .set ZwUnmapViewOfSection, NtUnmapViewOfSection
    .globl NtUnmapViewOfSectionEx
NtUnmapViewOfSectionEx:
    mov %rcx, %r10
    mov $0x01d4, %eax
    syscall
    ret
    .globl ZwUnmapViewOfSectionEx
    .set ZwUnmapViewOfSectionEx, NtUnmapViewOfSectionEx
    .globl NtExtendSection
NtExtendSection:
    mov %rcx, %r10
    mov $0x00e7, %eax
    syscall
    ret
    .globl ZwExtendSection
    .set ZwExtendSection, NtExtendSection
    .globl NtQuerySection
NtQuerySection:
    mov %rcx, %r10
    mov $0x0051, %eax
    syscall
    ret
    .globl ZwQuerySection
    .set ZwQuerySection, NtQuerySection
    .globl NtCreateFile
NtCreateFile:
    mov %rcx, %r10
    mov $0x0055, %eax
    syscall
    ret
    .globl ZwCreateFile
    .set ZwCreateFile, NtCreateFile
    .globl NtOpenFile
NtOpenFile:
    mov %rcx, %r10
    mov $0x0033, %eax
    syscall
    ret
    .globl ZwOpenFile
    .set ZwOpenFile, NtOpenFile
    .globl NtReadFile
NtReadFile:
    mov %rcx, %r10
    mov $0x0006, %eax
    syscall
    ret
    .globl ZwReadFile
    .set ZwReadFile, NtReadFile
    .globl NtWriteFile
NtWriteFile:
    mov %rcx, %r10
    mov $0x0008, %eax
    syscall
    ret
    .globl ZwWriteFile
    .set ZwWriteFile, NtWriteFile
    .globl NtFlushBuffersFile
NtFlushBuffersFile:
    mov %rcx, %r10
    mov $0x004b, %eax
    syscall
    ret
    .globl ZwFlushBuffersFile
    .set ZwFlushBuffersFile, NtFlushBuffersFile
    .globl NtFlushBuffersFileEx
NtFlushBuffersFileEx:
    mov %rcx, %r10
    mov $0x00eb, %eax
    syscall
    ret
    .globl ZwFlushBuffersFileEx
    .set ZwFlushBuffersFileEx, NtFlushBuffersFileEx
    .globl NtQueryInformationFile
NtQueryInformationFile:
    mov %rcx, %r10
    mov $0x0011, %eax
    syscall
    ret
    .globl ZwQueryInformationFile
    .set ZwQueryInformationFile, NtQueryInformationFile
    .globl NtSetInformationFile
NtSetInformationFile:
    mov %rcx, %r10
    mov $0x0027, %eax
    syscall
    ret
    .globl ZwSetInformationFile
    .set ZwSetInformationFile, NtSetInformationFile
    .globl NtQueryDirectoryFile
NtQueryDirectoryFile:
    mov %rcx, %r10
    mov $0x0035, %eax
    syscall
    ret
    .globl ZwQueryDirectoryFile
    .set ZwQueryDirectoryFile, NtQueryDirectoryFile
    .globl NtQueryDirectoryFileEx
NtQueryDirectoryFileEx:
    mov %rcx, %r10
    mov $0x0148, %eax
    syscall
    ret
    .globl ZwQueryDirectoryFileEx
    .set ZwQueryDirectoryFileEx, NtQueryDirectoryFileEx
    .globl NtQueryVolumeInformationFile
NtQueryVolumeInformationFile:
    mov %rcx, %r10
    mov $0x0049, %eax
    syscall
    ret
    .globl ZwQueryVolumeInformationFile
    .set ZwQueryVolumeInformationFile, NtQueryVolumeInformationFile
    .globl NtSetVolumeInformationFile
NtSetVolumeInformationFile:
    mov %rcx, %r10
    mov $0x01ba, %eax
    syscall
    ret
    .globl ZwSetVolumeInformationFile
    .set ZwSetVolumeInformationFile, NtSetVolumeInformationFile
    .globl NtQueryEaFile
NtQueryEaFile:
    mov %rcx, %r10
    mov $0x014b, %eax
    syscall
    ret
    .globl ZwQueryEaFile
    .set ZwQueryEaFile, NtQueryEaFile
    .globl NtSetEaFile
NtSetEaFile:
    mov %rcx, %r10
    mov $0x0199, %eax
    syscall
    ret
    .globl ZwSetEaFile
    .set ZwSetEaFile, NtSetEaFile
    .globl NtLockFile
NtLockFile:
    mov %rcx, %r10
    mov $0x0110, %eax
    syscall
    ret
    .globl ZwLockFile
    .set ZwLockFile, NtLockFile
    .globl NtUnlockFile
NtUnlockFile:
    mov %rcx, %r10
    mov $0x01d2, %eax
    syscall
    ret
    .globl ZwUnlockFile
    .set ZwUnlockFile, NtUnlockFile
    .globl NtCancelIoFile
NtCancelIoFile:
    mov %rcx, %r10
    mov $0x005d, %eax
    syscall
    ret
    .globl ZwCancelIoFile
    .set ZwCancelIoFile, NtCancelIoFile
    .globl NtCancelIoFileEx
NtCancelIoFileEx:
    mov %rcx, %r10
    mov $0x0092, %eax
    syscall
    ret
    .globl ZwCancelIoFileEx
    .set ZwCancelIoFileEx, NtCancelIoFileEx
    .globl NtDeviceIoControlFile
NtDeviceIoControlFile:
    mov %rcx, %r10
    mov $0x0007, %eax
    syscall
    ret
    .globl ZwDeviceIoControlFile
    .set ZwDeviceIoControlFile, NtDeviceIoControlFile
    .globl NtFsControlFile
NtFsControlFile:
    mov %rcx, %r10
    mov $0x0039, %eax
    syscall
    ret
    .globl ZwFsControlFile
    .set ZwFsControlFile, NtFsControlFile
    .globl NtDeleteFile
NtDeleteFile:
    mov %rcx, %r10
    mov $0x00d7, %eax
    syscall
    ret
    .globl ZwDeleteFile
    .set ZwDeleteFile, NtDeleteFile
    .globl NtQueryAttributesFile
NtQueryAttributesFile:
    mov %rcx, %r10
    mov $0x003d, %eax
    syscall
    ret
    .globl ZwQueryAttributesFile
    .set ZwQueryAttributesFile, NtQueryAttributesFile
    .globl NtQueryFullAttributesFile
NtQueryFullAttributesFile:
    mov %rcx, %r10
    mov $0x014c, %eax
    syscall
    ret
    .globl ZwQueryFullAttributesFile
    .set ZwQueryFullAttributesFile, NtQueryFullAttributesFile
    .globl NtClose
NtClose:
    mov %rcx, %r10
    mov $0x000f, %eax
    syscall
    ret
    .globl ZwClose
    .set ZwClose, NtClose
    .globl NtDuplicateObject
NtDuplicateObject:
    mov %rcx, %r10
    mov $0x003c, %eax
    syscall
    ret
    .globl ZwDuplicateObject
    .set ZwDuplicateObject, NtDuplicateObject
    .globl NtQueryObject
NtQueryObject:
    mov %rcx, %r10
    mov $0x0010, %eax
    syscall
    ret
    .globl ZwQueryObject
    .set ZwQueryObject, NtQueryObject
    .globl NtSetInformationObject
NtSetInformationObject:
    mov %rcx, %r10
    mov $0x005c, %eax
    syscall
    ret
    .globl ZwSetInformationObject
    .set ZwSetInformationObject, NtSetInformationObject
    .globl NtMakeTemporaryObject
NtMakeTemporaryObject:
    mov %rcx, %r10
    mov $0x0115, %eax
    syscall
    ret
    .globl ZwMakeTemporaryObject
    .set ZwMakeTemporaryObject, NtMakeTemporaryObject
    .globl NtMakePermanentObject
NtMakePermanentObject:
    mov %rcx, %r10
    mov $0x0114, %eax
    syscall
    ret
    .globl ZwMakePermanentObject
    .set ZwMakePermanentObject, NtMakePermanentObject
    .globl NtCreateEvent
NtCreateEvent:
    mov %rcx, %r10
    mov $0x0048, %eax
    syscall
    ret
    .globl ZwCreateEvent
    .set ZwCreateEvent, NtCreateEvent
    .globl NtOpenEvent
NtOpenEvent:
    mov %rcx, %r10
    mov $0x0040, %eax
    syscall
    ret
    .globl ZwOpenEvent
    .set ZwOpenEvent, NtOpenEvent
    .globl NtSetEvent
NtSetEvent:
    mov %rcx, %r10
    mov $0x000e, %eax
    syscall
    ret
    .globl ZwSetEvent
    .set ZwSetEvent, NtSetEvent
    .globl NtResetEvent
NtResetEvent:
    mov %rcx, %r10
    mov $0x0180, %eax
    syscall
    ret
    .globl ZwResetEvent
    .set ZwResetEvent, NtResetEvent
    .globl NtPulseEvent
NtPulseEvent:
    mov %rcx, %r10
    mov $0x0143, %eax
    syscall
    ret
    .globl ZwPulseEvent
    .set ZwPulseEvent, NtPulseEvent
    .globl NtQueryEvent
NtQueryEvent:
    mov %rcx, %r10
    mov $0x0056, %eax
    syscall
    ret
    .globl ZwQueryEvent
    .set ZwQueryEvent, NtQueryEvent
    .globl NtCreateMutant
NtCreateMutant:
    mov %rcx, %r10
    mov $0x00b6, %eax
    syscall
    ret
    .globl ZwCreateMutant
    .set ZwCreateMutant, NtCreateMutant
    .globl NtOpenMutant
NtOpenMutant:
    mov %rcx, %r10
    mov $0x012a, %eax
    syscall
    ret
    .globl ZwOpenMutant
    .set ZwOpenMutant, NtOpenMutant
    .globl NtReleaseMutant
NtReleaseMutant:
    mov %rcx, %r10
    mov $0x0020, %eax
    syscall
    ret
    .globl ZwReleaseMutant
    .set ZwReleaseMutant, NtReleaseMutant
    .globl NtQueryMutant
NtQueryMutant:
    mov %rcx, %r10
    mov $0x015b, %eax
    syscall
    ret
    .globl ZwQueryMutant
    .set ZwQueryMutant, NtQueryMutant
    .globl NtCreateSemaphore
NtCreateSemaphore:
    mov %rcx, %r10
    mov $0x00c3, %eax
    syscall
    ret
    .globl ZwCreateSemaphore
    .set ZwCreateSemaphore, NtCreateSemaphore
    .globl NtOpenSemaphore
NtOpenSemaphore:
    mov %rcx, %r10
    mov $0x0131, %eax
    syscall
    ret
    .globl ZwOpenSemaphore
    .set ZwOpenSemaphore, NtOpenSemaphore
    .globl NtReleaseSemaphore
NtReleaseSemaphore:
    mov %rcx, %r10
    mov $0x000a, %eax
    syscall
    ret
    .globl ZwReleaseSemaphore
    .set ZwReleaseSemaphore, NtReleaseSemaphore
    .globl NtQuerySemaphore
NtQuerySemaphore:
    mov %rcx, %r10
    mov $0x0163, %eax
    syscall
    ret
    .globl ZwQuerySemaphore
    .set ZwQuerySemaphore, NtQuerySemaphore
    .globl NtCreateTimer
NtCreateTimer:
    mov %rcx, %r10
    mov $0x00c7, %eax
    syscall
    ret
    .globl ZwCreateTimer
    .set ZwCreateTimer, NtCreateTimer
    .globl NtOpenTimer
NtOpenTimer:
    mov %rcx, %r10
    mov $0x0135, %eax
    syscall
    ret
    .globl ZwOpenTimer
    .set ZwOpenTimer, NtOpenTimer
    .globl NtSetTimer
NtSetTimer:
    mov %rcx, %r10
    mov $0x0062, %eax
    syscall
    ret
    .globl ZwSetTimer
    .set ZwSetTimer, NtSetTimer
    .globl NtCancelTimer
NtCancelTimer:
    mov %rcx, %r10
    mov $0x0061, %eax
    syscall
    ret
    .globl ZwCancelTimer
    .set ZwCancelTimer, NtCancelTimer
    .globl NtQueryTimer
NtQueryTimer:
    mov %rcx, %r10
    mov $0x0038, %eax
    syscall
    ret
    .globl ZwQueryTimer
    .set ZwQueryTimer, NtQueryTimer
    .globl NtWaitForSingleObject
NtWaitForSingleObject:
    mov %rcx, %r10
    mov $0x0004, %eax
    syscall
    ret
    .globl ZwWaitForSingleObject
    .set ZwWaitForSingleObject, NtWaitForSingleObject
    .globl NtWaitForMultipleObjects
NtWaitForMultipleObjects:
    mov %rcx, %r10
    mov $0x005b, %eax
    syscall
    ret
    .globl ZwWaitForMultipleObjects
    .set ZwWaitForMultipleObjects, NtWaitForMultipleObjects
    .globl NtSignalAndWaitForSingleObject
NtSignalAndWaitForSingleObject:
    mov %rcx, %r10
    mov $0x01be, %eax
    syscall
    ret
    .globl ZwSignalAndWaitForSingleObject
    .set ZwSignalAndWaitForSingleObject, NtSignalAndWaitForSingleObject
    .globl NtCreateKeyedEvent
NtCreateKeyedEvent:
    mov %rcx, %r10
    mov $0x00b3, %eax
    syscall
    ret
    .globl ZwCreateKeyedEvent
    .set ZwCreateKeyedEvent, NtCreateKeyedEvent
    .globl NtOpenKeyedEvent
NtOpenKeyedEvent:
    mov %rcx, %r10
    mov $0x0129, %eax
    syscall
    ret
    .globl ZwOpenKeyedEvent
    .set ZwOpenKeyedEvent, NtOpenKeyedEvent
    .globl NtReleaseKeyedEvent
NtReleaseKeyedEvent:
    mov %rcx, %r10
    mov $0x0176, %eax
    syscall
    ret
    .globl ZwReleaseKeyedEvent
    .set ZwReleaseKeyedEvent, NtReleaseKeyedEvent
    .globl NtWaitForKeyedEvent
NtWaitForKeyedEvent:
    mov %rcx, %r10
    mov $0x01da, %eax
    syscall
    ret
    .globl ZwWaitForKeyedEvent
    .set ZwWaitForKeyedEvent, NtWaitForKeyedEvent
    .globl NtCreateIoCompletion
NtCreateIoCompletion:
    mov %rcx, %r10
    mov $0x00af, %eax
    syscall
    ret
    .globl ZwCreateIoCompletion
    .set ZwCreateIoCompletion, NtCreateIoCompletion
    .globl NtOpenIoCompletion
NtOpenIoCompletion:
    mov %rcx, %r10
    mov $0x0124, %eax
    syscall
    ret
    .globl ZwOpenIoCompletion
    .set ZwOpenIoCompletion, NtOpenIoCompletion
    .globl NtSetIoCompletion
NtSetIoCompletion:
    mov %rcx, %r10
    mov $0x01a9, %eax
    syscall
    ret
    .globl ZwSetIoCompletion
    .set ZwSetIoCompletion, NtSetIoCompletion
    .globl NtSetIoCompletionEx
NtSetIoCompletionEx:
    mov %rcx, %r10
    mov $0x01aa, %eax
    syscall
    ret
    .globl ZwSetIoCompletionEx
    .set ZwSetIoCompletionEx, NtSetIoCompletionEx
    .globl NtRemoveIoCompletion
NtRemoveIoCompletion:
    mov %rcx, %r10
    mov $0x0009, %eax
    syscall
    ret
    .globl ZwRemoveIoCompletion
    .set ZwRemoveIoCompletion, NtRemoveIoCompletion
    .globl NtRemoveIoCompletionEx
NtRemoveIoCompletionEx:
    mov %rcx, %r10
    mov $0x0178, %eax
    syscall
    ret
    .globl ZwRemoveIoCompletionEx
    .set ZwRemoveIoCompletionEx, NtRemoveIoCompletionEx
    .globl NtQueryIoCompletion
NtQueryIoCompletion:
    mov %rcx, %r10
    mov $0x0158, %eax
    syscall
    ret
    .globl ZwQueryIoCompletion
    .set ZwQueryIoCompletion, NtQueryIoCompletion
    .globl NtQueryInformationProcess
NtQueryInformationProcess:
    mov %rcx, %r10
    mov $0x0019, %eax
    syscall
    ret
    .globl ZwQueryInformationProcess
    .set ZwQueryInformationProcess, NtQueryInformationProcess
    .globl NtSetInformationProcess
NtSetInformationProcess:
    mov %rcx, %r10
    mov $0x001c, %eax
    syscall
    ret
    .globl ZwSetInformationProcess
    .set ZwSetInformationProcess, NtSetInformationProcess
    .globl NtQueryInformationThread
NtQueryInformationThread:
    mov %rcx, %r10
    mov $0x0025, %eax
    syscall
    ret
    .globl ZwQueryInformationThread
    .set ZwQueryInformationThread, NtQueryInformationThread
    .globl NtSetInformationThread
NtSetInformationThread:
    mov %rcx, %r10
    mov $0x000d, %eax
    syscall
    ret
    .globl ZwSetInformationThread
    .set ZwSetInformationThread, NtSetInformationThread
    .globl NtOpenProcessToken
NtOpenProcessToken:
    mov %rcx, %r10
    mov $0x012e, %eax
    syscall
    ret
    .globl ZwOpenProcessToken
    .set ZwOpenProcessToken, NtOpenProcessToken
    .globl NtOpenProcessTokenEx
NtOpenProcessTokenEx:
    mov %rcx, %r10
    mov $0x0030, %eax
    syscall
    ret
    .globl ZwOpenProcessTokenEx
    .set ZwOpenProcessTokenEx, NtOpenProcessTokenEx
    .globl NtOpenThreadToken
NtOpenThreadToken:
    mov %rcx, %r10
    mov $0x0024, %eax
    syscall
    ret
    .globl ZwOpenThreadToken
    .set ZwOpenThreadToken, NtOpenThreadToken
    .globl NtOpenThreadTokenEx
NtOpenThreadTokenEx:
    mov %rcx, %r10
    mov $0x002f, %eax
    syscall
    ret
    .globl ZwOpenThreadTokenEx
    .set ZwOpenThreadTokenEx, NtOpenThreadTokenEx
    .globl NtDuplicateToken
NtDuplicateToken:
    mov %rcx, %r10
    mov $0x0042, %eax
    syscall
    ret
    .globl ZwDuplicateToken
    .set ZwDuplicateToken, NtDuplicateToken
    .globl NtQueryInformationToken
NtQueryInformationToken:
    mov %rcx, %r10
    mov $0x0021, %eax
    syscall
    ret
    .globl ZwQueryInformationToken
    .set ZwQueryInformationToken, NtQueryInformationToken
    .globl NtSetInformationToken
NtSetInformationToken:
    mov %rcx, %r10
    mov $0x01a3, %eax
    syscall
    ret
    .globl ZwSetInformationToken
    .set ZwSetInformationToken, NtSetInformationToken
    .globl NtAdjustPrivilegesToken
NtAdjustPrivilegesToken:
    mov %rcx, %r10
    mov $0x0041, %eax
    syscall
    ret
    .globl ZwAdjustPrivilegesToken
    .set ZwAdjustPrivilegesToken, NtAdjustPrivilegesToken
    .globl NtCreateKey
NtCreateKey:
    mov %rcx, %r10
    mov $0x001d, %eax
    syscall
    ret
    .globl ZwCreateKey
    .set ZwCreateKey, NtCreateKey
    .globl NtOpenKey
NtOpenKey:
    mov %rcx, %r10
    mov $0x0012, %eax
    syscall
    ret
    .globl ZwOpenKey
    .set ZwOpenKey, NtOpenKey
    .globl NtOpenKeyEx
NtOpenKeyEx:
    mov %rcx, %r10
    mov $0x0126, %eax
    syscall
    ret
    .globl ZwOpenKeyEx
    .set ZwOpenKeyEx, NtOpenKeyEx
    .globl NtDeleteKey
NtDeleteKey:
    mov %rcx, %r10
    mov $0x00d8, %eax
    syscall
    ret
    .globl ZwDeleteKey
    .set ZwDeleteKey, NtDeleteKey
    .globl NtFlushKey
NtFlushKey:
    mov %rcx, %r10
    mov $0x00ee, %eax
    syscall
    ret
    .globl ZwFlushKey
    .set ZwFlushKey, NtFlushKey
    .globl NtEnumerateKey
NtEnumerateKey:
    mov %rcx, %r10
    mov $0x0032, %eax
    syscall
    ret
    .globl ZwEnumerateKey
    .set ZwEnumerateKey, NtEnumerateKey
    .globl NtEnumerateValueKey
NtEnumerateValueKey:
    mov %rcx, %r10
    mov $0x0013, %eax
    syscall
    ret
    .globl ZwEnumerateValueKey
    .set ZwEnumerateValueKey, NtEnumerateValueKey
    .globl NtQueryKey
NtQueryKey:
    mov %rcx, %r10
    mov $0x0016, %eax
    syscall
    ret
    .globl ZwQueryKey
    .set ZwQueryKey, NtQueryKey
    .globl NtQueryValueKey
NtQueryValueKey:
    mov %rcx, %r10
    mov $0x0017, %eax
    syscall
    ret
    .globl ZwQueryValueKey
    .set ZwQueryValueKey, NtQueryValueKey
    .globl NtSetValueKey
NtSetValueKey:
    mov %rcx, %r10
    mov $0x0060, %eax
    syscall
    ret
    .globl ZwSetValueKey
    .set ZwSetValueKey, NtSetValueKey
    .globl NtDeleteValueKey
NtDeleteValueKey:
    mov %rcx, %r10
    mov $0x00db, %eax
    syscall
    ret
    .globl ZwDeleteValueKey
    .set ZwDeleteValueKey, NtDeleteValueKey
    .globl NtQuerySystemInformation
NtQuerySystemInformation:
    mov %rcx, %r10
    mov $0x0036, %eax
    syscall
    ret
    .globl ZwQuerySystemInformation
    .set ZwQuerySystemInformation, NtQuerySystemInformation
    .globl NtQuerySystemInformationEx
NtQuerySystemInformationEx:
    mov %rcx, %r10
    mov $0x0167, %eax
    syscall
    ret
    .globl ZwQuerySystemInformationEx
    .set ZwQuerySystemInformationEx, NtQuerySystemInformationEx
    .globl NtSetSystemInformation
NtSetSystemInformation:
    mov %rcx, %r10
    mov $0x01b2, %eax
    syscall
    ret
    .globl ZwSetSystemInformation
    .set ZwSetSystemInformation, NtSetSystemInformation
    .globl NtQueryPerformanceCounter
NtQueryPerformanceCounter:
    mov %rcx, %r10
    mov $0x0031, %eax
    syscall
    ret
    .globl ZwQueryPerformanceCounter
    .set ZwQueryPerformanceCounter, NtQueryPerformanceCounter
    .globl NtQuerySystemTime
NtQuerySystemTime:
    mov %rcx, %r10
    mov $0x005a, %eax
    syscall
    ret
    .globl ZwQuerySystemTime
    .set ZwQuerySystemTime, NtQuerySystemTime
    .globl NtDelayExecution
NtDelayExecution:
    mov %rcx, %r10
    mov $0x0034, %eax
    syscall
    ret
    .globl ZwDelayExecution
    .set ZwDelayExecution, NtDelayExecution
    .globl NtYieldExecution
NtYieldExecution:
    mov %rcx, %r10
    mov $0x0046, %eax
    syscall
    ret
    .globl ZwYieldExecution
    .set ZwYieldExecution, NtYieldExecution
    .globl NtSetTimerResolution
NtSetTimerResolution:
    mov %rcx, %r10
    mov $0x01b8, %eax
    syscall
    ret
    .globl ZwSetTimerResolution
    .set ZwSetTimerResolution, NtSetTimerResolution
    .globl NtQueryTimerResolution
NtQueryTimerResolution:
    mov %rcx, %r10
    mov $0x0168, %eax
    syscall
    ret
    .globl ZwQueryTimerResolution
    .set ZwQueryTimerResolution, NtQueryTimerResolution
    .globl NtContinue
NtContinue:
    mov %rcx, %r10
    mov $0x0043, %eax
    syscall
    ret
    .globl ZwContinue
    .set ZwContinue, NtContinue
    .globl NtContinueEx
NtContinueEx:
    mov %rcx, %r10
    mov $0x00a3, %eax
    syscall
    ret
    .globl ZwContinueEx
    .set ZwContinueEx, NtContinueEx
    .globl NtRaiseException
NtRaiseException:
    mov %rcx, %r10
    mov $0x016d, %eax
    syscall
    ret
    .globl ZwRaiseException
    .set ZwRaiseException, NtRaiseException
    .globl NtRaiseHardError
NtRaiseHardError:
    mov %rcx, %r10
    mov $0x016e, %eax
    syscall
    ret
    .globl ZwRaiseHardError
    .set ZwRaiseHardError, NtRaiseHardError
    .globl NtCreateDebugObject
NtCreateDebugObject:
    mov %rcx, %r10
    mov $0x00a8, %eax
    syscall
    ret
    .globl ZwCreateDebugObject
    .set ZwCreateDebugObject, NtCreateDebugObject
    .globl NtDebugActiveProcess
NtDebugActiveProcess:
    mov %rcx, %r10
    mov $0x00d2, %eax
    syscall
    ret
    .globl ZwDebugActiveProcess
    .set ZwDebugActiveProcess, NtDebugActiveProcess
    .globl NtDebugContinue
NtDebugContinue:
    mov %rcx, %r10
    mov $0x00d3, %eax
    syscall
    ret
    .globl ZwDebugContinue
    .set ZwDebugContinue, NtDebugContinue
    .globl NtWaitForDebugEvent
NtWaitForDebugEvent:
    mov %rcx, %r10
    mov $0x01d9, %eax
    syscall
    ret
    .globl ZwWaitForDebugEvent
    .set ZwWaitForDebugEvent, NtWaitForDebugEvent
    .globl NtRemoveProcessDebug
NtRemoveProcessDebug:
    mov %rcx, %r10
    mov $0x0179, %eax
    syscall
    ret
    .globl ZwRemoveProcessDebug
    .set ZwRemoveProcessDebug, NtRemoveProcessDebug
    .globl NtCreatePort
NtCreatePort:
    mov %rcx, %r10
    mov $0x00ba, %eax
    syscall
    ret
    .globl ZwCreatePort
    .set ZwCreatePort, NtCreatePort
    .globl NtConnectPort
NtConnectPort:
    mov %rcx, %r10
    mov $0x00a2, %eax
    syscall
    ret
    .globl ZwConnectPort
    .set ZwConnectPort, NtConnectPort
    .globl NtListenPort
NtListenPort:
    mov %rcx, %r10
    mov $0x0109, %eax
    syscall
    ret
    .globl ZwListenPort
    .set ZwListenPort, NtListenPort
    .globl NtAcceptConnectPort
NtAcceptConnectPort:
    mov %rcx, %r10
    mov $0x0002, %eax
    syscall
    ret
    .globl ZwAcceptConnectPort
    .set ZwAcceptConnectPort, NtAcceptConnectPort
    .globl NtCompleteConnectPort
NtCompleteConnectPort:
    mov %rcx, %r10
    mov $0x00a0, %eax
    syscall
    ret
    .globl ZwCompleteConnectPort
    .set ZwCompleteConnectPort, NtCompleteConnectPort
    .globl NtRequestWaitReplyPort
NtRequestWaitReplyPort:
    mov %rcx, %r10
    mov $0x0022, %eax
    syscall
    ret
    .globl ZwRequestWaitReplyPort
    .set ZwRequestWaitReplyPort, NtRequestWaitReplyPort
    .globl NtReplyPort
NtReplyPort:
    mov %rcx, %r10
    mov $0x000c, %eax
    syscall
    ret
    .globl ZwReplyPort
    .set ZwReplyPort, NtReplyPort
    .globl NtReplyWaitReceivePort
NtReplyWaitReceivePort:
    mov %rcx, %r10
    mov $0x000b, %eax
    syscall
    ret
    .globl ZwReplyWaitReceivePort
    .set ZwReplyWaitReceivePort, NtReplyWaitReceivePort
    .globl NtReplyWaitReceivePortEx
NtReplyWaitReceivePortEx:
    mov %rcx, %r10
    mov $0x002b, %eax
    syscall
    ret
    .globl ZwReplyWaitReceivePortEx
    .set ZwReplyWaitReceivePortEx, NtReplyWaitReceivePortEx
    .globl NtReplyWaitReplyPort
NtReplyWaitReplyPort:
    mov %rcx, %r10
    mov $0x017e, %eax
    syscall
    ret
    .globl ZwReplyWaitReplyPort
    .set ZwReplyWaitReplyPort, NtReplyWaitReplyPort
    .globl NtAccessCheck
NtAccessCheck:
    mov %rcx, %r10
    mov $0x0000, %eax
    syscall
    ret
    .globl ZwAccessCheck
    .set ZwAccessCheck, NtAccessCheck
    .globl NtPrivilegeCheck
NtPrivilegeCheck:
    mov %rcx, %r10
    mov $0x013d, %eax
    syscall
    ret
    .globl ZwPrivilegeCheck
    .set ZwPrivilegeCheck, NtPrivilegeCheck
    .globl NtSetSecurityObject
NtSetSecurityObject:
    mov %rcx, %r10
    mov $0x01af, %eax
    syscall
    ret
    .globl ZwSetSecurityObject
    .set ZwSetSecurityObject, NtSetSecurityObject
    .globl NtQuerySecurityObject
NtQuerySecurityObject:
    mov %rcx, %r10
    mov $0x0161, %eax
    syscall
    ret
    .globl ZwQuerySecurityObject
    .set ZwQuerySecurityObject, NtQuerySecurityObject
    .globl NtCloseObjectAuditAlarm
NtCloseObjectAuditAlarm:
    mov %rcx, %r10
    mov $0x003b, %eax
    syscall
    ret
    .globl ZwCloseObjectAuditAlarm
    .set ZwCloseObjectAuditAlarm, NtCloseObjectAuditAlarm
    .globl NtAccessCheckAndAuditAlarm
NtAccessCheckAndAuditAlarm:
    mov %rcx, %r10
    mov $0x0029, %eax
    syscall
    ret
    .globl ZwAccessCheckAndAuditAlarm
    .set ZwAccessCheckAndAuditAlarm, NtAccessCheckAndAuditAlarm
    .globl NtCreateJobObject
NtCreateJobObject:
    mov %rcx, %r10
    mov $0x00b0, %eax
    syscall
    ret
    .globl ZwCreateJobObject
    .set ZwCreateJobObject, NtCreateJobObject
    .globl NtOpenJobObject
NtOpenJobObject:
    mov %rcx, %r10
    mov $0x0125, %eax
    syscall
    ret
    .globl ZwOpenJobObject
    .set ZwOpenJobObject, NtOpenJobObject
    .globl NtAssignProcessToJobObject
NtAssignProcessToJobObject:
    mov %rcx, %r10
    mov $0x008f, %eax
    syscall
    ret
    .globl ZwAssignProcessToJobObject
    .set ZwAssignProcessToJobObject, NtAssignProcessToJobObject
    .globl NtTerminateJobObject
NtTerminateJobObject:
    mov %rcx, %r10
    mov $0x01c7, %eax
    syscall
    ret
    .globl ZwTerminateJobObject
    .set ZwTerminateJobObject, NtTerminateJobObject
    .globl NtQueryInformationJobObject
NtQueryInformationJobObject:
    mov %rcx, %r10
    mov $0x0150, %eax
    syscall
    ret
    .globl ZwQueryInformationJobObject
    .set ZwQueryInformationJobObject, NtQueryInformationJobObject
    .globl NtSetInformationJobObject
NtSetInformationJobObject:
    mov %rcx, %r10
    mov $0x019f, %eax
    syscall
    ret
    .globl ZwSetInformationJobObject
    .set ZwSetInformationJobObject, NtSetInformationJobObject
    .globl NtCreateDirectoryObject
NtCreateDirectoryObject:
    mov %rcx, %r10
    mov $0x00a9, %eax
    syscall
    ret
    .globl ZwCreateDirectoryObject
    .set ZwCreateDirectoryObject, NtCreateDirectoryObject
    .globl NtOpenDirectoryObject
NtOpenDirectoryObject:
    mov %rcx, %r10
    mov $0x0058, %eax
    syscall
    ret
    .globl ZwOpenDirectoryObject
    .set ZwOpenDirectoryObject, NtOpenDirectoryObject
    .globl NtQueryDirectoryObject
NtQueryDirectoryObject:
    mov %rcx, %r10
    mov $0x0149, %eax
    syscall
    ret
    .globl ZwQueryDirectoryObject
    .set ZwQueryDirectoryObject, NtQueryDirectoryObject
    .globl NtCreateSymbolicLinkObject
NtCreateSymbolicLinkObject:
    mov %rcx, %r10
    mov $0x00c4, %eax
    syscall
    ret
    .globl ZwCreateSymbolicLinkObject
    .set ZwCreateSymbolicLinkObject, NtCreateSymbolicLinkObject
    .globl NtOpenSymbolicLinkObject
NtOpenSymbolicLinkObject:
    mov %rcx, %r10
    mov $0x0133, %eax
    syscall
    ret
    .globl ZwOpenSymbolicLinkObject
    .set ZwOpenSymbolicLinkObject, NtOpenSymbolicLinkObject
    .globl NtQuerySymbolicLinkObject
NtQuerySymbolicLinkObject:
    mov %rcx, %r10
    mov $0x0164, %eax
    syscall
    ret
    .globl ZwQuerySymbolicLinkObject
    .set ZwQuerySymbolicLinkObject, NtQuerySymbolicLinkObject
    .globl NtRtlNtStatusToDosError
NtRtlNtStatusToDosError:
    mov %rcx, %r10
    mov $0x01db, %eax
    syscall
    ret
    .globl ZwRtlNtStatusToDosError
    .set ZwRtlNtStatusToDosError, NtRtlNtStatusToDosError
    .globl NtLoadDriver
NtLoadDriver:
    mov %rcx, %r10
    mov $0x0106, %eax
    syscall
    ret
    .globl ZwLoadDriver
    .set ZwLoadDriver, NtLoadDriver
    .globl NtUnloadDriver
NtUnloadDriver:
    mov %rcx, %r10
    mov $0x0107, %eax
    syscall
    ret
    .globl ZwUnloadDriver
    .set ZwUnloadDriver, NtUnloadDriver

