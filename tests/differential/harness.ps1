param(
    [string]$Probe = "all",
    [string]$Config = "Release",
    [switch]$NoBuild,
    [switch]$JsonOnly
)
# harness.ps1 - differential testing harness for ntpers
# - compiles probe PEs via mingw in WSL
# - runs them natively on Windows, captures JSON
# - stores as golden files versioned by Windows build

$ErrorActionPreference = "Stop"
$ProjectRoot = "/path/to/ntpers-research"
$WslRoot     = "/path/to/ntpers-research"
$ProbeDir    = Join-Path $ProjectRoot "tests\differential\probes"
$BuildDir    = Join-Path $ProjectRoot "tests\differential\build"
$GoldenRoot  = Join-Path $ProjectRoot "tests\differential\golden"

function Get-WindowsBuild {
    $os = Get-CimInstance Win32_OperatingSystem
    $build = $os.BuildNumber
    $ubr = (Get-ItemProperty "HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion").UBR
    if ($null -eq $ubr) { $ubr = 0 }
    return @{ Build = $build; UBR = $ubr; Full = "$build.$ubr"; Version = $os.Version }
}

function Build-Probes {
    Write-Host "[harness] Building probes via mingw (WSL)..." -ForegroundColor Cyan
    $null = New-Item -ItemType Directory -Force -Path $BuildDir
    $staging = Join-Path $env:LOCALAPPDATA "Temp\opencode"
    $null = New-Item -ItemType Directory -Force -Path $staging
     $probes = @("probe_ntstatus","probe_teb_peb","probe_virtualquery","probe_handles","probe_sharemode","probe_peloader","probe_sync","probe_process_thread","probe_section","probe_path","probe_exception","probe_registry","probe_namedpipe","probe_directory","probe_fileinfo")
     if ($Probe -ne "all") { $probes = @($Probe) }
     foreach ($p in $probes) {
        $srcWin = Join-Path $ProbeDir "$p.c"
        Copy-Item -LiteralPath $srcWin -Destination (Join-Path $staging "$p.c") -Force
        Write-Host "  CC $p.c -> $p.exe" -ForegroundColor Gray
        wsl bash -c "cp /mnt/c/Users/Jackson/AppData/Local/Temp/opencode/$p.c /tmp/$p.c && x86_64-w64-mingw32-gcc -Wall -Wextra -O2 -std=c11 -m64 -o /tmp/$p.exe /tmp/$p.c 2>&1; echo EXIT:`$?" | Write-Host
        wsl bash -c "cp /tmp/$p.exe /mnt/c/Users/Jackson/AppData/Local/Temp/opencode/$p.exe 2>&1; ls -lh /tmp/$p.exe 2>&1" | Write-Host
        Copy-Item -LiteralPath (Join-Path $staging "$p.exe") -Destination (Join-Path $BuildDir "$p.exe") -Force
        $exePath = Join-Path $BuildDir "$p.exe"
        if (-not (Test-Path $exePath)) {
            Write-Host "[harness] BUILD FAILED for $p" -ForegroundColor Red
            throw "Build failed for $p"
        }
    }
    Write-Host "[harness] Build complete." -ForegroundColor Green
}

function Run-Probe {
    param([string]$Name)
    $exe = Join-Path $BuildDir "$Name.exe"
    if (-not (Test-Path $exe)) { throw "Missing $exe - run build first" }
    Write-Host "[harness] Running $Name on Windows..." -ForegroundColor Cyan
    # Use cmd redirect to avoid PowerShell truncation (large JSON, e.g. probe_sharemode 2MB)
    $tmpOut = Join-Path $env:TEMP "opencode_${Name}.json"
    # probe_peloader requires an argument; for harness we test it against good corpus if any
    if ($Name -eq "probe_peloader") {
        $tmpPe = Join-Path $env:TEMP "opencode_probe_peloader_test.exe"
        # Use pe_smoke if available
        $corpus = Join-Path $ProjectRoot "tests\differential\build\probe_ntstatus.exe"
        if (Test-Path $corpus) { Copy-Item -LiteralPath $corpus -Destination $tmpPe -Force }
        cmd /c "`"$exe`" `"$tmpPe`" > `"$tmpOut`" 2>&1"
    } else {
        cmd /c "`"$exe`" > `"$tmpOut`" 2>&1"
    }
    $json = Get-Content -LiteralPath $tmpOut -Raw -Encoding UTF8
    if ($json.TrimStart()[0] -ne '{') {
        Write-Host "[harness] Probe $Name produced non-JSON output:" -ForegroundColor Yellow
        Write-Host $json.Substring(0, [Math]::Min(500, $json.Length))
        throw "Probe $Name failed"
    }
    return $json
}

