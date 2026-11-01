# Copyright (c) 2026 C. Klukas. All rights reserved.
# SPDX-License-Identifier: MIT
# Operator-side, offline build; never run toolchain provisioning inside a CI job.
param(
    [Parameter(Mandatory=$true)][string]$SourceArchive,
    [Parameter(Mandatory=$true)][string]$TemporaryRoot,
    [Parameter(Mandatory=$true)][string]$Destination,
    [Parameter(Mandatory=$true)][string]$CmakeProgram
)
$ErrorActionPreference = 'Stop'
foreach ($path in @($SourceArchive,$TemporaryRoot,$Destination,$CmakeProgram)) {
    if (![IO.Path]::IsPathRooted($path) -or $path -match '^[A-Za-z]:[^\\/]') { throw "Absolute path required: $path" }
}
if (Test-Path -LiteralPath $Destination) { throw 'Destination exists; select a new Ninja version directory' }
$TemporaryRoot = (Resolve-Path -LiteralPath $TemporaryRoot).Path
$SourceArchive = (Resolve-Path -LiteralPath $SourceArchive).Path
$CmakeProgram = (Resolve-Path -LiteralPath $CmakeProgram).Path
# Upstream v1.13.2 release source, not an arbitrary downloaded build script.
$archiveHash = '974d6b2f4eeefa25625d34da3cb36bdcebe7fbce40f4c16ac0835fd1c0cbae17'
if ((Get-FileHash -LiteralPath $SourceArchive).Hash.ToLowerInvariant() -ne $archiveHash) { throw 'Ninja v1.13.2 source archive hash mismatch' }
$oldTemp=$env:TEMP; $oldTmp=$env:TMP; $oldTmpdir=$env:TMPDIR
$oldVsTelemetry=$env:VSCMD_SKIP_SENDTELEMETRY; $oldDotnetTelemetry=$env:DOTNET_CLI_TELEMETRY_OPTOUT; $oldVcpkgTelemetry=$env:VCPKG_DISABLE_METRICS
$scratch=Join-Path $TemporaryRoot ('lpac-ninja-' + [guid]::NewGuid().ToString('N'))
$start=[DateTime]::UtcNow
try {
    New-Item -ItemType Directory -Path $scratch | Out-Null
    $env:TEMP=$scratch; $env:TMP=$scratch; $env:TMPDIR=$scratch
    $env:VSCMD_SKIP_SENDTELEMETRY="1"; $env:DOTNET_CLI_TELEMETRY_OPTOUT="1"; $env:VCPKG_DISABLE_METRICS="1"
    & "$env:SystemRoot\System32\tar.exe" -xf $SourceArchive -C $scratch
    if ($LASTEXITCODE) { throw 'Ninja source extraction failed' }
    $source=Join-Path $scratch 'ninja-1.13.2'
    $file=Join-Path $source 'src\subprocess-win32.cc'
    $originalHash='2ebc7c0606625e4a84359c0e9a0a763feb77f3d18c0787b03cf1a8e641515f81'
    if ((Get-FileHash -LiteralPath $file).Hash.ToLowerInvariant() -ne $originalHash) { throw 'Unexpected Ninja subprocess source' }
    $original=[IO.File]::ReadAllText($file)
    $patched=$original.Replace('\\\\.\\pipe\\ninja','\\\\.\\pipe\\LOCAL\\ninja')
    if ($patched -ceq $original) { throw 'Ninja LOCAL pipe patch did not match' }
    [IO.File]::WriteAllText($file,$patched,(New-Object Text.UTF8Encoding($false)))
    $patchedHash=(Get-FileHash -LiteralPath $file).Hash.ToLowerInvariant()
    if ($patchedHash -ne 'd6cbc5d6d350e4677991abe5828483fad39c0bc4d911a5ad1594d8d82d09e517') { throw 'Ninja patched source hash mismatch' }
    $build=Join-Path $scratch 'build'
    # PS 5 treats native stderr as an error record; inspect native exit codes.
    $ErrorActionPreference='Continue'
    & $CmakeProgram -S $source -B $build -G 'Visual Studio 17 2022' -A x64 -DBUILD_TESTING=OFF -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded *> "$scratch\configure.log"
    $code=$LASTEXITCODE
    if (!$code) {
        & $CmakeProgram --build $build --config Release -- /m:2 /nr:false *> "$scratch\build.log"
        $code=$LASTEXITCODE
    }
    $ErrorActionPreference='Stop'
    if ($code) {
        Get-Content "$scratch\configure.log" -Tail 30
        if (Test-Path "$scratch\build.log") { Get-Content "$scratch\build.log" -Tail 30 }
        throw "Ninja build failed ($code)"
    }
    $package=Join-Path $scratch 'package'
    New-Item -ItemType Directory -Path $package | Out-Null
    Copy-Item "$build\Release\ninja.exe" "$package\ninja.exe"
    Copy-Item "$source\COPYING" "$package\NINJA-LICENSE.txt"
    Copy-Item "$scratch\configure.log","$scratch\build.log" $package
    $end=[DateTime]::UtcNow
    @{version='1.13.2';patch='LOCAL pipe namespace';start=$start.ToString('o');end=$end.ToString('o');seconds=($end-$start).TotalSeconds;
      archive_sha256=$archiveHash;source_before=$originalHash;source_after=$patchedHash;
      sha256=(Get-FileHash "$package\ninja.exe").Hash.ToLowerInvariant();configuration='Release x64 static runtime'} |
        ConvertTo-Json | Set-Content -Encoding UTF8 "$package\ninja-build.json"
    New-Item -ItemType Directory -Path (Split-Path $Destination) -Force | Out-Null
    Move-Item -LiteralPath $package -Destination $Destination
    Write-Output "LPAC Ninja ready: $Destination"
} finally {
    $env:TEMP=$oldTemp; $env:TMP=$oldTmp; $env:TMPDIR=$oldTmpdir
    $env:VSCMD_SKIP_SENDTELEMETRY=$oldVsTelemetry; $env:DOTNET_CLI_TELEMETRY_OPTOUT=$oldDotnetTelemetry; $env:VCPKG_DISABLE_METRICS=$oldVcpkgTelemetry
    if (Test-Path -LiteralPath $scratch) { Remove-Item -LiteralPath $scratch -Recurse -Force }
}
