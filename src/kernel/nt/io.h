/* SPDX-License-Identifier: GPL-2.0 */
/*
 * io.h - NT I/O manager public header (kernel/nt workstream).
 *
 * Defines the file object, NT path semantics, the share-access enforcement
 * registry, and the Nt* service entry points consumed by the dispatch table.
 */

#ifndef NTIO_IO_H
#define NTIO_IO_H

#include "../../ntdll/nt_types.h"
#include "../../ntdll/ntstatus.h"
#include "kshim.h"
#ifdef __KERNEL__
#include <linux/fs.h>
#include <linux/dcache.h>
#include <linux/file.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/* NT file access rights                                                        */
/* -------------------------------------------------------------------------- */

#define FILE_READ_DATA          0x0001
#define FILE_WRITE_DATA         0x0002
#define FILE_APPEND_DATA        0x0004
#define FILE_READ_EA            0x0008
#define FILE_WRITE_EA           0x0010
#define FILE_EXECUTE            0x0020
#define FILE_READ_ATTRIBUTES    0x0080
#define FILE_WRITE_ATTRIBUTES   0x0100

#define DELETE                  0x00010000
#define READ_CONTROL            0x00020000
#define WRITE_DAC               0x00040000
#define WRITE_OWNER             0x00080000
#define SYNCHRONIZE             0x00100000

#define FILE_ALL_ACCESS         0x001F01FF
#define FILE_GENERIC_READ       0x00120089
#define FILE_GENERIC_WRITE      0x00120116
#define FILE_GENERIC_EXECUTE    0x001200A0

/* -------------------------------------------------------------------------- */
/* Share access                                                                 */
/* -------------------------------------------------------------------------- */

#define FILE_SHARE_READ         0x00000001
#define FILE_SHARE_WRITE        0x00000002
#define FILE_SHARE_DELETE       0x00000004
#define FILE_SHARE_VALID_FLAGS  0x00000007

/* -------------------------------------------------------------------------- */
/* Create disposition                                                           */
/* -------------------------------------------------------------------------- */

#define FILE_SUPERSEDE          0x00000000
#define FILE_OPEN               0x00000001
#define FILE_CREATE             0x00000002
#define FILE_OPEN_IF            0x00000003
#define FILE_OVERWRITE          0x00000004
#define FILE_OVERWRITE_IF       0x00000005
#define FILE_MAXIMUM_DISPOSITION 0x00000005

/* -------------------------------------------------------------------------- */
/* Create options                                                               */
/* -------------------------------------------------------------------------- */

#define FILE_DIRECTORY_FILE             0x00000001
#define FILE_WRITE_THROUGH              0x00000002
#define FILE_SEQUENTIAL_ONLY            0x00000004
#define FILE_NO_INTERMEDIATE_BUFFERING  0x00000008
#define FILE_SYNCHRONOUS_IO_ALERT       0x00000010
#define FILE_SYNCHRONOUS_IO_NONALERT    0x00000020
#define FILE_NON_DIRECTORY_FILE         0x00000040
#define FILE_CREATE_TREE_CONNECTION     0x00000080
#define FILE_COMPLETE_IF_OPLOCKED       0x00000100
#define FILE_NO_EA_KNOWLEDGE            0x00000200
#define FILE_OPEN_REMOTE_INSTANCE       0x00000400
#define FILE_RANDOM_ACCESS              0x00000800
#define FILE_DELETE_ON_CLOSE            0x00001000
#define FILE_OPEN_BY_FILE_ID            0x00002000
#define FILE_OPEN_FOR_BACKUP_INTENT     0x00004000
#define FILE_NO_COMPRESSION             0x00008000
#define FILE_OPEN_REPARSE_POINT         0x00200000
#define FILE_DISALLOW_EXCLUSIVE         0x00020000

#define FILE_VALID_OPTION_FLAGS         0x00FFFFFF
#define FILE_VALID_MANDATORY_FLAGS      0x0000FFFF
#define FILE_VALID_PIPE_OPTION_FLAGS    FILE_VALID_OPTION_FLAGS

/* -------------------------------------------------------------------------- */
/* IoStatus.Information values                                                  */
/* -------------------------------------------------------------------------- */

#define FILE_SUPERSEDED     0
#define FILE_OPENED         1
#define FILE_CREATED        2
#define FILE_OVERWRITTEN    3
#define FILE_EXISTS         4
#define FILE_DOES_NOT_EXIST 5

