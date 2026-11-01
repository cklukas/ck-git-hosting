---
description: "Install, connect and verify Linux and Windows CI runners with a ck-git-hosting main host."
---
# Install and connect CI runners

A **main host** is the Linux machine running ck-git-hosting. It holds the Git
repositories, CI queue, logs and artifacts. A **remote runner** is another
machine that connects to the main host over SSH, claims jobs, executes them,
and sends results back. The main host does not connect to the runner.

| Where | What to do |
|---|---|
| Main host | Enable CI for the project; register each runner's public key and labels. |
| Runner machine | Install the runner, configure its server address, key, directories and build tools; start it. |
| Project | Commit a workflow selecting the runner's labels, then push. |

The main host itself also has a Linux runner. Remote runners are optional.
A Windows runner requires a main host supporting runner protocol 2; upgrade the
main host first if necessary. Each remote runner needs its own name and SSH key.
Choose the instructions for your runner:

- **Windows x64:** [Install and connect a Windows runner](runner-windows.md),
  including MSVC tool staging, an ordinary-user console runner and an optional
  Windows service.
- **Debian or Raspberry Pi OS:** [Set up a Linux runner](#set-up-a-linux-runner).

## Set up a Linux runner

1. On the **runner machine**, install the `ck-git-runner` Debian package from
   the [project releases](https://github.com/cklukas/ck-git-hosting/releases).
   Replace the filename, host and runner name with your own:

   ```sh
   sudo apt install ./ck-git-runner_VERSION_ARCH.deb
   sudo ck-git-runner register --server ckgit@main-host --name linux-builder
   ```

   The registration helper creates a dedicated key, pins the host key and
   writes `/etc/ck-git-hosting/runner.ini`. Verify the host-key fingerprint
   with the main host administrator. Follow the helper's printed instructions
   to copy its **public** key to the main host; never copy its private key.
   The machine must meet the [Linux sandbox requirements](ci-runner.md#the-sandbox-and-its-requirements).

2. On the **main host**, use the path where you copied that public key. The
   command registers the runner and prints a restricted `authorized_keys` line;
   append that output to the hosting account's configured authorized-keys file:

   ```sh
   sudo -u ckgit ckgit-admin runner add linux-builder --public-key /path/to/linux-builder.pub \
     --labels linux,amd64 --config /etc/ck-git-hosting/server.ini \
     | sudo tee -a /etc/ck-git-hosting/authorized_keys
   ```

   Use `linux,arm64` for an ARM64 runner. If your installation uses different
   account or authorized-keys paths, use its configured paths instead.

3. Back on the **runner machine**, start the service and inspect its log:

   ```sh
   sudo systemctl enable --now ck-git-runner.service
   journalctl -u ck-git-runner.service -f
   ```

4. On the **main host**, confirm the runner has contacted it and enable CI for
   your project (the project must already exist):

   ```sh
   sudo -u ckgit ckgit-admin runner list --config /etc/ck-git-hosting/server.ini
   sudo -u ckgit ckgit-admin ci enable myproject --config /etc/ck-git-hosting/server.ini
   ```

   The [runner configuration reference](ci-remote-runners.md#set-up-a-remote-runner)
   explains `runner.ini`, server settings, labels, service management and
   troubleshooting. To target this runner, set `runs_on: [linux, amd64]` in a
   version-2 job, or use its unique name as a label.

## Verify a job and get its artifact

On a paired client, commit and push a version-2 `.ckgit/ci.yml` with a job
whose `runs_on` labels match the registered runner. For Windows, the
[Windows walkthrough](runner-windows.md#run-a-windows-job) includes a small
verification workflow and commands for its artifact. For simultaneous Linux
and Windows builds from one push, use the [two-platform example](06-ci-yml-reference.md#build-on-linux-and-windows-from-one-push).

The dashboard's **Runners** page shows connected runners, labels and current
jobs; a CI run shows the runner used, logs and artifacts. From a paired client:

```sh
ckgit ci list --project myproject
ckgit ci show RUN_ID --project myproject
ckgit ci artifacts RUN_ID --project myproject
mkdir -p ./downloads
ckgit ci download RUN_ID ARTIFACT_NAME --project myproject --into ./downloads
```

Replace the placeholders with the run ID and artifact name shown by the
previous commands. To stop assigning new work without interrupting a current
job, run `ckgit-admin runner disable NAME --config /etc/ck-git-hosting/server.ini`
as the `ckgit` account on the main host. See [manage runners](ci-remote-runners.md#manage-runners)
for enable, removal and key revocation.
