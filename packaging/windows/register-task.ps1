# naudio - register (or remove) the logon task for na_audio_daemon (issue #96 item 2, Windows).
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Called by the NSIS installer's install and uninstall sections. It lives here, as a script the
# package carries, rather than as escaped one-liners inside the generated .nsi: the substitution
# and the encoding below both have to be exactly right, and neither is reviewable through three
# layers of quoting.
#
# WHY THE XML IS RENDERED AT INSTALL TIME. A Task Scheduler action needs an ABSOLUTE program
# path, and on Windows the only true one is the directory the operator picked on the installer's
# page - $INSTDIR, which CMake cannot know. macOS and Linux need no equivalent because their
# package prefixes are fixed.
#
# TWO THINGS THAT LOOK COSMETIC AND ARE NOT:
#   * -Encoding Unicode writes UTF-16LE with a BOM, which is what schtasks reliably accepts.
#     The template carries NO encoding declaration precisely so this is legal: XML takes the
#     encoding from the BOM. A file whose declaration disagreed with its bytes would be refused
#     as "The task XML contains a value which is incorrectly formatted or out of range", naming
#     neither the encoding nor the line.
#   * The task is created from XML that says <Enabled>false</Enabled>, so it ships INERT. That
#     is the same property the launchd agent's Disabled key and the systemd unit's not-enabled
#     state give the other two platforms: installing must not start capturing at the next logon.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$InstallDir,
    [switch]$Unregister
)

$ErrorActionPreference = 'Stop'
$TaskName = '\naudio\naudio-daemon'
$rendered = Join-Path $InstallDir 'share\naudio\naudio-daemon-task.xml'

if ($Unregister) {
    # /F so a task that is already gone is not an error: an uninstall must not fail because a
    # previous uninstall, or the operator, already removed it.
    & schtasks.exe /Delete /TN $TaskName /F 2>&1 | Write-Output
    Remove-Item -LiteralPath $rendered -ErrorAction SilentlyContinue
    exit 0
}

$template = Join-Path $InstallDir 'share\naudio\naudio-daemon-task.xml.in'
$exe = Join-Path $InstallDir 'bin\na_audio_daemon.exe'
if (-not (Test-Path -LiteralPath $template)) {
    Write-Output "naudio: no task template at $template"; exit 1
}
if (-not (Test-Path -LiteralPath $exe)) {
    Write-Output "naudio: no daemon at $exe"; exit 1
}

# .Replace, not -replace: the operand is a literal token, and the regex operator would treat a
# metacharacter in an install path as a pattern.
(Get-Content -Raw -LiteralPath $template).Replace('@NAUDIO_SERVICE_EXEC@', $exe) |
    Set-Content -LiteralPath $rendered -Encoding Unicode -NoNewline

& schtasks.exe /Create /XML $rendered /TN $TaskName /F 2>&1 | Write-Output
if ($LASTEXITCODE -ne 0) {
    Write-Output "naudio: schtasks refused the logon task (exit $LASTEXITCODE)"
    exit $LASTEXITCODE
}
Write-Output "naudio: registered the logon task '$TaskName' (disabled; enable it to run at logon)"
exit 0
