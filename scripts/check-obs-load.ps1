<#
.SYNOPSIS
Loads the freshly built plugin into a PORTABLE copy of a given OBS release and
reports whether it loaded, registered its dock and shut down cleanly.

.DESCRIPTION
The gate (run-selftest.ps1) drives the OBS installed on this machine. This asks
a narrower question of an OBS that is NOT installed — typically a beta — without
touching the operator's installation, config or scene collections: the release
zip is unpacked under %TEMP% and started in portable mode, with a config of its
own, so nothing it does lands in %APPDATA% or %ProgramData%.

It exists because OBS 33 moved third-party plugins to a new folder layout and
still loads the old one as "legacy" until OBS 34. So each run installs the
plugin in one of three shapes and reads OBS's own log to see what happened:

  new     <obs>\plugins\obs-multireplay\obs-multireplay.dll        (OBS 33+)
  legacy  <obs>\obs-plugins\64bit\obs-multireplay.dll               (the
          portable face of the legacy loader; the ProgramData one, which the
          release zip targets, is not searched in portable mode)
  both    both of the above — OBS 33 must load the new one and skip the other

Branch Output is copied in from the installed OBS, in its own legacy layout,
because the plugin's startup depends on finding it.

Exit 0 = every requested layout passed, 1 = at least one failed, 2 = setup.

.EXAMPLE
pwsh -File scripts/check-obs-load.ps1 -ObsVersion 33.0.0-beta4
#>
param(
    [string]$ObsVersion = '33.0.0-beta4',
    [string[]]$Layouts = @('new', 'legacy', 'both'),
    [int]$Seconds = 30
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$dll = Join-Path $repo 'build_x64\RelWithDebInfo\obs-multireplay.dll'
$localeSrc = Join-Path $repo 'data\locale'
$installedObs = 'C:\Program Files\obs-studio'
$boDll = Join-Path $installedObs 'obs-plugins\64bit\osi-branch-output.dll'
$boData = Join-Path $installedObs 'data\obs-plugins\osi-branch-output'

foreach ($need in @($dll, $localeSrc, $boDll, $boData)) {
    if (-not (Test-Path -LiteralPath $need)) { Write-Error "missing: $need (build first; Branch Output must be installed)"; exit 2 }
}

$work = Join-Path $env:TEMP "obs-load-check\$ObsVersion"
$zip = Join-Path $env:TEMP "obs-load-check\OBS-Studio-$ObsVersion-Windows-x64.zip"
New-Item -ItemType Directory -Force -Path (Split-Path $zip) | Out-Null
if (-not (Test-Path -LiteralPath $zip)) {
    Write-Host "downloading OBS $ObsVersion..."
    & gh release download $ObsVersion --repo obsproject/obs-studio --pattern "OBS-Studio-$ObsVersion-Windows-x64.zip" --dir (Split-Path $zip)
    if ($LASTEXITCODE -ne 0) { Write-Error "could not download OBS $ObsVersion"; exit 2 }
}

function Reset-Obs {
    if (Test-Path -LiteralPath $work) { Remove-Item -LiteralPath $work -Recurse -Force }
    Expand-Archive -LiteralPath $zip -DestinationPath $work -Force
    # A zip that wraps everything in one folder is unwrapped, so $work is the
    # OBS root (the folder holding bin\64bit\obs64.exe) either way.
    $exe = Get-ChildItem -LiteralPath $work -Recurse -Filter 'obs64.exe' | Select-Object -First 1
    if (-not $exe) { throw "no obs64.exe in the $ObsVersion zip" }
    $root = $exe.Directory.Parent.Parent.FullName
    New-Item -ItemType File -Force -Path (Join-Path $root 'portable_mode.txt') | Out-Null

    # A config of its own: no first-run wizard and no update check (either
    # opens a modal — a beta offers the newest stable — and a modal disables
    # the main window, so the close request is refused and OBS gets killed),
    # and a MultiReplay config that does not need the setup dialog.
    $cfg = Join-Path $root 'config\obs-studio'
    New-Item -ItemType Directory -Force -Path $cfg | Out-Null
    # Each key only in the file OBS reads it from. FirstRun=true means "the
    # first run has HAPPENED" and lives in user.ini (OBSBasic.cpp opens the
    # wizard when it is false). The update check is an app setting, in
    # global.ini. NOT LastVersion: a made-up one makes OBS try to migrate the
    # "old" global config and stop on "Unable to migrate global configuration".
    Set-Content -LiteralPath (Join-Path $cfg 'user.ini') -Value "[General]`r`nFirstRun=true`r`n" -Encoding UTF8
    Set-Content -LiteralPath (Join-Path $cfg 'global.ini') -Value "[General]`r`nEnableAutoUpdates=false`r`n" -Encoding UTF8
    $session = Join-Path $root 'mr-session'
    New-Item -ItemType Directory -Force -Path $session | Out-Null
    $pcfg = Join-Path $cfg 'plugin_config\obs-multireplay'
    New-Item -ItemType Directory -Force -Path $pcfg | Out-Null
    @{ sessionFolder = $session; cameras = @(@{ sourceName = 'LoadCheck'; displayName = 'C1' }) } |
        ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $pcfg 'config.json') -Encoding UTF8

    # Branch Output, in the legacy portable layout it installs itself in.
    $boBin = Join-Path $root 'obs-plugins\64bit'
    New-Item -ItemType Directory -Force -Path $boBin | Out-Null
    Copy-Item -LiteralPath $boDll -Destination $boBin -Force
    Copy-Item -LiteralPath $boData -Destination (Join-Path $root 'data\obs-plugins') -Recurse -Force
    return $root
}