/* -------------------------------------------------------------------------- */
/* File attributes                                                              */
/* -------------------------------------------------------------------------- */

#define FILE_ATTRIBUTE_READONLY             0x00000001
#define FILE_ATTRIBUTE_HIDDEN               0x00000002
#define FILE_ATTRIBUTE_SYSTEM               0x00000004
#define FILE_ATTRIBUTE_DIRECTORY            0x00000010
#define FILE_ATTRIBUTE_ARCHIVE              0x00000020
#define FILE_ATTRIBUTE_DEVICE               0x00000040
#define FILE_ATTRIBUTE_NORMAL               0x00000080
#define FILE_ATTRIBUTE_TEMPORARY            0x00000100
#define FILE_ATTRIBUTE_SPARSE_FILE          0x00000200
#define FILE_ATTRIBUTE_REPARSE_POINT        0x00000400
#define FILE_ATTRIBUTE_COMPRESSED           0x00000800
#define FILE_ATTRIBUTE_OFFLINE              0x00001000
#define FILE_ATTRIBUTE_NOT_CONTENT_INDEXED  0x00002000
#define FILE_ATTRIBUTE_ENCRYPTED            0x00004000
#define FILE_ATTRIBUTE_INTEGRITY_STREAM     0x00008000
#define FILE_ATTRIBUTE_NO_SCRUB_DATA        0x00020000

/* -------------------------------------------------------------------------- */
/* FileInformationClass values                                                  */
/* -------------------------------------------------------------------------- */

#define FileDirectoryInformation        1
#define FileFullDirectoryInformation    2
#define FileBothDirectoryInformation    3
#define FileBasicInformation            4
#define FileStandardInformation         5
#define FileInternalInformation         6
#define FileEaInformation               7
#define FileAccessInformation           8
#define FileNameInformation             9
#define FileRenameInformation           10
#define FileLinkInformation             11
#define FileNamesInformation            12
#define FileDispositionInformation      13
#define FilePositionInformation         14
#define FileFullEaInformation           15
#define FileModeInformation             16
#define FileAlignmentInformation        17
#define FileAllInformation              18
#define FileAllocationInformation       19
#define FileEndOfFileInformation        20
#define FileAlternateNameInformation    21
#define FileStreamInformation           22
#define FilePipeInformation             23
#define FilePipeLocalInformation        24
#define FilePipeRemoteInformation       25
#define FileMailslotQueryInformation    26
#define FileMailslotSetInformation      27
#define FileCompressionInformation      28
#define FileObjectIdInformation         29
#define FileCompletionInformation       30
#define FileMoveDeviceInformation       31
#define FileQuotaInformation            32
#define FileReparsePointInformation     33
#define FileNetworkOpenInformation      34
#define FileAttributeTagInformation     35
#define FileTrackingInformation         36
#define FileIdBothDirectoryInformation  37
#define FileIdFullDirectoryInformation  38
#define FileValidDataLengthInformation  39
#define FileShortNameInformation        40
#define FileIoCompletionNotificationInformation 41
#define FileIoStatusBlockRangeInformation       42
#define FileIoPriorityHintInformation   43
#define FileSfioReserveInformation      44
#define FileSfioVolumeInformation       45
#define FileHardLinkInformation         46
#define FileProcessIdsUsingFileInformation 47
#define FileNormalizedNameInformation   48
#define FileNetworkPhysicalNameInformation 49
#define FileIdGlobalTxDirectoryInformation 50
#define FileIsRemoteDeviceInformation   51
#define FileUnusedInformation           52
#define FileNumaNodeInformation         53
#define FileStandardLinkInformation     54
#define FileRemoteProtocolInformation   55
#define FileRenameInformationBypassAccessCheck  56
#define FileLinkInformationBypassAccessCheck    57
#define FileVolumeNameInformation       58
#define FileIdInformation               59
#define FileIdExtdDirectoryInformation  60
#define FileReplaceCompletionInformation 61
#define FileHardLinkFullIdInformation   62
#define FileIdExtdBothDirectoryInformation 63
#define FileDispositionInformationEx    64
#define FileRenameInformationEx         65
#define FileRenameInformationExBypassAccessCheck 66
#define FileDesiredStorageClassInformation 67
#define FileStatInformation             68
#define FileMemoryPartitionInformation  69
#define FileStatLxInformation           70
#define FileCaseSensitiveInformation    71

