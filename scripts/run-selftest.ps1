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

.PARAMETER SoakMinutes
  M4 soak: after the normal gate passes, record for this many minutes in a
  project of its own and judge the SHAPE of a long run - every angle still
  producing in every 15 s interval, ring inside its budget, resident memory not
  climbing away from it, nothing malformed, OBS keeping its frames. 0 = skip
  (the default: an hour of recording is not something to start by accident).

.EXAMPLE
  pwsh -File scripts/run-selftest.ps1
.EXAMPLE
  pwsh -File scripts/run-selftest.ps1 -Sources "C1,C2" -Seconds 60
.EXAMPLE
  pwsh -File scripts/run-selftest.ps1 -Sources "C1,C2" -SoakMinutes 60
#>
[CmdletBinding()]
param(
    [int]$Seconds = 25,
    [int]$Cams = 2,
    [string]$Sources = "",
    [switch]$SkipBuild,
    [switch]$SkipReopen,
    [int]$SoakMinutes = 0
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

# --- 4b. A SCENE COLLECTION AND A PLUGIN CONFIG OF ITS OWN -------------------
# The gate used to run inside the operator's own scene collection and his own
# MultiReplay config, and the two got in each other's way three separate ways in
# one afternoon:
#
#   - it armed whatever OBS sources were NAMED in -Sources, which is not the
#     same thing as the angle labels on the panel: camera 1 was sourceName
#     "Media" (PARTITA.mp4) with displayName "C1", while two older sources were
#     literally called C1 and C2. The gate recorded and replayed the old ones
#     while the panel said C1;
#   - it rewrote his camera configuration and had to put it back afterwards,
#     which only works if the run survives to the end;
#   - OBS saves the scene collection on a clean exit and NOT on a crash, so a
#     run that died took his unsaved source edits with it.
#
# So the gate now builds a collection of its own containing only the sources it
# was asked for (copied WITHOUT their filters, Branch Output ones included),
# points OBS at it with --collection, and deletes it afterwards. His config.json
# is moved aside for the duration and put back, and global.ini's record of which
# collection is current is restored too: without that his next manual launch
# would open a collection this script had already deleted.
$scenesDir = Join-Path $env:APPDATA 'obs-studio\basic\scenes'
$globalIni = Join-Path $env:APPDATA 'obs-studio\global.ini'
$pluginCfg = Join-Path $env:APPDATA 'obs-studio\plugin_config\obs-multireplay\config.json'
$testCollection = 'MRSelfTest'
$testCollectionFile = Join-Path $scenesDir "$testCollection.json"
$cfgBackup = Join-Path $env:TEMP 'obs-multireplay-config.backup.json'
$iniBackup = Join-Path $env:TEMP 'obs-multireplay-global.backup.ini'

# global.ini is backed up so the operator's current-collection setting can be
# handed back, but it is NOT used to FIND the sources. It is written on exit and
# was measured saying "Senza titolo" while the collection actually in use was
# REC_schermo — believing it would have failed a run for a source that was right
# there. The sources are looked for in every collection instead, most recently
# modified first, which is also the honest answer to "where does this name live".
if (Test-Path $globalIni) { Copy-Item $globalIni $iniBackup -Force }
$collectionFiles = @(Get-ChildItem -Path $scenesDir -Filter '*.json' -ErrorAction SilentlyContinue |
                     Where-Object { $_.BaseName -ne $testCollection } |
                     Sort-Object LastWriteTime -Descending)

# WHICH COLLECTION THE OPERATOR IS ACTUALLY ON, decided here and forced back at
# the end. Not copied from global.ini, and this is the correction to a fault
# this script caused: OBS rewrites global.ini on exit with the collection IT was
# on, which is the gate's. One run that ended without the restore left the file
# naming MRSelfTest; the gate had already deleted it, so OBS fell back to some
# other collection at the next launch — one whose C1 has no file in it — wrote
# THAT into global.ini, and every run afterwards backed the wrong name up and
# put it faithfully back. Restoring a file the program under test rewrites is
# not restoring anything; the name has to be decided and re-asserted.
$operatorCollection = if ($collectionFiles.Count -gt 0) { $collectionFiles[0].BaseName } else { '' }
if ($operatorCollection) { Step "Operator collection is '$operatorCollection' — it will be restored at the end" }

$wantedSources = @()
if ($Sources) { $wantedSources = $Sources.Split(',') | ForEach-Object { $_.Trim() } }

# ONE SCENE PER CAMERA, plus one per replay bay — the shape of a real rig, and
# the shape the operator's own collection has (Cam1 scene / Cam2 scene / Scena /
# Scena B). A single scene holding every camera at once is not something anyone
# would build, and "to output" has nowhere to switch TO.
#
# The two replay scenes are written EMPTY on purpose: "MultiReplay - Replay A"
# and "…B" do not exist until the plugin has loaded, and a scene item naming a
# source that is not there yet is dropped by OBS when it reads the file. The gate
# puts them in at runtime, which is also what it already does for its own output
# scenes.
$collection = [ordered]@{
    name                  = $testCollection
    current_scene         = 'MRCam 1'
    current_program_scene = 'MRCam 1'
    scene_order           = @()
    sources               = @()
}
$copied = @()

function New-MRScene($name, $items) {
    [ordered]@{
        prev_ver = 520093697; name = $name; uuid = [guid]::NewGuid().ToString()
        id = 'scene'; versioned_id = 'scene'; settings = @{ items = $items }
        mixers = 0; sync = 0; flags = 0; volume = 1.0; balance = 0.5
        enabled = $true; muted = $false
        'push-to-mute' = $false; 'push-to-mute-delay' = 0
        'push-to-talk' = $false; 'push-to-talk-delay' = 0; hotkeys = @{}
    }
}
foreach ($want in $wantedSources) {
    $found = $null
    $foundIn = $null
    foreach ($cf in $collectionFiles) {
        $src = Get-Content $cf.FullName -Raw | ConvertFrom-Json
        $hit = $src.sources | Where-Object { $_.name -eq $want } | Select-Object -First 1
        if ($hit) { $found = $hit; $foundIn = $cf.BaseName; break }
    }
    if (-not $found) {
        Fail "no scene collection defines a source named '$want'"
        foreach ($cf in $collectionFiles) {
            $names = (Get-Content $cf.FullName -Raw | ConvertFrom-Json).sources |
                     ForEach-Object { $_.name }
            Write-Host "    $($cf.BaseName): $($names -join ', ')" -ForegroundColor DarkGray
        }
        Restore-OperatorEnvironment
        exit 2
    }
    # Without its filters: a Branch Output filter carried over would arm a
    # recording nobody asked for, writing to a path this run does not own.
    $found.PSObject.Properties.Remove('filters')
    # ...and without the operator's hotkey bindings, which belong to his
    # collection and not to a throwaway one.
    $found.PSObject.Properties.Remove('hotkeys')

    # A UUID OF ITS OWN. Copying the source verbatim carried the operator's uuid
    # across, so his collection and this throwaway one both declared a source
    # with the same one. libobs keeps a process-wide uuid -> source map, and two
    # collections claiming the same entry across a switch is how a camera comes
    # up for an instant and then goes black with its file path emptied — which
    # is exactly what was reported, and it is caused here, not by the plugin.
    $found | Add-Member -NotePropertyName uuid -NotePropertyValue ([guid]::NewGuid().ToString()) -Force

    # A MEDIA SOURCE IS NORMALISED, NOT JUST COPIED. Copying an OBS source
    # object verbatim carries whatever the operator's collection happens to
    # hold, and what it holds is not always complete: C2 came across with
    # local_file set and NO is_local_file, which leaves the flag to a default
    # rather than to a statement. A gate that plays the wrong thing — or
    # nothing — because of an implied default is a gate reporting on something
    # other than the plugin. If it names a file, it says so explicitly.
    if ($found.id -eq 'ffmpeg_source' -and $found.settings) {
        $path = $found.settings.local_file
        if ($path) {
            if (-not (Test-Path $path)) {
                Fail "source '$want' points at a file that is not there: $path"
                Restore-OperatorEnvironment
                exit 2
            }
            $found.settings | Add-Member -NotePropertyName is_local_file -NotePropertyValue $true -Force
            # Looping, because the gate records for longer than these clips run
            # and an angle that reaches its end stops producing packets — which
            # this gate would report, correctly, as a dead angle.
            $found.settings | Add-Member -NotePropertyName looping -NotePropertyValue $true -Force
        } else {
            Fail "source '$want' is a media source with no file in it — nothing to record"
            Restore-OperatorEnvironment
            exit 2
        }
    }
    $collection.sources += $found
    $copied += "$want (from $foundIn)"
    # bounds_type 2 (SCALE_INNER) over the full canvas, centred: C1 and C2 are
    # 720p files and the canvas is 1080p, so an item placed at scale 1.0 would
    # sit in the top-left corner at two thirds size. Fitted, they fill the frame
    # the way the operator has them.
    $item = [ordered]@{
        name = $want; source_uuid = $found.uuid
        visible = $true; locked = $false; rot = 0.0
        scale_ref = @{ x = 1920.0; y = 1080.0 }
        align = 0; bounds_type = 2; bounds_align = 0; bounds_crop = $false
        crop_left = 0; crop_top = 0; crop_right = 0; crop_bottom = 0
        id = 1; group_item_backup = $false
        pos = @{ x = 960.0; y = 540.0 }; pos_rel = @{ x = 0.0; y = 0.0 }
        scale = @{ x = 1.0; y = 1.0 }; scale_rel = @{ x = 1.0; y = 1.0 }
        bounds = @{ x = 1920.0; y = 1080.0 }
        bounds_rel = @{ x = 2.0; y = 2.0 }
        scale_filter = 'disable'; blend_method = 'default'; blend_type = 'normal'
        show_transition = @{ duration = 300 }; hide_transition = @{ duration = 300 }
        private_settings = @{}
    }
    # One scene per camera, named for the slot it feeds.
    $sceneName = "MRCam $($copied.Count)"
    $collection.sources += (New-MRScene $sceneName @($item))
    $collection.scene_order += @{ name = $sceneName }
}
# ...and one per replay bay, empty until the plugin has made its sources.
foreach ($bay in @('MRReplay A', 'MRReplay B')) {
    $collection.sources += (New-MRScene $bay @())
    $collection.scene_order += @{ name = $bay }
}
New-Item -ItemType Directory -Force -Path $scenesDir | Out-Null
$collection | ConvertTo-Json -Depth 30 | Set-Content $testCollectionFile -Encoding UTF8
Step "Scene collection '$testCollection' generated$(if ($copied) { " with $($copied -join ', ')" })"

# His MultiReplay config goes aside for the duration: the gate creates its own
# project and its own camera slots, and must not be able to leave a trace in the
# one he takes into a match.
#
# AND IT IS REPLACED, not just removed. A blank config has no session folder, so
# the pre-flight refuses the take before anything can attach — the first run with
# this isolation came back with cameras_armed 2 and every other check false,
# which is what "no folder to record into" looks like from the outside. The seed
# names the same sources the collection above carries, in the same slots, and
# points the recordings at a directory of this run's own that goes away with it.
$testSessionFolder = Join-Path $env:TEMP 'obs-multireplay-gate-session'
if (Test-Path $pluginCfg) { Move-Item $pluginCfg $cfgBackup -Force }
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $pluginCfg) | Out-Null
New-Item -ItemType Directory -Force -Path $testSessionFolder | Out-Null
$seedCams = @()
for ($i = 0; $i -lt 8; $i++) {
    if ($i -lt $wantedSources.Count) {
        $seedCams += [ordered]@{ sourceName = $wantedSources[$i]; displayName = $wantedSources[$i] }
    } else {
        $seedCams += [ordered]@{ sourceName = ''; displayName = '' }
    }
}
([ordered]@{
    sessionFolder      = $testSessionFolder
    currentProjectName = ''
    videoBitrateKbps   = 4000
    audioBitrateKbps   = 320
    videoEncoderId     = ''
    # The two things the run could not do without these, both reported from a
    # watched run: the big preview stayed black because follow-live mirrors
    # cameras[angle].sourceName and the slots were empty, and "to output" never
    # took the Program because outputSceneName was blank — with no scene named,
    # the coordinator deliberately does not touch it. These name the scenes the
    # collection above carries, one per bay.
    outputSceneName    = 'MRReplay A'
    outputSceneNameB   = 'MRReplay B'
    enableChannelB     = $true
    toOutputOnPlay     = $true
    doubleClickPlays   = $true
    replaySourceName   = ''
    musicSourceName    = ''
    eventIdDigits      = 4
    eventListCount     = 20
    showMultiview      = $true
    cameras            = $seedCams
} | ConvertTo-Json -Depth 10) | Set-Content $pluginCfg -Encoding UTF8
Step "MultiReplay config seeded (session folder $testSessionFolder)"

