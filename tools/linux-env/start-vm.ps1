# Starts the Debian 12 guest with QEMU for Windows (user-scope, no elevation).
# Boot path: the image's own kernel (vmlinuz-6.1.0-53-cloud-amd64) + initrd from initrd/build-initrd.sh.
# The image's GRUB boot is not used: cloud-init never ran there, so no SSH host keys or user were created.
# -Shell boots to a root bash on the serial console (ntpers.shell=bash) for provisioning.
# Ports: SSH 127.0.0.1:2222 -> guest:22; QEMU monitor 127.0.0.1:4444; serial console 127.0.0.1:5555 (TCP, via serial-cmd.ps1).
# Accelerator: TCG only. WHPX was tried once and produced no console output in 7 minutes (not verified as working).
# Usage: powershell -ExecutionPolicy Bypass -File tools\linux-env\start-vm.ps1 [-Shell] [-Mem 4096] [-Cpus 4]
param(
    [string]$LinuxEnv = 'C:\Users\Jackson\repos\linux-env',
    [switch]$Shell,
    [int]$Mem = 4096,
    [int]$Cpus = 4,
    [int]$SshPort = 2222,
    [int]$MonPort = 4444,
    [int]$SerialPort = 5555
)
$ErrorActionPreference = 'Stop'

$qemu = Join-Path $LinuxEnv 'qemu\qemu-system-x86_64.exe'
$qimg = Join-Path $LinuxEnv 'qemu\qemu-img.exe'
$base = Join-Path $LinuxEnv 'downloads\debian\debian-12-genericcloud-amd64.qcow2'
$disk = Join-Path $LinuxEnv 'disk\ntpers-guest.qcow2'
$kernel = Join-Path $LinuxEnv 'kernel\vmlinuz-6.1.0-53-cloud-amd64'
$initrd = Join-Path $LinuxEnv 'initrd\initrd.cpio'
$runDir = Join-Path $LinuxEnv 'run'
$pidFile = Join-Path $runDir 'qemu.pid'
$consoleLog = Join-Path $runDir 'console.log'

foreach ($p in @($qemu, $base, $kernel, $initrd)) {
    if (-not (Test-Path $p)) { throw "missing: $p (run fetch-artifacts.sh and build-initrd.sh)" }
}
New-Item -ItemType Directory -Force -Path (Split-Path $disk), $runDir | Out-Null

if (Test-Path $pidFile) {
    $old = [int]((Get-Content $pidFile -Raw).Trim())
    if (Get-Process -Id $old -ErrorAction SilentlyContinue) {
        throw "guest already running (pid $old); run stop-vm.ps1 first"
    }
}

# Copy-on-write overlay over the pristine base image (created once; the base image is never written).
if (-not (Test-Path $disk)) {
    & $qimg create -f qcow2 -F qcow2 -b $base $disk 25G | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'qemu-img create failed' }
}

$cmdline = if ($Shell) { 'console=ttyS0,115200 rw ntpers.shell=bash' } else { 'console=ttyS0,115200 rw' }
$argString = @(
    '-name ntpers-guest',
    '-accel tcg',
    '-machine q35',
    '-cpu max',
    "-m $Mem",
    "-smp $Cpus",
    "-kernel `"$kernel`"",
    "-initrd `"$initrd`"",
    "-append `"$cmdline`"",
    "-drive if=virtio,format=qcow2,file=`"$disk`"",
    "-netdev user,id=net0,hostfwd=tcp:127.0.0.1:$SshPort-:22",
    '-device virtio-net-pci,netdev=net0',
    '-display none',
    "-chardev socket,id=ser0,host=127.0.0.1,port=$SerialPort,server=on,wait=off,logfile=`"$consoleLog`",logappend=on",
    '-serial chardev:ser0',
    "-monitor tcp:127.0.0.1:${MonPort},server=on,wait=off",
    "-pidfile `"$pidFile`""
) -join ' '

$proc = Start-Process -FilePath $qemu -ArgumentList $argString -WindowStyle Hidden -PassThru -RedirectStandardError (Join-Path $runDir 'qemu-err.txt')
Start-Sleep -Seconds 3
if ($proc.HasExited) { throw "qemu exited early (code $($proc.ExitCode)); see $runDir\qemu-err.txt" }
$mode = if ($Shell) { 'provisioning shell' } else { 'systemd' }
Write-Host "guest starting ($mode), qemu pid $($proc.Id); ssh: tools/linux-env/ssh-guest.sh (port $SshPort)"
