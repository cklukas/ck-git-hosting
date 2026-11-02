# Copyright (c) 2026 C. Klukas. All rights reserved.
# SPDX-License-Identifier: MIT
# Trusted operator image expansion only. Does not boot a VM, accept terms,
# add accounts, copy credentials or run project code.
[CmdletBinding()]
param(
 [Parameter(Mandatory=$true)][string]$Iso,
 [Parameter(Mandatory=$true)][ValidatePattern('^[a-f0-9]{64}$')][string]$IsoSha256,
 [Parameter(Mandatory=$true)][string]$Destination,
 [Parameter(Mandatory=$true)][string]$Scratch
)
$ErrorActionPreference='Stop';$ProgressPreference='SilentlyContinue'
if(!(Test-Path V:\ -PathType Container)){throw 'Development drive V: unavailable'}
foreach($path in @($Iso,$Destination,$Scratch)){
 if(![IO.Path]::IsPathRooted($path) -or [IO.Path]::GetPathRoot($path) -ne 'V:\'){throw 'Select explicit V: image and scratch locations'}
 $parent=$path;while($parent -and !(Test-Path -LiteralPath $parent)){$parent=[IO.Path]::GetDirectoryName($parent)}
 while($parent){if((Get-Item -LiteralPath $parent -Force).Attributes -band [IO.FileAttributes]::ReparsePoint){throw 'Redirected image preparation path'};$parent=[IO.Path]::GetDirectoryName($parent)}
}
if(!(Test-Path $Iso -PathType Leaf) -or !(Test-Path $Scratch -PathType Container) -or (Test-Path $Destination)){throw 'Require existing ISO/private scratch and a new destination'}
$env:TEMP=$Scratch;$env:TMP=$Scratch;$env:TMPDIR=$Scratch;$env:VSCMD_SKIP_SENDTELEMETRY='1';$env:DOTNET_CLI_TELEMETRY_OPTOUT='1'
Get-Command Expand-WindowsImage,New-VHD,Mount-VHD,Format-Volume -ErrorAction Stop|Out-Null
New-Item -ItemType Directory $Destination|Out-Null
$started=[DateTime]::UtcNow;$scriptHash=(Get-FileHash $PSCommandPath).Hash.ToLowerInvariant();$diskMounted=$false;$isoMounted=$false;$status='failed';$failure=''
$vhd=Join-Path $Destination 'os.vhdx';$os=(Join-Path $Destination 'windows')+'\';$esp=(Join-Path $Destination 'efi')+'\';New-Item -ItemType Directory $os,$esp|Out-Null
@{schema=1;owner='ckgit-windows-vm-base';nonce=[guid]::NewGuid().ToString('N');created_utc=$started.ToString('o')}|ConvertTo-Json|Set-Content (Join-Path $Destination 'owner.json')
# Keep the verified ISO bytes stable throughout expansion.
$isoLock=[IO.File]::Open($Iso,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
try {
 $sha=[Security.Cryptography.SHA256]::Create();try{$actual=[BitConverter]::ToString($sha.ComputeHash($isoLock)).Replace('-','').ToLowerInvariant()}finally{$sha.Dispose()}
 if($actual -ne $IsoSha256){throw 'Windows ISO differs from selected Microsoft image identity'}
 if((Get-Volume -DriveLetter V).SizeRemaining -lt 160GB){throw 'Insufficient V: space for base image and reserve'}
 $already=(Get-DiskImage -ImagePath $Iso).Attached
 if(!$already){Mount-DiskImage -ImagePath $Iso -NoDriveLetter -Access ReadOnly|Out-Null;$isoMounted=$true}
 $volume=Get-DiskImage -ImagePath $Iso|Get-Volume;$root=$volume.Path;if(!$root){$root=$volume.UniqueId}
 $wim=Join-Path $root 'sources\install.wim';$image=Get-WindowsImage -ImagePath $wim -Index 1 -ScratchDirectory $Scratch -LogPath (Join-Path $Destination 'inspect.log')
 if($image.Architecture -ne 9 -or $image.EditionId -ne 'EnterpriseEval'){throw 'Expected inspected Windows x64 Enterprise Evaluation image index1'}
 New-VHD -Path $vhd -Dynamic -SizeBytes 80GB -BlockSizeBytes 2MB|Out-Null
 $disk=Mount-VHD -Path $vhd -NoDriveLetter -PassThru|Get-Disk;$diskMounted=$true
 if($disk.PartitionStyle -ne 'RAW'){throw 'New base disk is not blank'}
 $disk|Initialize-Disk -PartitionStyle GPT|Out-Null
 $efi=New-Partition -DiskNumber $disk.Number -Size 260MB -GptType '{c12a7328-f81f-11d2-ba4b-00a0c93ec93b}'
 $efi|Format-Volume -FileSystem FAT32 -NewFileSystemLabel 'CKGIT_EFI' -Confirm:$false|Out-Null
 $efi|Add-PartitionAccessPath -AccessPath $esp
 New-Partition -DiskNumber $disk.Number -Size 16MB -GptType '{e3c9e316-0b5c-4db8-817d-f92df00215ae}'|Out-Null
 $partition=New-Partition -DiskNumber $disk.Number -UseMaximumSize
 $partition|Format-Volume -FileSystem NTFS -NewFileSystemLabel 'CKGIT_OS' -Confirm:$false|Out-Null
 $partition|Add-PartitionAccessPath -AccessPath $os
 Expand-WindowsImage -ImagePath $wim -Index 1 -ApplyPath $os -CheckIntegrity -ScratchDirectory $Scratch -LogPath (Join-Path $Destination 'apply.log')|Out-Null
 # Explicit /s points only to the new disk's EFI volume and prevents host
 # NVRAM registration. Failure must not fall back to a host system partition.
 $info=New-Object Diagnostics.ProcessStartInfo
 $info.FileName='C:\Windows\System32\bcdboot.exe';$info.Arguments='"'+(Join-Path $os 'Windows')+'" /s "'+$esp.TrimEnd('\')+'" /f UEFI /v'
 $info.UseShellExecute=$false;$info.CreateNoWindow=$true;$info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true;$info.WorkingDirectory=$Destination
 $process=New-Object Diagnostics.Process;$process.StartInfo=$info
 try {
  if(!$process.Start()){throw 'Cannot start offline boot file creation'}
  $stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync()
  if(!$process.WaitForExit(120000)){$process.Kill();$process.WaitForExit();throw 'Offline boot file creation timed out'}
  $stdout.Result|Set-Content (Join-Path $Destination 'boot-files.stdout.log');$stderr.Result|Set-Content (Join-Path $Destination 'boot-files.stderr.log')
  if($process.ExitCode -ne 0 -or !(Test-Path (Join-Path $esp 'EFI\Microsoft\Boot\BCD')) -or !(Test-Path (Join-Path $esp 'EFI\Boot\bootx64.efi'))){throw 'Offline boot file creation failed; no fallback permitted'}
 } finally {$process.Dispose()}
 $license=Join-Path $os 'Windows\System32\en-US\Licenses\Eval\EnterpriseEval\license.rtf'
 if(!(Test-Path $license -PathType Leaf)){throw 'Expected evaluation license absent from selected image'}
 if(Test-Path $license){Copy-Item $license (Join-Path $Destination 'windows-evaluation-license.rtf');$licenseHash=(Get-FileHash $license).Hash.ToLowerInvariant()}
 $efi|Remove-PartitionAccessPath -AccessPath $esp
 $partition|Remove-PartitionAccessPath -AccessPath $os
 Dismount-VHD -Path $vhd;$diskMounted=$false
 $status='expanded-unbooted-awaiting-license-and-guest-agent'
} catch {$failure=$_.Exception.Message;throw} finally {
 if($diskMounted){foreach($part in @($efi,$partition)){if($null -ne $part){foreach($path in @($esp,$os)){try{$part|Remove-PartitionAccessPath -AccessPath $path -ErrorAction Stop}catch{}}}};Dismount-VHD -Path $vhd -ErrorAction Stop}
 if($isoMounted){Dismount-DiskImage -ImagePath $Iso -ErrorAction Stop|Out-Null}
 $isoLock.Dispose();$finished=[DateTime]::UtcNow
 @{schema=1;status=$status;scope='Offline OS expansion only; no VM boot or license acceptance; not an admitted CI base';source_iso=$Iso;iso_sha256=$IsoSha256;image_index=1;edition='EnterpriseEval';os_disk=$vhd;license_sha256=$licenseHash;started_utc=$started.ToString('o');finished_utc=$finished.ToString('o');elapsed_seconds=($finished-$started).TotalSeconds;script_sha256=$scriptHash;error=$failure}|ConvertTo-Json -Depth 3|Set-Content (Join-Path $Destination 'base-expansion.json')
}
