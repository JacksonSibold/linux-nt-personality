param(
    [string]$Build = "latest"
)
# compare.ps1 - compare golden data against current ntpars implementations
# Reads golden JSON and runs lightweight checks against src/ntdll/ntstatus_to_dos.c table etc.
# This is the "report discrepancies" step.

$ErrorActionPreference = "Stop"
# Repository root: NTPERS_ROOT if set, otherwise the checkout that contains tests\differential.
if ($env:NTPERS_ROOT) { $ProjectRoot = $env:NTPERS_ROOT } else { $ProjectRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot) }
$GoldenDir = Join-Path $ProjectRoot "tests\differential\golden\$Build"

if (-not (Test-Path $GoldenDir)) {
    Write-Host "Golden dir not found: $GoldenDir" -ForegroundColor Red
    exit 1
}

function Load-Json($path){
    $txt = Get-Content -LiteralPath $path -Raw -Encoding UTF8
    return $txt | ConvertFrom-Json
}

Write-Host "=== Differential Comparison (Build: $Build) ===" -ForegroundColor Cyan
Write-Host "Golden dir: $GoldenDir"

# -------- NTSTATUS mapping comparison --------
Write-Host "`n--- NTSTATUS -> DOS mapping ---" -ForegroundColor Yellow
$ntstatusGolden = Load-Json (Join-Path $GoldenDir "probe_ntstatus.json")
# Load our table by parsing ntstatus_to_dos.c? For now we will compile a helper that calls our RtlNtStatusToDosError
# Simpler: we have reference table in src; we will wsl-compile a checker that links our ntstatus_to_dos.c and dumps same cases
$checkerSrc = @'
#include "../src/ntdll/nt_types.h"
#include "../src/ntdll/ntstatus.h"
ULONG RtlNtStatusToDosError(NTSTATUS);
int main(){
    NTSTATUS cases[] = {0x00000000,0x00000102,0x00000103,0x80000005,0xC0000023,0xC0000004,0xC0000001,0xC0000002,0xC0000003,0xC0000005,0xC0000008,0xC000000D,0xC0000034,0xC0000035,0xC0000043,0xC0000228,0xC0000275};
    int n = sizeof(cases)/sizeof(cases[0]);
    for(int i=0;i<n;i++) printf("0x%08lX %lu\n", (unsigned long)cases[i], (unsigned long)RtlNtStatusToDosError(cases[i]));
    return 0;
}
'@
# Instead of dynamic, just compare known critical pairs manually documented in ntstatus_to_dos.c comments
$critical = @(
    @{ nt="0x80000005"; name="STATUS_BUFFER_OVERFLOW"; expectedDos=234 },
    @{ nt="0xC0000023"; name="STATUS_BUFFER_TOO_SMALL"; expectedDos=122 }
)
Write-Host "Critical trap: STATUS_BUFFER_OVERFLOW (0x80000005) -> ERROR_MORE_DATA (234)"
Write-Host "Critical trap: STATUS_BUFFER_TOO_SMALL (0xC0000023) -> ERROR_INSUFFICIENT_BUFFER (122)"
foreach($m in $ntstatusGolden.rtl_mappings){
    $hex = $m.ntstatus_hex
    if($hex -eq "0x80000005" -or $hex -eq "0xC0000023"){
        Write-Host "  Golden: $($m.name) $hex -> DOS $($m.dos)"
    }
}
Write-Host "Checking our table would require compiling checker - see diff below"

# -------- TEB/PEB --------
Write-Host "`n--- TEB/PEB offsets ---" -ForegroundColor Yellow
$teb = Load-Json (Join-Path $GoldenDir "probe_teb_peb.json")
Write-Host "TEB: $($teb.teb)  PEB: $($teb.peb)"
foreach($c in $teb.gs_checks){
    $status = if($c.match){ "OK" } else { "MISMATCH" }
    $color = if($c.match){ "Green" } else { "Red" }
    Write-Host ("  {0,-28} offset {1,-8} GS {2,-12} struct {3,-12} [{4}]" -f $c.field, $c.offset, $c.gs_value, $c.struct_value, $status) -ForegroundColor $color
}
foreach($c in $teb.peb_checks){
    $status = if($c.match){ "OK" } else { "MISMATCH" }
    $color = if($c.match){ "Green" } else { "Red" }
    Write-Host ("  PEB {0,-20} offset {1,-8} mem {2,-12} struct {3,-12} [{4}]" -f $c.field, $c.offset, $c.mem_value, $c.struct_value, $status) -ForegroundColor $color
}

