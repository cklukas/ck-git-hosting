---
description: "Install, configure and connect a native Windows x64 CI runner to your main host."
---
# Install and connect a Windows runner

This guide connects a Windows x64 machine to a Linux ck-git-hosting main host.
The runner pulls jobs over SSH and executes each job in a restricted Windows
AppContainer (LPAC). Job networking is denied. The main host retains the logs
and artifacts. See [Install and connect CI runners](runner-setup.md) for the
roles of the main host and runners, including Linux installation.

## Before you start

- A running, protocol-2-capable main host, SSH access to it, and permission to
  register a runner and enable CI for a hosted project.
- Windows x64, Git for Windows, and the `ck-git-runner-*-windows-x64-setup.exe`
  installer from a build or [GitHub release](https://github.com/cklukas/ck-git-hosting/releases).
  Pick a release that actually includes the Windows runner installer; older
  releases can predate it. Installing **CurrentUser** requires no administrator;
  **AllUsers** and Windows service registration require administrator rights.
- A Windows toolchain for your builds. The example below uses MSVC, a Windows
  SDK, CMake, Inno Setup and the LPAC-compatible Ninja built from the project's
  provisioning scripts. Tools must be staged in advance: jobs cannot download
  dependencies from the network.

The examples use `C:\ci-example\Programs\ck-git-runner`, `C:\ci-example\ProgramData\ckgit-runner`
and `C:\ci-example\tmp\ckrun`. Replace these with suitable absolute paths on your own
machine, ensure the chosen locations are writable by the runner account, and
keep the build path short. Set `TEMP`, `TMP` and `TMPDIR` to a suitable task
directory before staging tools. Replace `main-host`, `windows-builder`
and `myproject` consistently throughout this guide.

## 1. Install and prepare the runner on Windows

Run the runner installer, select the desired scope and installation directory,
and finish setup. It installs `ck-ci-runnerd.exe`, `ckgit-job-launcher.exe`,
`runner.ini.example` and `register-service.ps1`. It does **not** register a
key, configure the machine, or start a runner automatically.

In PowerShell, create private runner directories and a dedicated SSH key. Use
an unencrypted key for unattended service operation, and restrict access to it
and its parent directory to the runner account and administrators:

```powershell
$root = 'C:\ci-example\ProgramData\ckgit-runner'
New-Item -ItemType Directory -Force "$root\ssh", "$root\cache", "$root\logs", 'C:\ci-example\tmp\ckrun' | Out-Null
& 'C:\Program Files\Git\usr\bin\ssh-keygen.exe' -t ed25519 -f "$root\ssh\id_ed25519" -C 'windows-builder'
Copy-Item 'C:\ci-example\Programs\ck-git-runner\runner.ini.example' "$root\runner.ini"
```

When prompted, leave the passphrase empty for an unattended runner. Generate a
`known_hosts` file for `main-host` using your organization's normal host-key
verification procedure; compare the fingerprint with the main host over a
trusted channel before trusting it. Save it as `$root\ssh\known_hosts`. For example, obtain and inspect a candidate
on Windows (the fingerprint must be checked independently before use):

```powershell
$lines = & 'C:\Program Files\Git\usr\bin\ssh-keyscan.exe' -t ed25519 main-host
[IO.File]::WriteAllLines("$root\ssh\known_hosts", [string[]]$lines, (New-Object Text.UTF8Encoding($false)))
& 'C:\Program Files\Git\usr\bin\ssh-keygen.exe' -lf "$root\ssh\known_hosts"
```

Copy only `id_ed25519.pub` to the main host, using an existing administrator
login; the private key stays on Windows.

Edit `$root\runner.ini` and set `server=ckgit@main-host`,
`name=windows-builder`, `identity`, `known_hosts`, `ci_build_root`,
`ci_cache_root`, `ssh_program`, and the toolchain settings to the actual paths.
The installed example shows their format. `ssh_program` in the example selects
Git for Windows' SSH client. Set `ci_build_root=C:/ci-example/tmp/ckrun` if using the
paths above. Paths must be accessible to the account that will run the broker.

## 2. Register its public key on the main host

Copy the public key to a temporary file readable by the `ckgit` service account,
then run on the **main host**, replacing the path with that file:

```sh
sudo -u ckgit ckgit-admin runner add windows-builder --public-key /path/to/windows-builder.pub \
  --labels windows,x64 --config /etc/ck-git-hosting/server.ini \
  | sudo tee -a /etc/ck-git-hosting/authorized_keys
sudo -u ckgit ckgit-admin runner list --config /etc/ck-git-hosting/server.ini
sudo -u ckgit ckgit-admin ci enable myproject --config /etc/ck-git-hosting/server.ini
```

The generated line limits that key to the runner protocol. Use the actual
configured account and authorized-keys file if your installation differs.
Keep the key and name unique; labels are assigned by the main host, not by the
runner. The two labels above match `runs_on: [windows, x64]`.

## 3. Stage build tools on Windows

The provisioning scripts live in the [repository's `packaging/windows` directory](https://github.com/cklukas/ck-git-hosting/tree/master/packaging/windows),
not in the runner installer. Obtain that directory from the same project version
as the runner. Install the required vendor tools first and obtain the upstream
Ninja 1.13.2 source archive. From PowerShell, replace every placeholder below
with an existing absolute path and set `$scripts` to that directory:

```powershell
$scripts = 'C:\ci-example\dev\ck-git-hosting\packaging\windows'
$temp = 'C:\ci-example\tmp\runner-toolchain-setup'
New-Item -ItemType Directory -Force $temp | Out-Null
$env:TEMP = $temp; $env:TMP = $temp; $env:TMPDIR = $temp
& "$scripts\build-lpac-ninja.ps1" -SourceArchive 'C:\ci-example\downloads\ninja-1.13.2.tar.gz' `
  -TemporaryRoot $temp -Destination 'C:\ci-example\dev\runner-ninja-1.13.2' `
  -CmakeProgram 'C:\ci-example\tools\cmake\bin\cmake.exe'
& "$scripts\stage-toolchain.ps1" -TemporaryRoot $temp `
  -Destination 'C:\ci-example\ProgramData\ckgit-runner\toolchain' `
  -MsvcRoot 'C:\path\to\MSVC\version' -SdkRoot 'C:\path\to\Windows Kits\10' `
  -SdkVersion '10.0.x.y' -CmakeRoot 'C:\ci-example\tools\cmake' `
  -InnoRoot 'C:\path\to\Inno Setup 6' -NinjaRoot 'C:\ci-example\dev\runner-ninja-1.13.2'
```

The paths above are examples, not installed defaults. Both destination
folders must be new. The first script verifies and builds the pinned Ninja with
its LPAC pipe fix; the second copies and verifies the tool files. Ensure
`ci_toolchain_root`, `ci_toolchain_paths`, `ci_toolchain_env.INCLUDE` and
`ci_toolchain_env.LIB` in `runner.ini` match the staged layout in the installed
example. For another project, stage all tools its commands need; shell script
steps additionally need the selected shell in the staged tools.

## 4. Start the runner

For a **CurrentUser console runner**, run these in the account and logon
session that will own the runner:

```powershell
$runner = 'C:\ci-example\Programs\ck-git-runner\ck-ci-runnerd.exe'
$config = 'C:\ci-example\ProgramData\ckgit-runner\runner.ini'
& $runner check --config $config
& $runner serve --remote --config $config
```

Keep the console session open. The `check` command tests the required sandbox
and Windows session; a session conflict must be fixed before jobs can run.

For an **AllUsers Windows service**, open an elevated PowerShell and register
it explicitly. Create the log directory first. The script creates a stopped,
manual-start service using `NT SERVICE\ckgit-runner`:

```powershell
& 'C:\ci-example\Programs\ck-git-runner\register-service.ps1' `
  -Runner 'C:\ci-example\Programs\ck-git-runner\ck-ci-runnerd.exe' `
  -Config 'C:\ci-example\ProgramData\ckgit-runner\runner.ini' `
  -Log 'C:\ci-example\ProgramData\ckgit-runner\logs\service.log' -Name 'ckgit-runner'
```

Before starting it, grant that service identity read/execute access to the
installed runner, configuration, private SSH key, known-hosts and staged
toolchain; grant modify access to the configured build, cache, temporary and
log directories. Restrict the key to the service identity and administrators,
and set its owner to the service identity so SSH accepts it. Do not make the
key or toolchain writable by untrusted users. The registration helper does not
change ACLs. Use your organization's ACL provisioning procedure and verify
effective access for `NT SERVICE\ckgit-runner`. Set service `TEMP`, `TMP` and
`TMPDIR` if its default temporary directory is unsuitable. For example, after
registering the service, the following **elevated** PowerShell grants the
service access to the example paths. Use a dedicated runner data directory and
review its existing permissions first; the last block replaces the key's ACL
with only the service, local Administrators and SYSTEM:

```powershell
$root = 'C:\ci-example\ProgramData\ckgit-runner'
$name = 'ckgit-runner'
$sid = (New-Object Security.Principal.NTAccount("NT SERVICE\$name")).Translate([Security.Principal.SecurityIdentifier])
$inherit = 'ContainerInherit,ObjectInherit'
$acl = Get-Acl $root
$acl.AddAccessRule((New-Object Security.AccessControl.FileSystemAccessRule($sid,'ReadAndExecute',$inherit,'None','Allow')))
Set-Acl $root $acl
foreach ($dir in @("$root\cache", "$root\logs", 'C:\ci-example\tmp\ckrun', 'C:\ci-example\tmp\ckrun-service-tmp')) {
  New-Item -ItemType Directory -Force $dir | Out-Null
  $acl = Get-Acl $dir
  $acl.AddAccessRule((New-Object Security.AccessControl.FileSystemAccessRule($sid,'Modify',$inherit,'None','Allow')))
  Set-Acl $dir $acl
}
$key = "$root\ssh\id_ed25519"
$keyAcl = New-Object Security.AccessControl.FileSecurity
$keyAcl.SetAccessRuleProtection($true,$false)
$keyAcl.SetOwner($sid)
foreach ($id in @($sid, (New-Object Security.Principal.SecurityIdentifier('S-1-5-18')), (New-Object Security.Principal.SecurityIdentifier('S-1-5-32-544')))) {
  $keyAcl.AddAccessRule((New-Object Security.AccessControl.FileSystemAccessRule($id,'FullControl','Allow')))
}
Set-Acl $key $keyAcl
New-ItemProperty "HKLM:\SYSTEM\CurrentControlSet\Services\$name" -Name Environment -PropertyType MultiString `
  -Value @('TEMP=C:\ci-example\tmp\ckrun-service-tmp','TMP=C:\ci-example\tmp\ckrun-service-tmp','TMPDIR=C:\ci-example\tmp\ckrun-service-tmp') -Force | Out-Null
```

Ensure the service also has read/execute permissions on the installation and
all parent directories. The script grants access; it does not remove existing
access to the other directories. Review those ACLs, particularly the toolchain,
and replace the paths if your configuration differs. Then:

```powershell
Start-Service ckgit-runner
Get-Service ckgit-runner
Get-Content 'C:\ci-example\ProgramData\ckgit-runner\logs\service.log' -Tail 30
# After confirming it connects and can run jobs:
Set-Service ckgit-runner -StartupType Automatic
```

A service has a different logon session from your console; a console `check`
does not prove service-session startup. Inspect both `service.log` and
`service.log.host.log` if it fails. See [Windows service details](ci-remote-runners.md#windows-runner)
for identity, sandbox and upgrade behavior.

## Run a Windows job

Commit this minimal `.ckgit/ci.yml` to `myproject` and push its `main` branch.
It uses the staged CMake to create a small artifact; it does not need a shell.
Change the branch to the project's default branch if different:

```yaml
version: 2
on: { branches: [main] }
jobs:
  - name: windows-check
    runs_on: [windows, x64]
    steps:
      - run: [cmake.exe, -E, make_directory, dist]
      - run: [cmake.exe, -E, touch, dist/runner-ok.txt]
    artifacts:
      name: windows-check
      paths: [dist]
```

On a paired client, inspect the run and download its artifact:

```sh
ckgit ci list --project myproject
ckgit ci show RUN_ID --project myproject
ckgit ci artifacts RUN_ID --project myproject
mkdir -p ./downloads
ckgit ci download RUN_ID windows-check --project myproject --into ./downloads
```

The job should show `windows-builder` as its runner. If it remains pending,
check the runner's service or console log, SSH host-key verification, the
server's `runner list`, project CI enablement, and exact label spelling. The
[workflow reference](06-ci-yml-reference.md#build-on-linux-and-windows-from-one-push)
shows how this repository builds Windows installers alongside a Linux job.
