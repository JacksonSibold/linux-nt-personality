#!/bin/sh
# gen_def.sh <drvhost_exports.c> <ntos|hal>
#
# Prints a .def file for dlltool with one line per export the host implements,
# read from the X_NT / X_HAL rows of src/kernel/nt/driver/drvhost_exports.c.
# The import library built from it is what a MinGW driver links against, so the
# driver's import table names exactly the host's exports.
set -eu
src=$1
mod=$2
case "$mod" in
  ntos) macro=X_NT; lib=ntoskrnl.exe ;;
  hal)  macro=X_HAL; lib=hal.dll ;;
  *) echo "gen_def.sh: unknown module $mod" >&2; exit 2 ;;
esac
printf 'LIBRARY %s\nEXPORTS\n' "$lib"
grep -E "^[[:space:]]*${macro}\(\"" "$src" \
  | sed -E "s/^[[:space:]]*${macro}\(\"([^\"]*)\".*/\1/" \
  | while IFS= read -r n; do
      case "$n" in
        PsThreadType|IoFileObjectType|IoDeviceObjectType|ExEventObjectType|NtBuildNumber|KeNumberProcessors)
          printf '%s DATA\n' "$n" ;;
        *) printf '%s\n' "$n" ;;
      esac
    done
