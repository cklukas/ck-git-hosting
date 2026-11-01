# Copyright (c) 2026 C. Klukas. All rights reserved.
# SPDX-License-Identifier: MIT
# Administrative opt-in test. Supply a dedicated fixture installer AppId;
# never use the production installer or an existing installation.
param(
 [Parameter(Mandatory=$true)][string]$Installer,
 [Parameter(Mandatory=$true)][string]$InstallerSha256,
 [Parameter(Mandatory=$true)][string]$FakeSsh,
 [Parameter(Mandatory=$true)][string]$TestRoot,
 [Parameter(Mandatory=$true)][ValidatePattern('^\{[0-9A-Fa-f-]{36}\}$')][string]$TestAppId,
 [switch]$ExerciseRestartFailure,
 [switch]$ExerciseMultipleServices
)
$ErrorActionPreference='Stop'
$setup=(Resolve-Path -LiteralPath $Installer).Path
$FakeSsh=(Resolve-Path -LiteralPath $FakeSsh).Path
$TestRoot=(Resolve-Path -LiteralPath $TestRoot).Path
if(!(New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)){throw 'Administrator rights required'}
if((Get-FileHash $setup).Hash -ne $InstallerSha256){throw 'Installer hash mismatch'}
if(Test-Path "HKLM:\Software\Microsoft\Windows\CurrentVersion\Uninstall\${TestAppId}_is1"){throw 'Fixture AppId is already installed'}
$id=[guid]::NewGuid().ToString('N');$name='ckgit-a-'+$id;$foreignName='ckgit-z-'+$id
$root=Join-Path $TestRoot ('service-upgrade-'+$id);$app=Join-Path $root 'installed'
New-Item -ItemType Directory $root | Out-Null
$oldTemp=$env:TEMP;$oldTmp=$env:TMP;$oldTmpdir=$env:TMPDIR
$env:TEMP=$root;$env:TMP=$root;$env:TMPDIR=$root
try {
$r=@{success=$false;start=[DateTime]::UtcNow.ToString('o');installer_sha256=(Get-FileHash $setup).Hash}
try {
 $p=Start-Process -FilePath $setup -ArgumentList @('/ALLUSERS','/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART',('/DIR="'+$app+'"'),'/TASKS=') -PassThru -Wait
 if($p.ExitCode){throw "Initial install failed $($p.ExitCode)"}
 New-Item -ItemType Directory "$root\work","$root\tools\bin","$root\logs","$root\fake" -Force | Out-Null
 Copy-Item $FakeSsh "$root\fake\ssh.exe"
 [IO.File]::WriteAllText("$root\runner.ini","schema_version=1`nserver=runner@example.test`nname=fixture`nci_build_root=$root\work`nci_toolchain_root=$root\tools`nssh_program=$root\fake\ssh.exe`n",(New-Object Text.UTF8Encoding($false)))
 & "$app\register-service.ps1" -Runner "$app\ck-ci-runnerd.exe" -Config "$root\runner.ini" -Log "$root\logs\service.log" -Name $name
 $sid=(New-Object Security.Principal.NTAccount("NT SERVICE\$name")).Translate([Security.Principal.SecurityIdentifier])
 $acl=Get-Acl $root;$acl.AddAccessRule((New-Object Security.AccessControl.FileSystemAccessRule($sid,'ReadAndExecute','ContainerInherit,ObjectInherit','None','Allow')));Set-Acl $root $acl
 foreach($dir in @('work','logs')){$acl=Get-Acl "$root\$dir";$acl.AddAccessRule((New-Object Security.AccessControl.FileSystemAccessRule($sid,'Modify','ContainerInherit,ObjectInherit','None','Allow')));Set-Acl "$root\$dir" $acl}
 Start-Service $name;(Get-Service $name).WaitForStatus('Running',[TimeSpan]::FromSeconds(40))
 $s=Get-CimInstance Win32_Service -Filter "Name='$name'";if(!$s.ProcessId){throw 'Service has no process'}
 $r.service_before=$s.ProcessId
 $other=$foreignName;$otherDir=if($ExerciseMultipleServices){$app}else{"$root\foreign"}
 if(!$ExerciseMultipleServices){
  New-Item -ItemType Directory $otherDir -Force | Out-Null
  Copy-Item "$app\ck-ci-runnerd.exe","$app\ckgit-job-launcher.exe" $otherDir
 }
 New-Item -ItemType Directory "$root\otherwork" -Force | Out-Null
 [IO.File]::WriteAllText("$root\other.ini",([IO.File]::ReadAllText("$root\runner.ini")).Replace("$root\work","$root\otherwork"),(New-Object Text.UTF8Encoding($false)))
 & "$app\register-service.ps1" -Runner "$otherDir\ck-ci-runnerd.exe" -Config "$root\other.ini" -Log "$root\logs\other.log" -Name $other
 $otherSid=(New-Object Security.Principal.NTAccount("NT SERVICE\$other")).Translate([Security.Principal.SecurityIdentifier])
 $acl=Get-Acl $root;$acl.AddAccessRule((New-Object Security.AccessControl.FileSystemAccessRule($otherSid,'ReadAndExecute','ContainerInherit,ObjectInherit','None','Allow')));Set-Acl $root $acl
 foreach($dir in @('otherwork','logs')){$acl=Get-Acl "$root\$dir";$acl.AddAccessRule((New-Object Security.AccessControl.FileSystemAccessRule($otherSid,'Modify','ContainerInherit,ObjectInherit','None','Allow')));Set-Acl "$root\$dir" $acl}
 Start-Service $other;(Get-Service $other).WaitForStatus('Running',[TimeSpan]::FromSeconds(40))
 $otherBefore=Get-CimInstance Win32_Service -Filter "Name='$other'";$r.foreign_pid_before=$otherBefore.ProcessId
 if($ExerciseRestartFailure){
  $watcher=Start-Job -ArgumentList $name -ScriptBlock {
   param($target)
   Write-Output 'ready'
   for($i=0;$i -lt 6000;$i++){
    $svc=Get-Service -Name $target -ErrorAction Stop
    if($svc.Status -eq 'Stopped'){Set-Service -Name $target -StartupType Disabled;return 'disabled'}
    Start-Sleep -Milliseconds 10
   }
   throw 'Service did not stop during upgrade'
  }
  $watcherReady=$false
  for($i=0;$i -lt 200;$i++){
   if(@(Receive-Job -Job $watcher -Keep) -contains 'ready'){$watcherReady=$true;break}
   if($watcher.State -in @('Failed','Completed','Stopped')){throw 'Restart-failure watcher exited early'}
   Start-Sleep -Milliseconds 50
  }
  if(!$watcherReady){throw 'Restart-failure watcher was not ready'}
 }
 $up=Start-Process -FilePath $setup -ArgumentList @('/ALLUSERS','/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART',('/DIR="'+$app+'"'),'/TASKS=',('/LOG="'+$root+'\upgrade.log"')) -PassThru
 if(!$up.WaitForExit(90000)){$up.Kill();$up.WaitForExit();throw 'Upgrade exceeded 90 seconds'}
 $r.upgrade_exit=$up.ExitCode
 if($ExerciseRestartFailure){
  $marker=Receive-Job -Job $watcher -Wait -AutoRemoveJob -ErrorAction Stop;$watcher=$null
  if(@($marker) -notcontains 'disabled' -or $up.ExitCode -ne 20){throw 'Expected a reported service restart failure'}
 } else {
  if($up.ExitCode -ne 0){throw 'Upgrade failed'}
  (Get-Service $name).WaitForStatus('Running',[TimeSpan]::FromSeconds(40))
 }
 $s=Get-CimInstance Win32_Service -Filter "Name='$name'";$r.service_after=$s.State;$r.pid_after=$s.ProcessId
 $otherAfter=Get-CimInstance Win32_Service -Filter "Name='$other'"
 if($ExerciseMultipleServices){
  (Get-Service $other).WaitForStatus('Running',[TimeSpan]::FromSeconds(40))
  $otherAfter=Get-CimInstance Win32_Service -Filter "Name='$other'"
 }
 $r.foreign_pid_after=$otherAfter.ProcessId;$r.foreign_state=$otherAfter.State
 if($otherAfter.State -ne 'Running' -or
    ($ExerciseMultipleServices -and $otherAfter.ProcessId -eq $otherBefore.ProcessId) -or
    (!$ExerciseMultipleServices -and $otherAfter.ProcessId -ne $otherBefore.ProcessId)){throw 'Second runner service had an unexpected lifecycle'}
 $r.runner_sha256=(Get-FileHash "$app\ck-ci-runnerd.exe").Hash
 $r.success=if($ExerciseRestartFailure){$up.ExitCode -eq 20 -and $r.service_after -eq 'Stopped'}else{$up.ExitCode -eq 0 -and $r.service_after -eq 'Running'}
} catch {$r.error=$_.ToString();$r.position=$_.InvocationInfo.PositionMessage}
finally {
 if($watcher){Stop-Job $watcher -ErrorAction SilentlyContinue;Remove-Job $watcher -Force -ErrorAction SilentlyContinue}
 $foreign=Get-CimInstance Win32_Service -Filter "Name='$foreignName'"
 if($foreign){if($foreign.PathName -notlike "*$otherDir*"){throw 'Unexpected second fixture path'};if($foreign.State -ne 'Stopped'){Stop-Service $foreign.Name;(Get-Service $foreign.Name).WaitForStatus('Stopped',[TimeSpan]::FromSeconds(20))};& sc.exe delete $foreign.Name | Out-Null}
 $s=Get-CimInstance Win32_Service -Filter "Name='$name'"
 if($s){if($s.PathName -notlike "*$app*"){throw 'Unexpected service path'};if($s.State -ne 'Stopped'){Stop-Service $name;(Get-Service $name).WaitForStatus('Stopped',[TimeSpan]::FromSeconds(20))};& sc.exe delete $name | Out-Null}
 if(Test-Path "$app\unins000.exe"){$p=Start-Process -FilePath "$app\unins000.exe" -ArgumentList @('/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART') -PassThru -Wait;$r.uninstall_exit=$p.ExitCode}
 $r.end=[DateTime]::UtcNow.ToString('o');$r | ConvertTo-Json | Set-Content -Encoding UTF8 "$root\result.json"
}
Get-Content "$root\result.json"
if(!$r.success -or $r.uninstall_exit -ne 0){throw "Service upgrade qualification failed; evidence retained at $root"}
Copy-Item "$root\result.json" (Join-Path $TestRoot ("service-upgrade-$id.json"))
if(Get-CimInstance Win32_Process | Where-Object {$_.ExecutablePath -like "$root\*"}){throw 'Fixture process survived cleanup'}
Remove-Item -LiteralPath $root -Recurse -Force

} finally { $env:TEMP=$oldTemp;$env:TMP=$oldTmp;$env:TMPDIR=$oldTmpdir }
