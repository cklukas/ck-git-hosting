# Copyright (c) 2026 C. Klukas. All rights reserved.
# SPDX-License-Identifier: MIT
# Rejection tests only: never mount a disk or change a service/registry key.
param([Parameter(Mandatory=$true)][string]$ScriptsDir,
      [Parameter(Mandatory=$true)][string]$PackagingDir,
      [Parameter(Mandatory=$true)][string]$TestRoot)
$ErrorActionPreference='Stop'
if(!(Get-Command Get-FileHash -ErrorAction SilentlyContinue)){
 function global:Get-FileHash {
  param([Parameter(Position=0)][string]$Path,[string]$LiteralPath,[string]$Algorithm='SHA256')
  $target=if($LiteralPath){$LiteralPath}else{$Path}
  $full=(Resolve-Path -LiteralPath $target).Path
  $sha=[Security.Cryptography.SHA256]::Create()
  try{
   $stream=[IO.File]::OpenRead($full)
   try{$hash=[BitConverter]::ToString($sha.ComputeHash($stream)).Replace('-','')}finally{$stream.Dispose()}
  }finally{$sha.Dispose()}
  [PSCustomObject]@{Algorithm=$Algorithm;Hash=$hash;Path=$full}
 }
}
if(!(Test-Path -LiteralPath 'V:\' -PathType Container)){throw 'V: is required'}
$ScriptsDir=(Resolve-Path -LiteralPath $ScriptsDir).Path
if([IO.Path]::GetPathRoot($TestRoot) -ne 'V:\' -and $env:TMPDIR -and [IO.Path]::GetPathRoot($env:TMPDIR) -eq 'V:\'){$TestRoot=$env:TMPDIR}
New-Item -ItemType Directory -Force -Path $TestRoot | Out-Null
$TestRoot=(Resolve-Path -LiteralPath $TestRoot).Path
if([IO.Path]::GetPathRoot($TestRoot) -ne 'V:\'){throw 'Select a V: test root'}
$fixture=Join-Path $TestRoot ('hcs-setup-test-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $fixture | Out-Null
$oldTemp=$env:TEMP; $oldTmp=$env:TMP; $oldTmpdir=$env:TMPDIR
try {
 $env:TEMP=$fixture; $env:TMP=$fixture; $env:TMPDIR=$fixture
 foreach($name in @('setup-windows-hcs.ps1','prepare-windows-vm-toolchain.ps1','prepare-windows-vm-base.ps1','register-windows-vm-socket.ps1','windows-vm-image-policy.ps1')){
  $tokens=$null;$errors=$null
  [System.Management.Automation.Language.Parser]::ParseFile((Join-Path $ScriptsDir $name),[ref]$tokens,[ref]$errors)|Out-Null
  if($errors.Count){throw "Invalid PowerShell: $name"}
 }
 # Stage a tiny fake toolchain and independently reconstruct its inventory.
 # No vendor program executes and no virtual disk is created.
 $vendors=Join-Path $fixture 'vendors'; $msvc=Join-Path $vendors 'msvc'; $sdkRoot=Join-Path $vendors 'sdk'
 $cmake=Join-Path $vendors 'cmake'; $inno=Join-Path $vendors 'inno'; $ninja=Join-Path $vendors 'ninja'
 foreach($dir in @("$msvc\bin\Hostx64\x64","$msvc\include","$msvc\lib\x64","$sdkRoot\Include\10.0.1","$sdkRoot\Lib\10.0.1\ucrt\x64","$sdkRoot\Lib\10.0.1\um\x64","$sdkRoot\bin\10.0.1\x64",$cmake,$inno,$ninja)){
  New-Item -ItemType Directory -Path $dir -Force | Out-Null
  [IO.File]::WriteAllText((Join-Path $dir 'fixture.txt'),$dir)
 }
 [IO.File]::WriteAllText((Join-Path $ninja 'ninja.exe'),'fixture')
 @{version='1.13.2';patch='LOCAL pipe namespace';source_after='d6cbc5d6d350e4677991abe5828483fad39c0bc4d911a5ad1594d8d82d09e517';sha256=(Get-FileHash (Join-Path $ninja 'ninja.exe')).Hash} | ConvertTo-Json | Set-Content (Join-Path $ninja 'ninja-build.json')
 $staged=Join-Path $fixture 'staged'
 & (Join-Path $PackagingDir 'stage-toolchain.ps1') -Destination $staged -TemporaryRoot $fixture -MsvcRoot $msvc -SdkRoot $sdkRoot -SdkVersion '10.0.1' -CmakeRoot $cmake -InnoRoot $inno -NinjaRoot $ninja | Out-Null
 $inventory=Join-Path $staged 'toolchain.sha256'
 $record=Get-Content (Join-Path $staged 'toolchain-build.json') -Raw | ConvertFrom-Json
 if($record.manifest_sha256 -cne (Get-FileHash $inventory).Hash.ToLowerInvariant()){throw 'Staged toolchain record does not bind its inventory'}
 $items=New-Object 'System.Collections.Generic.List[string]'
 foreach($entry in Get-ChildItem -LiteralPath $staged -Recurse -File){
  $relative=$entry.FullName.Substring($staged.Length+1).Replace('\','/')
  if($relative -in @('toolchain.sha256','toolchain-build.json')){continue}
  $items.Add((Get-FileHash -LiteralPath $entry.FullName).Hash.ToLowerInvariant()+'  '+$relative)
 }
 $items.Sort([StringComparer]::Ordinal)
 $reconstructed=Join-Path $fixture 'reconstructed.sha256'
 [IO.File]::WriteAllLines($reconstructed,$items,(New-Object Text.UTF8Encoding($false)))
 if((Get-FileHash $reconstructed).Hash -cne (Get-FileHash $inventory).Hash){throw 'Staged inventory differs from the SDK copier inventory'}
 $source=Join-Path $fixture 'source'; New-Item -ItemType Directory $source | Out-Null
 $settings=Join-Path $fixture 'settings.ini'; [IO.File]::WriteAllText($settings,"identity=V:/secret`n")
 $destination=Join-Path $fixture 'sdk'
 $rejected=$false
 try {& (Join-Path $ScriptsDir 'prepare-windows-vm-toolchain.ps1') -Source $source -InventorySha256 ('a'*64) -Destination $destination -Scratch $fixture -GuestSettings $settings} catch {$rejected=$true}
 if(!$rejected -or (Test-Path -LiteralPath $destination)){throw 'Unsafe guest settings were not rejected before image creation'}
 [IO.File]::WriteAllText($settings,"ci_toolchain_paths=../escape`n")
 $rejected=$false
 try {& (Join-Path $ScriptsDir 'prepare-windows-vm-toolchain.ps1') -Source $source -InventorySha256 ('a'*64) -Destination $destination -Scratch $fixture -GuestSettings $settings} catch {$rejected=$true}
 if(!$rejected -or (Test-Path -LiteralPath $destination)){throw 'Escaping guest path was not rejected before image creation'}
 [IO.File]::WriteAllText($settings,"ci_toolchain_paths=cmake/bin;ninja`nci_toolchain_env.INCLUDE=`${TOOLS}/include`n")
 $failure=''
 try {& (Join-Path $ScriptsDir 'prepare-windows-vm-toolchain.ps1') -Source $source -InventorySha256 ('a'*64) -Destination $destination -Scratch $fixture -GuestSettings $settings} catch {$failure=$_.Exception.Message}
 if(!$failure -or $failure -notmatch 'toolchain-build.json' -or (Test-Path -LiteralPath $destination)){throw 'Valid guest settings were not accepted up to toolchain inventory verification'}
 $base=Join-Path $fixture 'base';New-Item -ItemType Directory $base | Out-Null
 $os=Join-Path $base 'os.vhdx';[IO.File]::WriteAllText($os,'fixture')
 $license=Join-Path $base 'windows-evaluation-license.rtf';[IO.File]::WriteAllText($license,'license')
 $hash=(Get-FileHash -LiteralPath $license -Algorithm SHA256).Hash.ToLowerInvariant()
 @{status='expanded-unbooted-awaiting-license-and-guest-agent';os_disk=$os;license_sha256=$hash} | ConvertTo-Json | Set-Content (Join-Path $base 'base-expansion.json')
 @{owner='ckgit-windows-vm-base';nonce='0123456789abcdef0123456789abcdef'} | ConvertTo-Json | Set-Content (Join-Path $base 'owner.json')
 $sdk=Join-Path $fixture 'sdk.vhdx';[IO.File]::WriteAllText($sdk,'fixture')
 $rejected=$false
 try {& (Join-Path $ScriptsDir 'setup-windows-hcs.ps1') -Phase Complete -Destination $base -Scratch $fixture -SdkDisk $sdk -AcceptedLicenseSha256 ('b'*64)} catch {$rejected=$true}
 if(!$rejected -or (Test-Path (Join-Path $base 'windows-license-acceptance.json')) -or (Test-Path (Join-Path $base 'work-template.vhdx'))){throw 'Unaccepted license was not rejected before image mutation'}
 Write-Output 'HCS setup script parsing and pre-mutation rejection checks passed'
} finally {
 $env:TEMP=$oldTemp; $env:TMP=$oldTmp; $env:TMPDIR=$oldTmpdir
 if(Test-Path -LiteralPath $fixture){Remove-Item -LiteralPath $fixture -Recurse -Force}
}
