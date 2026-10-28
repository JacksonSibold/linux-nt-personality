# Code reused from other open source projects

Status date: 2026-10-28. This page records every piece of code taken from another project, so that its origin and licence stay
visible. It is a factual record and not legal advice. It does not change any licence grant.

## Sources

| Project | Licence | Used for | Reference commit |
|---|---|---|---|
| Wine (`wine-mirror/wine`) | LGPL-2.1-or-later, per file header | ntdll Rtl functions, Win32 behaviour, test expectations | `0738d528db52` (2026-10-28) |
| ReactOS (`reactos/reactos`) | GPL-2.0-or-later, per file header (some files BSD or LGPL, named in the header) | ntoskrnl and Rtl behaviour, driver-facing structures | `e46f4bb1a1ea` (2026-10-25) |

## Rules

1. Each file that holds copied or closely derived code keeps the upstream copyright lines and licence text in its header,
   followed by a line saying what changed.
2. Upstream licence decides the file's licence. Wine code stays LGPL-2.1-or-later. ReactOS code stays GPL-2.0-or-later. A file does
   not mix the two: where both projects implement the same function, take one.
3. Wine's and ReactOS's own tests are a source of expected values. The values (inputs and outputs) are facts about Windows
   behaviour and are recorded in the tests here with a comment that names the upstream test.
4. Nothing is copied from Windows itself, from leaked Windows source, or from any binary. ReactOS and Wine were written without
   them, and this project keeps to the same rule. If a function's behaviour is not documented and not covered by an upstream
   test, it is left unimplemented.
5. A row is added below in the same commit that adds the code.

## Reused code

| Local file | Upstream file(s) | Project | What was taken | Changes |
|---|---|---|---|---|
