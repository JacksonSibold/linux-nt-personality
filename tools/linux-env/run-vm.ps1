# tools/linux-env/run-vm.ps1 - boot the NT kernel in host QEMU (TCG) with an initramfs and
# rebuild the guest results from the serial log.
#
# Windows counterpart of `vm/runvm.sh` with VM_TRANSPORT=initrd-serial. It runs QEMU with
# -kernel/-initrd (no 9p, no KVM), with a wall-clock timeout, captures serial to a file, then
# runs tools/vm_serial_collect.py, which verifies every frame and writes .dodresult/ under -OutDir.
#
# This is a boot-and-collect runner, not a verifier. It does not run the freshness gate, and its
# results are not gate evidence. The gate-bearing runner is vm/runvm.sh on a Linux host.
#
# Application Control: if QEMU is refused (exit 126, or the start is blocked), the launch is retried
# once and the attempt is recorded in <OutDir>\run-vm-record.txt. The block is never bypassed.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File tools\linux-env\run-vm.ps1 `
#       -Initrd <initramfs.cpio.gz> -OutDir <dir> [-Test /mnt/share/vm/test_m0.sh] [-TimeoutSec 240]
#
# Exit: the guest's result.exitcode when the serial emit verifies; 124 when the guest did not
# finish its emit (timeout or crash); 1 on a transport verification failure; 126 when QEMU was
# refused twice; 2 on usage errors.
param(
    [string]$LinuxEnv = 'C:\Users\Jackson\repos\linux-env',
    [string]$KernelDir = 'C:\Users\Jackson\repos\linux-env\kernel-6.18.56',
    [Parameter(Mandatory = $true)][string]$Initrd,
    [Parameter(Mandatory = $true)][string]$OutDir,
    [string]$Test = '/mnt/share/vm/test_m0.sh',
    [int]$TimeoutSec = 240,
    [int]$Mem = 1024,
    [int]$Cpus = 1,
    [string]$Python = $(if ($env:NTPERS_PYTHON) { $env:NTPERS_PYTHON } else { 'python' })
)
$ErrorActionPreference = 'Stop'

$RepoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$qemu = Join-Path $LinuxEnv 'qemu\qemu-system-x86_64.exe'
$bz = Join-Path $KernelDir 'bzImage'
$collector = Join-Path $RepoRoot 'tools\vm_serial_collect.py'
$resultFile = Join-Path $OutDir '.dodresult\result.exitcode'
$record = Join-Path $OutDir 'run-vm-record.txt'

foreach ($p in @($qemu, $bz, $Initrd, $collector)) {
    if (-not (Test-Path -LiteralPath $p)) {
        Write-Output "run-vm: missing $p"
        exit 2
    }
}

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
Remove-Item -ErrorAction SilentlyContinue -LiteralPath $resultFile
$stamp = (Get-Date).ToUniversalTime().ToString('yyyyMMddTHHmmssZ')
$log = Join-Path $OutDir "serial-$stamp.log"

$append = "console=ttyS0,115200 earlyprintk=serial,ttyS0,115200 panic=-1 dodtransport=initrd-serial dodtest=$Test"
$qemuArgs = @(
    '-name', 'ntpers-serial',
    '-accel', 'tcg',
    '-machine', 'q35',
    '-cpu', 'max',
    '-m', "$Mem",
    '-smp', "$Cpus",
    '-kernel', ('"' + $bz + '"'),
    '-initrd', ('"' + $Initrd + '"'),
    '-append', ('"' + $append + '"'),
    '-display', 'none',
    '-monitor', 'none',
    '-nic', 'none',
    '-no-reboot',
    '-serial', ('"file:' + $log + '"')
)

function Invoke-Qemu {
    param([string[]]$Arguments)
    if (Test-Path -LiteralPath $log) { Remove-Item -LiteralPath $log -Force }
    try {
        $proc = Start-Process -FilePath $qemu -ArgumentList $Arguments -PassThru -WindowStyle Hidden
    } catch {
        return [pscustomobject]@{ Rc = 126; TimedOut = $false; Error = $_.Exception.Message }
    }
    if ($proc.WaitForExit($TimeoutSec * 1000)) {
        return [pscustomobject]@{ Rc = $proc.ExitCode; TimedOut = $false; Error = '' }
    }
    $proc.Kill()
    return [pscustomobject]@{ Rc = 124; TimedOut = $true; Error = '' }
}

Write-Output "run-vm: kernel=$bz"
Write-Output "run-vm: initrd=$Initrd"
Write-Output "run-vm: test=$Test"
Write-Output "run-vm: timeout=${TimeoutSec}s"
Write-Output "run-vm: log=$log"

$run = Invoke-Qemu -Arguments $qemuArgs
$attempts = 1
if ($run.Rc -eq 126) {
    $note = "attempt 1 refused (rc=126) $($run.Error); retrying once"
    Write-Output "run-vm: $note"
    Add-Content -LiteralPath $record -Value "$stamp $note"
    $run = Invoke-Qemu -Arguments $qemuArgs
    $attempts = 2
}
$summary = "attempt $attempts qemu rc=$($run.Rc) timed_out=$($run.TimedOut) $($run.Error)"
Add-Content -LiteralPath $record -Value "$stamp $summary"
Write-Output "run-vm: qemu $summary"
if ($run.Rc -eq 126 -and -not $run.TimedOut) {
    Write-Output "run-vm: QEMU refused to start twice (Application Control or policy). Not bypassed."
    exit 126
}

$global:LASTEXITCODE = 0
try {
    & $Python $collector --log $log --out $OutDir
    $collectRc = $LASTEXITCODE
} catch {
    Write-Output "run-vm: cannot run collector with '$Python': $($_.Exception.Message)"
    exit 2
}
Write-Output "run-vm: collector rc=$collectRc"

if ($collectRc -eq 0 -and (Test-Path -LiteralPath $resultFile)) {
    $guest = (Get-Content -Raw -LiteralPath $resultFile).Trim()
    Write-Output "run-vm: guest exited with code $guest"
    exit [int]$guest
}
if ($collectRc -eq 3 -or $run.TimedOut) {
    Write-Output "run-vm: TIMEOUT or incomplete emit after ${TimeoutSec}s (no result files)"
    Get-Content -LiteralPath $log -Tail 40 -ErrorAction SilentlyContinue
    exit 124
}
Write-Output "run-vm: ERROR serial transport verification failed (collector rc $collectRc); see $log"
exit 1
