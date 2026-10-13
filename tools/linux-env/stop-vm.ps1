# Stops the guest: sends system_powerdown via the QEMU monitor, waits, then falls back to killing the process.
# Usage: powershell -ExecutionPolicy Bypass -File tools\linux-env\stop-vm.ps1 [-LinuxEnv C:\...\linux-env]
param(
    [string]$LinuxEnv = 'C:\Users\Jackson\repos\linux-env',
    [int]$MonPort = 4444,
    [int]$WaitSeconds = 90
)
$ErrorActionPreference = 'Stop'
$pidFile = Join-Path $LinuxEnv 'run\qemu.pid'
if (-not (Test-Path $pidFile)) { Write-Host 'no pid file; guest not running'; exit 0 }
$pid_ = [int]((Get-Content $pidFile -Raw).Trim())
if (-not (Get-Process -Id $pid_ -ErrorAction SilentlyContinue)) {
    Remove-Item $pidFile -ErrorAction SilentlyContinue
    Write-Host 'guest not running (stale pid file removed)'
    exit 0
}

try {
    $client = New-Object System.Net.Sockets.TcpClient('127.0.0.1', $MonPort)
    $writer = New-Object System.IO.StreamWriter($client.GetStream())
    $writer.AutoFlush = $true
    $writer.WriteLine('system_powerdown')
    $client.Close()
    Write-Host 'sent system_powerdown'
} catch {
    Write-Host "monitor unavailable: $($_.Exception.Message)"
}

$deadline = (Get-Date).AddSeconds($WaitSeconds)
while ((Get-Process -Id $pid_ -ErrorAction SilentlyContinue) -and ((Get-Date) -lt $deadline)) {
    Start-Sleep -Seconds 2
}
if (Get-Process -Id $pid_ -ErrorAction SilentlyContinue) {
    Write-Host 'guest did not power off in time; stopping qemu process'
    Stop-Process -Id $pid_ -Force
}
Remove-Item $pidFile -ErrorAction SilentlyContinue
Write-Host 'stopped'
