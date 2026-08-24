<#
.SYNOPSIS
  Evidence capture for the black-GUI / frozen-output fault.

.DESCRIPTION
  Everything measurable from inside the plugin has been measured and is clean:
  the UI thread never stalls, the OBS graphics thread keeps its frame budget,
  no display is stranded and none is left on a re-parented ancestor
  (obs_displays_reparented = 0). The application is alive and drawing. What
  stops is what reaches the screen, on ONE output, cursor included.

  Nothing in-process can see that layer, so this script measures it from
  outside. It does two things, and the SECOND is the one that decides the
  question:

  1. An ETW ring buffer over the compositor providers (Dwm-Redir is literally
     the redirection surface that goes black; DxgKrnl carries present and
     plane assignment). Low cost, runs all day, dumped on demand.

  2. A SCREEN GRAB OF EVERY MONITOR at the moment of the fault. This is the
     discriminator we have not been able to make any other way, because
     BitBlt from the screen DC reads the COMPOSED DESKTOP:

       - grab comes back BLACK/STALE  -> the composed surface itself is wrong.
                                         The fault is in DWM's composition,
                                         and the frozen cursor is being
                                         composited into it.
       - grab comes back CORRECT      -> composition is fine and the panel was
                                         drawn properly all along; what fails
                                         is SCANOUT — the plane the monitor is
                                         actually reading. That is the MPO /
                                         DirectFlip path, i.e. below the
                                         application, and no plugin-side
                                         change can fix it.

     Both outcomes are conclusive, which is the point. One of them ends the
     investigation with a fix on our side; the other ends it with a defensible
     upstream report instead of a workaround.

  Run Dump from the monitor that still works — the fault leaves one alive.

.EXAMPLE
  pwsh -File scripts/trace-blackscreen.ps1 -Action Start    # elevated, once
  pwsh -File scripts/trace-blackscreen.ps1                  # at the fault
  pwsh -File scripts/trace-blackscreen.ps1 -Action Stop
#>

[CmdletBinding()]
param(
    [ValidateSet('Start', 'Dump', 'Stop', 'Status')]
    [string]$Action = 'Dump',

    # Where evidence bundles are written. One folder per dump.
    [string]$OutDir = "$env:USERPROFILE\Desktop\mr-blackscreen",

    # Ring buffer size, MB. 64 holds several minutes of compositor events.
    [int]$RingMB = 64,

    # Decode the .etl to a text summary. Off by default: tracerpt takes a
    # while on a full ring and the .etl is the artefact that matters.
    [switch]$Decode
)

$ErrorActionPreference = 'Stop'
$SessionName = 'MRBlackScreen'

# The compositor providers, by GUID so this does not depend on the display
# language of the machine. Verified present on the affected rig.
$Providers = [ordered]@{
    'Microsoft-Windows-Dwm-Redir'      = '{7D99F6A4-1BEC-4C09-9703-3AAA8148347F}'
    'Microsoft-Windows-Dwm-Core'       = '{9E9BBA3C-2E38-40CB-99F4-9E8281425164}'
    'Microsoft-Windows-Dwm-Compositor' = '{044A9015-D96C-5DD1-0199-72D258325298}'
    'Microsoft-Windows-Dwm-Api'        = '{292A52C4-FA27-4461-B526-54A46430BD54}'
    'Microsoft-Windows-DxgKrnl'        = '{802EC45A-1E99-4B83-9920-87C98277BA9D}'
}

