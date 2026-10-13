# Boots the NT kernel (bzImage) and the M0 initramfs in a second QEMU VM on the host, with -kernel.
# Serial output goes to m0-serial.log in the kernel directory; the script prints its tail.
# No network (-nic none), no display, no monitor. QEMU runs TCG only, as user code, no elevation.
# Usage: powershell -ExecutionPolicy Bypass -File tools\linux-env\boot-m0-host.ps1 [-TimeoutSec 240]
param(
    [string]$LinuxEnv = 'C:\Users\Jackson\repos\linux-env',
    [string]$KernelDir = 'C:\Users\Jackson\repos\linux-env\kernel-6.18.56',
    [int]$TimeoutSec = 240,
    [int]$Mem = 1024,
    [int]$Cpus = 1,
    # Initramfs and serial log, relative to KernelDir. Defaults are the M0 boot; the differential
    # probes use -Initrd probes-initramfs.cpio.gz -Serial probes-serial.log (tools/linux-env/probes-initramfs.sh).
    [string]$Initrd = 'm0-initramfs.cpio.gz',
    [string]$Serial = 'm0-serial.log',
    # Kernel command line. The probes boot adds log_buf_len=16M: the default printk ring is 128 KB
    # (CONFIG_LOG_BUF_SHIFT=17) and the probes' stdout is several MB, so the ring would wrap before
    # the serial console drains it (docs/status/DIFFERENTIAL-RESULTS.md).
    [string]$Append = 'console=ttyS0,115200 earlyprintk=serial,ttyS0,115200 panic=-1'
)
$ErrorActionPreference = 'Stop'

$qemu = Join-Path $LinuxEnv 'qemu\qemu-system-x86_64.exe'
$bz   = Join-Path $KernelDir 'bzImage'
$ird  = Join-Path $KernelDir $Initrd
$log  = Join-Path $KernelDir $Serial

foreach ($p in @($qemu, $bz, $ird)) {
    if (-not (Test-Path $p)) { throw "missing: $p" }
}
Remove-Item -ErrorAction SilentlyContinue $log

$qargs = @(
    '-name', 'ntpers-m0',
    '-accel', 'tcg',
    '-machine', 'q35',
    '-cpu', 'max',
    '-m', "$Mem",
    '-smp', "$Cpus",
    '-kernel', $bz,
    '-initrd', $ird,
    '-append', ('"' + $Append + '"'),
    '-display', 'none',
    '-monitor', 'none',
    '-nic', 'none',
    '-no-reboot',
    '-serial', "file:$log"
)

$proc = Start-Process -FilePath $qemu -ArgumentList $qargs -PassThru -WindowStyle Hidden
if ($proc.WaitForExit($TimeoutSec * 1000)) {
    Write-Output "qemu exited, rc=$($proc.ExitCode)"
} else {
    $proc.Kill()
    Write-Output "TIMEOUT after $TimeoutSec s; qemu killed"
}
if (Test-Path $log) {
    Get-Content $log -Tail 200
} else {
    Write-Output "no serial log at $log"
}
