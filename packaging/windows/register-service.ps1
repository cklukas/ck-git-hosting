# Copyright (c) 2026 C. Klukas. All rights reserved.
# SPDX-License-Identifier: MIT
# Register only; the administrator must grant the service account access to
# the configured inputs and writable work/log directories before starting it.
[CmdletBinding()]
param(
  [Parameter(Mandatory=$true)][string]$Runner,
  [Parameter(Mandatory=$true)][string]$Config,
  [Parameter(Mandatory=$true)][string]$Log,
  [string]$Name = 'ckgit-runner'
)
$ErrorActionPreference = 'Stop'
$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (!$principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Administrator rights are required' }
if ($Name -notmatch '^[A-Za-z0-9][A-Za-z0-9_-]{0,79}$') { throw 'Invalid service name' }
foreach ($path in @($Runner, $Config, $Log)) {
  if (($path -notmatch '^[A-Za-z]:[\\/]' -and $path -notmatch '^\\\\[^\\]+\\[^\\]+') -or $path.Contains('"')) { throw 'Paths must be absolute and cannot contain quotes' }
}
$Runner = (Resolve-Path -LiteralPath $Runner).Path
$Config = (Resolve-Path -LiteralPath $Config).Path
$Log = [IO.Path]::GetFullPath($Log)
if (!(Test-Path -LiteralPath (Split-Path -Parent $Log) -PathType Container)) { throw 'Log directory must exist' }
if (!(Test-Path -LiteralPath (Join-Path (Split-Path -Parent $Runner) 'ckgit-job-launcher.exe') -PathType Leaf)) { throw 'Runner launcher is missing' }
if (Get-Service -Name $Name -ErrorAction SilentlyContinue) { throw 'A service with this name already exists' }
$binary = '"' + $Runner + '" service --config "' + $Config + '" --log "' + $Log + '" --service-name ' + $Name
if (!('CkgitServiceRegistration' -as [type])) {
  Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class CkgitServiceRegistration {
 [DllImport("advapi32.dll", CharSet=CharSet.Unicode, SetLastError=true)] static extern IntPtr OpenSCManager(string machine, string database, uint access);
 [DllImport("advapi32.dll", CharSet=CharSet.Unicode, SetLastError=true)] static extern IntPtr CreateService(IntPtr manager, string name, string display, uint access, uint type, uint start, uint error, string binary, string group, IntPtr tag, string dependencies, string account, string password);
 [DllImport("advapi32.dll")] static extern bool CloseServiceHandle(IntPtr handle);
 public static void Create(string name, string binary) {
  IntPtr manager=OpenSCManager(null,null,2);
  if(manager==IntPtr.Zero) throw new System.ComponentModel.Win32Exception();
  try {
   IntPtr service=CreateService(manager,name,name,1,0x10,3,1,binary,null,IntPtr.Zero,null,"NT SERVICE\\"+name,null);
   if(service==IntPtr.Zero) throw new System.ComponentModel.Win32Exception();
   CloseServiceHandle(service);
  } finally { CloseServiceHandle(manager); }
 }
}
'@
}
[CkgitServiceRegistration]::Create($Name, $binary)
Write-Output "Registered $Name without starting it. Grant NT SERVICE\$Name the required access before starting."