function Test-Elevated {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    (New-Object Security.Principal.WindowsPrincipal $id).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Test-SessionRunning {
    # logman reports a missing session on stderr with a non-zero exit; swallow
    # both and answer the question rather than throwing.
    $out = & logman query $SessionName -ets 2>&1
    return ($LASTEXITCODE -eq 0)
}

function Get-RingPath { Join-Path $OutDir "$SessionName.etl" }

function Start-Session {
    if (-not (Test-Elevated)) {
        throw "An ETW session needs an elevated shell. Open PowerShell as administrator and run this again."
    }
    if (Test-SessionRunning) {
        Write-Host "Trace already running." -ForegroundColor Yellow
        return
    }
    New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
    $ring = Get-RingPath

    $first = $true
    foreach ($name in $Providers.Keys) {
        $guid = $Providers[$name]
        if ($first) {
            # Circular: the file wraps in place, so this can be left running
            # for hours and still be dumped the moment the fault appears.
            & logman create trace $SessionName -ow -o $ring `
                -mode Circular -max $RingMB -bs 64 -nb 16 256 `
                -p $guid 0xffffffffffffffff 0x5 -ets | Out-Null
            $first = $false
        } else {
            & logman update trace $SessionName -p $guid 0xffffffffffffffff 0x5 -ets | Out-Null
        }
        if ($LASTEXITCODE -ne 0) {
            Write-Host "  ! provider not added: $name" -ForegroundColor Yellow
        } else {
            Write-Host "  + $name"
        }
    }
    Write-Host ""
    Write-Host "Tracing. Ring $RingMB MB at $ring" -ForegroundColor Green
    Write-Host "When the screen goes black, from the monitor that still works, run:" -ForegroundColor Green
    Write-Host "  pwsh -File scripts/trace-blackscreen.ps1" -ForegroundColor Green
}

function Save-ScreenGrabs([string]$dest) {
    # THE DISCRIMINATOR. CopyFromScreen is a BitBlt out of the screen DC, i.e.
    # a read of the composed desktop — so what comes back says whether the
    # composition is stale or whether it is fine and only scanout is wrong.
    Add-Type -AssemblyName System.Drawing, System.Windows.Forms
    $notes = @()
    $i = 0
    foreach ($s in [System.Windows.Forms.Screen]::AllScreens) {
        $i++
        $b = $s.Bounds
        $bmp = New-Object System.Drawing.Bitmap($b.Width, $b.Height)
        try {
            $g = [System.Drawing.Graphics]::FromImage($bmp)
            try {
                $g.CopyFromScreen($b.X, $b.Y, 0, 0, $bmp.Size)
            } finally { $g.Dispose() }

            $file = Join-Path $dest ("screen{0}{1}.png" -f $i, $(if ($s.Primary) { '-primary' } else { '' }))
            $bmp.Save($file, [System.Drawing.Imaging.ImageFormat]::Png)

            # A cheap "is this picture black?" so the bundle answers the
            # question in text too, not only by eye: sample a grid and report
            # mean luminance and spread. A stale-but-not-black frame shows as
            # normal spread, and that is a different answer from both.
            $sum = 0.0; $sumSq = 0.0; $n = 0
            for ($y = 0; $y -lt $b.Height; $y += 37) {
                for ($x = 0; $x -lt $b.Width; $x += 37) {
                    $p = $bmp.GetPixel($x, $y)
                    $l = 0.299 * $p.R + 0.587 * $p.G + 0.114 * $p.B
                    $sum += $l; $sumSq += $l * $l; $n++
                }
            }
            $mean = $sum / $n
            $sd = [math]::Sqrt([math]::Max(0.0, ($sumSq / $n) - ($mean * $mean)))
            $verdict = if ($sd -lt 1.0) { 'UNIFORM (a flat fill — a truly blank surface)' }
                       elseif ($mean -lt 8.0) { 'NEARLY BLACK but not flat' }
                       else { 'HAS CONTENT (composition produced a real picture)' }
            $notes += ("screen{0} {1}x{2} at ({3},{4}){5}: mean luminance {6:N1}, spread {7:N1} -> {8}" -f `
                $i, $b.Width, $b.Height, $b.X, $b.Y, $(if ($s.Primary) { ' [primary]' } else { '' }), $mean, $sd, $verdict)
        } finally { $bmp.Dispose() }
    }
    return $notes
}

function Save-Context([string]$dest) {
    $lines = @()
    $lines += "captured        : $(Get-Date -Format o)"
    $lines += "cursor position : $([System.Windows.Forms.Cursor]::Position)"
    $lines += ""

    $lines += "--- monitors (as Windows reports them) ---"
    foreach ($s in [System.Windows.Forms.Screen]::AllScreens) {
        $lines += ("  {0} {1} primary={2}" -f $s.DeviceName, $s.Bounds, $s.Primary)
    }
    $lines += ""

    $lines += "--- MPO policy ---"
    $v = (Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows\Dwm' -Name OverlayTestMode -EA SilentlyContinue).OverlayTestMode
    $lines += if ($null -eq $v) { "  OverlayTestMode absent -> MPO ENABLED (Windows default)" }
              elseif ($v -eq 5) { "  OverlayTestMode = 5 -> MPO DISABLED" }
              else { "  OverlayTestMode = $v" }
    $lines += ""

    $lines += "--- OBS process ---"
    $obs = Get-Process obs64 -EA SilentlyContinue
    if ($obs) {
        $lines += ("  pid {0}, responding={1}, started {2}" -f $obs.Id, $obs.Responding, $obs.StartTime)
        # Responding=False would mean the message pump is stuck. Every log so
        # far says it is not, and the operator can still close the window from
        # the taskbar during the fault — this records that fact at the moment
        # it matters instead of inferring it afterwards.
        $lines += ("  main window title: '{0}'" -f $obs.MainWindowTitle)
    } else {
        $lines += "  not running"
    }
    $lines += ""

    $lines += "--- display driver ---"
    Get-CimInstance Win32_PnPSignedDriver -EA SilentlyContinue |
        Where-Object { $_.DeviceClass -eq 'DISPLAY' } |
        ForEach-Object { $lines += ("  {0} {1} ({2})" -f $_.DeviceName, $_.DriverVersion, $_.DriverDate) }
    $lines += ""

    $lines += "--- display driver resets in the last hour (a TDR would show here) ---"
    $tdr = Get-WinEvent -FilterHashtable @{LogName='System'; StartTime=(Get-Date).AddHours(-1)} -EA SilentlyContinue |
        Where-Object { $_.Id -in 4101,4102 -or $_.ProviderName -match 'Display|dxgkrnl|LiveKernelEvent' }
    if ($tdr) { $tdr | ForEach-Object { $lines += ("  {0} id={1} {2}" -f $_.TimeCreated, $_.Id, $_.ProviderName) } }
    else { $lines += "  none" }
    $lines += ""

    $lines += "--- tail of the current OBS log ---"
    $log = Get-ChildItem "$env:APPDATA\obs-studio\logs\*.txt" -EA SilentlyContinue |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if ($log) {
        $lines += "  $($log.FullName)"
        $lines += (Get-Content $log.FullName -Tail 40 | ForEach-Object { "  $_" })
    } else { $lines += "  no OBS log found" }

    $file = Join-Path $dest 'context.txt'
    $lines | Set-Content -Path $file -Encoding UTF8
    return $file
}

function Invoke-Dump {
    Add-Type -AssemblyName System.Windows.Forms
    $stamp = Get-Date -Format 'yyyy-MM-dd_HH-mm-ss'
    $dest = Join-Path $OutDir "dump_$stamp"
    New-Item -ItemType Directory -Force -Path $dest | Out-Null

    # SCREEN GRABS FIRST, and the ordering is deliberate: stopping an ETW
    # session takes a moment and touches the graphics stack's bookkeeping.
    # The picture must be of the fault, not of whatever this script provoked.
    Write-Host "Grabbing every monitor..." -ForegroundColor Cyan
    $notes = Save-ScreenGrabs $dest
    $notes | ForEach-Object { Write-Host "  $_" }

    Write-Host "Recording context..." -ForegroundColor Cyan
    $ctx = Save-Context $dest
    Add-Content -Path $ctx -Encoding UTF8 -Value (@("", "--- screen grabs ---") + ($notes | ForEach-Object { "  $_" }))

    if (Test-SessionRunning) {
        Write-Host "Flushing the ETW ring..." -ForegroundColor Cyan
        # Circular ETW keeps one file open; stopping is what finalises it.
        & logman stop $SessionName -ets | Out-Null
        $ring = Get-RingPath
        if (Test-Path $ring) {
            $etl = Join-Path $dest 'compositor.etl'
            Move-Item $ring $etl -Force
            Write-Host ("  {0} ({1:N1} MB)" -f $etl, ((Get-Item $etl).Length / 1MB))
            if ($Decode) {
                Write-Host "  decoding summary (this takes a minute)..." -ForegroundColor Cyan
                & tracerpt $etl -summary (Join-Path $dest 'summary.txt') `
                    -o (Join-Path $dest 'events.xml') -of XML -y | Out-Null
            }
        }
        if (Test-Elevated) {
            Write-Host "Restarting the ring so the next occurrence is caught too..." -ForegroundColor Cyan
            Start-Session
        } else {
            Write-Host "Ring stopped. Re-run -Action Start from an elevated shell to keep tracing." -ForegroundColor Yellow
        }
    } else {
        Write-Host "No ETW session running — grabs and context captured anyway." -ForegroundColor Yellow
        Write-Host "They are the decisive half; the trace only says which component did it." -ForegroundColor Yellow
    }

    Write-Host ""
    Write-Host "Evidence bundle: $dest" -ForegroundColor Green
    Write-Host "Read the grab of the BLACK monitor first — it decides the question:" -ForegroundColor Green
    Write-Host "  black/uniform -> DWM composition is producing a blank surface" -ForegroundColor Green
    Write-Host "  correct GUI   -> composition is fine; scanout/plane is at fault (below us)" -ForegroundColor Green
}

function Stop-Session {
    if (-not (Test-SessionRunning)) { Write-Host "No trace running."; return }
    if (-not (Test-Elevated)) { throw "Stopping the session needs an elevated shell." }
    & logman stop $SessionName -ets | Out-Null
    Write-Host "Trace stopped. Ring left at $(Get-RingPath)" -ForegroundColor Green
}

function Show-Status {
    Write-Host ("elevated shell : {0}" -f (Test-Elevated))
    Write-Host ("trace running  : {0}" -f (Test-SessionRunning))
    Write-Host ("ring file      : {0}" -f (Get-RingPath))
    if (Test-Path (Get-RingPath)) {
        Write-Host ("ring size      : {0:N1} MB" -f ((Get-Item (Get-RingPath)).Length / 1MB))
    }
    Write-Host ("bundles in     : {0}" -f $OutDir)
}

switch ($Action) {
    'Start'  { Start-Session }
    'Dump'   { Invoke-Dump }
    'Stop'   { Stop-Session }
    'Status' { Show-Status }
}
