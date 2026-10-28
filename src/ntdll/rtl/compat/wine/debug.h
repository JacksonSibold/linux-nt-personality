/*
 * wine/debug.h - stand-in for Wine's debug channel header. Tracing is compiled out; the helpers return empty strings.
 * Part of the Rtl port (see ../../README-port.md and docs/policy/THIRD-PARTY.md).
 */
#ifndef RTL_COMPAT_WINE_DEBUG_H
#define RTL_COMPAT_WINE_DEBUG_H

#include <stdarg.h>
#ifdef __MINGW32__
/* The ported sources call _snwprintf and friends; the host build gets them renamed from printf.c instead. */
#include <stdio.h>
#include <wchar.h>
#endif

#include "windef.h"
#include "winnt.h"
#include "winternl.h"

#define TRACE(...) do { } while (0)
#define TRACE_(ch) TRACE
#define WARN(...) do { } while (0)
#define WARN_(ch) WARN
#define ERR(...) do { } while (0)
#define ERR_(ch) ERR
#define FIXME(...) do { } while (0)
#define FIXME_(ch) FIXME
#define TRACE_ON(ch) 0
#define WARN_ON(ch) 0
#define WINE_DEFAULT_DEBUG_CHANNEL(ch) typedef int rtl_port_debug_channel_unused_##ch
#define WINE_DECLARE_DEBUG_CHANNEL(ch) typedef int rtl_port_debug_channel_unused_##ch

/* The helpers are only used inside TRACE() and in the host unit tests, which get them from tests/rtl-host/host_support.c. */
const char *wine_dbgstr_an(const char *s, int n);
const char *wine_dbgstr_a(const char *s);
const char *wine_dbgstr_wn(const WCHAR *s, int n);
const char *wine_dbgstr_w(const WCHAR *s);
const char *wine_dbgstr_us(const UNICODE_STRING *s);
const char *wine_dbgstr_guid(const GUID *g);
const char *wine_dbgstr_longlong(unsigned long long v);
#define debugstr_an wine_dbgstr_an
#define debugstr_a wine_dbgstr_a
#define debugstr_wn wine_dbgstr_wn
#define debugstr_w wine_dbgstr_w
#define debugstr_us wine_dbgstr_us
#define debugstr_guid wine_dbgstr_guid

#endif
