# Host project sites with Pages

## Pages

A build can publish a static site that anyone on the LAN can browse. Add a
top-level `pages:` block naming the directory to publish:

```text
version: 1
pages: { path: public }
jobs:
  - name: site
    steps:
      - run: make site        # writes ./public
```

The site is published from a **successful build of the repository's default
branch** — feature-branch and tag builds never replace it. Each publish is a new
versioned copy, and the newest `pages_keep_versions` are kept, so a bad deploy
can be rolled back by re-running an earlier commit's build. Deleting the project
removes its sites.

If the steps pass but publishing the site fails — for example the site exceeds
`kMaximumPagesSiteBytes` — the **run is marked failed** (with the reason in its
detail) rather than reporting success while the live site silently stays on the
previous version. Keep generated caches (a Sphinx `.doctrees` directory, say)
out of the published directory so they do not count against the size limit.

Any tool that writes a plain directory of files works here. For a
Markdown-only project, `ckdocs` — this project's own dependency-free
documentation-site generator, which is what publishes this very site — needs
nothing beyond that directory and a `--strict` flag; see [Documentation sites
from Markdown](07-docs-sites.md) for the full guide and this repository's own
`.ckgit/ci.yml` for a complete worked step.

### Serving it on the intranet

Sites are served by a **separate process, `ck-pagesd`, on its own port** — a
different origin from the dashboard on purpose, because a site's own JavaScript
must never run on the dashboard's origin (that is exactly why GitHub uses
`github.io` and GitLab `*.gitlab.io`). Turn serving on:

1. Set `pages_root` and `pages_http_port` in `server.ini` (a fresh install
   already sets `pages_root`; just uncomment `pages_http_port`).
2. `sudo systemctl enable --now ck-pages.service`.

Then browse `http://<server>:<pages_http_port>/<project>/`. For a friendly name
with no DNS server, enable mDNS on the host (`sudo apt install avahi-daemon`)
and use `http://<host>.local:<pages_http_port>/<project>/` — that works on
macOS, Linux, and Windows 10+ (Android browsers are unreliable over mDNS; use
the IP there). True per-project subdomains (`http://<project>.pages.<host>/`)
need a LAN resolver with a wildcard entry, such as `dnsmasq`, and are a later
option — the port form needs no DNS at all.

`ck-pagesd` serves read-only static files only, resolves each path without
following symlinks, and runs under a tight sandbox with no write access and no
access to the control socket, the repositories, or the metadata store. Firewall
`pages_http_port` to the intended subnet.

**Trust note:** a site's JavaScript is as trusted as whoever can push to the
project — it runs in the visitor's browser, the same property GitHub Pages has.
Serving it on its own origin protects the dashboard; it does not vet the site's
content. On a trusted-committer LAN this is the accepted trade.
