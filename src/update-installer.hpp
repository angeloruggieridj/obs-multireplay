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
inline std::string script()
{
	return "$ErrorActionPreference = 'Stop'\n"
	       "# Written by obs-multireplay. Waits for OBS to close, unpacks\n"
	       "# the update over the installed plugin, then starts OBS again.\n"
	       "#\n"
	       "# NOTHING is interpolated into this file. The three paths it\n"
	       "# works on are read from install-update.txt beside it, with\n"
	       "# -LiteralPath, so an apostrophe or an ampersand in a folder\n"
	       "# name is a character and not syntax.\n"
	       "$here = Split-Path -LiteralPath $PSCommandPath\n"
	       "$cfg = @(Get-Content -LiteralPath (Join-Path $here "
	       "'install-update.txt') -Encoding UTF8)\n"
	       "if ($cfg.Count -lt 2) { exit 2 }\n"
	       "$archive = $cfg[0]\n"
	       "$target  = $cfg[1]\n"
	       "$exe     = if ($cfg.Count -ge 3 -and $cfg[2]) { $cfg[2] } "
	       "else { '' }\n"
	       "$proc = Get-Process obs64 -ErrorAction SilentlyContinue\n"
	       "if ($proc -and $proc[0].Path) { $exe = $proc[0].Path }\n"
	       "for ($i = 0; $i -lt 900; $i++) {\n"
	       "  if (-not (Get-Process obs64 -ErrorAction SilentlyContinue)) "
	       "{ break }\n"
	       "  Start-Sleep -Seconds 1\n"
	       "}\n"
	       "if (Get-Process obs64 -ErrorAction SilentlyContinue) { exit 1 }\n"
	       "$stage = Join-Path ([IO.Path]::GetTempPath()) "
	       "'obs-multireplay-unpack'\n"
	       "if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath "
	       "$stage -Recurse -Force }\n"
	       "Expand-Archive -LiteralPath $archive -DestinationPath $stage "
	       "-Force\n"
	       "# The archive may carry the plugin folder at its root or a\n"
	       "# level down; take whichever actually holds the binary.\n"
	       "$src = $stage\n"
	       "$dll = Get-ChildItem -LiteralPath $stage -Recurse -Filter "
	       "'obs-multireplay.dll' | Select-Object -First 1\n"
	       "if ($dll) { $src = $dll.Directory.Parent.Parent.FullName }\n"
	       "New-Item -ItemType Directory -Force -Path $target | Out-Null\n"
	       "Copy-Item -LiteralPath (Join-Path $src '*') -Destination "
	       "$target -Recurse -Force\n"
	       "Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction "
	       "SilentlyContinue\n"
	       "if ($exe) { Start-Process -FilePath $exe }\n";
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
