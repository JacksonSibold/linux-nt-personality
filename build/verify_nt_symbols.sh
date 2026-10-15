#!/usr/bin/env bash
# Independent check of whether the NT subsystem is really linked into vmlinux.
# tests/kernel/check_kernel_complete.sh currently reports every symbol missing
# even though built-in.a contains 17 objects and nm shows 355 nt_svc_* symbols,
# so this exists to determine which of the two is telling the truth.
set -u
VM="${1:-$HOME/ntpers/linux-6.18/vmlinux}"

echo "vmlinux: $VM"
nm "$VM" > /tmp/nmout.txt 2>/dev/null
echo "total symbols: $(wc -l < /tmp/nmout.txt)"
echo

for s in do_nt_syscall nt_service_table nt_enter_personality nt_exit_personality nt_process_exit_handles \
         ObCreateHandle ObReferenceObjectByHandle nt_create_event se_access_check \
         nt_banner_init; do
    printf '%-32s ' "$s"
    if grep -qE "[ ]$s\$" /tmp/nmout.txt; then echo PRESENT; else echo ABSENT; fi
done

echo
echo "--- NT service entry points (nt_svc_*) ---"
printf 'count: %s\n' "$(grep -c 'nt_svc_' /tmp/nmout.txt)"
for s in NtAllocateVirtualMemory NtCreateFile NtCreateMutant NtQueryInformationFile \
         NtCreateIoCompletion NtWaitForMultipleObjects; do
    printf '%-32s ' "$s"
    if grep -qE "nt_svc_$s\$" /tmp/nmout.txt; then
        echo "PRESENT (as nt_svc_$s)"
    elif grep -qE "[ ]$s\$" /tmp/nmout.txt; then
        echo "PRESENT (bare)"
    else
        echo ABSENT
    fi
done
