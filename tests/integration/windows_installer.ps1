# Copyright (c) 2026 C. Klukas. All rights reserved.
# SPDX-License-Identifier: MIT
param(
    [Parameter(Mandatory=$true)][string]$Installer,
    [Parameter(Mandatory=$true)][string]$Binary,
    [Parameter(Mandatory=$true)][string]$TestRoot,
    [ValidateSet('CurrentUser','AllUsers')][string]$Mode = 'CurrentUser',
    [ValidateSet('ckdocs','ckgit','ck-git-runner')][string]$Product = 'ckdocs',
    [string]$DocsBinary,
    [string]$LauncherBinary,
    [ValidateSet('both','ckgit','ckdocs')][string]$InitialComponents = 'both',
    [ValidatePattern('^\{[0-9A-Fa-f-]{36}\}$')][string]$TestAppId,
    [ValidateSet('Inherited','Present','Missing')][string]$DependencyPathMode = 'Inherited',
    [switch]$ExerciseRunningUpgrade,
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
$productId = if ($Product -eq 'ckgit') { '{CDB31BA9-7F4D-4F73-9A15-5812D2493B99}' } elseif ($Product -eq 'ck-git-runner') { '{4527D4DC-5653-4650-969F-19D76EF349B8}' } else { '{9BA75E20-ED1F-4FBE-8F01-0F052E0F9700}' }
if ($TestAppId) { $productId = $TestAppId } # Dedicated fixture package; never a production installation.
if ($Product -ne 'ckgit' -and $InitialComponents -ne 'both') { throw 'Component selection is only available in the combined client installer' }
if ($ExerciseRunningUpgrade -and $Product -ne 'ckdocs') { throw 'Running upgrade fixture currently requires the standalone ckdocs installer' }
$executable = if ($Product -eq 'ck-git-runner') { 'ck-ci-runnerd.exe' } else { "$Product.exe" }
if ($Product -eq 'ck-git-runner') {
    if (!$LauncherBinary) { throw 'LauncherBinary is required for the runner installer' }
    $LauncherBinary = (Resolve-Path -LiteralPath $LauncherBinary).Path
}
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
$passed = $false
$preview = $null
try {
    $env:TEMP=$fixture; $env:TMP=$fixture; $env:TMPDIR=$fixture
    if ($Product -eq 'ckgit') {
        $emptyArgs = '/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /' + $Mode.ToUpperInvariant() +
            ' /COMPONENTS="" /DIR="' + $directory + '" /LOG="' + (Join-Path $fixture 'empty-selection.log') + '"'
        $empty = Start-Process -FilePath $Installer -ArgumentList $emptyArgs -PassThru -Wait
        if ($empty.ExitCode -eq 0 -or (Test-Path $uninstallKey)) { throw 'Empty component selection was accepted' }
        if ((Test-Path (Join-Path $directory 'ckgit.exe')) -or (Test-Path (Join-Path $directory 'ckdocs.exe'))) {
            throw 'Empty component selection installed a program'
        }
    }
    for ($attempt=0; $attempt -lt 2; $attempt++) {
        if ($attempt -eq 1 -and $ExerciseRunningUpgrade) {
            $source = Join-Path $fixture 'preview source'
            New-Item -ItemType Directory $source | Out-Null
            [IO.File]::WriteAllText((Join-Path $source 'README.md'), "# Upgrade preview`n`nKeep this preview available until setup closes it.`n", (New-Object Text.UTF8Encoding($false)))
            $previewExe = Join-Path $directory 'ckdocs.exe'
            $previewLog = Join-Path $fixture 'preview.log'
            $preview = Start-Process -FilePath $previewExe -ArgumentList ('serve --root "' + $source + '" --port 0') -PassThru -WindowStyle Hidden -RedirectStandardOutput $previewLog -RedirectStandardError (Join-Path $fixture 'preview-error.log')
            $previewHandle = $preview.Handle # Retain the handle so ExitCode remains observable.
            $port = $null
            for ($i=0; $i -lt 100; $i++) {
                $preview.Refresh()
                if ($preview.HasExited) { throw 'Installed preview exited before upgrade' }
                if ((Get-Content $previewLog -Raw -ErrorAction SilentlyContinue) -match 'serve ready on (\d+)') { $port=$Matches[1]; break }
                Start-Sleep -Milliseconds 100
            }
            if (!$port -or $preview.MainModule.FileName -ne $previewExe) { throw 'Expected installed preview did not start' }
            $page = Invoke-WebRequest "http://127.0.0.1:$port/index.html" -UseBasicParsing -TimeoutSec 5
            if ($page.Content -notmatch 'Keep this preview available') { throw 'Installed preview content mismatch' }
        }
        $args = '/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /' + $Mode.ToUpperInvariant() +
            ' /TASKS=addtopath /DIR="' + $directory + '" /LOG="' + (Join-Path $fixture "install-$attempt.log") + '"'
        $components = if ($attempt -eq 0 -and $InitialComponents -ne 'both') { $InitialComponents } else { 'ckgit,ckdocs' }
        if ($Product -eq 'ckgit' -and ($attempt -ne 0 -or $InitialComponents -ne 'both')) {
            $args += ' /COMPONENTS="' + $components + '"'
        }
        $previousProcessPath = $env:PATH
        try {
            if ($DependencyPathMode -ne 'Inherited') {
                $dependencyDirectory = Join-Path $fixture ('portable tools ' + [char]0x00FC)
                New-Item -ItemType Directory $dependencyDirectory -Force | Out-Null
                if ($DependencyPathMode -eq 'Present') {
                    # Presence-only preflight must not execute these fixture files.
                    [IO.File]::WriteAllText((Join-Path $dependencyDirectory 'git.exe'), 'not executable')
                    [IO.File]::WriteAllText((Join-Path $dependencyDirectory 'ssh.exe'), 'not executable')
                }
                $env:PATH = '"' + $dependencyDirectory + '"'
            }
            $process = Start-Process -FilePath $Installer -ArgumentList $args -PassThru -Wait
        } finally { $env:PATH = $previousProcessPath }
        $installLog = Get-Content (Join-Path $fixture "install-$attempt.log") -Raw
        foreach ($tool in @('Git','SSH')) {
            $expectedTool = if ($DependencyPathMode -eq 'Missing') { 'not found on PATH' }
                elseif ($DependencyPathMode -eq 'Present') { Join-Path $dependencyDirectory ($tool.ToLowerInvariant() + '.exe') }
                else { '' }
            if (!$installLog.Contains("$tool on installer PATH: $expectedTool")) { throw "Installer dependency check missing or incorrect: $tool" }
        }
        if ($process.ExitCode -ne 0) { throw "Installer failed: $($process.ExitCode)" }
        if ($preview) {
            if (!$preview.WaitForExit(10000) -or $preview.ExitCode -ne 0) { throw 'Installer did not close the preview cleanly' }
            if (@(Get-ChildItem $fixture -Directory -Filter 'ckdocs-serve-*').Count) { throw 'Preview shutdown left its temporary site behind' }
            $preview.Dispose(); $preview = $null
        }
        $expected = @{}; $expected[$executable] = $Binary
        if ($Product -eq 'ckgit') {
            $expected = @{}
            if ($components -match 'ckgit') { $expected['ckgit.exe'] = $Binary }
            if ($components -match 'ckdocs') { $expected['ckdocs.exe'] = $DocsBinary }
            foreach ($name in @('ckgit.exe','ckdocs.exe')) {
                if (!$expected.ContainsKey($name) -and (Test-Path (Join-Path $directory $name))) { throw "Unselected component was installed: $name" }
            }
        }
        foreach ($name in $expected.Keys) {
            if ((Get-FileHash (Join-Path $directory $name)).Hash -ne (Get-FileHash $expected[$name]).Hash) { throw "Installed executable differs from verified binary: $name" }
            & (Join-Path $directory $name) --version
            if ($LASTEXITCODE -ne 0) { throw "Installed executable does not run: $name" }
        }
        if ($Product -eq 'ck-git-runner' -and (Get-FileHash (Join-Path $directory 'ckgit-job-launcher.exe')).Hash -ne (Get-FileHash $LauncherBinary).Hash) {
            throw 'Bundled job launcher differs from the verified build'
        }
        if ($attempt -eq 0) { Set-Content (Join-Path $directory 'user-data.txt') 'preserve me' }
        elseif ((Get-Content (Join-Path $directory 'user-data.txt')) -ne 'preserve me') { throw 'Reinstall changed user data' }
    }
    $process = Start-Process -FilePath (Join-Path $directory 'unins000.exe') -ArgumentList ('/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /LOG="' + (Join-Path $fixture 'uninstall.log') + '"') -Wait -PassThru
    if ($process.ExitCode -ne 0 -or (Test-Path (Join-Path $directory $executable))) { throw 'Uninstall failed' }
    if ($Product -eq 'ckgit' -and (Test-Path (Join-Path $directory 'ckdocs.exe'))) { throw 'Uninstall left the ckdocs component' }
    if ((Get-Content (Join-Path $directory 'user-data.txt')) -ne 'preserve me') { throw 'Uninstall removed user data' }
    $afterPath = (Get-Item $environmentKey).GetValue('Path', $null, [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
    if ($previousPath -cne $afterPath) { throw 'PATH did not return to its previous value' }
    if (Test-Path $uninstallKey) { throw 'Uninstall registration remains' }
    $passed = $true
    Write-Output "Installer $Product $Mode install/reinstall/uninstall passed; initial components=$InitialComponents; dependency PATH=$DependencyPathMode; administrator token=$admin"
} finally {
    if ($preview) {
        $preview.Refresh()
        if (!$preview.HasExited) { $preview.Kill(); $preview.WaitForExit() }
        $preview.Dispose()
    }
    $env:TEMP=$oldTemp; $env:TMP=$oldTmp; $env:TMPDIR=$oldTmpdir
    # Preserve a failed installation and its logs for diagnosis. Never delete
    # files behind an installed uninstaller or reset a concurrently edited PATH.
    if ($passed -and !(Test-Path $uninstallKey)) { Remove-Item -LiteralPath $fixture -Recurse -Force }
}
