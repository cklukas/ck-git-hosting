# Copyright (c) 2026 C. Klukas. All rights reserved.
# SPDX-License-Identifier: MIT
# Copies installed, operator-approved tools; never changes their original ACLs.
param(
    [Parameter(Mandatory=$true)][string]$Destination,
    [Parameter(Mandatory=$true)][string]$TemporaryRoot,
    [Parameter(Mandatory=$true)][string]$MsvcRoot,
    [Parameter(Mandatory=$true)][string]$SdkRoot,
    [Parameter(Mandatory=$true)][string]$SdkVersion,
    [Parameter(Mandatory=$true)][string]$CmakeRoot,
    [Parameter(Mandatory=$true)][string]$InnoRoot
)
$ErrorActionPreference = 'Stop'
foreach ($path in @($Destination,$TemporaryRoot,$MsvcRoot,$SdkRoot,$CmakeRoot,$InnoRoot)) {
    if (![IO.Path]::IsPathRooted($path) -or $path -match '^[A-Za-z]:[^\\/]') { throw "Absolute path required: $path" }
}
if (Test-Path -LiteralPath $Destination) { throw 'Destination already exists; select a new toolchain version' }
$TemporaryRoot = (Resolve-Path -LiteralPath $TemporaryRoot).Path
$oldTemp=$env:TEMP; $oldTmp=$env:TMP; $oldTmpdir=$env:TMPDIR
$stage = Join-Path $TemporaryRoot ('toolchain-stage-' + [guid]::NewGuid().ToString('N'))
$start = [DateTime]::UtcNow
$manifest = New-Object 'System.Collections.Generic.List[string]'
$omissions = New-Object 'System.Collections.Generic.List[string]'
try {
    New-Item -ItemType Directory -Path $stage | Out-Null
    $env:TEMP=$stage; $env:TMP=$stage; $env:TMPDIR=$stage
    $trees = @(
        @((Join-Path $MsvcRoot 'bin\Hostx64\x64'),'msvc\bin\Hostx64\x64'),
        @((Join-Path $MsvcRoot 'include'),'msvc\include'),
        @((Join-Path $MsvcRoot 'lib\x64'),'msvc\lib\x64'),
        @((Join-Path $SdkRoot "Include\$SdkVersion"),'sdk\include'),
        @((Join-Path $SdkRoot "Lib\$SdkVersion\ucrt\x64"),'sdk\lib\ucrt\x64'),
        @((Join-Path $SdkRoot "Lib\$SdkVersion\um\x64"),'sdk\lib\um\x64'),
        @((Join-Path $SdkRoot "bin\$SdkVersion\x64"),'sdk\bin\x64'),
        @($CmakeRoot,'cmake'), @($InnoRoot,'inno')
    )
    foreach ($tree in $trees) {
        $source = (Resolve-Path -LiteralPath $tree[0]).Path.TrimEnd('\')
        $target = Join-Path $stage $tree[1]
        if ((Get-Item -LiteralPath $source).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Redirected tool source: $source" }
        New-Item -ItemType Directory -Path $target -Force | Out-Null
        foreach ($item in Get-ChildItem -LiteralPath $source -Recurse -Force) {
            if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Redirected tool input: $($item.FullName)" }
            $relative = $item.FullName.Substring($source.Length + 1)
            # Optional VC telemetry helpers stay resident and consume sandbox
            # process slots. Keep installed tools intact; omit only this copy.
            if (!$item.PSIsContainer -and $item.Name -ieq 'vctip.exe') {
                $omissions.Add((Join-Path $tree[1] $relative).Replace('\','/'))
                continue
            }
            $copy = Join-Path $target $relative
            if ($item.PSIsContainer) { New-Item -ItemType Directory -Path $copy -Force | Out-Null; continue }
            New-Item -ItemType Directory -Path (Split-Path $copy) -Force | Out-Null
            Copy-Item -LiteralPath $item.FullName -Destination $copy
            $before = (Get-FileHash -LiteralPath $item.FullName -Algorithm SHA256).Hash
            if ($before -ne (Get-FileHash -LiteralPath $copy -Algorithm SHA256).Hash) { throw "Tool copy mismatch: $relative" }
            $manifest.Add($before.ToLowerInvariant() + '  ' + (Join-Path $tree[1] $relative).Replace('\','/'))
        }
        Write-Output "Verified staged $($tree[1])"
    }
    [IO.File]::WriteAllLines((Join-Path $stage 'toolchain.sha256'), $manifest, (New-Object Text.UTF8Encoding($false)))
    [IO.File]::WriteAllLines((Join-Path $stage 'toolchain-omissions.txt'), $omissions, (New-Object Text.UTF8Encoding($false)))
    $end = [DateTime]::UtcNow
    @{start=$start.ToString('o');end=$end.ToString('o');seconds=($end-$start).TotalSeconds;files=$manifest.Count;sdk=$SdkVersion} |
        ConvertTo-Json | Set-Content (Join-Path $stage 'toolchain-build.json')
    New-Item -ItemType Directory -Path (Split-Path $Destination) -Force | Out-Null
    Move-Item -LiteralPath $stage -Destination $Destination
    Write-Output "Toolchain ready: $Destination ($($manifest.Count) verified files)"
} finally {
    $env:TEMP=$oldTemp; $env:TMP=$oldTmp; $env:TMPDIR=$oldTmpdir
    if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
}
