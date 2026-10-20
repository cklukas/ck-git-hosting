---
description: Publish a ckdocs site on ck-git Pages and GitHub Pages from the same source.
---
# Publish a documentation site

## Publishing on ck-git Pages and GitHub Pages

This repository publishes its documentation to both ck-git Pages and GitHub
Pages using the same source and configuration.

| | ck-git Pages (the LAN) | GitHub Pages |
|---|---|---|
| Where the build runs | this project's own sandboxed CI, on push | a GitHub Actions workflow, on push to `master` |
| How `ckdocs` gets there | compiled by that CI run's own `make … all` | compiled by that workflow's own `make … all` — no dependency on a released package |
| What publishes it | a top-level `pages: { path: public }` in `.ckgit/ci.yml`, read by [`ck-pagesd`](../operations/pages-hosting.md#pages) | `actions/upload-pages-artifact` + `actions/deploy-pages` in `.github/workflows/pages.yml` |
| Reached at | `http://<server>:<pages_http_port>/ck-git-hosting/`, and the dashboard's **Docs** button | `https://cklukas.github.io/ck-git-hosting/` |

Both publish only a successful build of the repository's own default
branch — a tag build never touches either (see [Pages](../operations/pages-hosting.md#pages)
for the LAN side's exact rule; `ckgit default-branch BRANCH` changes which
branch that is on the LAN side). This repository's own `.ckgit/ci.yml` and
`.github/workflows/pages.yml` are the worked example: the docs build runs
[`scripts/docs-web-screenshots.sh`](../../scripts/docs-web-screenshots.sh), then
`ckdocs build --strict`. A failed screenshot capture or broken documentation
link stops publication. The [`docs_site.sh`](../../tests/integration/docs_site.sh)
integration test checks the documentation links against this `ckdocs.yml` on
every push.

## Several sites from several branches

A [portal configuration](portals.md) can
render a site from another branch with `ref:`. The build then needs that
branch's tree. In a full clone with the branch fetched, ckdocs extracts it
by itself; a CI checkout usually has neither, so give it the branch one of
these two ways.

**GitHub Actions.** `actions/checkout` fetches only the built commit by
default. Fetch the other branch as a remote-tracking branch before the
build; ckdocs resolves `origin/<ref>` when `<ref>` is not a local branch:

```yaml
steps:
  - uses: actions/checkout@v4
  - name: Fetch the branch the portal also renders
    run: git fetch --depth=1 origin main:refs/remotes/origin/main
  - name: Build the documentation
    run: ckdocs build --out public --strict
```

**ck-git CI.** The CI checkout is created with `git archive` and has no
`.git`, so ckdocs cannot extract a ref there. Instead, list the project
itself as a [sister](../operations/06-ci-yml-reference.md) with the branch
as its `ref`. Its tree is placed at `../<name>` and exported as
`CKGIT_SISTER_<NAME>`; pass that directory to ckdocs with `--ref-tree`:

```yaml
version: 1
sisters:
  - name: paglets
    ref: main
pages:
  path: public
jobs:
  - name: docs
    steps:
      - run: ckdocs build --ref-tree main="$CKGIT_SISTER_PAGLETS" --out public --strict
```

A project may list itself as a sister only with an explicit `ref`, so the
sister is always a pinned branch, tag, or commit rather than a second copy
of the checkout.
