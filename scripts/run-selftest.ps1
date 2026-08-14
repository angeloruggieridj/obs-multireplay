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

.PARAMETER SkipReopen
  Stop after the take. By default the gate then relaunches OBS on the same
  project folder, with nothing recording, to check that a REOPENED project
  still has a timeline (see the reopen pass at the bottom).

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
    [switch]$SkipBuild,
    [switch]$SkipReopen
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
# Measurement window + engine checks + the dock pass, which now replays a
# two-angle sequence end to end (two 5 s clips in real time). A run that is
# merely slow must not be reported as "no report at all".
$timeout = $Seconds + 120
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
if ($r.verdict -ne 'PASS') {
    Fail 'M0 GATE: FAIL'
    Write-Host '--- plugin lines from the OBS log ---' -ForegroundColor DarkGray
    Select-String -Path $log.FullName -Pattern '\[tap\]|\[selftest\]|osi-branch-output' |
        Select-Object -Last 40 | ForEach-Object { $_.Line }
    exit 1
}

# --- 8. Second pass: the project as it is REOPENED ---------------------------
# The run above proves the engine inside a take. It cannot prove what happens
# when OBS is closed and started again on the same project, because its ring is
# still full of that take - so the live edge exists no matter what the files on
# disk say. That is exactly the case that was broken (a flat position bar over
# hours of usable footage), so it gets its own OBS process, on the folder the
# first pass just left behind, with nothing recording.
if ($SkipReopen) {
    Write-Host 'M0 GATE: PASS (reopen pass skipped)' -ForegroundColor Green
    exit 0
}

$reopenReport = Join-Path $env:TEMP 'obs-multireplay-selftest-reopen.json'
if (Test-Path $reopenReport) { Remove-Item $reopenReport -Force }

# The pass finds the project by NAME (MRSelfTest), through the same Open Project
# call the operator's menu makes - no path is handed to it. It restores the
# operator's own project and deletes the test one before it finishes.
$env:OBS_MULTIREPLAY_SELFTEST_REOPEN = '1'
$env:OBS_MULTIREPLAY_SELFTEST_OUT = $reopenReport

Step 'Relaunching OBS on the same project (reopen pass)'
$proc2 = Start-Process -FilePath $obsExe -WorkingDirectory $obsDir `
    -ArgumentList '--disable-shutdown-check', '--multi' -PassThru

# The file lengths are demuxed one per watcher pass and only accepted when two
# reads agree, so this pass waits in the tens of seconds by design.
$waited = 0
while (-not (Test-Path $reopenReport) -and $waited -lt 150) {
    if ($proc2.HasExited) { break }
    Start-Sleep -Seconds 2
    $waited += 2
}
$haveReopen = Test-Path $reopenReport

Step 'Closing OBS'
if (-not $proc2.HasExited) {
    $proc2.CloseMainWindow() | Out-Null
    Start-Sleep -Seconds 5
    if (-not $proc2.HasExited) { Stop-Process -Id $proc2.Id -Force }
}
Start-Sleep -Seconds 2

Remove-Item Env:OBS_MULTIREPLAY_SELFTEST_REOPEN -ErrorAction SilentlyContinue

$log2 = Get-ChildItem "$env:APPDATA\obs-studio\logs\*.txt" |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1
Write-Host ''
Write-Host "OBS log (reopen): $($log2.FullName)" -ForegroundColor DarkGray

if (-not $haveReopen) {
    Fail "No reopen report at $reopenReport - that pass did not complete."
    Select-String -Path $log2.FullName -Pattern '\[selftest\]|\[segments\]' |
        Select-Object -Last 30 | ForEach-Object { $_.Line }
    exit 2
}

$json2 = Get-Content $reopenReport -Raw
Write-Host '--- REOPEN REPORT ---' -ForegroundColor Yellow
Write-Host $json2
$r2 = $json2 | ConvertFrom-Json
if ($r2.verdict -eq 'PASS') {
    Write-Host 'M0 GATE: PASS (take + reopen)' -ForegroundColor Green
    exit 0
} else {
    Fail 'M0 GATE: FAIL (reopened project)'
    Select-String -Path $log2.FullName -Pattern '\[selftest\]|\[segments\]' |
        Select-Object -Last 30 | ForEach-Object { $_.Line }
    exit 1
}
