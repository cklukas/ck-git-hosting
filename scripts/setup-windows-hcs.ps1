# Copyright (c) 2026 C. Klukas. All rights reserved.
# SPDX-License-Identifier: MIT
# Explicit, operator-run preparation of a new, offline HCS runner image.
[CmdletBinding()]
param(
 [Parameter(Mandatory=$true)][ValidateSet('Prepare','Complete')][string]$Phase,
 [Parameter(Mandatory=$true)][string]$Destination,
 [Parameter(Mandatory=$true)][string]$Scratch,
 [string]$Iso,
 [ValidatePattern('^[a-f0-9]{64}$')][string]$IsoSha256,
 [string]$SdkDisk,
 [ValidatePattern('^[a-f0-9]{64}$')][string]$AcceptedLicenseSha256,
 [ValidateRange(2048,32768)][int]$MemoryMB=8192,
 [ValidateRange(1,16)][int]$Processors=4,
 [ValidateRange(10,2048)][int]$WorkDiskGiB=120,
 [ValidateRange(1,4096)][int]$MinimumFreeGiB=20
)
$ErrorActionPreference='Stop'; $ProgressPreference='SilentlyContinue'
if(!(Test-Path -LiteralPath 'V:\' -PathType Container)){throw 'The Windows HCS backend currently requires V:'}
$admin=New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if(!$admin.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)){throw 'Image provisioning requires an elevated administrator PowerShell'}
function Check-VPath([string]$Path,[bool]$MustExist) {
 if(!$Path -or ![IO.Path]::IsPathRooted($Path) -or [IO.Path]::GetPathRoot($Path) -ne 'V:\'){throw 'Choose absolute V: paths for image inputs, destination and scratch'}
 if($MustExist -and !(Test-Path -LiteralPath $Path)){throw 'A selected path does not exist'}
 $p=[IO.Path]::GetFullPath($Path); while($p -and !(Test-Path -LiteralPath $p)){$p=[IO.Path]::GetDirectoryName($p)}
 while($p){if((Get-Item -LiteralPath $p -Force).Attributes -band [IO.FileAttributes]::ReparsePoint){throw 'A selected path has a reparse-point ancestor'};$p=[IO.Path]::GetDirectoryName($p)}
}
Check-VPath $Destination ($Phase -eq 'Complete'); Check-VPath $Scratch $true
if(!(Test-Path -LiteralPath $Scratch -PathType Container)){throw 'Scratch must be an existing private directory'}
$env:TEMP=$Scratch; $env:TMP=$Scratch; $env:TMPDIR=$Scratch
$Destination=[IO.Path]::GetFullPath($Destination)
if($Phase -eq 'Prepare') {
 if(!$Iso -or !$IsoSha256){throw 'Prepare requires -Iso and -IsoSha256'}
 Check-VPath $Iso $true
 if(Test-Path -LiteralPath $Destination){throw 'Choose a new destination; existing images are never modified'}
 & (Join-Path $PSScriptRoot 'prepare-windows-vm-base.ps1') -Iso $Iso -IsoSha256 $IsoSha256 -Destination $Destination -Scratch $Scratch
 $record=Get-Content -LiteralPath (Join-Path $Destination 'base-expansion.json') -Raw | ConvertFrom-Json
 if($record.status -cne 'expanded-unbooted-awaiting-license-and-guest-agent'){throw 'Offline image preparation did not complete'}
 $preparedHash=(Get-FileHash -LiteralPath (Join-Path $Destination 'os.vhdx') -Algorithm SHA256).Hash.ToLowerInvariant()
 [IO.File]::WriteAllText((Join-Path $Destination 'prepared-os.sha256'),$preparedHash+"`n",(New-Object Text.UTF8Encoding($false)))
 Write-Output ('Review the Windows license: '+(Join-Path $Destination 'windows-evaluation-license.rtf'))
 Write-Output ('License SHA-256: '+$record.license_sha256)
 Write-Output 'Then run Complete with -AcceptedLicenseSha256 set to that reviewed license hash and an operator-prepared SDK disk.'
 return
}
if(!$SdkDisk -or !$AcceptedLicenseSha256){throw 'Complete requires -SdkDisk and the reviewed -AcceptedLicenseSha256'}
Check-VPath $SdkDisk $true
$record=Get-Content -LiteralPath (Join-Path $Destination 'base-expansion.json') -Raw | ConvertFrom-Json
$owner=Get-Content -LiteralPath (Join-Path $Destination 'owner.json') -Raw | ConvertFrom-Json
if($owner.owner -cne 'ckgit-windows-vm-base' -or $owner.nonce -cnotmatch '^[a-f0-9]{32}$'){throw 'Destination is not an owned HCS image preparation'}
$os=Join-Path $Destination 'os.vhdx'; $license=Join-Path $Destination 'windows-evaluation-license.rtf'
if($record.status -cne 'expanded-unbooted-awaiting-license-and-guest-agent' -or $record.os_disk -cne $os -or !(Test-Path -LiteralPath $os -PathType Leaf) -or !(Test-Path -LiteralPath $license -PathType Leaf)){throw 'Destination is not a newly prepared offline image'}
$actualLicense=(Get-FileHash -LiteralPath $license -Algorithm SHA256).Hash.ToLowerInvariant()
if($actualLicense -cne $AcceptedLicenseSha256 -or $record.license_sha256 -cne $actualLicense){throw 'Reviewed license hash does not match this image'}
$preparedHashPath=Join-Path $Destination 'prepared-os.sha256'
if(!(Test-Path -LiteralPath $preparedHashPath -PathType Leaf)){throw 'Prepared OS hash record is absent'}
$preparedHash=[IO.File]::ReadAllText($preparedHashPath).Trim()
if($preparedHash -cnotmatch '^[a-f0-9]{64}$' -or (Get-FileHash -LiteralPath $os -Algorithm SHA256).Hash.ToLowerInvariant() -cne $preparedHash){throw 'Prepared OS image differs from the verified expansion'}
# The current host-check binary admits this specific, previously qualified
# evaluation image. Do not claim support for other Windows licenses/images.
if($actualLicense -cne '6b5d4b32236a921bacdefe19852ec0699ca2eacd72d4e57f50a2de06ede21c8e'){throw 'This HCS preview supports only the qualified Windows Enterprise Evaluation license'}
$installRoot=Split-Path -Parent $PSScriptRoot
$binaryRoot=$installRoot
# The installer keeps executables beside hcs/; the portable ZIP uses bin/.
if(!(Test-Path -LiteralPath (Join-Path $binaryRoot 'ckgit-windows-vm-guest.exe') -PathType Leaf)){$binaryRoot=Join-Path $installRoot 'bin'}
$guest=Join-Path $binaryRoot 'ckgit-windows-vm-guest.exe'
$hostCheck=Join-Path $binaryRoot 'ckgit-windows-vm-host-check.exe'
foreach($p in @($guest,$hostCheck,$SdkDisk)){if(!(Test-Path -LiteralPath $p -PathType Leaf)){throw 'Installed HCS binaries and SDK disk must exist'}}
$work=Join-Path $Destination 'work-template.vhdx'; $profile=Join-Path $Destination 'images.ini'; $accept=Join-Path $Destination 'windows-license-acceptance.json'
foreach($p in @($work,$profile,$accept,(Join-Path $Destination 'guest-installed.json'))){if(Test-Path -LiteralPath $p){throw 'Preparation already started; inspect the destination instead of overwriting it'}}
if((Get-Volume -DriveLetter V).SizeRemaining -lt ([long]$MinimumFreeGiB * 1GB)){throw 'Insufficient free space for the selected reserve; image is not admitted'}
# Never attach or detach an image that is already in use by any VM or task.
if((Get-VHD -Path $os).Attached -or (Get-VHD -Path $SdkDisk).Attached){throw 'Selected OS or SDK disk is already attached; no disk was modified'}
# Validate the operator-prepared SDK disk before touching the new OS disk.
$sdkMounted=$false
try {
 $disk=Mount-VHD -Path $SdkDisk -ReadOnly -NoDriveLetter -PassThru | Get-Disk; $sdkMounted=$true
 $volumes=@(Get-Partition -DiskNumber $disk.Number | Get-Volume | Where-Object FileSystemLabel -CEQ 'CKGIT_SDK')
 if($volumes.Count -ne 1){throw 'SDK disk must contain one CKGIT_SDK volume'}
 $root=$volumes[0].Path; if(!$root){$root=$volumes[0].UniqueId}
 if(!(Test-Path -LiteralPath (Join-Path $root 'guest-toolchain.ini') -PathType Leaf) -or !(Test-Path -LiteralPath (Join-Path $root 'approved') -PathType Container)){throw 'SDK disk needs approved tools and a credential-free guest-toolchain.ini'}
 $guestIni=Join-Path $root 'guest-toolchain.ini'
 if((Get-Item -LiteralPath $guestIni).Length -gt 16384){throw 'Guest toolchain profile is too large'}
 $fields=@{}
 foreach($line in [IO.File]::ReadAllLines($guestIni)){
  if(!$line -or $line.StartsWith('#')){continue}
  if($line -notmatch '^([A-Za-z0-9_.]+)=([^\x00-\x1f\x7f]*)$'){throw 'Malformed guest toolchain profile'}
  $key=$Matches[1]; if($fields.ContainsKey($key)){throw 'Duplicate guest toolchain setting'}; $fields[$key]=$Matches[2]
  if($key -notin @('schema_version','server','name','ci_build_root','ci_toolchain_root','ci_allow_network','ci_allow_degraded','ci_toolchain_paths') -and $key -cnotmatch '^ci_toolchain_env\.[A-Z_][A-Z0-9_]*$'){throw 'Guest profile contains unsupported or credential-bearing fields'}
 }
 if($fields['schema_version'] -cne '1' -or $fields['server'] -cne 'ckgit@example.invalid' -or $fields['name'] -cne 'offline-vm-guest' -or $fields['ci_build_root'] -cne 'V:/tmp/ckgit-vm-job' -or $fields['ci_toolchain_root'] -cne 'V:/tools/approved' -or $fields['ci_allow_network'] -cne 'false' -or $fields['ci_allow_degraded'] -cne 'false' -or !$fields['ci_toolchain_paths']){throw 'Guest toolchain profile is not the expected offline, credential-free configuration'}
 $toolPaths=$fields['ci_toolchain_paths'].Split(';')
 if($toolPaths.Count -gt 32){throw 'Too many guest tool directories'}
 foreach($relative in $toolPaths){
  if(!$relative -or $relative.Contains('\') -or $relative.Contains(':') -or $relative.StartsWith('/') -or @($relative.Split('/') | Where-Object {$_ -eq '..' -or $_ -eq ''}).Count){throw 'Guest tool directories must be relative slash-separated paths'}
  $dir=Join-Path $root 'approved'
  foreach($part in $relative.Split('/')){
   $dir=Join-Path $dir $part
   if(!(Test-Path -LiteralPath $dir -PathType Container) -or ((Get-Item -LiteralPath $dir -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)){throw 'Guest tool directory is missing or redirected'}
  }
 }
} finally {if($sdkMounted){Dismount-VHD -Path $SdkDisk -ErrorAction Stop}}
& (Join-Path $PSScriptRoot 'register-windows-vm-socket.ps1') -Scratch $Scratch -Register | Out-Null
& $hostCheck --preflight
if($LASTEXITCODE){throw 'HCS preflight failed'}
# Install the guest service only into this newly prepared, offline OS disk.
$mounted=$false; $loaded=@()
try {
 $disk=Mount-VHD -Path $os -NoDriveLetter -PassThru | Get-Disk; $mounted=$true
 $parts=@(Get-Partition -DiskNumber $disk.Number | Where-Object GptType -EQ '{ebd0a0a2-b9e5-4433-87c0-68b6b72699c7}')
 if($parts.Count -ne 1){throw 'Prepared OS disk partition layout differs'}
 $volume=$parts[0] | Get-Volume; $root=$volume.Path; if(!$root){$root=$volume.UniqueId}
 $target=Join-Path $root 'Program Files\ckgit-ci'; if(Test-Path -LiteralPath $target){throw 'Guest service destination already exists'}
 New-Item -ItemType Directory -Path $target | Out-Null
 Copy-Item -LiteralPath $guest -Destination (Join-Path $target 'ckgit-windows-vm-guest.exe')
 if((Get-FileHash -LiteralPath $guest).Hash -cne (Get-FileHash -LiteralPath (Join-Path $target 'ckgit-windows-vm-guest.exe')).Hash){throw 'Guest copy verification failed'}
 $template=[IO.File]::ReadAllText((Join-Path $PSScriptRoot 'windows-vm-unattend.xml.template'))
 if(!$template.Contains('__HUMAN_WINDOWS_LICENSE_ACCEPTANCE_REQUIRED__') -or !$template.Contains('__RANDOM_GUEST_ONLY_PASSWORD__')){throw 'Unexpected unattended template'}
 $answer=$template.Replace('__HUMAN_WINDOWS_LICENSE_ACCEPTANCE_REQUIRED__','true').Replace('__RANDOM_GUEST_ONLY_PASSWORD__',([guid]::NewGuid().ToString('N')+'aA1!'))
 $panther=Join-Path $root 'Windows\Panther'; [IO.Directory]::CreateDirectory($panther)|Out-Null; $answerPath=Join-Path $panther 'Unattend.xml'
 if(Test-Path -LiteralPath $answerPath){throw 'Offline answer file already exists'}
 [IO.File]::WriteAllText($answerPath,$answer,(New-Object Text.UTF8Encoding($false))); $answer=$null
 foreach($kind in @('SYSTEM','SOFTWARE')) {
  $name='CKGitSetup-'+[guid]::NewGuid().ToString('N'); $hive=Join-Path $root ("Windows\System32\config\"+$kind)
  & "$env:SystemRoot\System32\reg.exe" load ("HKLM\"+$name) $hive | Out-Null
  if($LASTEXITCODE){throw 'Cannot load the offline guest registry hive'}
  $loaded+= $name
  $key=[Microsoft.Win32.Registry]::LocalMachine.OpenSubKey($name,$true)
  try {
   if($kind -eq 'SYSTEM') {
    $select=$key.OpenSubKey('Select'); try {$current=[int]$select.GetValue('Current')} finally {$select.Dispose()}
    if($current -lt 1 -or $current -gt 9){throw 'Offline control set is invalid'}
    $service=$key.CreateSubKey(('ControlSet{0:D3}\Services\ckgit-vm-guest' -f $current))
    try {$service.SetValue('Type',16,[Microsoft.Win32.RegistryValueKind]::DWord);$service.SetValue('Start',2,[Microsoft.Win32.RegistryValueKind]::DWord);$service.SetValue('ErrorControl',1,[Microsoft.Win32.RegistryValueKind]::DWord);$service.SetValue('ImagePath','"C:\Program Files\ckgit-ci\ckgit-windows-vm-guest.exe" --service',[Microsoft.Win32.RegistryValueKind]::ExpandString);$service.SetValue('ObjectName','LocalSystem');$service.Flush()} finally {$service.Dispose()}
   } else {
    foreach($path in @('Policies\Microsoft\VisualStudio\SQM','Microsoft\VSCommon\17.0\SQM','Wow6432Node\Microsoft\VSCommon\17.0\SQM')){$policy=$key.CreateSubKey($path);try{$policy.SetValue('OptIn',0,[Microsoft.Win32.RegistryValueKind]::DWord);$policy.Flush()}finally{$policy.Dispose()}}
   }
  } finally {$key.Dispose()}
  & "$env:SystemRoot\System32\reg.exe" unload ("HKLM\"+$name) | Out-Null
  if($LASTEXITCODE){throw 'Cannot unload the offline guest registry hive'}
  $loaded=@($loaded | Where-Object {$_ -ne $name})
 }
} finally {
 foreach($name in $loaded){& "$env:SystemRoot\System32\reg.exe" unload ("HKLM\"+$name) | Out-Null}
 if($mounted){Dismount-VHD -Path $os -ErrorAction Stop}
}
@{instruction='Accept the Windows license and initialize this VM';license_sha256=$actualLicense;accepted_utc=[DateTime]::UtcNow.ToString('o')} | ConvertTo-Json | Set-Content -LiteralPath $accept -Encoding UTF8
@{guest_sha256=(Get-FileHash -LiteralPath $guest).Hash.ToLowerInvariant();os_disk=$os} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $Destination 'guest-installed.json') -Encoding UTF8
New-VHD -Path $work -Dynamic -SizeBytes ([long]$WorkDiskGiB * 1GB) -BlockSizeBytes 2MB | Out-Null
$mounted=$false
try {$disk=Mount-VHD -Path $work -NoDriveLetter -PassThru|Get-Disk; $mounted=$true; $disk|Initialize-Disk -PartitionStyle GPT|Out-Null; $part=New-Partition -DiskNumber $disk.Number -UseMaximumSize; $part|Format-Volume -FileSystem NTFS -NewFileSystemLabel CKGIT_WORK -Confirm:$false|Out-Null}
finally {if($mounted){Dismount-VHD -Path $work -ErrorAction Stop}}
# Keep the selected SDK bytes stable during first boot and profile creation.
$sdkLock=[IO.File]::Open($SdkDisk,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
try {
 $sdkHash=(Get-FileHash -LiteralPath $SdkDisk -Algorithm SHA256).Hash.ToLowerInvariant()
 & $hostCheck --initialize-base $os $work $SdkDisk $accept
 if($LASTEXITCODE){throw 'First boot did not complete; the image has not been admitted'}
 if((Get-FileHash -LiteralPath $SdkDisk -Algorithm SHA256).Hash.ToLowerInvariant() -cne $sdkHash){throw 'SDK disk changed during first boot'}
} finally {$sdkLock.Dispose()}
$lines=@('schema_version=1',('os_disk='+$os),('os_sha256='+(Get-FileHash -LiteralPath $os).Hash.ToLowerInvariant()),('work_disk='+$work),('work_sha256='+(Get-FileHash -LiteralPath $work).Hash.ToLowerInvariant()),('sdk_disk='+[IO.Path]::GetFullPath($SdkDisk)),('sdk_sha256='+$sdkHash),('memory_mb='+$MemoryMB),('processors='+$Processors))
$pending=Join-Path $Destination 'images.pending.ini'
if(Test-Path -LiteralPath $pending){throw 'A prior unfinished profile exists; inspect the destination'}
[IO.File]::WriteAllLines($pending,$lines,(New-Object Text.UTF8Encoding($false)))
$verify=Join-Path $Destination 'storage-check'; New-Item -ItemType Directory -Path $verify | Out-Null
try {
 & $hostCheck --storage-check $pending $verify
 if($LASTEXITCODE){throw 'Storage verification failed; do not enable the runner'}
 Move-Item -LiteralPath $pending -Destination $profile
} finally {
 Remove-Item -LiteralPath $pending -Force -ErrorAction SilentlyContinue
 Remove-Item -LiteralPath $verify -Force -ErrorAction SilentlyContinue
}
Write-Output ('HCS image profile ready: '+$profile)
Write-Output 'Configure ci_windows_backend=hyperv-hcs and ci_windows_vm_profile on the runner; register its matching label on the main host.'