/* FsInformationClass values (NtQueryVolumeInformationFile). */
#define FileFsVolumeInformation         1
#define FileFsLabelInformation          2
#define FileFsSizeInformation           3
#define FileFsDeviceInformation         4
#define FileFsAttributeInformation      5
#define FileFsControlInformation        6
#define FileFsFullSizeInformation       7
#define FileFsObjectIdInformation       8
#define FileFsDriverPathInformation     9
#define FileFsVolumeFlagsInformation    10
#define FileFsSectorSizeInformation     11
#define FileFsDataCopyInformation       12
#define FILE_DEVICE_DISK                0x00000007
#define FILE_DEVICE_NAMED_PIPE          0x00000011
#define FILE_DEVICE_CONSOLE             0x00000050

/* -------------------------------------------------------------------------- */
/* Information structures                                                       */
/* -------------------------------------------------------------------------- */

typedef struct _FILE_BASIC_INFORMATION {
	LARGE_INTEGER CreationTime;
	LARGE_INTEGER LastAccessTime;
	LARGE_INTEGER LastWriteTime;
	LARGE_INTEGER ChangeTime;
	ULONG FileAttributes;
} FILE_BASIC_INFORMATION, *PFILE_BASIC_INFORMATION;

typedef struct _FILE_STANDARD_INFORMATION {
	LARGE_INTEGER AllocationSize;
	LARGE_INTEGER EndOfFile;
	ULONG NumberOfLinks;
	BOOLEAN DeletePending;
	BOOLEAN Directory;
} FILE_STANDARD_INFORMATION, *PFILE_STANDARD_INFORMATION;

typedef struct _FILE_POSITION_INFORMATION {
	LARGE_INTEGER CurrentByteOffset;
} FILE_POSITION_INFORMATION, *PFILE_POSITION_INFORMATION;

typedef struct _FILE_NAME_INFORMATION {
	ULONG FileNameLength;
	WCHAR FileName[1];
} FILE_NAME_INFORMATION, *PFILE_NAME_INFORMATION;

typedef struct _FILE_DISPOSITION_INFORMATION {
	BOOLEAN DeleteFile;
} FILE_DISPOSITION_INFORMATION, *PFILE_DISPOSITION_INFORMATION;

typedef struct _FILE_RENAME_INFORMATION {
	BOOLEAN ReplaceIfExists;
	HANDLE RootDirectory;
	ULONG FileNameLength;
	WCHAR FileName[1];
} FILE_RENAME_INFORMATION, *PFILE_RENAME_INFORMATION;

typedef struct _FILE_END_OF_FILE_INFORMATION {
	LARGE_INTEGER EndOfFile;
} FILE_END_OF_FILE_INFORMATION, *PFILE_END_OF_FILE_INFORMATION;

/* Sub-structures referenced by FILE_ALL_INFORMATION */
typedef struct _FILE_INTERNAL_INFORMATION {
	LARGE_INTEGER IndexNumber;
} FILE_INTERNAL_INFORMATION, *PFILE_INTERNAL_INFORMATION;

typedef struct _FILE_EA_INFORMATION {
	ULONG EaSize;
} FILE_EA_INFORMATION, *PFILE_EA_INFORMATION;

typedef struct _FILE_ACCESS_INFORMATION {
	ACCESS_MASK AccessFlags;
} FILE_ACCESS_INFORMATION, *PFILE_ACCESS_INFORMATION;

typedef struct _FILE_MODE_INFORMATION {
	ULONG Mode;
} FILE_MODE_INFORMATION, *PFILE_MODE_INFORMATION;

typedef struct _FILE_ALIGNMENT_INFORMATION {
	ULONG AlignmentRequirement;
} FILE_ALIGNMENT_INFORMATION, *PFILE_ALIGNMENT_INFORMATION;

typedef struct _FILE_NETWORK_OPEN_INFORMATION {
	LARGE_INTEGER CreationTime;
	LARGE_INTEGER LastAccessTime;
	LARGE_INTEGER LastWriteTime;
	LARGE_INTEGER ChangeTime;
	LARGE_INTEGER AllocationSize;
	LARGE_INTEGER EndOfFile;
	ULONG FileAttributes;
} FILE_NETWORK_OPEN_INFORMATION, *PFILE_NETWORK_OPEN_INFORMATION;

typedef struct _FILE_ATTRIBUTE_TAG_INFORMATION {
	ULONG FileAttributes;
	ULONG ReparseTag;
} FILE_ATTRIBUTE_TAG_INFORMATION, *PFILE_ATTRIBUTE_TAG_INFORMATION;

