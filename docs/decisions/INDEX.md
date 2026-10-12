# Decision index

Date: 2026-10-12. Branch: `haiku/ldrfix`.

Status values: Proposed (recorded, not built), Implemented (commit given), Superseded.

| Record | Title | Finding | Status |
|---|---|---|---|
| [0029](0029-loader-mingw-build-selection.md) | MinGW build compiles only loader stubs (LDR-1) | `docs/review/ntdll.md` LDR-1 | Proposed (needs VM) |
| [0030](0030-loader-iat-rva-patch.md) | IAT slot writes use a file offset, not an RVA (LDR-2) | `docs/review/ntdll.md` LDR-2 | Implemented in a18c03e |
| [0031](0031-loader-dllmain-tls-order.md) | DllMain and TLS callbacks are never called (LDR-3) | `docs/review/ntdll.md` LDR-3 | Proposed (needs VM) |
| [0032](0032-loader-tls-callback-table-rva.md) | TLS callback table read at its preferred-base address (LDR-4) | `docs/review/ntdll.md` LDR-4 | Implemented in bef4d79 |
