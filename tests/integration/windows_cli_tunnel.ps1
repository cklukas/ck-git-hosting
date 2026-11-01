# Copyright (c) 2026 C. Klukas. All rights reserved.
# SPDX-License-Identifier: MIT
# Opt-in: uses an explicitly configured real SSH dashboard connection.
param(
    [Parameter(Mandatory=$true)][string]$Client,
    [Parameter(Mandatory=$true)][string]$ConsoleHelper,
    [Parameter(Mandatory=$true)][string]$Config,
    [Parameter(Mandatory=$true)][string]$TestRoot
)
$ErrorActionPreference='Stop'
$Client=(Resolve-Path -LiteralPath $Client).Path
$ConsoleHelper=(Resolve-Path -LiteralPath $ConsoleHelper).Path
$Config=(Resolve-Path -LiteralPath $Config).Path
$TestRoot=(Resolve-Path -LiteralPath $TestRoot).Path
$root=Join-Path $TestRoot ('cli tunnel '+[Guid]::NewGuid().ToString('N'))
$oldTemp=$env:TEMP;$oldTmp=$env:TMP;$oldTmpdir=$env:TMPDIR
$process=$null;$listener=$null;$response=$null
try {
    New-Item -ItemType Directory $root | Out-Null
    $env:TEMP=$root;$env:TMP=$root;$env:TMPDIR=$root
    $listener=New-Object Net.Sockets.TcpListener([Net.IPAddress]::Loopback,0)
    $listener.Start();$port=$listener.LocalEndpoint.Port;$listener.Stop()
    # SSH/automation parents may inherit the Windows ignore-Ctrl+C attribute.
    # Start the fixture with normal console input processing, as in a terminal.
    if (!('CkgitTunnelConsole' -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class CkgitTunnelConsole {
    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern bool SetConsoleCtrlHandler(IntPtr handler, bool add);
}
'@
    }
    if (![CkgitTunnelConsole]::SetConsoleCtrlHandler([IntPtr]::Zero,$false)) { throw 'Cannot enable normal Ctrl+C inheritance for fixture' }
    $process=Start-Process -FilePath $Client -ArgumentList ('web --no-open --port '+$port+' --config "'+$Config+'"') -PassThru -WindowStyle Hidden -RedirectStandardOutput "$root\stdout.log" -RedirectStandardError "$root\stderr.log"
    $handle=$process.Handle
    for($i=0;$i -lt 100;$i++) {
        if($process.HasExited){throw 'Client exited before its tunnel was ready'}
        try {
            $request=[Net.HttpWebRequest]::Create("http://127.0.0.1:$port/")
            $request.Proxy=$null;$request.Timeout=500
            $response=$request.GetResponse()
            break
        } catch [Net.WebException] { Start-Sleep -Milliseconds 100 }
    }
    if(!$response -or [int]$response.StatusCode -ne 200){throw 'Dashboard did not answer through the tunnel'}
    $response.Close();$response=$null
    if($process.MainModule.FileName -ne $Client){throw 'Unexpected client executable'}
    $start=[DateTime]::UtcNow
    & $ConsoleHelper interrupt-console $process.Id
    if($LASTEXITCODE){throw 'Cannot interrupt isolated client console'}
    if(!$process.WaitForExit(8000)){throw 'Client remained running eight seconds after Ctrl+C'}
    if($process.ExitCode -ne 0){throw "Client exited with $($process.ExitCode) after Ctrl+C"}
    if([IO.File]::ReadAllText("$root\stdout.log") -notmatch 'Tunnel closed\.'){throw 'Graceful tunnel shutdown not confirmed'}
    $listener=New-Object Net.Sockets.TcpListener([Net.IPAddress]::Loopback,$port)
    $listener.Server.ExclusiveAddressUse=$true
    $listener.Start();$listener.Stop()
    Write-Output "Real dashboard tunnel and Ctrl+C passed in $(([DateTime]::UtcNow-$start).TotalSeconds)s; port $port released"
} catch {
    foreach($name in @('stdout','stderr')){if(Test-Path "$root\$name.log"){Get-Content "$root\$name.log" -Tail 20}}
    throw
} finally {
    if($response){$response.Close()}
    if($listener){$listener.Stop()}
    if($process){if(!$process.HasExited){$process.Kill();$process.WaitForExit()};$process.Dispose()}
    $env:TEMP=$oldTemp;$env:TMP=$oldTmp;$env:TMPDIR=$oldTmpdir
    if(Test-Path $root){Remove-Item -LiteralPath $root -Recurse -Force}
}