/* Create options that FileModeInformation reports (NT Mode bits). */
#define FILE_MODE_OPTIONS_MASK (FILE_WRITE_THROUGH | FILE_SEQUENTIAL_ONLY | 				FILE_NO_INTERMEDIATE_BUFFERING | 				FILE_SYNCHRONOUS_IO_ALERT | 				FILE_SYNCHRONOUS_IO_NONALERT | FILE_DELETE_ON_CLOSE)

typedef struct _FILE_ALLOCATION_INFORMATION {
	LARGE_INTEGER AllocationSize;
} FILE_ALLOCATION_INFORMATION, *PFILE_ALLOCATION_INFORMATION;

typedef struct _FILE_DISPOSITION_INFORMATION_EX {
	ULONG Flags;
} FILE_DISPOSITION_INFORMATION_EX, *PFILE_DISPOSITION_INFORMATION_EX;

typedef struct _FILE_RENAME_INFORMATION_EX {
	ULONG Flags;
	HANDLE RootDirectory;
	ULONG FileNameLength;
	WCHAR FileName[1];
} FILE_RENAME_INFORMATION_EX, *PFILE_RENAME_INFORMATION_EX;

/* FILE_DISPOSITION_INFORMATION_EX.Flags and FILE_RENAME_INFORMATION_EX.Flags. */
#define FILE_DISPOSITION_DELETE         0x00000001
#define FILE_RENAME_REPLACE_IF_EXISTS   0x00000001

typedef struct _FILE_FS_DEVICE_INFORMATION {
	ULONG DeviceType;
	ULONG Characteristics;
} FILE_FS_DEVICE_INFORMATION, *PFILE_FS_DEVICE_INFORMATION;

/* Volume classes of NtQueryVolumeInformationFile (sizes as in winternl / ntifs). */
typedef struct _FILE_FS_VOLUME_INFORMATION {
	LARGE_INTEGER VolumeCreationTime;
	ULONG VolumeSerialNumber;
	ULONG VolumeLabelLength;
	BOOLEAN SupportsObjects;
	WCHAR VolumeLabel[1];
} FILE_FS_VOLUME_INFORMATION, *PFILE_FS_VOLUME_INFORMATION;

typedef struct _FILE_FS_SIZE_INFORMATION {
	LARGE_INTEGER TotalAllocationUnits;
	LARGE_INTEGER AvailableAllocationUnits;
	ULONG SectorsPerAllocationUnit;
	ULONG BytesPerSector;
} FILE_FS_SIZE_INFORMATION, *PFILE_FS_SIZE_INFORMATION;

typedef struct _FILE_FS_ATTRIBUTE_INFORMATION {
	ULONG FileSystemAttributes;
	LONG MaximumComponentNameLength;
	ULONG FileSystemNameLength;
	WCHAR FileSystemName[1];
} FILE_FS_ATTRIBUTE_INFORMATION, *PFILE_FS_ATTRIBUTE_INFORMATION;

typedef struct _FILE_FS_CONTROL_INFORMATION {
	LARGE_INTEGER FreeSpaceStartFilteringThreshold;
	LARGE_INTEGER FreeSpaceThresholdLowerBound;
	LARGE_INTEGER FreeSpaceThresholdUpperBound;
	LARGE_INTEGER DefaultQuotaThreshold;
	LARGE_INTEGER DefaultQuotaLimit;
	ULONG FileSystemControlFlags;
	ULONG Padding;
} FILE_FS_CONTROL_INFORMATION, *PFILE_FS_CONTROL_INFORMATION;

typedef struct _FILE_FS_FULL_SIZE_INFORMATION {
	LARGE_INTEGER TotalAllocationUnits;
	LARGE_INTEGER CallerAvailableAllocationUnits;
	LARGE_INTEGER ActualAvailableAllocationUnits;
	ULONG SectorsPerAllocationUnit;
	ULONG BytesPerSector;
} FILE_FS_FULL_SIZE_INFORMATION, *PFILE_FS_FULL_SIZE_INFORMATION;

typedef struct _FILE_FS_OBJECTID_INFORMATION {
	UCHAR ObjectId[16];
	UCHAR ExtendedInfo[48];	/* BirthVolumeId, BirthObjectId, DomainId */
} FILE_FS_OBJECTID_INFORMATION, *PFILE_FS_OBJECTID_INFORMATION;

