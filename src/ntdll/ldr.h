/*
 * ldr.h - user-mode PE loader (winldr) for the ntpers NT personality.
 *
 * This is the ntdll loader: LdrLoadDll, LdrUnloadDll, LdrGetDllHandle,
 * LdrGetProcedureAddress, plus import resolution, export forwarders, API sets,
 * delay-load imports, bound imports, DllMain ordering, TLS callbacks, and
 * reference counting.
 *
 * The loader is intentionally user-mode, mirroring the real Windows split where
 * ntdll!Ldr* lives in user space and the kernel only maps the initial image.
 */

#ifndef LDR_H
#define LDR_H

#include "nt_types.h"
#include "ntstatus.h"
#include "../loader/pe_parse.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/* Win32-ish handle / procedure types (defined here to avoid editing nt_types) */
/* -------------------------------------------------------------------------- */

#ifndef NTAPI
#define NTAPI
#endif
#ifndef VOID
#define VOID void
#endif
#ifndef HINSTANCE
typedef PVOID HINSTANCE;
#endif
#ifndef LPVOID
typedef PVOID LPVOID;
#endif

typedef PVOID HMODULE;
typedef LONG (NTAPI *FARPROC)(void);

#define DECLARE_HANDLE(name) typedef struct name##__ { int unused; } *name

/* -------------------------------------------------------------------------- */
/* DllMain / TLS callback signatures                                          */
/* -------------------------------------------------------------------------- */

#define DLL_PROCESS_ATTACH 1
#define DLL_THREAD_ATTACH  2
#define DLL_THREAD_DETACH  3
#define DLL_PROCESS_DETACH 0

typedef BOOL (NTAPI *DllMain_t)(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved);
typedef VOID (NTAPI *PIMAGE_TLS_CALLBACK)(PVOID DllHandle, DWORD Reason, PVOID Reserved);

/* -------------------------------------------------------------------------- */
/* Loader flags                                                               */
/* -------------------------------------------------------------------------- */

#define LDR_DLL_LOAD_AS_DATAFILE 0x00000002
#define LDR_DLL_PIN              0x00000001

/* -------------------------------------------------------------------------- */
/* LDR_DATA_TABLE_ENTRY (Windows Server 2022 x64 layout, subset)              */
/* -------------------------------------------------------------------------- */

typedef struct _LDR_DATA_TABLE_ENTRY {
    LIST_ENTRY InLoadOrderLinks;
    LIST_ENTRY InMemoryOrderLinks;
    LIST_ENTRY InInitializationOrderLinks;
    PVOID DllBase;
    PVOID EntryPoint;
    ULONG SizeOfImage;
    UNICODE_STRING FullDllName;
    UNICODE_STRING BaseDllName;
    ULONG Flags;
    USHORT LoadCount;
    USHORT TlsIndex;
    LIST_ENTRY HashLinks;
    ULONG TimeDateStamp;
    PVOID OriginalBase;          /* our extension: preferred ImageBase */
} LDR_DATA_TABLE_ENTRY, *PLDR_DATA_TABLE_ENTRY;

NT_STATIC_ASSERT(offsetof(LDR_DATA_TABLE_ENTRY, InLoadOrderLinks)           == 0x00,
                 "LDR_DATA_TABLE_ENTRY.InLoadOrderLinks offset");
NT_STATIC_ASSERT(offsetof(LDR_DATA_TABLE_ENTRY, InMemoryOrderLinks)          == 0x10,
                 "LDR_DATA_TABLE_ENTRY.InMemoryOrderLinks offset");
NT_STATIC_ASSERT(offsetof(LDR_DATA_TABLE_ENTRY, InInitializationOrderLinks)  == 0x20,
                 "LDR_DATA_TABLE_ENTRY.InInitializationOrderLinks offset");
NT_STATIC_ASSERT(offsetof(LDR_DATA_TABLE_ENTRY, DllBase)                     == 0x30,
                 "LDR_DATA_TABLE_ENTRY.DllBase offset");
NT_STATIC_ASSERT(offsetof(LDR_DATA_TABLE_ENTRY, EntryPoint)                  == 0x38,
                 "LDR_DATA_TABLE_ENTRY.EntryPoint offset");
NT_STATIC_ASSERT(offsetof(LDR_DATA_TABLE_ENTRY, SizeOfImage)                 == 0x40,
                 "LDR_DATA_TABLE_ENTRY.SizeOfImage offset");
NT_STATIC_ASSERT(offsetof(LDR_DATA_TABLE_ENTRY, FullDllName)                 == 0x48,
                 "LDR_DATA_TABLE_ENTRY.FullDllName offset");
NT_STATIC_ASSERT(offsetof(LDR_DATA_TABLE_ENTRY, BaseDllName)                 == 0x58,
                 "LDR_DATA_TABLE_ENTRY.BaseDllName offset");
NT_STATIC_ASSERT(offsetof(LDR_DATA_TABLE_ENTRY, Flags)                       == 0x68,
                 "LDR_DATA_TABLE_ENTRY.Flags offset");