function Install-Plugin([string]$root, [string]$layout) {
    if ($layout -in @('new', 'both')) {
        $dir = Join-Path $root 'plugins\obs-multireplay'
        New-Item -ItemType Directory -Force -Path (Join-Path $dir 'data') | Out-Null
        Copy-Item -LiteralPath $dll -Destination $dir -Force
        Copy-Item -LiteralPath $localeSrc -Destination (Join-Path $dir 'data') -Recurse -Force
    }
    if ($layout -in @('legacy', 'both')) {
        $bin = Join-Path $root 'obs-plugins\64bit'
        $data = Join-Path $root 'data\obs-plugins\obs-multireplay'
        New-Item -ItemType Directory -Force -Path $bin, $data | Out-Null
        Copy-Item -LiteralPath $dll -Destination $bin -Force
        Copy-Item -LiteralPath $localeSrc -Destination $data -Recurse -Force
    }
}

# CloseMainWindow() lets Windows pick the "main" window, and on a fresh config
# that is our dock: it starts floating, and WM_CLOSE on a floating dock hides it
# and nothing else — OBS runs on, gets killed, and never writes its shutdown.
# So the window is picked by title ("OBS <version> ..."), among this process's.
Add-Type -Namespace LoadCheck -Name Win -MemberDefinition @'
public delegate bool EnumProc(System.IntPtr hwnd, System.IntPtr lp);
[System.Runtime.InteropServices.DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, System.IntPtr lp);
[System.Runtime.InteropServices.DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(System.IntPtr hwnd, out uint pid);
[System.Runtime.InteropServices.DllImport("user32.dll", CharSet = System.Runtime.InteropServices.CharSet.Unicode)] public static extern int GetWindowText(System.IntPtr hwnd, System.Text.StringBuilder s, int n);
[System.Runtime.InteropServices.DllImport("user32.dll")] public static extern bool PostMessage(System.IntPtr hwnd, uint msg, System.IntPtr w, System.IntPtr l);
'@

function Close-ObsMainWindow([int]$processId) {
    $found = [IntPtr]::Zero
    $cb = [LoadCheck.Win+EnumProc] {
        param($hwnd, $lp)
        [uint32]$owner = 0
        $null = [LoadCheck.Win]::GetWindowThreadProcessId($hwnd, [ref]$owner)
        if ($owner -eq $processId) {
            $sb = New-Object System.Text.StringBuilder 512
            $null = [LoadCheck.Win]::GetWindowText($hwnd, $sb, 512)
            if ($sb.ToString() -like 'OBS *') { $script:found = $hwnd; return $false }
        }
        return $true
    }
    $script:found = [IntPtr]::Zero
    $null = [LoadCheck.Win]::EnumWindows($cb, [IntPtr]::Zero)
    if ($script:found -eq [IntPtr]::Zero) { return $false }
    return [LoadCheck.Win]::PostMessage($script:found, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) # WM_CLOSE
}

$results = @()
foreach ($layout in $Layouts) {
    Write-Host "=== OBS $ObsVersion, layout '$layout'"
    $root = Reset-Obs
    Install-Plugin $root $layout
    $bin = Join-Path $root 'bin\64bit'
    $proc = Start-Process -FilePath (Join-Path $bin 'obs64.exe') -WorkingDirectory $bin -PassThru `
        -ArgumentList '--portable', '--multi', '--disable-shutdown-check', '--disable-missing-files-check'
    # NOT --minimize-to-tray: a window in the tray is no main window, and
    # CloseMainWindow() then has nothing to close — OBS gets killed instead,
    # and a killed OBS writes neither "plugin unloaded" nor its leak count.
    Start-Sleep -Seconds $Seconds
    $alive = -not $proc.HasExited
    # A graceful close: OBS runs obs_module_unload only on a real shutdown,
    # and "plugin unloaded" plus the leak count are part of the verdict.
    if ($alive) {
        if (-not (Close-ObsMainWindow $proc.Id)) { $null = $proc.CloseMainWindow() }
        if (-not $proc.WaitForExit(30000)) {
            Stop-Process -Id $proc.Id -Force
            # Until it is gone it still holds its DLLs, and the next layout's
            # reset cannot delete the folder.
            $proc.WaitForExit()
            $closed = $false
        } else { $closed = $true }
    } else { $closed = $false }

    $log = Get-ChildItem -LiteralPath (Join-Path $root 'config\obs-studio\logs') -Filter '*.txt' |
        Sort-Object LastWriteTime | Select-Object -Last 1
    $text = if ($log) { Get-Content -LiteralPath $log.FullName -Raw } else { '' }
    $copy = Join-Path (Split-Path $zip) "log-$ObsVersion-$layout.txt"
    Set-Content -LiteralPath $copy -Value $text -Encoding UTF8

    $check = [ordered]@{
        layout            = $layout
        running_after     = $alive
        loaded            = $text -match '\[obs-multireplay\] plugin loaded successfully'
        dock_registered   = $text -notmatch '\[obs-multireplay\].*(could not|failed to) (add|register) (the )?dock'
        replay_source     = $text -match 'MultiReplay - Replay A'
        no_load_failure   = $text -notmatch "Failed to load module.*obs-multireplay"
        duplicate_skipped = ($layout -ne 'both') -or ($text -match "Skipping legacy plugin 'obs-multireplay'")
        closed_cleanly    = $closed -and ($text -match '\[obs-multireplay\] plugin unloaded')
        leaks             = if ($text -match 'Number of memory leaks: (\d+)') { [int]$Matches[1] } else { -1 }
        log               = $copy
    }
    $check.pass = $check.running_after -and $check.loaded -and $check.dock_registered -and $check.replay_source -and
        $check.no_load_failure -and $check.duplicate_skipped -and $check.closed_cleanly -and ($check.leaks -eq 0)
    $results += [pscustomobject]$check
    $check.GetEnumerator() | ForEach-Object { Write-Host ("  {0,-18} {1}" -f $_.Key, $_.Value) }
}

$report = Join-Path $env:TEMP "obs-load-check-$ObsVersion.json"
$results | ConvertTo-Json -Depth 3 | Set-Content -LiteralPath $report -Encoding UTF8
Write-Host "report: $report"
if ($results | Where-Object { -not $_.pass }) { Write-Host 'OBS LOAD CHECK: FAIL'; exit 1 }
Write-Host 'OBS LOAD CHECK: PASS'
exit 0