typedef struct _FILE_STREAM_INFORMATION {
	ULONG NextEntryOffset;
	ULONG StreamNameLength;
	LARGE_INTEGER StreamSize;
	LARGE_INTEGER StreamAllocationSize;
	WCHAR StreamName[1];
} FILE_STREAM_INFORMATION, *PFILE_STREAM_INFORMATION;

/* NT times are 100 ns ticks since 1601-01-01. 0 and -1 mean "unchanged" in a basic-info set. */
#define NTIO_EPOCH_TICKS	116444736000000000LL
/* The largest volume class (FILE_FS_OBJECTID_INFORMATION), and the fixed part of
 * FILE_REPARSE_POINT_INFORMATION (FileReference, Tag). */
#define NTIO_VOL_MAX		64
#define NTIO_REPARSE_POINT_MIN	16

typedef struct _FILE_COMPRESSION_INFORMATION {
	LARGE_INTEGER CompressedFileSize;
	USHORT CompressionFormat;
	UCHAR CompressionUnitShift;
	UCHAR ChunkShift;
	UCHAR ClusterShift;
	UCHAR Reserved[3];
} FILE_COMPRESSION_INFORMATION, *PFILE_COMPRESSION_INFORMATION;

typedef struct _FILE_ALL_INFORMATION {
	FILE_BASIC_INFORMATION BasicInformation;
	FILE_STANDARD_INFORMATION StandardInformation;
	FILE_INTERNAL_INFORMATION InternalInformation;
	FILE_EA_INFORMATION EaInformation;
	FILE_ACCESS_INFORMATION AccessInformation;
	FILE_POSITION_INFORMATION PositionInformation;
	FILE_MODE_INFORMATION ModeInformation;
	FILE_ALIGNMENT_INFORMATION AlignmentInformation;
	FILE_NAME_INFORMATION NameInformation;
} FILE_ALL_INFORMATION, *PFILE_ALL_INFORMATION;

/* Directory information structures (NtQueryDirectoryFile). */
typedef struct _FILE_DIRECTORY_INFORMATION {
	ULONG NextEntryOffset;
	ULONG FileIndex;
	LARGE_INTEGER CreationTime;
	LARGE_INTEGER LastAccessTime;
	LARGE_INTEGER LastWriteTime;
	LARGE_INTEGER ChangeTime;
	LARGE_INTEGER EndOfFile;
	LARGE_INTEGER AllocationSize;
	ULONG FileAttributes;
	ULONG FileNameLength;
	WCHAR FileName[1];
} FILE_DIRECTORY_INFORMATION, *PFILE_DIRECTORY_INFORMATION;

typedef struct _FILE_FULL_DIRECTORY_INFORMATION {
	ULONG NextEntryOffset;
	ULONG FileIndex;
	LARGE_INTEGER CreationTime;
	LARGE_INTEGER LastAccessTime;
	LARGE_INTEGER LastWriteTime;
	LARGE_INTEGER ChangeTime;
	LARGE_INTEGER EndOfFile;
	LARGE_INTEGER AllocationSize;
	ULONG FileAttributes;
	ULONG FileNameLength;
	ULONG EaSize;
	WCHAR FileName[1];
} FILE_FULL_DIRECTORY_INFORMATION, *PFILE_FULL_DIRECTORY_INFORMATION;

typedef struct _FILE_BOTH_DIRECTORY_INFORMATION {
	ULONG NextEntryOffset;
	ULONG FileIndex;
	LARGE_INTEGER CreationTime;
	LARGE_INTEGER LastAccessTime;
	LARGE_INTEGER LastWriteTime;
	LARGE_INTEGER ChangeTime;
	LARGE_INTEGER EndOfFile;
	LARGE_INTEGER AllocationSize;
	ULONG FileAttributes;
	ULONG FileNameLength;
	ULONG EaSize;
	UCHAR ShortNameLength;
	WCHAR ShortName[12];
	WCHAR FileName[1];
} FILE_BOTH_DIRECTORY_INFORMATION, *PFILE_BOTH_DIRECTORY_INFORMATION;

typedef struct _FILE_ID_BOTH_DIRECTORY_INFORMATION {
	ULONG NextEntryOffset;
	ULONG FileIndex;
	LARGE_INTEGER CreationTime;
	LARGE_INTEGER LastAccessTime;
	LARGE_INTEGER LastWriteTime;
	LARGE_INTEGER ChangeTime;
	LARGE_INTEGER EndOfFile;
	LARGE_INTEGER AllocationSize;
	ULONG FileAttributes;
	ULONG FileNameLength;
	ULONG EaSize;
	UCHAR ShortNameLength;
	WCHAR ShortName[12];
	LARGE_INTEGER FileId;
	WCHAR FileName[1];
} FILE_ID_BOTH_DIRECTORY_INFORMATION, *PFILE_ID_BOTH_DIRECTORY_INFORMATION;