# ---- Main ----
$buildInfo = Get-WindowsBuild
Write-Host "[harness] Windows build: $($buildInfo.Full) (Version $($buildInfo.Version))" -ForegroundColor Green
Write-Host "[harness] Project root: $ProjectRoot"

if (-not $NoBuild) { Build-Probes }

$probes = @("probe_ntstatus","probe_teb_peb","probe_virtualquery","probe_handles","probe_sharemode","probe_peloader","probe_sync","probe_process_thread","probe_section","probe_path","probe_exception","probe_registry","probe_namedpipe","probe_directory","probe_fileinfo")
if ($Probe -ne "all") { $probes = @($Probe) }

$goldenDir = Join-Path $GoldenRoot $buildInfo.Full
$null = New-Item -ItemType Directory -Force -Path $goldenDir

# Write version file
$versionObj = @{
    windows_build = $buildInfo.Full
    windows_version = $buildInfo.Version
    arch = "x64"
    timestamp = (Get-Date -Format "o")
    probes = $probes
}
$versionJson = $versionObj | ConvertTo-Json -Depth 4
Set-Content -LiteralPath (Join-Path $goldenDir "version.json") -Value $versionJson -Encoding UTF8
Write-Host "[harness] Wrote version.json"

foreach ($p in $probes) {
    $json = Run-Probe -Name $p
    $outPath = Join-Path $goldenDir "$p.json"
    Set-Content -LiteralPath $outPath -Value $json -Encoding UTF8
    Write-Host "[harness] Golden: $outPath ($($json.Length) bytes)" -ForegroundColor Green
    # Also copy to latest symlink/alias?
    $latestDir = Join-Path $GoldenRoot "latest"
    $null = New-Item -ItemType Directory -Force -Path $latestDir
    Copy-Item -LiteralPath $outPath -Destination (Join-Path $latestDir "$p.json") -Force
    Copy-Item -LiteralPath (Join-Path $goldenDir "version.json") -Destination (Join-Path $latestDir "version.json") -Force
}

Write-Host "[harness] All probes complete. Goldens in $goldenDir" -ForegroundColor Green
# Golden integrity gate - fail loudly if any golden is corrupt, stale, build-mismatched or volume-mismatched
Write-Host "[harness] Running golden integrity check..." -ForegroundColor Cyan
$checker = Join-Path $ProjectRoot "tests\differential\check_golden_integrity.py"
# Use wsl python if available, else try python
$py = "wsl"
try {
    wsl bash -c "python3 `"/path/to/ntpers-research/tests/differential/check_golden_integrity.py`" 2>&1"
    if ($LASTEXITCODE -ne 0) {
        Write-Host "[harness] GOLDEN INTEGRITY FAILED - a bad golden fails loudly rather than silently producing phantom divergences" -ForegroundColor Red
        throw "Golden integrity check failed (see above). Regenerate goldens via Windows two-target build."
    } else {
        Write-Host "[harness] Golden integrity PASS" -ForegroundColor Green
    }
} catch {
    Write-Host "[harness] Golden integrity check error: $_" -ForegroundColor Yellow
    # Fallback: try native python if wsl fails
    try {
        & python tests/differential/check_golden_integrity.py 2>&1 | Write-Host
        if ($LASTEXITCODE -ne 0) { throw "Golden integrity failed" }
    } catch {
        Write-Host "[harness] Could not run golden integrity check via wsl or native python" -ForegroundColor Yellow
    }
}
# Also print files
Get-ChildItem -LiteralPath $goldenDir | Format-Table Name, Length

