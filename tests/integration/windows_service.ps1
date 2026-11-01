# Copyright (c) 2026 C. Klukas. All rights reserved.
# SPDX-License-Identifier: MIT
# Opt-in, administrator only: registers a temporary virtual-account service.
# Uses a fake SSH endpoint; no production configuration or credentials.
param(
 [Parameter(Mandatory=$true)][string]$Runner,
 [Parameter(Mandatory=$true)][string]$FakeSsh,
 [Parameter(Mandatory=$true)][string]$TestRoot
)
$ErrorActionPreference='Stop'
$Runner=(Resolve-Path -LiteralPath $Runner).Path
$FakeSsh=(Resolve-Path -LiteralPath $FakeSsh).Path
$TestRoot=(Resolve-Path -LiteralPath $TestRoot).Path
$admin=(New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if(!$admin){throw 'Service qualification requires administrator rights'}
$name='ckgit-service-test-'+[Guid]::NewGuid().ToString('N')
$root=Join-Path $TestRoot $name
$acl=New-Object Security.AccessControl.DirectorySecurity
$acl.SetAccessRuleProtection($true,$false)
foreach($id in @('S-1-5-18','S-1-5-32-544')) {
 $acl.AddAccessRule((New-Object Security.AccessControl.FileSystemAccessRule((New-Object Security.Principal.SecurityIdentifier($id)),'FullControl','ContainerInherit,ObjectInherit','None','Allow')))
}
[IO.Directory]::CreateDirectory($root,$acl) | Out-Null
New-Item -ItemType Directory "$root\tmp" | Out-Null
$oldTemp=$env:TEMP;$oldTmp=$env:TMP;$oldTmpdir=$env:TMPDIR
$env:TEMP="$root\tmp";$env:TMP=$env:TEMP;$env:TMPDIR=$env:TEMP
function Read-SharedLog([string]$Path){
 $file=[IO.File]::Open($Path,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::ReadWrite)
 $reader=New-Object IO.StreamReader($file)
 try {return $reader.ReadToEnd()} finally {$reader.Dispose()}
}
try {
if (!('CkgitServiceFixture' -as [type])) { Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class CkgitServiceFixture {
 [DllImport("advapi32.dll",CharSet=CharSet.Unicode,SetLastError=true)] static extern IntPtr OpenSCManager(string machine,string database,uint access);
 [DllImport("advapi32.dll",CharSet=CharSet.Unicode,SetLastError=true)] static extern IntPtr CreateService(IntPtr manager,string name,string display,uint access,uint type,uint start,uint error,string binary,string group,IntPtr tag,string dependencies,string account,string password);
 [DllImport("advapi32.dll",SetLastError=true)] static extern bool DeleteService(IntPtr service);
 [DllImport("advapi32.dll")] static extern bool CloseServiceHandle(IntPtr handle);
 public static IntPtr Create(string name,string command) {
  IntPtr manager=OpenSCManager(null,null,2);
  if(manager==IntPtr.Zero)throw new System.ComponentModel.Win32Exception();
  try {
   IntPtr service=CreateService(manager,name,name,0xF01FF,0x10,3,1,command,null,IntPtr.Zero,null,"NT SERVICE\\"+name,null);
   if(service==IntPtr.Zero)throw new System.ComponentModel.Win32Exception();
   return service;
  } finally {CloseServiceHandle(manager);}
 }
 public static void Delete(IntPtr handle) {try {if(!DeleteService(handle))throw new System.ComponentModel.Win32Exception();}finally{CloseServiceHandle(handle);}}
}
'@
}
New-Item -ItemType Directory "$root\bin","$root\work","$root\logs","$root\tools\bin" | Out-Null
Copy-Item $Runner "$root\bin\ck-ci-runnerd.exe"
Copy-Item (Join-Path (Split-Path $Runner) 'ckgit-job-launcher.exe') "$root\bin\ckgit-job-launcher.exe"
Copy-Item $FakeSsh "$root\bin\ssh-fixture.exe"
@"
schema_version=1
server=runner@example.test
name=service-test
ci_build_root=$root\work
ssh_program=$root\bin\ssh-fixture.exe
ci_toolchain_root=$root\tools
ci_toolchain_paths=bin
"@ | Set-Content -Encoding ASCII "$root\runner.ini"
$handle=[IntPtr]::Zero
$r=@{success=$false;start=[DateTime]::UtcNow.ToString('o');runner_sha256=(Get-FileHash "$root\bin\ck-ci-runnerd.exe").Hash}
try {
 $command='"'+$root+'\bin\ck-ci-runnerd.exe" service --config "'+$root+'\runner.ini" --log "'+$root+'\logs\service.log" --service-name '+$name
 $handle=[CkgitServiceFixture]::Create($name,$command)
 $sid=(New-Object Security.Principal.NTAccount("NT SERVICE\$name")).Translate([Security.Principal.SecurityIdentifier])
 $acl=New-Object Security.AccessControl.DirectorySecurity
 $acl.SetAccessRuleProtection($true,$false)
 foreach($id in @('S-1-5-18','S-1-5-32-544')){
  $acl.AddAccessRule((New-Object Security.AccessControl.FileSystemAccessRule((New-Object Security.Principal.SecurityIdentifier($id)),'FullControl','ContainerInherit,ObjectInherit','None','Allow')))
 }
 $acl.AddAccessRule((New-Object Security.AccessControl.FileSystemAccessRule($sid,'ReadAndExecute','ContainerInherit,ObjectInherit','None','Allow')))
 Set-Acl -LiteralPath $root -AclObject $acl
 foreach($dir in @('work','logs')){
  $a=Get-Acl "$root\$dir"
  $a.AddAccessRule((New-Object Security.AccessControl.FileSystemAccessRule($sid,'Modify','ContainerInherit,ObjectInherit','None','Allow')))
  Set-Acl "$root\$dir" $a
 }
 Start-Service -Name $name
 $svc=Get-Service -Name $name
 $svc.WaitForStatus('Running',[TimeSpan]::FromSeconds(30))
 $proc=Get-CimInstance Win32_Service -Filter "Name='$name'"
 $r.account=$proc.StartName;$r.pid=$proc.ProcessId
 if($proc.StartName -ne "NT SERVICE\$name"){throw 'Unexpected service identity'}
 $running=Get-CimInstance Win32_Process -Filter "ProcessId=$($proc.ProcessId)"
 if($running.ExecutablePath -ne "$root\bin\ck-ci-runnerd.exe"){throw 'Unexpected service executable'}
 for($i=0;$i -lt 100;$i++){
  if((Test-Path "$root\logs\service.log") -and (Read-SharedLog "$root\logs\service.log").Contains('Connected to')){break}
  Start-Sleep -Milliseconds 100
 }
 if(!(Read-SharedLog "$root\logs\service.log").Contains('Connected to')){throw 'Service never negotiated the fixture protocol'}
 $hostLog=Read-SharedLog "$root\logs\service.log.host.log"
 if($hostLog -notmatch 'Service worker pid=(\d+)'){throw 'Worker PID missing'}
 $workerId=[int]$Matches[1]
 $worker=Get-CimInstance Win32_Process -Filter "ProcessId=$workerId"
 if(!$worker -or $worker.ExecutablePath -ne "$root\bin\ck-ci-runnerd.exe" -or $worker.CommandLine -notmatch 'service-worker'){throw 'Persistent worker is not running'}
 $owner=Invoke-CimMethod -InputObject $worker -MethodName GetOwnerSid
 if($owner.ReturnValue -ne 0 -or $owner.Sid -ne $sid.Value){throw 'Unexpected worker identity'}
 $r.worker_pid=$workerId;$r.worker_sid=$owner.Sid
 $stop=[DateTime]::UtcNow
 Stop-Service -Name $name
 $svc.WaitForStatus('Stopped',[TimeSpan]::FromSeconds(10))
 $r.stop_seconds=([DateTime]::UtcNow-$stop).TotalSeconds
 if(Get-Process -Id $proc.ProcessId -ErrorAction SilentlyContinue){throw 'Service process survived stop'}
 if(Get-ChildItem "$root\work" -Directory -Filter 'runner-session-*'){throw 'Service session workspace survived stop'}
 $r.success=$true
} catch {$r.error=$_.ToString();$r.position=$_.InvocationInfo.PositionMessage}
finally {
 if($handle -ne [IntPtr]::Zero){
  $svc=Get-Service -Name $name -ErrorAction SilentlyContinue
  if($svc -and $svc.Status -ne 'Stopped'){Stop-Service -Name $name -ErrorAction Continue;$svc.WaitForStatus('Stopped',[TimeSpan]::FromSeconds(15))}
  if($svc){$svc.Dispose()}
  [CkgitServiceFixture]::Delete($handle)
 }
 $r.end=[DateTime]::UtcNow.ToString('o')
 $r | ConvertTo-Json | Set-Content -Encoding UTF8 (Join-Path $TestRoot ($name+'.json'))
}
Get-Content (Join-Path $TestRoot ($name+'.json')) -Raw
if(Test-Path "$root\logs\service.log"){Get-Content "$root\logs\service.log" -Tail 15}
if(Get-CimInstance Win32_Process | Where-Object {$_.ExecutablePath -like "$root\*"}){throw 'Service fixture process survived cleanup'}
if(!$r.success){throw "Service qualification failed; evidence retained at $root"}
Remove-Item -LiteralPath $root -Recurse -Force

} finally {
 $env:TEMP=$oldTemp;$env:TMP=$oldTmp;$env:TMPDIR=$oldTmpdir
}