typedef struct _FILE_NAMES_INFORMATION {
	ULONG NextEntryOffset;
	ULONG FileIndex;
	ULONG FileNameLength;
	WCHAR FileName[1];
} FILE_NAMES_INFORMATION, *PFILE_NAMES_INFORMATION;

/* NtQueryDirectoryFileEx QueryFlags. */
#define SL_RESTART_SCAN                 0x00000001
#define SL_RETURN_SINGLE_ENTRY          0x00000002
#define SL_INDEX_SPECIFIED              0x00000004
#define SL_RETURN_ON_DISK_ENTRIES_ONLY  0x00000008

/* -------------------------------------------------------------------------- */
/* File object                                                                  */
/* -------------------------------------------------------------------------- */

struct nt_file_object {
	ULONG magic;
	struct kref ref;
	spinlock_t lock;

	struct file *filp;
	struct dentry *dentry; /* borrowed ref to the opened dentry */
	/* Driver-host device handle (driver/drvhost_svc.c) for a file that names a driver
	 * device; NULL otherwise. Such a file has no filp. Closed with the file. */
	void *drv_handle;

	ACCESS_MASK granted_access;
	ULONG share_access;
	ULONG create_options;
	/* AllocationSize given to NtCreateFile (0 if none, or for an open). A section over an empty
	 * file maps at this size; see mmf_section in mm.c. */
	LONGLONG alloc_size;
	bool synchronous;
	bool delete_on_close;
	bool delete_pending;
	bool is_directory;

	/* IOCP association. */
	struct nt_iocp *iocp;
	ULONG iocp_key;

	/* For synchronous I/O, the current file position. */
	loff_t current_byte_offset;

	/* Share registry linkage. */
	struct list_head share_entry;
	dev_t share_dev;
	ino_t share_ino;

	/* Async request list (simplified). */
	struct list_head async_requests;

	/* Name cache (Linux path). */
	char *linux_path;

	/* Directory enumeration state (userspace shim). A snapshot of entry names
	 * is taken when a scan starts; dir_pos is the next entry to return. */
	char **dir_names;
	size_t dir_count;
	size_t dir_pos;
	char *dir_pattern;
};

#ifndef STATUS_NOT_FOUND
#define STATUS_NOT_FOUND ((NTSTATUS)0xC0000225L)
#endif

/* NtFlushBuffersFileEx flags. */
#define FLUSH_FLAGS_FILE_DATA_ONLY      0x00000001
#define FLUSH_FLAGS_NO_SYNC             0x00000002
#define FLUSH_FLAGS_FILE_DATA_SYNC_ONLY 0x00000004
#define FLUSH_FLAGS_VALID_MASK          (FLUSH_FLAGS_FILE_DATA_ONLY | \
					 FLUSH_FLAGS_NO_SYNC | \
					 FLUSH_FLAGS_FILE_DATA_SYNC_ONLY)

/* -------------------------------------------------------------------------- */
/* Service entry points (called through nt_service_table).                      */
/* -------------------------------------------------------------------------- */

void *ntio_handle_peek(HANDLE h);
NTSTATUS nt_svc_NtCreateFile(PHANDLE FileHandle, ACCESS_MASK DesiredAccess,
			     POBJECT_ATTRIBUTES ObjectAttributes,
			     PIO_STATUS_BLOCK IoStatusBlock,
			     PLARGE_INTEGER AllocationSize,
			     ULONG FileAttributes,
			     ULONG ShareAccess,
			     ULONG CreateDisposition,
			     ULONG CreateOptions,
			     PVOID EaBuffer,
			     ULONG EaLength);

NTSTATUS nt_svc_NtOpenFile(PHANDLE FileHandle, ACCESS_MASK DesiredAccess,
			   POBJECT_ATTRIBUTES ObjectAttributes,
			   PIO_STATUS_BLOCK IoStatusBlock,
			   ULONG ShareAccess,
			   ULONG OpenOptions);

NTSTATUS nt_svc_NtReadFile(HANDLE FileHandle, HANDLE Event,
			   PVOID ApcRoutine, PVOID ApcContext,
			   PIO_STATUS_BLOCK IoStatusBlock,
			   PVOID Buffer, ULONG Length,
			   PLARGE_INTEGER ByteOffset,
			   PULONG Key);

