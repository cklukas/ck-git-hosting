# Clone and update projects

## Find projects

```sh
ckgit projects
ckgit projects --uncloned
```

`ckgit projects` lists hosted projects and their local management state.
`--uncloned` selects projects absent from this device's management records; it
does not search the disk for ordinary Git clones. JSON includes a stable schema
version, project state, selected path, and SSH clone URL.

To populate an existing folder, use `ckgit clone --all --into /path/to/projects`.
The command displays its scope and asks for confirmation on a terminal;
`--dry-run` previews and `--yes` executes without prompting. Repeated
`--project NAME` selects a smaller set. Already managed projects are skipped,
existing folders are never overwritten, and failures do not stop other clones.
Missing or unresolved management records remain visible: repair the selection
with `checkout set-canonical` or `migrate`, or `forget` it before cloning anew.

`ckgit clone` uses only the configured `user@host` plus a validated project
name, creates the destination only when it does not already exist, and names
the resulting remote from `remote_name` (normally `ckgit`). If this device has
no other main checkout for that project, the clone joins ordinary `status` and
`sync` automatically, including with a custom destination name. If another
main checkout already exists, the new folder is a secondary copy; the result
names the main folder and prints the exact command to select the new copy.

## Fetch and update

```sh
ckgit fetch --project my-project
ckgit update --project my-project
```

`ckgit fetch` downloads all hosted branches into the configured remote-tracking
namespace and downloads tags, without changing local branches or working files.
`--branch NAME` and `--no-tags` limit that operation. `ckgit update` downloads
the current branch and tags, then fast-forwards only a clean, attached checkout.
It never switches branches, rebases, or creates a merge commit. Dirty files,
diverged history, missing branches, and conflicting tags require attention;
other projects continue. Both commands use managed checkouts by default, accept
`--repo PATH` or `--project NAME`, and offer read-only `--dry-run`. When incoming
objects are absent locally, a preview explains that ancestry requires a fetch.
`sync` continues to upload committed local refs only.