# Put it all back whatever happens to the run — a failure, a crash, a Ctrl-C.
function Restore-OperatorEnvironment {
    if (Test-Path $testCollectionFile) { Remove-Item $testCollectionFile -Force -ErrorAction SilentlyContinue }
    # The footage this run wrote goes with it: a gate that leaves gigabytes
    # behind is a gate nobody runs twice.
    if (Test-Path $testSessionFolder) { Remove-Item $testSessionFolder -Recurse -Force -ErrorAction SilentlyContinue }
    if (Test-Path $pluginCfg) { Remove-Item $pluginCfg -Force -ErrorAction SilentlyContinue }
    if (Test-Path $cfgBackup) { Move-Item $cfgBackup $pluginCfg -Force -ErrorAction SilentlyContinue }
    if (Test-Path $iniBackup) {
        Copy-Item $iniBackup $globalIni -Force -ErrorAction SilentlyContinue
        Remove-Item $iniBackup -Force -ErrorAction SilentlyContinue
    }
    # The collection name is ASSERTED, not restored: the backup was taken from a
    # file OBS rewrites on exit, so it can already carry the gate's own name or a
    # stale one. See the note where $operatorCollection is decided.
    if ($operatorCollection -and (Test-Path $globalIni)) {
        (Get-Content $globalIni) `
            -replace '^SceneCollection=.*$', "SceneCollection=$operatorCollection" `
            -replace '^SceneCollectionFile=.*$', "SceneCollectionFile=$operatorCollection" |
            Set-Content $globalIni -Encoding UTF8
    }
}
trap { Restore-OperatorEnvironment; break }

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
    -ArgumentList '--disable-shutdown-check', '--multi', '--collection', $testCollection -PassThru