NTSTATUS nt_svc_NtWriteFile(HANDLE FileHandle, HANDLE Event,
			    PVOID ApcRoutine, PVOID ApcContext,
			    PIO_STATUS_BLOCK IoStatusBlock,
			    PVOID Buffer, ULONG Length,
			    PLARGE_INTEGER ByteOffset,
			    PULONG Key);

NTSTATUS nt_svc_NtQueryAttributesFile(POBJECT_ATTRIBUTES ObjectAttributes,
				      PFILE_BASIC_INFORMATION FileInformation);
NTSTATUS nt_svc_NtQueryInformationFile(HANDLE FileHandle,
				       PIO_STATUS_BLOCK IoStatusBlock,
				       PVOID FileInformation,
				       ULONG Length,
				       ULONG FileInformationClass);

NTSTATUS nt_svc_NtSetInformationFile(HANDLE FileHandle,
				     PIO_STATUS_BLOCK IoStatusBlock,
				     PVOID FileInformation,
				     ULONG Length,
				     ULONG FileInformationClass);

NTSTATUS nt_svc_NtFlushBuffersFile(HANDLE FileHandle,
				   PIO_STATUS_BLOCK IoStatusBlock);

NTSTATUS nt_svc_NtFlushBuffersFileEx(HANDLE FileHandle, ULONG Flags,
				     PVOID Parameters, ULONG ParametersSize,
				     PIO_STATUS_BLOCK IoStatusBlock);

NTSTATUS nt_svc_NtCancelIoFile(HANDLE FileHandle,
			       PIO_STATUS_BLOCK IoStatusBlock);

NTSTATUS nt_svc_NtCancelIoFileEx(HANDLE FileHandle,
				 PIO_STATUS_BLOCK IoRequestToCancel,
				 PIO_STATUS_BLOCK IoStatusBlock);

NTSTATUS nt_svc_NtDeleteFile(POBJECT_ATTRIBUTES ObjectAttributes);

NTSTATUS nt_svc_NtQueryFullAttributesFile(POBJECT_ATTRIBUTES ObjectAttributes,
					  PFILE_NETWORK_OPEN_INFORMATION FileInformation);

NTSTATUS nt_svc_NtLockFile(HANDLE FileHandle, HANDLE Event,
			   PVOID ApcRoutine, PVOID ApcContext,
			   PIO_STATUS_BLOCK IoStatusBlock,
			   PLARGE_INTEGER ByteOffset, PLARGE_INTEGER Length,
			   ULONG Key, BOOLEAN FailImmediately,
			   BOOLEAN ExclusiveLock);

NTSTATUS nt_svc_NtUnlockFile(HANDLE FileHandle,
			     PIO_STATUS_BLOCK IoStatusBlock,
			     PLARGE_INTEGER ByteOffset, PLARGE_INTEGER Length,
			     ULONG Key);

NTSTATUS nt_svc_NtQueryDirectoryFile(HANDLE FileHandle, HANDLE Event,
				     PVOID ApcRoutine, PVOID ApcContext,
				     PIO_STATUS_BLOCK IoStatusBlock,
				     PVOID FileInformation, ULONG Length,
				     ULONG FileInformationClass,
				     BOOLEAN ReturnSingleEntry,
				     PUNICODE_STRING FileName,
				     BOOLEAN RestartScan);

NTSTATUS nt_svc_NtQueryDirectoryFileEx(HANDLE FileHandle, HANDLE Event,
				       PVOID ApcRoutine, PVOID ApcContext,
				       PIO_STATUS_BLOCK IoStatusBlock,
				       PVOID FileInformation, ULONG Length,
				       ULONG FileInformationClass,
				       ULONG QueryFlags,
				       PUNICODE_STRING FileName);

NTSTATUS nt_svc_NtQueryVolumeInformationFile(HANDLE FileHandle,
					       PIO_STATUS_BLOCK IoStatusBlock,
					       PVOID FsInformation, ULONG Length,
					       ULONG FsInformationClass);

NTSTATUS nt_svc_NtSetVolumeInformationFile(HANDLE FileHandle,
					     PIO_STATUS_BLOCK IoStatusBlock,
					     PVOID FsInformation, ULONG Length,
					     ULONG FsInformationClass);

