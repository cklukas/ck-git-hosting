# Copyright (c) 2026 C. Klukas. All rights reserved.
# SPDX-License-Identifier: MIT
# Trusted operator preparation only. No installer, network or guest execution.
[CmdletBinding()]
param(
 [Parameter(Mandatory=$true)][string]$Source,
 [Parameter(Mandatory=$true)][ValidatePattern('^[a-f0-9]{64}$')][string]$InventorySha256,
 [Parameter(Mandatory=$true)][string]$Destination,
 [Parameter(Mandatory=$true)][string]$Scratch,
 [string]$GuestSettings
)
$ErrorActionPreference='Stop';$ProgressPreference='SilentlyContinue'
if(!(Test-Path V:\ -PathType Container)){throw 'Development drive V: unavailable'}
$selectedPaths=@($Source,$Destination,$Scratch)
if($GuestSettings){$selectedPaths+= $GuestSettings}
foreach($path in $selectedPaths) {
 if(![IO.Path]::IsPathRooted($path) -or [IO.Path]::GetPathRoot($path) -ne 'V:\'){throw 'Select explicit V: source, destination and scratch paths'}
 $parent=$path
 while($parent -and !(Test-Path -LiteralPath $parent)){$parent=[IO.Path]::GetDirectoryName($parent)}
 while($parent){if((Get-Item -LiteralPath $parent -Force).Attributes -band [IO.FileAttributes]::ReparsePoint){throw 'Redirected preparation path'};$parent=[IO.Path]::GetDirectoryName($parent)}
}
if(!(Test-Path -LiteralPath $Source -PathType Container) -or !(Test-Path -LiteralPath $Scratch -PathType Container)){throw 'Source and private scratch must exist'}
$guestLines=$null
if($GuestSettings){
 if(!(Test-Path -LiteralPath $GuestSettings -PathType Leaf)){throw 'Guest settings file is missing'}
 $guestLines=New-Object System.Collections.Generic.List[string]
 $guestLines.AddRange([string[]]@('schema_version=1','server=ckgit@example.invalid','name=offline-vm-guest','ci_build_root=V:/tmp/ckgit-vm-job','ci_toolchain_root=V:/tools/approved','ci_allow_network=false','ci_allow_degraded=false'))
 $seen=@{}; $hasPaths=$false
 foreach($line in [IO.File]::ReadAllLines([IO.Path]::GetFullPath($GuestSettings))){
  if(!$line -or $line.StartsWith('#')){continue}
  if($line -notmatch '^(ci_toolchain_paths|ci_toolchain_env\.[A-Z_][A-Z0-9_]*)=([^\x00-\x1f\x7f]*)$'){throw 'Guest settings may contain only ci_toolchain_paths and uppercase ci_toolchain_env assignments'}
  $key=$Matches[1]; if($seen.ContainsKey($key)){throw 'Duplicate guest toolchain setting'}; $seen[$key]=$true
  if($key -eq 'ci_toolchain_paths'){
   $hasPaths=$true; $paths=$Matches[2].Split(';')
   if($paths.Count -gt 32){throw 'Too many guest tool directories'}
   foreach($relative in $paths){
    if(!$relative -or $relative.Contains('\') -or $relative.Contains(':') -or $relative.StartsWith('/') -or @($relative.Split('/') | Where-Object {$_ -eq '..' -or $_ -eq ''}).Count){throw 'Guest tool directories must be relative slash-separated paths'}
   }
  }
  $guestLines.Add($line)
 }
 if(!$hasPaths){throw 'Guest settings must explicitly select ci_toolchain_paths'}
}
if(Test-Path -LiteralPath $Destination){throw 'Use a new destination; refusing to mix VM disk generations'}
$env:TEMP=$Scratch;$env:TMP=$Scratch;$env:TMPDIR=$Scratch
$env:VSCMD_SKIP_SENDTELEMETRY='1';$env:DOTNET_CLI_TELEMETRY_OPTOUT='1';$env:VCPKG_DISABLE_METRICS='1'
$sourceRecord=Get-Content (Join-Path $Source 'toolchain-build.json') -Raw|ConvertFrom-Json
if($sourceRecord.manifest_sha256 -ne $InventorySha256 -or (Get-FileHash (Join-Path $Source 'toolchain.sha256')).Hash.ToLowerInvariant() -ne $InventorySha256){throw 'Source toolkit inventory differs from operator-selected identity'}
New-Item -ItemType Directory -Path $Destination|Out-Null
$nonce=[guid]::NewGuid().ToString('N');$vhd=Join-Path $Destination 'toolchain.vhdx';$mount=Join-Path $Destination 'mount'
New-Item -ItemType Directory -Path $mount|Out-Null
@{schema=1;owner='ckgit-windows-vm-toolchain';nonce=$nonce;created_utc=[DateTime]::UtcNow.ToString('o')}|ConvertTo-Json|Set-Content (Join-Path $Destination 'owner.json')
$started=[DateTime]::UtcNow;$timer=[Diagnostics.Stopwatch]::StartNew();$mounted=$false;$status='failed';$failure=''
try {
 if((Get-Volume -DriveLetter V).SizeRemaining -lt 80GB){throw 'Insufficient V: space for one reviewed SDK disk plus reserve'}
 New-VHD -Path $vhd -Dynamic -SizeBytes 40GB -BlockSizeBytes 2MB|Out-Null
 $disk=Mount-VHD -Path $vhd -NoDriveLetter -PassThru|Get-Disk;$mounted=$true
 if($disk.PartitionStyle -ne 'RAW'){throw 'New SDK disk is not blank'}
 $disk|Initialize-Disk -PartitionStyle GPT|Out-Null
 $partition=New-Partition -DiskNumber $disk.Number -UseMaximumSize
 $partition|Format-Volume -FileSystem NTFS -NewFileSystemLabel 'CKGIT_SDK' -Confirm:$false|Out-Null
 $partition|Add-PartitionAccessPath -AccessPath $mount
 $approved=Join-Path $mount 'approved';New-Item -ItemType Directory $approved|Out-Null
 Add-Type -TypeDefinition @'
using System;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Collections.Generic;
using System.Collections.Concurrent;
using System.Threading;
using System.Threading.Tasks;
public static class VmSdkCopy {
 public static string Hash(string path) {using(var sha=SHA256.Create())using(var f=new FileStream(path,FileMode.Open,FileAccess.Read,FileShare.Read,1048576))return BitConverter.ToString(sha.ComputeHash(f)).Replace("-","").ToLowerInvariant();}
 public static string Copy(string source,string destination) {
  var files=new List<string>();var directories=new Stack<string>();directories.Push(source);
  while(directories.Count>0){var directory=directories.Pop();if((File.GetAttributes(directory)&FileAttributes.ReparsePoint)!=0)throw new Exception("Redirected SDK directory");
   foreach(var entry in Directory.GetFileSystemEntries(directory)){
    var attrs=File.GetAttributes(entry);if((attrs&FileAttributes.ReparsePoint)!=0)throw new Exception("Redirected SDK input");
    var relative=entry.Substring(source.TrimEnd('\\').Length+1);
    if((attrs&FileAttributes.Directory)!=0){Directory.CreateDirectory(Path.Combine(destination,relative));directories.Push(entry);}else files.Add(entry);
   }
  }
  var records=new ConcurrentBag<string>();int completed=0;
  Parallel.ForEach(files,new ParallelOptions{MaxDegreeOfParallelism=6},file=>{
   var relative=file.Substring(source.TrimEnd('\\').Length+1);var output=Path.Combine(destination,relative);
   var inputHash=Hash(file);File.Copy(file,output,false);if(Hash(output)!=inputHash)throw new Exception("SDK copy identity mismatch");
   if(relative!="toolchain.sha256" && relative!="toolchain-build.json" && relative!=".orthomap-owner.json")records.Add(inputHash+"  "+relative.Replace('\\','/'));
   int count=Interlocked.Increment(ref completed);if(count%2000==0)Console.WriteLine("Verified SDK files="+count);
  });
  // The admitted inventory is PowerShell WriteAllLines: Windows CRLF and UTF-8 without BOM.
  var lines=records.OrderBy(x=>x,StringComparer.Ordinal).ToArray();
  var inventory=Path.Combine(destination,"copied-inventory.sha256");File.WriteAllLines(inventory,lines,new System.Text.UTF8Encoding(false));
  var hash=Hash(inventory);File.Delete(inventory);return hash;
 }
}
'@
 $copied=[VmSdkCopy]::Copy([IO.Path]::GetFullPath($Source),$approved)
 if($copied -ne $InventorySha256){throw ('Copied SDK inventory differs: '+$copied)}
 if($guestLines){[IO.File]::WriteAllLines((Join-Path $mount 'guest-toolchain.ini'),$guestLines,(New-Object Text.UTF8Encoding($false)))}
 $partition|Remove-PartitionAccessPath -AccessPath $mount
 Dismount-VHD -Path $vhd;$mounted=$false
 $diskHash=(Get-FileHash $vhd -Algorithm SHA256).Hash.ToLowerInvariant()
 $status='passed'
} catch {$failure=$_.Exception.Message;throw} finally {
 if($mounted){Dismount-VHD -Path $vhd -ErrorAction Stop;$mounted=$false}
 $timer.Stop()
 @{schema=1;status=$status;scope='Reviewed offline SDK on an NTFS virtual disk; no guest boot or CI qualification';source=$Source;source_inventory_sha256=$InventorySha256;destination=$vhd;disk_sha256=$diskHash;started_utc=$started.ToString('o');finished_utc=[DateTime]::UtcNow.ToString('o');elapsed_seconds=$timer.Elapsed.TotalSeconds;script_sha256=(Get-FileHash $PSCommandPath).Hash.ToLowerInvariant();error=$failure}|ConvertTo-Json -Depth 3|Set-Content (Join-Path $Destination 'sdk-disk.json')
}
