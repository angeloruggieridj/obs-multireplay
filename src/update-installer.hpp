/*
obs-multireplay — the update helper, with no path anywhere inside it
Copyright (C) 2026 obs-multireplay contributors
SPDX-License-Identifier: GPL-2.0-or-later

THE BUG THIS FILE IS THE SHAPE OF.

The helper used to be built by string interpolation:

    out << "$archive = '" << s.stagedPath << "'\n"

An apostrophe closes a PowerShell single-quoted literal. That is not a
hypothetical attacker; that is an operator called O'Brien, whose temp folder is
`C:\Users\O'Brien\AppData\Local\Temp\...` and whose updater therefore wrote a
syntactically invalid script, exited 1, and told him "could not start the
installer". And it was launched through `std::system()`, i.e. through cmd.exe,
where `&`, `^`, `|` and `%VAR%` in a path are interpreted before PowerShell sees
anything at all.

So: THE SCRIPT IS A CONSTANT. It contains no path, no name and no substitution
of any kind. What it needs to know it reads from a parameter file written beside
it — three lines of UTF-8, positional, read with `Get-Content -LiteralPath`,
which does not interpret what it reads. The only thing that still crosses a
command line is the script's own path, and that goes through CreateProcessW with
proper argument quoting instead of through a shell.

Pure: no OBS, no Windows headers, no filesystem. Same rule as
master-timeline.hpp — which is what lets "does an apostrophe in the path still
produce a valid script" be a unit test instead of a story.
*/

#pragma once

#include <string>
#include <vector>

