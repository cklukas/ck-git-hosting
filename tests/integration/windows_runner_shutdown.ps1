# Copyright (c) 2026 C. Klukas. All rights reserved.
# SPDX-License-Identifier: MIT
# Opt-in: creates disposable AppContainer profiles and two hidden consoles.
param(
    [Parameter(Mandatory=$true)][string]$Runner,
    [Parameter(Mandatory=$true)][string]$FakeSsh,
    [Parameter(Mandatory=$true)][string]$TestRoot
)
$ErrorActionPreference='Stop'
$Runner=(Resolve-Path -LiteralPath $Runner).Path
$FakeSsh=(Resolve-Path -LiteralPath $FakeSsh).Path
$TestRoot=(Resolve-Path -LiteralPath $TestRoot).Path
$fixture=Join-Path $TestRoot ('runner-shutdown-'+[Guid]::NewGuid().ToString('N'))
$oldTemp=$env:TEMP; $oldTmp=$env:TMP; $oldTmpdir=$env:TMPDIR
$oldWait=$env:CKGIT_TEST_RUNNER_WAIT; $oldReady=$env:CKGIT_TEST_RUNNER_READY
$runnerProcess=$null; $sshProcess=$null
try {
    New-Item -ItemType Directory "$fixture\tools\bin","$fixture\build","$fixture\tmp" | Out-Null
    $env:TEMP="$fixture\tmp"; $env:TMP=$env:TEMP; $env:TMPDIR=$env:TEMP
    $config=Join-Path $fixture 'runner.ini'
    [IO.File]::WriteAllText($config,"schema_version=1`nserver=ckgit@example.invalid`nname=shutdown-fixture`nci_build_root=$fixture\build`nci_toolchain_root=$fixture\tools`nssh_program=$FakeSsh`n",(New-Object Text.UTF8Encoding($false)))
    foreach($operation in @('hello','claim')) {
        $env:CKGIT_TEST_RUNNER_WAIT=$operation
        $env:CKGIT_TEST_RUNNER_READY=Join-Path $fixture "$operation-ready.txt"
        $runnerProcess=Start-Process -FilePath $Runner -ArgumentList ('serve --remote --config "'+$config+'"') -PassThru -WindowStyle Hidden -RedirectStandardOutput "$fixture\$operation.log" -RedirectStandardError "$fixture\$operation-error.log"
        $runnerHandle=$runnerProcess.Handle
        for($i=0;$i -lt 100;$i++) {
            if($runnerProcess.HasExited){throw 'Runner exited before the waiting operation'}
            if((Test-Path $env:CKGIT_TEST_RUNNER_READY) -and [IO.File]::ReadAllText($env:CKGIT_TEST_RUNNER_READY) -match '^\d+$'){break}
            Start-Sleep -Milliseconds 100
        }
        if(!(Test-Path $env:CKGIT_TEST_RUNNER_READY)){throw 'Waiting SSH fixture did not start'}
        $sshPid=[int]([IO.File]::ReadAllText($env:CKGIT_TEST_RUNNER_READY))
        $sshProcess=Get-Process -Id $sshPid
        $sshHandle=$sshProcess.Handle
        if($runnerProcess.MainModule.FileName -ne $Runner -or $sshProcess.MainModule.FileName -ne $FakeSsh){throw 'Unexpected fixture executable'}
        $start=[DateTime]::UtcNow
        & $FakeSsh close-console $runnerProcess.Id
        if($LASTEXITCODE){throw 'Console close failed'}
        if(!$runnerProcess.WaitForExit(8000) -or $runnerProcess.ExitCode -ne 0){throw "Runner did not close normally during $operation"}
        if(!$sshProcess.WaitForExit(1000)){throw 'SSH process survived broker shutdown'}
        if(@(Get-ChildItem "$fixture\build" -Directory).Count){throw 'Shutdown left a session workspace'}
        $lock=[IO.File]::Open("$fixture\build\runner.lock",'Open','ReadWrite','None');$lock.Dispose()
        Write-Output "Runner close during $operation passed in $(([DateTime]::UtcNow-$start).TotalSeconds)s; SSH stopped and session removed"
        $runnerProcess.Dispose();$runnerProcess=$null
        $sshProcess.Dispose();$sshProcess=$null
    }
} catch {
    Get-ChildItem $fixture -Filter '*.log' | ForEach-Object { Write-Output $_.Name; Get-Content $_.FullName -Tail 20 }
    throw
} finally {
    # Only handles captured from this fixture; never terminate by executable name.
    foreach($process in @($runnerProcess,$sshProcess)) {
        if($process){if(!$process.HasExited){$process.Kill();$process.WaitForExit()};$process.Dispose()}
    }
    $env:TEMP=$oldTemp;$env:TMP=$oldTmp;$env:TMPDIR=$oldTmpdir
    $env:CKGIT_TEST_RUNNER_WAIT=$oldWait;$env:CKGIT_TEST_RUNNER_READY=$oldReady
    if(Test-Path $fixture){Remove-Item -LiteralPath $fixture -Recurse -Force}
}
