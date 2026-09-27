# Dashboard behavior and limits

For a visual introduction, see the [dashboard tour](../operations/08-web-dashboard.md).

## Projects and revisions

The project table sorts by the newest commit, with a link to sort by name.
Each overview shows the last commit and author, repository size, dated branch
and tag lists, checkout registrations, recent events, and the default branch's
README. The in-memory index supplies the table and default overview without spawning
Git during a request. Selecting a revision on the overview loads that revision's
commit and README. Startup shows indexing placeholders while a background
thread builds the records. Push notifications queue a refresh; a 60-second
fingerprint sweep also discovers administrative pushes and deleted projects.

Every page includes the text logo and an About dialog with the running build
version, copyright, and license. Native browser controls open and dismiss it.

The Files view has a classic expandable tree with connector lines, folder and
file-type icons, a branch/tag picker, and a preview. Folder controls expand
inline without JavaScript; folder names open the directory preview. The current
path opens automatically, and only the selected item is highlighted. On desktop
the tree panel fills the available screen height, with a 32rem minimum.
Branch, tag, and commit labels distinguish a moving named revision from an
immutable snapshot. Files, breadcrumbs, section navigation, and pagination keep
the selected revision; Permalink pins it, and Latest default branch explicitly
returns to the default revision. Calendar branch/tag choices preserve the month.

READMEs render a built-in Markdown subset in the overview and in directories.
Other Markdown files also open rendered, with a Source mode and heading links.
GitHub-flavoured tables, alerts (`> [!NOTE]`), task lists, and strikethrough
render as on GitHub, and YAML front matter is kept out of the rendered view.
Raw HTML is escaped, comments outside code are hidden, and relative document
links retain the selected branch or tag. Directory links also work without a
trailing slash. Images, including SVG, load through an immutable protected raw
route. Source views have line numbers, highlighted line links, optional
wrapping, and server-side syntax highlighting for C/C++, Python, shell, YAML,
and JSON (fenced code blocks in Markdown get the same), with no JavaScript. On narrow screens, file navigation is collapsed behind Show files,
and project/commit rows rearrange for reading. Empty projects explain how to
start; missing files retain the project, revision picker, and parent links.
Text and README source previews are limited to 512 KiB. The file preview
embeds images up to 4 MiB; README image references use the raw route's 16 MiB
limit, which also bounds downloads. Files beyond these limits remain available
through Git. The sidebar preloads trees up to 5000 entries and 32 path
components in one bounded Git read. Larger trees fall back to loading the
current directory and its ancestors; opening another folder loads its contents.
These expanded listings retain a combined 5000-entry limit. A rendered page is
limited to 8 MiB of HTML.

Commits and Graph show 50 commits per page with a server-rendered SVG graph.
Lane lines span each row and remain connected when commit text wraps.
Older links retain the selected revision and preserve merged histories; locating
a cursor scans a bounded traversal, so deep paging costs more than a single
page of work. Commit pages include the message, parent links, file statistics,
and a diff capped at 512 KiB. Calendar views show UTC commit activity, and day
pages list up to 200 commits with a link to the tree at the end of that day.
Activity and cursor traversal are bounded to 200000 commits. Limit notices
identify abbreviated output; time and output failures return a bounded error.

Every page works without JavaScript and shares one layout with inline CSS.
HTTP requests perform no writes. There is no persistent index or SQLite
database; standard bare Git repositories remain the source of truth.

## Dashboard tunnel

`ckgit web` opens the server's loopback dashboard through an SSH tunnel: it
picks a free local port (8420 when available), starts `ssh -N -L` without a
shell using the administrator's own login on the server host (the restricted
`ckgit` account allows no forwarding), waits until the dashboard answers,
prints the URL, opens the browser, and closes the tunnel again on Ctrl+C.
The configured `web_host` chooses the forwarding login and `web_port` chooses
the dashboard port; explicit `ADMIN-HOST` and `--remote-port` override them.
The remote port must match `http_port` in the server's `server.ini`.
`--project NAME` opens that hosted project's overview directly instead of the
project index.
