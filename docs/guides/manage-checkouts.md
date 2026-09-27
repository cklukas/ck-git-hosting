# Manage local checkouts

## Main checkout

Every device has one main checkout per managed project. Its absolute local
path is stored privately in mode-`0600` `canonical.ini` beside `client.ini`.
The server receives only the path information allowed by `public_path_mode`;
the default `basename` mode works with ordinary sync and never needs to be
changed to `full` just to find this device's folders. Publishing or explicitly
syncing another folder is refused with a message naming the main one unless
`--replace-checkout` is supplied. `publish`, `sync --repo`, and `register` with
that flag update the same main selection as `checkout set-canonical`. The
inventory and lock are shared by configurations in the same directory. Keep
configurations for different servers or device identities in separate
directories; server reports are separate for each client ID.

## Inspect and select checkouts

`status`, default `sync`, and `checkout list` show the same managed project set
and effective main selection. Their output begins with the scope, device, and
server. `--repo .` explicitly selects the current repository for status or sync.
Discovery stays explicit: `status --scan` inspects configured roots;
`checkout list --scan` also lists additional paired copies without selecting
them. `sync --scan` discovers paired checkouts under those roots and obeys
the same main selection as ordinary sync. If no selection exists, it uses a
single discovered checkout; duplicates require an explicit choice. A selected
checkout that is absent from the discovery result is skipped, with no fallback
to a different copy.
`ckgit checkout set-canonical NAME PATH` verifies that `PATH` is a working tree
connected to `NAME`, then updates the server report and private main selection.
The command spelling is retained for compatibility: "canonical" means the
main checkout on this device. It does not upload commits or remove another copy.

For checkouts registered by an older client, run `ckgit checkout migrate
--dry-run`, then `ckgit checkout migrate`. Existing absolute reports can be
adopted directly after validation. Basename-only reports are matched against
configured `scan_root` directories using the folder name and validated remote.
Only a unique match is adopted. Missing or ambiguous matches remain unresolved
and list how to select a folder with `checkout set-canonical`. Migration never
uploads refs or changes the path privacy setting.

## Stop managing a checkout

`ckgit checkout forget NAME` previews removal of this device's local selection
and server registration, then asks for confirmation (`--yes` for automation).
It also works when the checkout folder is missing. Files, remotes, commits,
the hosted repository, and other devices' records remain intact. The server
must be reachable; a failed remote removal preserves the local selection.
Use `register` to manage the checkout again. Deliberately running `sync --scan`
can also rediscover paired copies.