NTSTATUS nt_svc_NtQueryEaFile(HANDLE FileHandle,
			      PIO_STATUS_BLOCK IoStatusBlock,
			      PVOID Buffer, ULONG Length,
			      BOOLEAN ReturnSingleEntry,
			      PVOID EaList, ULONG EaListLength,
			      PULONG EaIndex, BOOLEAN RestartScan);

NTSTATUS nt_svc_NtSetEaFile(HANDLE FileHandle,
			    PIO_STATUS_BLOCK IoStatusBlock,
			    PVOID Buffer, ULONG Length);

NTSTATUS nt_svc_NtDeviceIoControlFile(HANDLE FileHandle, HANDLE Event,
				      PVOID ApcRoutine, PVOID ApcContext,
				      PIO_STATUS_BLOCK IoStatusBlock,
				      ULONG IoControlCode,
				      PVOID InputBuffer, ULONG InputBufferLength,
				      PVOID OutputBuffer, ULONG OutputBufferLength);

NTSTATUS nt_svc_NtFsControlFile(HANDLE FileHandle, HANDLE Event,
				PVOID ApcRoutine, PVOID ApcContext,
				PIO_STATUS_BLOCK IoStatusBlock,
				ULONG FsControlCode,
				PVOID InputBuffer, ULONG InputBufferLength,
				PVOID OutputBuffer, ULONG OutputBufferLength);

#ifdef __KERNEL__
NTSTATUS nt_svc_NtClose(unsigned long a0, unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5, unsigned long a6, unsigned long a7, unsigned long a8, unsigned long a9, unsigned long a10, unsigned long a11);
#else
NTSTATUS nt_svc_NtClose(HANDLE Handle);
#endif

/* -------------------------------------------------------------------------- */
/* Internal helpers exposed for tests.                                          */
/* -------------------------------------------------------------------------- */

/* Share-access registry. */
void ntio_share_lock(void);
void ntio_share_unlock(void);
bool ntio_check_share_access(dev_t dev, ino_t ino,
			     ACCESS_MASK desired_access,
			     ULONG share_access,
			     struct nt_file_object *exclude);
void ntio_register_share(struct nt_file_object *file);
void ntio_unregister_share(struct nt_file_object *file);

/* Handle table used by this workstream (minimal in-process implementation). */
NTSTATUS ntio_handle_create(void *obj, ACCESS_MASK granted, HANDLE *out);
void *ntio_handle_lookup(HANDLE h, ACCESS_MASK required);
void *ntio_handle_lookup_any_of(HANDLE h, ACCESS_MASK mask);
/* Standard handles for exec (binfmt_pe): install a file as a handle, release one. */
int ntio_install_std_file(struct file *filp, u64 *out_handle);
void ntio_release_std_handle(u64 h);
/* Process exit: free every slot owned by process id owner (see io.c). */
void ntio_release_owned_handles(ULONG owner);
ULONG ntio_handles_in_use(void);
ULONG ntio_handles_owned_by(ULONG owner);
#ifdef __KERNEL__
void nt_process_exit_handles(void);
#endif
NTSTATUS ntio_handle_close(HANDLE h);
NTSTATUS ntio_duplicate_file_handle(struct nt_file_object *f, ACCESS_MASK new_access, HANDLE *out_handle);
ACCESS_MASK ntio_handle_granted(HANDLE h);
ULONG ntio_count_handles_for_file(struct nt_file_object *f);
ULONG ntio_pointer_count_for_file(struct nt_file_object *f);

/* Object reference counting. */
struct nt_file_object *ntio_file_get(struct nt_file_object *file);
void ntio_file_put(struct nt_file_object *file);

/* Value of the first word of every file object (ntio_handle_peek on a handle
 * tells a file from an IOCP by it). */
#define NTIO_FILE_MAGIC 0x46494C45UL

/* File-backed sections (mm.c). The size is the file's current size. Extend
 * grows the file to at least size bytes. */
NTSTATUS ntio_file_section_size(struct nt_file_object *file, LONGLONG *size);
NTSTATUS ntio_file_section_extend(struct nt_file_object *file, LONGLONG size);

/* Path helpers. */
NTSTATUS ntio_resolve_path(POBJECT_ATTRIBUTES obj_attr, char *linux_path,
			   size_t pathlen, bool *is_dir);

/* Test helpers to reset state. */
void ntio_reset_handle_table(void);
void ntio_reset_share_registry(void);

#ifdef __cplusplus
}
#endif

#endif /* NTIO_IO_H */