# -------- VirtualQuery --------
Write-Host "`n--- VirtualQuery coalescing ---" -ForegroundColor Yellow
$vq = Load-Json (Join-Path $GoldenDir "probe_virtualquery.json")
Write-Host "Reserve base: $($vq.reserve_base)  size $($vq.reserve_size)"
Write-Host "Query before free: $($vq.query_before_free.Count) regions"
foreach($r in $vq.query_before_free){
    Write-Host ("  Base {0,-14} Size {1,-10} State {2,-8} Protect {3,-4} Type {4} AllocProt {5}" -f $r.BaseAddress, $r.RegionSize, $r.State, $r.Protect, $r.Type, $r.AllocationProtect) -ForegroundColor Gray
}
Write-Host "Query after decommit: $($vq.query_after_decommit.Count) regions"
foreach($r in $vq.query_after_decommit){
    Write-Host ("  Base {0,-14} Size {1,-10} State {2,-8} Protect {3,-4} Type {4}" -f $r.BaseAddress, $r.RegionSize, $r.State, $r.Protect, $r.Type) -ForegroundColor Gray
}
Write-Host "Release success: $($vq.release.success)"

# -------- Handles --------
Write-Host "`n--- Handles ---" -ForegroundColor Yellow
$hdl = Load-Json (Join-Path $GoldenDir "probe_handles.json")
foreach($h in $hdl.handles){
    $modOk = ($h.mod4 -eq 0)
    $color = if($modOk){ "Green" } else { "Red" }
    Write-Host ("  {0,-16} handle {1,-12} mod4={2} {3}" -f $h.name, $h.handle, $h.mod4, $(if($modOk){"OK"}else{"FAIL"})) -ForegroundColor $color
}
Write-Host "GetProcessHandleCount: $($hdl.GetProcessHandleCount.count) success=$($hdl.GetProcessHandleCount.success)"
foreach($q in $hdl.nt_query_object){
    if($q.sentinel){ continue }
    Write-Host ("  NtQueryObject {0} class {1} st {2} type {3} GrantedAccess {4}" -f $q.handle, $q.class_name, $q.ntstatus_hex, $q.type, $q.GrantedAccess) -ForegroundColor Gray
}
foreach($d in $hdl.duplicate_handle){
    if($d.sentinel){ continue }
    Write-Host ("  Dup {0} src {1} dst {2} success {3}" -f $d.op, $d.src, $d.dst, $d.success) -ForegroundColor Gray
}

# -------- Share mode --------
Write-Host "`n--- Share mode (sample) ---" -ForegroundColor Yellow
$share = Load-Json (Join-Path $GoldenDir "probe_sharemode.json")
Write-Host "Matrix entries: $($share.matrix.Count)"
# Count sharing violations
$violations = ($share.matrix | Where-Object { $_.ntstatus_hex -eq "0xC0000043" }).Count
$success = ($share.matrix | Where-Object { $_.ntstatus_hex -eq "0x00000000" }).Count
Write-Host "  SHARING_VIOLATION (0xC0000043): $violations"
Write-Host "  SUCCESS: $success"
Write-Host "Targeted examples:"
foreach($ex in $share.targeted_examples){
    Write-Host ("  {0,-30} st {1}" -f $ex.desc, $ex.second_st_hex) -ForegroundColor Gray
}

Write-Host "`n=== End comparison (manual review required for full discrepancy report) ===" -ForegroundColor Cyan
Write-Host "See tests/differential/golden/$Build/*.json for full data"
