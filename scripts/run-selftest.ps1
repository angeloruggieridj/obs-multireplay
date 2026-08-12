<#
.SYNOPSIS
  Runs the obs-multireplay M0 gate end to end, unattended.

.DESCRIPTION
  Builds the plugin, installs it into the per-user OBS plugin directory (no
  admin rights needed), launches OBS with the self-test environment, waits for
  the JSON verdict the plugin writes, closes OBS and prints the result.

  Exit code 0 = PASS, 1 = FAIL, 2 = the run never produced a report.

.PARAMETER Seconds
  Measurement window inside OBS (default 25).

.PARAMETER Cams
  Number of synthetic cameras to create (default 2). Ignored when -Sources is given.

.PARAMETER Sources
  Comma-separated names of EXISTING OBS sources to tap instead of synthetic
  ones - this is how the gate runs against the real capture cards.

.PARAMETER SkipBuild
  Reuse the DLL already in build_x64 instead of rebuilding.

.EXAMPLE
  pwsh -File scripts/run-selftest.ps1
.EXAMPLE
  pwsh -File scripts/run-selftest.ps1 -Sources "C1,Cam2" -Seconds 60
#>
[CmdletBinding()]
param(
    [int]$Seconds = 25,
    [int]$Cams = 2,
    [string]$Sources = "",
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$buildDir = Join-Path $repo 'build_x64'
$obsExe = 'C:\Program Files\obs-studio\bin\64bit\obs64.exe'
$obsDir = Split-Path -Parent $obsExe
# On Windows OBS only scans %ProgramData%\obs-studio\plugins - the per-user
# %APPDATA% plugin path is a macOS/Linux convention and is NOT searched here.
# This directory is user-owned (recreated once), so installing needs no admin.
$pluginDir = 'C:\ProgramData\obs-studio\plugins\obs-multireplay'
$report = Join-Path $env:TEMP 'obs-multireplay-selftest.json'

function Step($msg) { Write-Host "==> $msg" -ForegroundColor Cyan }
function Fail($msg) { Write-Host "!!! $msg" -ForegroundColor Red }

if (-not (Test-Path $obsExe)) { Fail "OBS not found at $obsExe"; exit 2 }

# --- 0. OBS must not already be running (it would ignore our environment) ----
$running = Get-Process obs64 -ErrorAction SilentlyContinue
if ($running) {
    Fail "OBS is already running (PID $($running.Id)). Close it and retry."
    exit 2
}

# --- 1. Build ----------------------------------------------------------------
if (-not $SkipBuild) {
    Step 'Building'
    & cmake --build $buildDir --config RelWithDebInfo | Out-Null
    if ($LASTEXITCODE -ne 0) { Fail 'build failed'; exit 2 }
}

# --- 2. Unit tests -----------------------------------------------------------
Step 'Unit tests'
& ctest --test-dir $buildDir -C RelWithDebInfo --output-on-failure
if ($LASTEXITCODE -ne 0) { Fail 'unit tests failed'; exit 1 }

# --- 3. Install into the per-user plugin path (no admin) ---------------------
Step "Installing to $pluginDir"
New-Item -ItemType Directory -Force -Path "$pluginDir\bin\64bit" | Out-Null
New-Item -ItemType Directory -Force -Path "$pluginDir\data\locale" | Out-Null
Copy-Item "$buildDir\RelWithDebInfo\obs-multireplay.dll" "$pluginDir\bin\64bit\" -Force
Copy-Item "$buildDir\RelWithDebInfo\obs-multireplay.pdb" "$pluginDir\bin\64bit\" -Force -ErrorAction SilentlyContinue
Copy-Item "$repo\data\locale\*.ini" "$pluginDir\data\locale\" -Force

# --- 4. Make sure OBS' plugin manager has the module enabled -----------------
$modulesJson = Join-Path $env:APPDATA 'obs-studio\plugin_manager\modules.json'
if (Test-Path $modulesJson) {
    $mods = Get-Content $modulesJson -Raw | ConvertFrom-Json
    $entry = $mods | Where-Object module_name -eq 'obs-multireplay'
    if ($entry -and -not $entry.enabled) {
        Step 'Enabling obs-multireplay in the OBS plugin manager'
        $entry.enabled = $true
        $mods | ConvertTo-Json -Depth 10 | Set-Content $modulesJson -Encoding UTF8
    }
}

# --- 5. Launch OBS with the self-test environment ----------------------------
if (Test-Path $report) { Remove-Item $report -Force }

$env:OBS_MULTIREPLAY_SELFTEST = '1'
$env:OBS_MULTIREPLAY_SELFTEST_OUT = $report
$env:OBS_MULTIREPLAY_SELFTEST_SECS = "$Seconds"
$env:OBS_MULTIREPLAY_SELFTEST_CAMS = "$Cams"
$env:OBS_MULTIREPLAY_DEBUG = '1'
if ($Sources) { $env:OBS_MULTIREPLAY_SELFTEST_SOURCES = $Sources }
else { Remove-Item Env:OBS_MULTIREPLAY_SELFTEST_SOURCES -ErrorAction SilentlyContinue }

Step "Launching OBS (window will appear; measurement window ${Seconds}s)"
# --disable-shutdown-check stops OBS offering safe mode after we close it.
$proc = Start-Process -FilePath $obsExe -WorkingDirectory $obsDir `
    -ArgumentList '--disable-shutdown-check', '--multi' -PassThru

# --- 6. Wait for the plugin to write its verdict -----------------------------
$timeout = $Seconds + 75
$waited = 0
while (-not (Test-Path $report) -and $waited -lt $timeout) {
    if ($proc.HasExited) { break }
    Start-Sleep -Seconds 2
    $waited += 2
}

$haveReport = Test-Path $report

Step 'Closing OBS'
if (-not $proc.HasExited) {
    $proc.CloseMainWindow() | Out-Null
    Start-Sleep -Seconds 5
    if (-not $proc.HasExited) { Stop-Process -Id $proc.Id -Force }
}
Start-Sleep -Seconds 2

# --- 7. Verdict --------------------------------------------------------------
$log = Get-ChildItem "$env:APPDATA\obs-studio\logs\*.txt" |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1
Write-Host ''
Write-Host "OBS log: $($log.FullName)" -ForegroundColor DarkGray

if (-not $haveReport) {
    Fail "No report at $report - the self-test did not complete."
    Write-Host '--- plugin lines from the OBS log ---' -ForegroundColor DarkGray
    Select-String -Path $log.FullName -Pattern 'multireplay|\[tap\]|\[selftest\]|branch' |
        Select-Object -Last 40 | ForEach-Object { $_.Line }
    exit 2
}

$json = Get-Content $report -Raw
Write-Host '--- M0 REPORT ---' -ForegroundColor Yellow
Write-Host $json
$r = $json | ConvertFrom-Json
if ($r.verdict -eq 'PASS') {
    Write-Host 'M0 GATE: PASS' -ForegroundColor Green
    exit 0
} else {
    Fail 'M0 GATE: FAIL'
    Write-Host '--- plugin lines from the OBS log ---' -ForegroundColor DarkGray
    Select-String -Path $log.FullName -Pattern '\[tap\]|\[selftest\]|osi-branch-output' |
        Select-Object -Last 40 | ForEach-Object { $_.Line }
    exit 1
}
