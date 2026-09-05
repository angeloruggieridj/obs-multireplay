<#
§9.5 — local convenience for the check push.yaml already makes authoritative
in CI ("Check Tag Matches buildspec"): the DLL reports buildspec.json's
`version` + `prerelease` (see PLUGIN_VERSION_FULL in CMakeLists.txt), the
release is named by the tag, and if those two ever diverge the updater
compares the wrong pair of versions and silently stops offering updates to
everyone already running that build -- which is exactly what happened once
while the binary said "1.0.0" and the tag said "1.0.0-beta7" (see CLAUDE.md,
2026-08-27). CI still has the last word; this just says so before a push
instead of after one, when a wrong tag is still one command away from fixed.

Usage:
    pwsh -File scripts/check-tag-version.ps1                # checks the tag on HEAD
    pwsh -File scripts/check-tag-version.ps1 -Tag 1.0.1-beta1  # checks a tag not made yet
#>
param(
    [string]$Tag
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot

if (-not $Tag) {
    try {
        $Tag = (git -C $repoRoot describe --tags --exact-match 2>$null)
    } catch {
        $Tag = $null
    }
    if (-not $Tag) {
        Write-Host "HEAD is not exactly at a tag, and none was given with -Tag." -ForegroundColor Yellow
        Write-Host "Nothing to check yet -- this only matters right before 'git push origin <tag>'."
        exit 0
    }
}

$buildspec = Get-Content (Join-Path $repoRoot 'buildspec.json') -Raw | ConvertFrom-Json
$version = $buildspec.version
$pre = $buildspec.prerelease
$declared = if ($pre) { "$version-$pre" } else { $version }

if ($Tag -ne $declared) {
    Write-Host "MISMATCH: tag '$Tag' vs buildspec '$declared'" -ForegroundColor Red
    Write-Host "Update buildspec.json's version/prerelease to match the tag, or retag to match buildspec." -ForegroundColor Red
    exit 1
}

Write-Host "OK: tag and buildspec agree on $declared" -ForegroundColor Green
exit 0
