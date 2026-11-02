# Copyright (c) 2026 C. Klukas. All rights reserved.
# SPDX-License-Identifier: MIT
# Operator-only registration for one private Hyper-V socket application.
# Does not register a CI runner, start a VM, add networking or grant file access.
[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$Scratch,[switch]$Register)
$ErrorActionPreference='Stop'
if(!(Test-Path V:\ -PathType Container) -or ![IO.Path]::IsPathRooted($Scratch) -or [IO.Path]::GetPathRoot($Scratch) -ne 'V:\' -or !(Test-Path -LiteralPath $Scratch -PathType Container)){throw 'Select an existing private scratch directory on V:'}
$env:TEMP=$Scratch;$env:TMP=$Scratch;$env:TMPDIR=$Scratch
$p='HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Virtualization\GuestCommunicationServices\8f22bd55-d776-43ea-bd82-93b2ee39f6da'
if(Test-Path -LiteralPath $p){$item=Get-ItemProperty -LiteralPath $p;if($item.Owner -cne 'ckgit-windows-ci' -or $item.ElementName -cne 'ck-git offline CI'){throw 'Reserved socket GUID already belongs to another application'}}elseif($Register){$key=New-Item -Path $p -ErrorAction Stop;try{$key.SetValue('ElementName','ck-git offline CI');$key.SetValue('Owner','ckgit-windows-ci')}finally{$key.Close()}}else{throw 'Socket application absent; an administrator must run this script with -Register'}
$item=Get-ItemProperty -LiteralPath $p
if($item.Owner -cne 'ckgit-windows-ci' -or $item.ElementName -cne 'ck-git offline CI'){throw 'Socket application registration differs'}
@{scope='Dedicated Hyper-V socket application only; no runner enabled';guid='8f22bd55-d776-43ea-bd82-93b2ee39f6da';owner=$item.Owner;reference='https://learn.microsoft.com/en-us/windows-server/virtualization/hyper-v/make-integration-service'}|ConvertTo-Json
