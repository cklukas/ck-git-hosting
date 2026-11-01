# Copyright (c) 2026 C. Klukas. All rights reserved.
# SPDX-License-Identifier: MIT
# Opt-in broker qualification: creates OS-managed AppContainer profiles.
param(
    [Parameter(Mandatory=$true)][string]$Runner,
    [Parameter(Mandatory=$true)][string]$FakeSsh,
    [Parameter(Mandatory=$true)][string]$TestRoot
)
$ErrorActionPreference = 'Stop'
$Runner = (Resolve-Path -LiteralPath $Runner).Path
$FakeSsh = (Resolve-Path -LiteralPath $FakeSsh).Path
$TestRoot = (Resolve-Path -LiteralPath $TestRoot).Path
$fixture = Join-Path $TestRoot ('runner-start-' + [guid]::NewGuid().ToString('N'))
$oldTemp=$env:TEMP; $oldTmp=$env:TMP; $oldTmpdir=$env:TMPDIR
try {
    New-Item -ItemType Directory -Path "$fixture\tools\bin","$fixture\build","$fixture\tmp" | Out-Null
    $env:TEMP="$fixture\tmp"; $env:TMP=$env:TEMP; $env:TMPDIR=$env:TEMP
    $config = Join-Path $fixture 'runner.ini'
    $text = "schema_version=1`nserver=ckgit@example.invalid`nname=fixture`nci_build_root=$fixture\build`nci_toolchain_root=$fixture\tools`nssh_program=$FakeSsh`n"
    [IO.File]::WriteAllText($config, $text, (New-Object Text.UTF8Encoding($false)))
    & $Runner --version
    if ($LASTEXITCODE) { throw 'Runner version failed' }
    & $Runner check --config $config
    if ($LASTEXITCODE) { throw 'Runner broker check failed' }
    & $Runner serve --remote --config $config --once
    if ($LASTEXITCODE) { throw 'Runner did not negotiate and drain empty queue' }
    if (@(Get-ChildItem "$fixture\build" -Directory).Count) { throw 'Runner left a session workspace' }
    $oldLegacy=$env:CKGIT_TEST_LEGACY_RUNNER_SERVER
    try {
        $env:CKGIT_TEST_LEGACY_RUNNER_SERVER='1'
        $start = New-Object Diagnostics.ProcessStartInfo
        $start.FileName = $Runner
        $start.Arguments = 'serve --remote --once --config "' + $config + '"'
        $start.UseShellExecute = $false
        $start.RedirectStandardError = $true
        $process = [Diagnostics.Process]::Start($start)
        $errorText = $process.StandardError.ReadToEnd()
        $process.WaitForExit()
        if ($process.ExitCode -eq 0 -or $errorText -notmatch 'requires runner protocol 2') {
            throw 'Windows runner accepted a legacy-only server'
        }
        $process.Dispose()
    } finally { $env:CKGIT_TEST_LEGACY_RUNNER_SERVER=$oldLegacy }
    if (@(Get-ChildItem "$fixture\build" -Directory).Count) { throw 'Rejected server left a session workspace' }
    $lock = [IO.File]::Open("$fixture\build\runner.lock", 'Open', 'ReadWrite', 'None')
    try {
        $start = New-Object Diagnostics.ProcessStartInfo
        $start.FileName = $Runner
        $start.Arguments = 'check --config "' + $config + '"'
        $start.UseShellExecute = $false
        $start.RedirectStandardError = $true
        $process = [Diagnostics.Process]::Start($start)
        $errorText = $process.StandardError.ReadToEnd()
        $process.WaitForExit()
        if ($process.ExitCode -eq 0 -or $errorText -notmatch 'already active') { throw 'Runner single-instance lock failed' }
        $process.Dispose()
    } finally { $lock.Dispose() }
    Write-Output 'Windows runner broker, protocol negotiation, cleanup and single-instance checks passed'
} finally {
    $env:TEMP=$oldTemp; $env:TMP=$oldTmp; $env:TMPDIR=$oldTmpdir
    if (Test-Path $fixture) { Remove-Item -LiteralPath $fixture -Recurse -Force }
}
