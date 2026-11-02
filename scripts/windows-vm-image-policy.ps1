# Copyright (c) 2026 C. Klukas. All rights reserved.
# SPDX-License-Identifier: MIT
# Called only by trusted offline-image preparation, never by job source.
function Set-CkGitOfflineWindowsBuildPolicy {
 [CmdletBinding()]
 param([Parameter(Mandatory=$true)][string]$OsRoot,[Parameter(Mandatory=$true)][string]$Scratch)
 $hiveFile=Join-Path $OsRoot 'Windows\System32\config\SYSTEM'
 if(![IO.File]::Exists($hiveFile) -or !(Test-Path -LiteralPath $Scratch -PathType Container)){throw 'Offline Windows hive and private scratch required'}
 $env:TEMP=$Scratch;$env:TMP=$Scratch;$env:TMPDIR=$Scratch
 $env:VSCMD_SKIP_SENDTELEMETRY='1';$env:DOTNET_CLI_TELEMETRY_OPTOUT='1';$env:VCPKG_DISABLE_METRICS='1'
 function Invoke-ImagePolicyReg([string]$arguments) {
  $info=New-Object Diagnostics.ProcessStartInfo;$info.FileName='C:\Windows\System32\reg.exe';$info.Arguments=$arguments
  $info.UseShellExecute=$false;$info.CreateNoWindow=$true;$info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true;$info.WorkingDirectory=$Scratch
  $p=New-Object Diagnostics.Process;$p.StartInfo=$info
  try {if(!$p.Start()){throw 'Cannot start offline registry operation'};$stdout=$p.StandardOutput.ReadToEndAsync();$stderr=$p.StandardError.ReadToEndAsync();if(!$p.WaitForExit(30000)){$p.Kill();$p.WaitForExit();throw 'Offline registry operation timed out'};if($p.ExitCode){throw ('Offline registry operation failed: '+$stderr.Result.Trim())}}finally{$p.Dispose()}
 }
 $name='CKGitImagePolicy-'+[guid]::NewGuid().ToString('N');$loaded=$false;$changes=@()
 try {
  Invoke-ImagePolicyReg ('load "HKLM\'+$name+'" "'+$hiveFile+'"');$loaded=$true
  $key=[Microsoft.Win32.Registry]::LocalMachine.OpenSubKey($name,$true)
  try {
   $sets=@($key.GetSubKeyNames()|Where-Object {$_ -cmatch '^ControlSet[0-9]{3}$'});if(!$sets.Count){throw 'Offline Windows has no control set'}
   foreach($set in $sets){$fileSystem=$key.CreateSubKey($set+'\Control\FileSystem');try{$before=$fileSystem.GetValue('LongPathsEnabled',$null);$fileSystem.SetValue('LongPathsEnabled',1,[Microsoft.Win32.RegistryValueKind]::DWord);if($fileSystem.GetValue('LongPathsEnabled') -ne 1){throw 'Long-path image policy did not persist'};$changes+=@{control_set=$set;previous=$before;LongPathsEnabled=1}}finally{$fileSystem.Dispose()}}
  }finally{$key.Dispose()}
 }finally{if($loaded){Invoke-ImagePolicyReg ('unload "HKLM\'+$name+'"')}}
 return @{scope='Offline private CI image only; longPathAware applications still required';changes=$changes;host_registry_modified=$false}
}