namespace multireplay {

namespace update_installer {

// Beside the script, in the staging directory.
inline constexpr const char *kParamFileName = "install-update.txt";
inline constexpr const char *kScriptFileName = "install-update.ps1";

// THE PLUGIN FOLDER, FROM THE BINARY OBS ACTUALLY LOADED — in either layout.
//
//   OBS 32 and earlier (and OBS 33, as "legacy"):
//       <plugins>/obs-multireplay/bin/64bit/obs-multireplay.dll
//   OBS 33 onwards:
//       <plugins>/obs-multireplay/obs-multireplay.dll
//
// It used to climb three levels unconditionally. On OBS 33's layout three
// levels up from the DLL is C:\ProgramData\obs-studio — the folder that holds
// EVERY plugin — and that is what the update helper would have renamed to a
// backup and unpacked over. So: climb past bin/64bit only when it is there.
// Empty when the path has no folder at all. Pure string work, both separators,
// so it is a unit test and not a Windows-only story.
inline std::string pluginDirFromBinary(const std::string &binaryPath)
{
	const auto parent = [](const std::string &p) -> std::string {
		const size_t cut = p.find_last_of("/\\");
		return cut == std::string::npos ? std::string() : p.substr(0, cut);
	};
	const auto leafIs = [](const std::string &p, const char *want) {
		const size_t cut = p.find_last_of("/\\");
		const std::string leaf = cut == std::string::npos ? p : p.substr(cut + 1);
		std::string lower;
		for (char c : leaf)
			lower += (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
		return lower == want;
	};

	const std::string dir = parent(binaryPath);
	if (dir.empty())
		return {};
	if (leafIs(dir, "64bit")) {
		const std::string bin = parent(dir);
		if (leafIs(bin, "bin")) {
			const std::string root = parent(bin);
			if (!root.empty())
				return root;
		}
	}
	return dir;
}

// The three things the helper has to be told, in the order it reads them.
struct Params {
	std::string archivePath; // the downloaded asset
	std::string targetDir;   // where the plugin is installed
	std::string obsExePath;  // what to start again afterwards; may be empty
};

// The parameter file's contents. One value per line, verbatim: no quoting, no
// escaping, nothing to get wrong — a newline is the only character a path
// cannot contain on either Windows or POSIX, and it is the only separator.
inline std::string paramFile(const Params &p)
{
	// Trailing newline included: Get-Content on a file without one still
	// yields the last line, but a file that ends properly is one fewer
	// thing to wonder about when reading it by hand after a failed update.
	return p.archivePath + "\n" + p.targetDir + "\n" + p.obsExePath + "\n";
}

// The helper itself. A CONSTANT — grep it for a backslash and you will find
// none that belongs to a path.
//
// THE RELEASE ZIP STAYS IN THE LEGACY LAYOUT (<plugin>/bin/64bit), although
// OBS 33 prefers <plugin>/<plugin>.dll, and the reason is the helper that is
// ALREADY INSTALLED: the copy built into 1.0.0 and every beta before it takes
// the first obs-multireplay.dll the zip holds and climbs two folders. A DLL at
// the plugin folder's root is found first (measured under Windows PowerShell
// 5.1), and two folders up from it is the parent of the unpack folder — that
// helper would copy the whole of %TEMP% into the plugin folder. So the zip
// keeps bin/64bit (which OBS 33 still loads, as "legacy", until OBS 34), and it
// is THIS helper, from 1.0.1 on, that puts the second copy in the new place.
// The install tree is cmake/windows/helpers.cmake's — the template's file,
// which is why this is written here and not there.
//
// IT RUNS IN THREE STAGES. "Install when OBS closes" shipped and never
// installed anything; the cause was the launch flags (DETACHED_PROCESS, see
// startDetached() in updater.cpp), but the investigation found a second risk
// worth closing too: OBS runs inside a Windows JOB OBJECT (measured:
// IsProcessInJob is true for an obs64.exe whose parent is Explorer, while a
// plain cmd.exe started by Explorer is in none), and a job can take its
// processes with it when it closes. A helper that is OBS's child is inside
// that job. So:
//
//   launch   what CreateProcessW starts. It asks WMI to start the waiter, which
//            makes WmiPrvSE its creator: outside the job, in the same session,
//            with the same user and the same %TEMP% (all measured). If WMI is
//            unavailable it carries on in-process, i.e. exactly what it did
//            before, and says so in the log.
//   wait     waits for OBS to exit, then installs — or, when the plugin folder
//            is not writable by this account (an install done by the Inno
//            Setup installer is owned by Administrators), starts the
//            `install` stage elevated and waits for it. Starts OBS again
//            either way, as the operator and never elevated.
//   install  the copy itself.
//
// Every stage appends to install-update.log beside the script and the outcome
// goes to install-result.txt, which the plugin reads and reports at the next
// start: an update that fails must not fail in silence a second time.
inline std::string script()
{
	return R"PS(param([string]$Stage = 'launch')
$ErrorActionPreference = 'Stop'
# Written by obs-multireplay. Waits for OBS to close, unpacks the update
# over the installed plugin, then starts OBS again.
#
# NOTHING is interpolated into this file. The three paths it works on are
# read from install-update.txt beside it, with -LiteralPath, so an
# apostrophe or an ampersand in a folder name is a character and not syntax.
$here = Split-Path -LiteralPath $PSCommandPath
$log = Join-Path $here 'install-update.log'
$result = Join-Path $here 'install-result.txt'
function Say([string]$m) {
  $line = (Get-Date -Format 'yyyy-MM-dd HH:mm:ss') + ' [' + $Stage + '] ' + $m
  try { Add-Content -LiteralPath $log -Value $line -Encoding UTF8 } catch {}
}
function Done([string]$m) {
  Say $m
  try { Set-Content -LiteralPath $result -Value $m -Encoding UTF8 } catch {}
}
$ps = Join-Path $PSHOME 'powershell.exe'
$self = '-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "' + $PSCommandPath + '" -Stage '
$exe = ''
$backup = ''
$target = ''
try {
  if ($Stage -eq 'launch') {
    if (Test-Path -LiteralPath $result) { Remove-Item -LiteralPath $result -Force }
    Say 'armed'
    # Out of the job OBS runs in: a process WMI creates is not our child.
    try {
      $si = New-CimInstance -ClassName Win32_ProcessStartup -ClientOnly -Property @{ ShowWindow = [uint16]0 }
      $r = Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments @{ CommandLine = ('"' + $ps + '" ' + $self + 'wait'); ProcessStartupInformation = $si }
      if ($r.ReturnValue -eq 0) { Say ('waiter started, pid ' + $r.ProcessId); exit 0 }
      Say ('WMI refused to start the waiter: ' + $r.ReturnValue)
    } catch { Say ('WMI unavailable: ' + $_.Exception.Message) }
    Say 'waiting in-process instead'
    $Stage = 'wait'
  }

  $cfg = @(Get-Content -LiteralPath (Join-Path $here 'install-update.txt') -Encoding UTF8)
  if ($cfg.Count -lt 2) { Done 'fail: install-update.txt is incomplete'; exit 2 }
  $archive = $cfg[0]
  $target  = $cfg[1]
  $exe     = if ($cfg.Count -ge 3 -and $cfg[2]) { $cfg[2] } else { '' }

  if ($Stage -eq 'wait') {
    $proc = Get-Process obs64 -ErrorAction SilentlyContinue
    if ($proc -and $proc[0].Path) { $exe = $proc[0].Path }
    Say 'waiting for OBS to close'
    for ($i = 0; $i -lt 3600; $i++) {
      if (-not (Get-Process obs64 -ErrorAction SilentlyContinue)) { break }
      Start-Sleep -Seconds 1
    }
    if (Get-Process obs64 -ErrorAction SilentlyContinue) {
      Done 'fail: OBS was still running an hour later'
      exit 1
    }
    Start-Sleep -Seconds 2
    # Can THIS account write there? Asked by writing, not by reading ACLs.
    $canWrite = $true
    try {
      foreach ($d in @((Split-Path -Parent $target), $target)) {
        if (Test-Path -LiteralPath $d) {
          $probe = Join-Path $d ('.mr-write-test-' + [guid]::NewGuid().ToString('N'))
          [IO.File]::WriteAllText($probe, 'x')
          Remove-Item -LiteralPath $probe -Force
        }
      }
    } catch { $canWrite = $false }
    if (-not $canWrite) {
      Say 'the plugin folder needs administrator rights: asking'
      try {
        $p = Start-Process -FilePath $ps -ArgumentList ($self + 'install') -Verb RunAs -Wait -PassThru
        Say ('elevated install exited with ' + $p.ExitCode)
      } catch {
        Done ('fail: administrator rights were refused (' + $_.Exception.Message + ')')
      }
      if ($exe) { Start-Process -FilePath $exe -WorkingDirectory (Split-Path -Parent $exe) }
      exit 0
    }
  }

  Say ('installing ' + $archive + ' into ' + $target)
  $unpack = Join-Path ([IO.Path]::GetTempPath()) 'obs-multireplay-unpack'
  if (Test-Path -LiteralPath $unpack) { Remove-Item -LiteralPath $unpack -Recurse -Force }
  Expand-Archive -LiteralPath $archive -DestinationPath $unpack -Force
  # The archive may carry the plugin folder at its root or a level down, and
  # the binary in the legacy layout (bin/64bit under the plugin folder), in
  # the OBS 33 one (directly in it) or both; the plugin folder is the one
  # above bin/64bit when the binary sits there, else the folder of the binary.
  $src = $unpack
  $dll = Get-ChildItem -LiteralPath $unpack -Recurse -Filter 'obs-multireplay.dll' | Select-Object -First 1
  if ($dll) {
    $src = $dll.Directory.FullName
    if ($dll.Directory.Name -eq '64bit' -and $dll.Directory.Parent.Name -eq 'bin') {
      $src = $dll.Directory.Parent.Parent.FullName
    }
  }
  # BACKUP FIRST: anything that throws below puts it back (see catch).
  if (Test-Path -LiteralPath $target) {
    $backupName = (Split-Path -Leaf $target) + '.bak-' + (Get-Date -Format 'yyyyMMddHHmmss')
    $backup = Join-Path (Split-Path -Parent $target) $backupName
    Rename-Item -LiteralPath $target -NewName $backupName
  }
  New-Item -ItemType Directory -Force -Path $target | Out-Null
  # NOT Copy-Item -LiteralPath (Join-Path $src '*'): -LiteralPath turns off
  # wildcard expansion, so that copied a file NAMED one asterisk and on
  # Windows PowerShell 5.1 silently copied nothing at all. Enumerating with
  # Get-ChildItem -LiteralPath and piping each real item into Copy-Item keeps
  # every path literal while actually copying the files.
  Get-ChildItem -LiteralPath $src -Force | Copy-Item -Destination $target -Recurse -Force
  # BOTH LAYOUTS, whatever the archive carried. OBS 33 loads the binary in
  # the plugin folder and skips the bin/64bit copy as a duplicate; OBS 32 and
  # earlier only ever look in bin/64bit; OBS 34 drops bin/64bit.
  $legacyDir = Join-Path (Join-Path $target 'bin') '64bit'
  $top = Join-Path $target 'obs-multireplay.dll'
  $old = Join-Path $legacyDir 'obs-multireplay.dll'
  if ((Test-Path -LiteralPath $old) -and -not (Test-Path -LiteralPath $top)) {
    Copy-Item -LiteralPath $old -Destination $top -Force
  } elseif ((Test-Path -LiteralPath $top) -and -not (Test-Path -LiteralPath $old)) {
    New-Item -ItemType Directory -Force -Path $legacyDir | Out-Null
    Copy-Item -LiteralPath $top -Destination $old -Force
  }
  if ($backup -and (Test-Path -LiteralPath $backup)) {
    Remove-Item -LiteralPath $backup -Recurse -Force -ErrorAction SilentlyContinue
  }
  $backup = ''
  Remove-Item -LiteralPath $unpack -Recurse -Force -ErrorAction SilentlyContinue
  Done 'ok'
} catch {
  $why = $_.Exception.Message
  if ($backup -and (Test-Path -LiteralPath $backup)) {
    try {
      if (Test-Path -LiteralPath $target) { Remove-Item -LiteralPath $target -Recurse -Force }
      Rename-Item -LiteralPath $backup -NewName (Split-Path -Leaf $target)
      $why = $why + ' (the previous version was put back)'
    } catch { $why = $why + ' (the previous version is in ' + $backup + ')' }
  }
  Done ('fail: ' + $why)
}
if ($Stage -eq 'wait' -and $exe) {
  Start-Process -FilePath $exe -WorkingDirectory (Split-Path -Parent $exe)
}
)PS";
}

// One argument, quoted the way CommandLineToArgvW parses it back. Needed
// because CreateProcessW takes ONE string and every process on Windows splits
// it again itself; the rule about backslashes before a quote is the part
// everybody gets wrong.
inline std::string quoteArg(const std::string &arg)
{
	if (!arg.empty() && arg.find_first_of(" \t\n\v\"") == std::string::npos)
		return arg;

	std::string out = "\"";
	for (size_t i = 0;; i++) {
		size_t backslashes = 0;
		while (i < arg.size() && arg[i] == '\\') {
			i++;
			backslashes++;
		}
		if (i == arg.size()) {
			// Escape the run so it does not escape the closing
			// quote we are about to add.
			out.append(backslashes * 2, '\\');
			break;
		}
		if (arg[i] == '"') {
			out.append(backslashes * 2 + 1, '\\');
			out += '"';
		} else {
			out.append(backslashes, '\\');
			out += arg[i];
		}
	}
	out += '"';
	return out;
}

// The whole command line for CreateProcessW. argv[0] is quoted like any other
// argument: an OBS installed under "Program Files" is the ordinary case.
inline std::string commandLine(const std::vector<std::string> &argv)
{
	std::string out;
	for (const std::string &a : argv) {
		if (!out.empty())
			out += ' ';
		out += quoteArg(a);
	}
	return out;
}

// The arguments the helper is launched with. Kept here so the test can assert
// on them: -File takes ONE path and everything after it would be the script's
// own arguments, which is why there are none.
inline std::vector<std::string> argvFor(const std::string &powershellExe,
					const std::string &scriptPath)
{
	return {powershellExe, "-NoProfile", "-ExecutionPolicy", "Bypass",
		"-WindowStyle", "Hidden",      "-File",           scriptPath};
}

} // namespace update_installer

} // namespace multireplay
