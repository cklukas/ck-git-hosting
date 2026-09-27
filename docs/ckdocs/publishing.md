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
for the LAN side's exact rule). This repository's own `.ckgit/ci.yml` and
`.github/workflows/pages.yml` are the worked example: the docs build runs
[`scripts/docs-web-screenshots.sh`](../../scripts/docs-web-screenshots.sh), then
`ckdocs build --strict`. A failed screenshot capture or broken documentation
link stops publication. The [`docs_site.sh`](../../tests/integration/docs_site.sh)
integration test checks the documentation links against this `ckdocs.yml` on
every push.