NT_STATIC_ASSERT(offsetof(LDR_DATA_TABLE_ENTRY, LoadCount)                   == 0x6c,
                 "LDR_DATA_TABLE_ENTRY.LoadCount offset");

/* -------------------------------------------------------------------------- */
/* Opaque loader context                                                      */
/* -------------------------------------------------------------------------- */

typedef struct winldr_ctx winldr_ctx_t;

/* -------------------------------------------------------------------------- */
/* Hooks for observing DllMain / TLS callbacks (tests use these)              */
/* -------------------------------------------------------------------------- */

typedef void (*ldr_dll_main_hook_t)(winldr_ctx_t *ctx,
                                    PLDR_DATA_TABLE_ENTRY mod,
                                    ULONG reason,
                                    PVOID reserved,
                                    void *user);

typedef void (*ldr_tls_callback_hook_t)(winldr_ctx_t *ctx,
                                        PLDR_DATA_TABLE_ENTRY mod,
                                        ULONG reason,
                                        PVOID reserved,
                                        PIMAGE_TLS_CALLBACK cb,
                                        void *user);

/* -------------------------------------------------------------------------- */
/* Init-log entry (for test inspection)                                       */
/* -------------------------------------------------------------------------- */

typedef struct {
    const char *name;       /* module base name (valid for log lifetime) */
    ULONG        reason;
    int          is_tls;    /* 1 if TLS callback, 0 if DllMain */
    ULONG_PTR    cb_va;     /* callback VA (TLS only) */
} winldr_init_log_entry_t;

/* -------------------------------------------------------------------------- */
/* Public API                                                                 */
/* -------------------------------------------------------------------------- */

winldr_ctx_t *winldr_ctx_create(void);
void          winldr_ctx_destroy(winldr_ctx_t *ctx);

PPEB_LDR_DATA winldr_peb_ldr(winldr_ctx_t *ctx);

void winldr_set_dll_main_hook(winldr_ctx_t *ctx, ldr_dll_main_hook_t hook, void *user);
void winldr_set_tls_callback_hook(winldr_ctx_t *ctx, ldr_tls_callback_hook_t hook, void *user);

void winldr_set_exe_dir(winldr_ctx_t *ctx, const char *path);
void winldr_set_system32_dir(winldr_ctx_t *ctx, const char *path);

void winldr_register_known_dll(winldr_ctx_t *ctx, const char *name);

/* API-set resolution.  If a schema file is not loaded, a built-in minimal
 * fallback is used for common api-ms-win-* names. */
NTSTATUS winldr_load_apiset_schema(winldr_ctx_t *ctx, const char *path);
const char *winldr_apiset_resolve(winldr_ctx_t *ctx, const char *virtual_name);

NTSTATUS LdrLoadDll(winldr_ctx_t *ctx,
                    const WCHAR *path,          /* search path override or NULL */
                    ULONG flags,
                    const WCHAR *name,          /* required */
                    HMODULE *out);

NTSTATUS LdrUnloadDll(winldr_ctx_t *ctx, HMODULE module);

NTSTATUS LdrGetDllHandle(winldr_ctx_t *ctx,
                         const WCHAR *name,
                         HMODULE *out);

NTSTATUS LdrGetProcedureAddress(winldr_ctx_t *ctx,
                                HMODULE module,
                                const ANSI_STRING *name,
                                WORD ordinal,
                                FARPROC *out);

/* Adds a reference to a loaded module (flags 0), or pins it (flags LDR_DLL_PIN).
 * Used by GetModuleHandleEx. Takes the loader lock. */
NTSTATUS LdrAddRefDll(winldr_ctx_t *ctx, ULONG flags, HMODULE module);

/* Process loader context (K32-4). ntdll creates one context in LdrInitializeThunk
 * and publishes it in the PEB, so kernel32 and the other shims pass the same
 * context to the Ldr* calls. The slot is PEB.SparePointers[0]. That PEB tail is
 * unverified (nt_types.h), so this is a personality convention, not a Windows
 * one. */
#define LDR_PEB_CONTEXT_SLOT(peb) ((peb)->SparePointers[0])

static inline winldr_ctx_t *ldr_process_context(PPEB peb)
{
    return peb ? (winldr_ctx_t *)LDR_PEB_CONTEXT_SLOT(peb) : NULL;
}

/* Resolve all delay-load imports for a module (used by the helper and tests). */
NTSTATUS ldr_resolve_delay_imports(winldr_ctx_t *ctx, HMODULE module);

/* -------------------------------------------------------------------------- */
/* Test / introspection helpers                                               */
/* -------------------------------------------------------------------------- */

PLDR_DATA_TABLE_ENTRY winldr_first_module(winldr_ctx_t *ctx, int load_order);
PLDR_DATA_TABLE_ENTRY winldr_next_module(winldr_ctx_t *ctx, PLDR_DATA_TABLE_ENTRY cur, int load_order);
PLDR_DATA_TABLE_ENTRY winldr_find_module(winldr_ctx_t *ctx, const char *name);

const winldr_init_log_entry_t *winldr_get_init_log(winldr_ctx_t *ctx, size_t *count);
void winldr_clear_init_log(winldr_ctx_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* LDR_H */