# --- 6. Wait for the plugin to write its verdict -----------------------------
# Measurement window + engine checks + the dock pass, which replays a two-angle
# sequence end to end (two 5 s clips in real time) + the M4 health pass, which
# is a second short take of its own and waits out a killed camera. A run that is
# merely slow must not be reported as "no report at all".
$timeout = $Seconds + 240
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
    Restore-OperatorEnvironment
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
    Restore-OperatorEnvironment
    exit 1
}

# --- 8. Second pass: the project as it is REOPENED ---------------------------
# The run above proves the engine inside a take. It cannot prove what happens
# when OBS is closed and started again on the same project, because its ring is
# still full of that take - so the live edge exists no matter what the files on
# disk say. That is exactly the case that was broken (a flat position bar over
# hours of usable footage), so it gets its own OBS process, on the folder the
# first pass just left behind, with nothing recording.
function Invoke-SoakPass {
    param([int]$Minutes)

    $soakReport = Join-Path $env:TEMP 'obs-multireplay-selftest-soak.json'
    if (Test-Path $soakReport) { Remove-Item $soakReport -Force }

    $env:OBS_MULTIREPLAY_SELFTEST_SOAK = '1'
    $env:OBS_MULTIREPLAY_SELFTEST_SOAK_MIN = "$Minutes"
    $env:OBS_MULTIREPLAY_SELFTEST_OUT = $soakReport
    Remove-Item Env:OBS_MULTIREPLAY_SELFTEST_REOPEN -ErrorAction SilentlyContinue

    Step "Soak pass: recording for $Minutes minute(s) in its own project"
    $p = Start-Process -FilePath $obsExe -WorkingDirectory $obsDir `
        -ArgumentList '--disable-shutdown-check', '--multi', '--collection', $testCollection -PassThru

    # The pass itself is Minutes long; add the setup, the teardown and the
    # remove_all of what may be several GB.
    $limit = $Minutes * 60 + 180
    $waited = 0
    while (-not (Test-Path $soakReport) -and $waited -lt $limit) {
        if ($p.HasExited) { break }
        Start-Sleep -Seconds 10
        $waited += 10
    }
    $have = Test-Path $soakReport

    Step 'Closing OBS'
    if (-not $p.HasExited) {
        $p.CloseMainWindow() | Out-Null
        Start-Sleep -Seconds 5
        if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
    }
    Start-Sleep -Seconds 2
    Remove-Item Env:OBS_MULTIREPLAY_SELFTEST_SOAK -ErrorAction SilentlyContinue

    if (-not $have) {
        Fail "No soak report at $soakReport - that pass did not complete."
        return 2
    }
    $sj = Get-Content $soakReport -Raw
    Write-Host '--- SOAK REPORT ---' -ForegroundColor Yellow
    Write-Host $sj
    if (($sj | ConvertFrom-Json).verdict -eq 'PASS') { return 0 }
    Fail 'SOAK: FAIL'
    return 1
}

if ($SkipReopen) {
    Write-Host 'M0 GATE: PASS (reopen pass skipped)' -ForegroundColor Green
    if ($SoakMinutes -gt 0) { exit (Invoke-SoakPass -Minutes $SoakMinutes) }
    Restore-OperatorEnvironment
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
    -ArgumentList '--disable-shutdown-check', '--multi', '--collection', $testCollection -PassThru

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
    Restore-OperatorEnvironment
    exit 2
}

$json2 = Get-Content $reopenReport -Raw
Write-Host '--- REOPEN REPORT ---' -ForegroundColor Yellow
Write-Host $json2
$r2 = $json2 | ConvertFrom-Json
if ($r2.verdict -eq 'PASS') {
    Write-Host 'M0 GATE: PASS (take + reopen)' -ForegroundColor Green
    if ($SoakMinutes -gt 0) { exit (Invoke-SoakPass -Minutes $SoakMinutes) }
    Restore-OperatorEnvironment
    exit 0
} else {
    Fail 'M0 GATE: FAIL (reopened project)'
    Select-String -Path $log2.FullName -Pattern '\[selftest\]|\[segments\]' |
        Select-Object -Last 30 | ForEach-Object { $_.Line }
    Restore-OperatorEnvironment
    exit 1
}
