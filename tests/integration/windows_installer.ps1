# Copyright (c) 2026 C. Klukas. All rights reserved.
# SPDX-License-Identifier: MIT
param(
    [Parameter(Mandatory=$true)][string]$Installer,
    [Parameter(Mandatory=$true)][string]$Binary,
    [Parameter(Mandatory=$true)][string]$TestRoot,
    [ValidateSet('CurrentUser','AllUsers')][string]$Mode = 'CurrentUser',
    [ValidateSet('ckdocs','ckgit')][string]$Product = 'ckdocs',
    [string]$DocsBinary,
    [switch]$RequireNonAdmin
)
$ErrorActionPreference = 'Stop'
$Installer = (Resolve-Path -LiteralPath $Installer).Path
$Binary = (Resolve-Path -LiteralPath $Binary).Path
$TestRoot = (Resolve-Path -LiteralPath $TestRoot).Path
$admin = (New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if ($RequireNonAdmin -and $admin) { throw 'This test requires a token without administrator rights' }
if ($Mode -eq 'AllUsers' -and !$admin) { throw 'AllUsers test needs an administrator token' }
$hive = if ($Mode -eq 'AllUsers') { 'HKLM:' } else { 'HKCU:' }
$productId = if ($Product -eq 'ckgit') { '{CDB31BA9-7F4D-4F73-9A15-5812D2493B99}' } else { '{9BA75E20-ED1F-4FBE-8F01-0F052E0F9700}' }
$executable = "$Product.exe"
if ($Product -eq 'ckgit') {
    if (!$DocsBinary) { throw 'DocsBinary is required to verify the bundled ckdocs executable' }
    $DocsBinary = (Resolve-Path -LiteralPath $DocsBinary).Path
}
$uninstallKey = "$hive\Software\Microsoft\Windows\CurrentVersion\Uninstall\${productId}_is1"
if (Test-Path $uninstallKey) { throw "An existing $Product installation must not be changed by this test" }
$environmentKey = if ($Mode -eq 'AllUsers') { 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\Environment' } else { 'HKCU:\Environment' }
$previousPath = (Get-Item $environmentKey).GetValue('Path', $null, [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
$fixture = Join-Path $TestRoot ($Product + '-install-' + [Guid]::NewGuid().ToString('N'))
$directory = Join-Path $fixture 'application with spaces'
New-Item -ItemType Directory $fixture | Out-Null
$oldTemp=$env:TEMP; $oldTmp=$env:TMP; $oldTmpdir=$env:TMPDIR
try {
    $env:TEMP=$fixture; $env:TMP=$fixture; $env:TMPDIR=$fixture
    for ($attempt=0; $attempt -lt 2; $attempt++) {
        $args = '/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /' + $Mode.ToUpperInvariant() +
            ' /TASKS=addtopath /DIR="' + $directory + '" /LOG="' + (Join-Path $fixture "install-$attempt.log") + '"'
        $process = Start-Process -FilePath $Installer -ArgumentList $args -PassThru -Wait
        if ($process.ExitCode -ne 0) { throw "Installer failed: $($process.ExitCode)" }
        if ((Get-FileHash (Join-Path $directory $executable)).Hash -ne (Get-FileHash $Binary).Hash) { throw 'Installed executable differs from verified binary' }
        & (Join-Path $directory $executable) --version
        if ($LASTEXITCODE -ne 0) { throw 'Installed executable does not run' }
        if ($Product -eq 'ckgit' -and (Get-FileHash (Join-Path $directory 'ckdocs.exe')).Hash -ne (Get-FileHash $DocsBinary).Hash) {
            throw 'Bundled ckdocs differs from the verified build'
        }
        if ($attempt -eq 0) { Set-Content (Join-Path $directory 'user-data.txt') 'preserve me' }
        elseif ((Get-Content (Join-Path $directory 'user-data.txt')) -ne 'preserve me') { throw 'Reinstall changed user data' }
    }
    $process = Start-Process -FilePath (Join-Path $directory 'unins000.exe') -ArgumentList ('/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /LOG="' + (Join-Path $fixture 'uninstall.log') + '"') -Wait -PassThru
    if ($process.ExitCode -ne 0 -or (Test-Path (Join-Path $directory $executable))) { throw 'Uninstall failed' }
    if ((Get-Content (Join-Path $directory 'user-data.txt')) -ne 'preserve me') { throw 'Uninstall removed user data' }
    $afterPath = (Get-Item $environmentKey).GetValue('Path', $null, [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
    if ($previousPath -cne $afterPath) { throw 'PATH did not return to its previous value' }
    if (Test-Path $uninstallKey) { throw 'Uninstall registration remains' }
    Write-Output "Installer $Product $Mode install/reinstall/uninstall passed; administrator token=$admin"
} finally {
    $env:TEMP=$oldTemp; $env:TMP=$oldTmp; $env:TMPDIR=$oldTmpdir
    # Preserve a failed installation and its logs for diagnosis. Never delete
    # files behind an installed uninstaller or reset a concurrently edited PATH.
    if (!(Test-Path $uninstallKey)) { Remove-Item -LiteralPath $fixture -Recurse -Force }
}
