// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "ckgit/dashboard.hpp"
#include "ckgit/project_index.hpp"
#include "ckgit/web_renderer.hpp"
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unistd.h>
namespace {
void require(bool condition, const std::string& message) { if (!condition) throw std::runtime_error("dashboard: " + message); }
std::string git(const std::filesystem::path& path, std::vector<std::string> args, const std::string& date = "2024-02-29T12:00:00Z") {
  std::vector<std::string> command{"env", "GIT_AUTHOR_DATE=" + date, "GIT_COMMITTER_DATE=" + date, "git", "-C", path.string(), "-c", "commit.gpgSign=false"};
  command.insert(command.end(), args.begin(), args.end());
  auto r = ckgit::runProcess(command, std::chrono::seconds(10), 4 * 1024 * 1024);
  require(r.exit_code == 0 && !r.timed_out && !r.output_truncated, "fixture git failed: " + r.output);
  while (!r.output.empty() && r.output.back() == '\n') r.output.pop_back();
  return r.output;
}
void write(const std::filesystem::path& p, const std::string& content) { std::ofstream f(p, std::ios::binary); f << content; require(bool(f), "write fixture"); }
std::size_t occurrences(const std::string& text, const std::string& needle) {
  std::size_t count = 0;
  for (std::size_t at = 0; (at = text.find(needle, at)) != std::string::npos; at += needle.size()) ++count;
  return count;
}
std::pair<std::set<int>, std::set<int>> graphBoundaries(const std::string& row) {
  std::pair<std::set<int>, std::set<int>> boundaries;
  for (std::size_t start = 0; (start = row.find("<path ", start)) != std::string::npos;) {
    const auto value = row.find(" d=\"", start);
    require(value != std::string::npos, "graph edge has coordinates");
    const auto end = row.find('"', value + 4);
    std::istringstream coordinates(row.substr(value + 4, end - value - 4));
    char move = 0, operation = 0;
    int x = 0, y = 0, end_x = 0, end_y = 0;
    coordinates >> move >> x >> y >> operation;
    if (operation == 'V') { end_x = x; coordinates >> end_y; }
    else if (operation == 'L') coordinates >> end_x >> end_y;
    require(bool(coordinates) && move == 'M' && (operation == 'V' || operation == 'L'),
            "graph edge is a readable lane segment");
    if (y == 0) boundaries.first.insert(x);
    if (end_y == 48) boundaries.second.insert(end_x);
    start = end + 1;
  }
  return boundaries;
}
std::string fileNavigation(const std::string& html) {
  const auto start = html.find("<aside class=\"tree-pane\"");
  const auto end = html.find("</aside>", start);
  require(start != std::string::npos && end != std::string::npos, "file navigation is a bounded aside");
  return html.substr(start, end + 8 - start);
}
std::string folderOpening(const std::string& navigation, const std::string& url) {
  const auto target = navigation.find("href=\"" + url + "\"");
  const auto summary = navigation.rfind("<summary", target);
  const auto details = navigation.rfind("<details", summary);
  const auto end = navigation.find('>', details);
  require(target != std::string::npos && summary != std::string::npos && details != std::string::npos &&
          end != std::string::npos && details < summary && summary < target,
          "folder has a native disclosure and linked summary: " + url);
  return navigation.substr(details, end + 1 - details);
}
std::string makeTree(const std::filesystem::path& repository, const std::filesystem::path& input,
                     const std::string& records) {
  write(input, records);
  // runProcess intentionally supplies /dev/null on stdin. This fixed shell
  // fragment redirects only the fixture file passed as an argument; no path or
  // repository content is interpolated into shell code.
  auto result = ckgit::runProcess({"sh", "-c", "exec git -C \"$1\" mktree < \"$2\"", "tree-fixture",
                                 repository.string(), input.string()}, std::chrono::seconds(10));
  require(result.exit_code == 0 && !result.timed_out && !result.output_truncated,
          "create a tree fixture using Git: " + result.output);
  while (!result.output.empty() && result.output.back() == '\n') result.output.pop_back();
  return result.output;
}
void testClassicTreeLimits(const std::filesystem::path& root) {
  const auto repository = root / "tree-limits.git";
  git(root, {"init", "-q", "--bare", repository.string()});
  git(repository, {"config", "user.name", "Tree Limits"});
  git(repository, {"config", "user.email", "tree-limits@example.test"});
  const auto input = root / "tree-records";
  const auto empty_file = root / "empty-blob";
  write(empty_file, "");
  const auto blob = git(repository, {"hash-object", "-w", empty_file.string()});
  const auto commitTree = [&](const std::string& tree) {
    return git(repository, {"commit-tree", tree, "-m", "Tree boundary fixture"});
  };
  const auto treeRejected = [&](const std::string& commit, const std::string& label, const std::string& reason = "") {
    bool rejected = false;
    try { static_cast<void>(ckgit::WebRepository(repository).fileTree(commit)); }
    catch (const ckgit::TreeLimitError& error) {
      rejected = error.status == 503 && (reason.empty() || std::string(error.what()).find(reason) != std::string::npos);
    }
    require(rejected, label);
  };
  const auto render = [&](const std::string& commit, const std::string& path) {
    ckgit::ProjectSummary project;
    project.name = "tree-limits"; project.default_branch = "main";
    project.valid_head = true; project.head_id = commit; project.branch_count = 1;
    return ckgit::renderDashboard(ckgit::parseHttpRoute("/project/tree-limits/tree/" + commit + ":" + path),
                                  project, repository).body;
  };
  const auto empty_commit = commitTree(makeTree(repository, input, ""));
  require(ckgit::WebRepository(repository).fileTree(empty_commit).empty(), "a committed empty tree has no phantom children");
  require(render(empty_commit, "").find("No README") != std::string::npos, "an empty committed tree remains browsable");
  std::string records;
  for (std::size_t i = 0; i < ckgit::kMaximumTreeEntries; ++i) {
    const auto name = std::to_string(10000 + i);
    records += "100644 blob " + blob + "\tf" + name + "\n";
  }
  const auto maximum_commit = commitTree(makeTree(repository, input, records));
  require(ckgit::WebRepository(repository).fileTree(maximum_commit).size() == ckgit::kMaximumTreeEntries,
          "the recursive file tree accepts its exact 5000-entry boundary");
  records += "100644 blob " + blob + "\toverflow\n";
  const auto too_wide_commit = commitTree(makeTree(repository, input, records));
  treeRejected(too_wide_commit, "a single directory above 5000 entries is bounded");
  bool rendering_rejected = false;
  try { static_cast<void>(render(too_wide_commit, "")); }
  catch (const ckgit::WebError& error) { rendering_rejected = error.status == 503; }
  require(rendering_rejected, "a too-wide directory also remains bounded during fallback rendering");

  records.clear();
  for (std::size_t i = 0; i < ckgit::kMaximumTreeEntries / 2; ++i)
    records += "100644 blob " + blob + "\tf" + std::to_string(10000 + i) + "\n";
  const auto half = makeTree(repository, input, records);
  const auto aggregate = commitTree(makeTree(repository, input,
      "040000 tree " + half + "\ta\n040000 tree " + half + "\tb\n"));
  treeRejected(aggregate, "the aggregate cap includes children of unopened sibling folders");
  const auto fallback_page = render(aggregate, "a");
  const auto fallback = fileNavigation(fallback_page);
  require(fallback_page.find("Large repository:") != std::string::npos &&
          fallback.find("Open folder to load contents") != std::string::npos &&
          fallback.find("/project/tree-limits/blob/" + aggregate + ":a/f10000") != std::string::npos &&
          fallback.find("/project/tree-limits/tree/" + aggregate + ":b") != std::string::npos,
          "large repositories preserve the current folder and actionable sibling navigation");

  // Long paths can exceed the byte budget while both entry count and depth are
  // valid. Git plumbing creates this case without long filesystem paths.
  records.clear();
  for (std::size_t i = 0; i < 1000; ++i)
    records += "100644 blob " + blob + "\tf" + std::to_string(10000 + i) + "\n";
  auto long_output_tree = makeTree(repository, input, records);
  for (std::size_t depth = 1; depth < ckgit::kMaximumTreeDepth; ++depth)
    long_output_tree = makeTree(repository, input, "040000 tree " + long_output_tree + "\t" + std::string(200, 'd') + "\n");
  const auto too_many_bytes = commitTree(long_output_tree);
  treeRejected(too_many_bytes, "recursive listing output is bounded independently of count and depth", "output");
  require(render(too_many_bytes, "").find("Open folder to load contents") != std::string::npos,
          "byte-limit fallback still permits browsing the repository root");

  // These 1510 entries use about 3 MiB in Git's recursive listing, but encoding
  // '#' in each 200-character folder component expands leaf URLs threefold.
  // Their full sidebar would exceed 4 MiB despite valid count, depth, and Git
  // output, so rendering must retry with the current folder's bounded listing.
  records.clear();
  for (std::size_t i = 0; i < 1500; ++i)
    records += "100644 blob " + blob + "\tf" + std::to_string(10000 + i) + "\n";
  auto expanded_html_tree = makeTree(repository, input, records);
  const std::string encoded_folder(200, '#');
  for (std::size_t depth = 0; depth < 10; ++depth)
    expanded_html_tree = makeTree(repository, input, "040000 tree " + expanded_html_tree + "\t" + encoded_folder + "\n");
  const auto expanded_html_commit = commitTree(expanded_html_tree);
  require(ckgit::WebRepository(repository).fileTree(expanded_html_commit).size() == 1510,
          "HTML expansion fixture remains within every recursive Git listing limit");
  const auto html_fallback_page = render(expanded_html_commit, "");
  const auto html_fallback = fileNavigation(html_fallback_page);
  require(html_fallback_page.find("Large repository:") != std::string::npos &&
          html_fallback.find("Open folder to load contents") != std::string::npos &&
          html_fallback.find("/project/tree-limits/tree/" + expanded_html_commit + ":" +
                             ckgit::encodePathSegment(encoded_folder)) != std::string::npos &&
          html_fallback.size() < 4 * 1024 * 1024,
          "encoded sidebar growth falls back once while preserving a usable repository root");

  auto deep_tree = makeTree(repository, input, "100644 blob " + blob + "\tleaf.txt\n");
  for (std::size_t depth = 1; depth < ckgit::kMaximumTreeDepth; ++depth)
    deep_tree = makeTree(repository, input, "040000 tree " + deep_tree + "\td\n");
  const auto deepest = commitTree(deep_tree);
  require(ckgit::WebRepository(repository).fileTree(deepest).size() == ckgit::kMaximumTreeDepth,
          "a file at the 32-component depth boundary remains expandable");
  const auto too_deep = commitTree(makeTree(repository, input, "040000 tree " + deep_tree + "\td\n"));
  treeRejected(too_deep, "a 33-component entry cannot create an unbounded nested HTML tree");
  require(render(too_deep, "").find("Open folder to load contents") != std::string::npos,
          "an over-deep sibling does not make the repository root unavailable");
}
}
void testDashboard() {
  const char* temp = std::getenv("TMPDIR");
  const char* configured = std::getenv("CKGIT_TEST_ROOT");
  const std::string approved = configured ? configured : "/Volumes/PRO-BLADE/tmp";
  require(temp && std::string(temp).starts_with(approved + "/"), "approved TMPDIR required");
  const auto root = std::filesystem::path(temp) / ("dashboard-" + std::to_string(getpid()));
  std::filesystem::create_directory(root);
  struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code e; std::filesystem::remove_all(path,e); } } cleanup{root};
  auto work = root / "work"; auto repos = root / "repos";
  std::filesystem::create_directory(repos);
  git(root, {"init", "-q", "--initial-branch=main", work.string()});
  git(work, {"config", "user.name", "<script>Author</script>"});
  git(work, {"config", "user.email", "test@example.test"});
  std::filesystem::create_directory(work / "docs");
  std::filesystem::create_directories(work / "docs/nested");
  std::filesystem::create_directories(work / "src/internal");
  std::filesystem::create_directories(work / "configuration");
  write(work / "README.md", "# Welcome\n\n<script>unsafe()</script>\n\n![Diagram](diagram.svg)\n\n[Doc](docs/README.md)\n");
  write(work / "docs/README.md", "---\ntitle: Directory\nnav_order: 2\n---\n# Directory\n\n**Strong** paragraph.\n");
  write(work / "docs/nested/guide.md", "# Nested guide\n\nVisible below an expandable ancestor.\n");
  write(work / "src/internal/engine.cpp", "int engine() { return 42; }\n");
  write(work / "configuration/settings.ini", "enabled=true\n");
  write(work / "release.zip", "archive icon fixture\n");
  write(work / "diagram.svg", "<svg onload=\"alert(1)\"><script>RAW_ONLY</script><foreignObject>unsafe</foreignObject></svg>");
  write(work / "ü #%.txt", "first\n<script>second</script>\n");
  write(work / "binary.dat", std::string(9000,'a') + std::string(1,'\0'));
  write(work / "large.png", std::string(ckgit::kMaximumInlineImageBytes + 1, 'x'));
  write(work / "many-lines.txt", std::string(200000, '\n'));
  write(work / "large.txt", std::string(ckgit::kMaximumPreviewBytes + 1, 'z'));
  std::filesystem::create_symlink("/etc/passwd", work / "link.txt");
  git(work, {"add", "."}); git(work, {"commit", "-q", "-m", "<script>Initial</script>"}, "2024-02-28T10:00:00Z");
  auto initial = git(work, {"rev-parse", "HEAD"});
  git(work, {"update-index", "--add", "--cacheinfo", "160000," + initial + ",vendor"});
  git(work, {"commit", "-q", "-m", "Submodule"});
  git(work, {"branch", "side"});
  for (int i = 0; i < 53; ++i) git(work, {"commit", "-q", "--allow-empty", "-m", "Main " + std::to_string(i)});
  git(work, {"checkout", "-q", "side"});
  for (int i = 0; i < 4; ++i) git(work, {"commit", "-q", "--allow-empty", "-m", "Side " + std::to_string(i)});
  git(work, {"checkout", "-q", "main"});
  git(work, {"merge", "-q", "--no-ff", "side", "-m", "Merge side"}, "2024-03-01T00:00:00Z");
  git(work, {"tag", "main"});
  git(root, {"clone", "-q", "--bare", work.string(), (repos / "demo.git").string()});
  ckgit::ProjectIndex index(repos); index.sweep(); const auto project = index.find("demo");
  require(project && project->index_error.empty(), "project indexed");
  ckgit::WebRepository reader(repos / "demo.git");
  const auto resolved = reader.resolve("main");
  require(resolved.ambiguous && resolved.name == "heads/main", "branch/tag collision notice");
  require(reader.resolve("tags/main").id == resolved.id, "explicit tag");
  const auto commit = reader.commit(resolved.id);
  require(commit.parents.size() == 2 && commit.subject == "Merge side", "merge commit parse");
  auto first = reader.commits(resolved.id); require(first.commits.size() == 50 && first.has_more, "first page 50");
  auto second = reader.commits(resolved.id, first.commits.back().id);
  require(second.commits.size() == 10 && !second.has_more, "second page includes merged side history");
  std::set<std::string> all;
  for (const auto& c : first.commits) require(all.insert(c.id).second, "first-page uniqueness");
  for (const auto& c : second.commits) require(all.insert(c.id).second, "cross-page uniqueness");
  require(all.size() == 60, "complete DAG pagination");
  require(reader.asOf(resolved.id, "2020-01-01").empty(), "before first commit has no tree");
  require(reader.asOf(resolved.id, "2024-02-28") == initial, "historic first-parent tree");
  require(reader.day(resolved.id, "2024-03-01").commits.size() == 1, "UTC day bounds");
  require(reader.activity(resolved.id).counts.at("2024-02-29") == 58, "leap-day count");
  const auto page = [&](const std::string& suffix) {
    const auto route = ckgit::parseHttpRoute("/project/demo/" + suffix);
    require(route.kind != ckgit::RouteKind::kNotFound, "valid fixture route " + suffix);
    return ckgit::renderDashboard(route, *project, repos / "demo.git");
  };
  auto tree = page("tree/" + resolved.id + ":").body;
  require(tree.find("Welcome") != std::string::npos && tree.find("<script>") == std::string::npos, "README renders escaped");
  require(tree.find("submodule") != std::string::npos && tree.find("symlink") != std::string::npos, "special tree entries labeled");
  const auto directory_readme = page("tree/" + resolved.id + ":docs").body;
  require(directory_readme.find("<strong>Strong</strong>") != std::string::npos, "directory README");
  require(directory_readme.find("title: Directory") == std::string::npos && directory_readme.find("<hr>") == std::string::npos,
          "a README's front matter is kept out of its rendered view");
  auto text = page("blob/" + resolved.id + ":" + ckgit::encodePathSegment("ü #%.txt")).body;
  require(text.find("id=\"L2\"") != std::string::npos && text.find("&lt;script&gt;second") != std::string::npos, "line numbers escape content");
  auto svg = page("blob/" + resolved.id + ":diagram.svg").body;
  require(svg.find("<img src=") != std::string::npos && svg.find("RAW_ONLY") == std::string::npos, "SVG isolated as image");
  auto raw = page("raw/" + resolved.id + ":diagram.svg");
  require(raw.raw && raw.content_type == "image/svg+xml" && raw.body.find("RAW_ONLY") != std::string::npos, "raw exact SVG bytes");
  require(page("blob/" + resolved.id + ":large.png").body.find("<img ") == std::string::npos, "large image never embedded");
  require(page("blob/" + resolved.id + ":large.txt").body.find("512 KiB") != std::string::npos, "bounded text preview");
  require(page("blob/" + resolved.id + ":binary.dat").body.find("Binary file") != std::string::npos, "NUL after 8KiB remains binary");
  bool rendering_limited = false;
  try { static_cast<void>(page("blob/" + resolved.id + ":many-lines.txt")); }
  catch (const ckgit::WebError& e) { rendering_limited = e.status == 503; }
  require(rendering_limited, "HTML expansion is bounded for a tiny-line file");
  auto symlink = page("blob/" + resolved.id + ":link.txt").body;
  require(symlink.find("/etc/passwd") != std::string::npos && symlink.find("root:") == std::string::npos, "symlink target displayed never read");
  require(page("commit/" + initial).body.find("Diff truncated at 512 KiB") != std::string::npos, "bounded root diff visible notice");
  require(page("commits/" + resolved.id).body.find("<svg ") != std::string::npos, "history inline graph");
  require(page("calendar/" + resolved.id + "/2024/02").body.find("2024-02-29") != std::string::npos, "calendar leap day");
  require(page("day/" + resolved.id + "/2020-01-01").body.find("No tree existed") != std::string::npos, "day before history");
  const auto named = page("tree/tags/main:docs").body;
  require(named.find("Tag main") != std::string::npos && named.find("/project/demo/blob/tags/main:docs/README.md") != std::string::npos,
          "tag identity retained in tree navigation");
  require(named.find("/project/demo/commits/tags/main") != std::string::npos && named.find("/project/demo/overview/tags/main") != std::string::npos,
          "section navigation retains chosen tag");
  const auto expanded = fileNavigation(page("blob/tags/main:docs/nested/guide.md").body);
  require(folderOpening(expanded, "/project/demo/tree/tags/main:docs").find(" open") != std::string::npos &&
          folderOpening(expanded, "/project/demo/tree/tags/main:docs/nested").find(" open") != std::string::npos,
          "every selected ancestor starts expanded");
  require(folderOpening(expanded, "/project/demo/tree/tags/main:src").find(" open") == std::string::npos &&
          folderOpening(expanded, "/project/demo/tree/tags/main:src/internal").find(" open") == std::string::npos &&
          expanded.find("/project/demo/blob/tags/main:src/internal/engine.cpp") != std::string::npos,
          "unselected folders start collapsed with their actual children available without JavaScript");
  require(occurrences(expanded, " selected\"") == 1,
          "selection highlights exactly the current row, not its ancestor subtrees");
  require(occurrences(expanded, "aria-current=\"page\"") == 1,
          "only the current file link announces the selected page");
  for (const auto* category : {"folder", "code", "markdown", "config", "image", "archive", "file", "link", "submodule"})
    require(expanded.find(std::string("tree-icon icon-") + category) != std::string::npos,
            std::string("classic file tree has an inline icon for ") + category);
  require(expanded.find("Open folder to load contents") == std::string::npos,
          "ordinary repositories contain full expandable children rather than placeholders");
  require(expanded.find("link.txt/") == std::string::npos && expanded.find("vendor/") == std::string::npos,
          "symlinks and submodules stay leaves rather than traversable folders");
  require(tree.find("Commit " + resolved.id.substr(0, 8)) != std::string::npos, "shared commit never relabeled with a branch name");
  const auto rendered_doc = page("blob/heads/main:docs/README.md").body;
  require(rendered_doc.find("<strong>Strong</strong>") != std::string::npos && rendered_doc.find("id=\"directory\"") != std::string::npos,
          "linked Markdown renders with heading targets");
  require(rendered_doc.find("/project/demo/source/heads/main:docs/README.md") != std::string::npos, "Markdown source mode link keeps branch");
  require(rendered_doc.find("title: Directory") == std::string::npos && rendered_doc.find("<hr>") == std::string::npos,
          "a Markdown file's front matter is kept out of its rendered view");
  const auto source_doc = page("source/heads/main:docs/README.md").body;
  require(source_doc.find("id=\"L1\"") != std::string::npos && source_doc.find("**Strong**") != std::string::npos &&
          source_doc.find("Wrap lines") != std::string::npos, "source mode includes line navigation and wrapping");
  require(source_doc.find("title: Directory") != std::string::npos, "source mode still shows the front matter");
  const auto directory_redirect = page("blob/tags/main:docs");
  require(directory_redirect.status == 302 && directory_redirect.location == "/project/demo/tree/tags/main:docs", "directory without slash redirects preserving tag");
  const auto selected_overview = page("overview/heads/side").body;
  require(selected_overview.find("Branch side") != std::string::npos && selected_overview.find("Side 3") != std::string::npos &&
          selected_overview.find("Latest default branch") != std::string::npos, "overview uses selected commit and offers deliberate reset");
  const auto historic_calendar = page("calendar/tags/main/2024/02").body;
  require(historic_calendar.find("/project/demo/calendar/heads/side/2024/02") != std::string::npos &&
          historic_calendar.find("/project/demo/day/tags/main/2024-02-29") != std::string::npos, "calendar picker preserves month and day links retain tag");
  const auto unavailable_route = ckgit::parseHttpRoute("/project/demo/blob/tags/main:docs/missing.md");
  const auto missing = ckgit::renderDashboardError(unavailable_route, *project, 404, "Path was not found.");
  require(missing.status == 404 && missing.body.find("Tag main") != std::string::npos &&
          missing.body.find("/project/demo/tree/tags/main:docs") != std::string::npos, "missing content retains recovery context");
  const auto transient = ckgit::renderDashboardError(unavailable_route, *project, 503, "Please retry.");
  require(transient.status == 503 && transient.body.find("Reload this page to retry") != std::string::npos, "transient recovery differs from missing path");
  ckgit::ProjectSummary empty; empty.name = "empty"; empty.default_branch = "main";
  const auto empty_body = ckgit::renderDashboard(ckgit::parseHttpRoute("/project/empty/tree/heads/main:"), empty, repos / "does-not-exist").body;
  require(empty_body.find("No commits published yet") != std::string::npos && empty_body.find("href=\"/project/empty/tree/") == std::string::npos,
          "empty project needs no git read and omits unusable sections");
  empty.indexing = true;
  require(ckgit::renderProjectDetail(empty).find("No commits published yet") == std::string::npos, "indexing is not mistaken for empty");
  empty.indexing = false; empty.index_error = "index failed";
  require(ckgit::renderProjectDetail(empty).find("Index temporarily unavailable") != std::string::npos, "index failure has distinct recovery");
  auto missing_default = *project; missing_default.valid_head = false; missing_default.head_id.clear();
  require(ckgit::renderProjectDetail(missing_default).find("default branch is unavailable") != std::string::npos &&
          ckgit::renderProjectDetail(missing_default).find("No commits published yet") == std::string::npos, "missing default still offers existing refs");
  ckgit::ActivityData data;
  require(ckgit::renderCalendarGrid("demo", resolved.id, 2023, 2, data).find("2023-02-29") == std::string::npos, "ordinary February");
  auto binary_readme = ckgit::renderReadme(*project, resolved.id, "README.md", std::string("a\0b",3));
  require(binary_readme.find("binary") != std::string::npos, "binary README handled");
  require(ckgit::rawContentType("bad.unknown") == "application/octet-stream" && ckgit::rawContentType("x.html") == "text/plain; charset=utf-8", "raw whitelist");
  std::vector<ckgit::WebCommit> graph(4);
  graph[0].id = "a"; graph[0].parents = {"b", "c", "d"}; graph[1].id = "b"; graph[1].parents = {"d"}; graph[2].id = "c"; graph[2].parents = {"d"}; graph[3].id = "d";
  auto rows = ckgit::renderGraphRows(graph);
  require(rows.size() == 4 && rows.front().find("width=\"46\"") != std::string::npos && rows.back().find("width=\"18\"") != std::string::npos, "octopus lanes converge");
  require(graphBoundaries(rows.front()).first.empty(), "a new graph tip has no invented incoming stem");
  for (std::size_t i = 0; i + 1 < rows.size(); ++i)
    require(graphBoundaries(rows[i]).second == graphBoundaries(rows[i + 1]).first,
            "every merge lane meets the next row at the identical boundary coordinate");
  require(graphBoundaries(rows.back()).second.empty(), "the root commit has no dangling outgoing stem");
  for (const auto& row : rows) {
    const auto split = row.find("</svg>");
    require(split != std::string::npos, "graph has an edge layer");
    const auto edges = row.substr(0, split + 6);
    const auto node = row.substr(split + 6);
    require(edges.find("class=\"graph graph-lines\"") != std::string::npos &&
            edges.find("preserveAspectRatio=\"none\"") != std::string::npos && edges.find("<circle") == std::string::npos &&
            occurrences(edges, "<path ") == occurrences(edges, "vector-effect=\"non-scaling-stroke\""),
            "only edge geometry stretches with a tall row, with constant stroke width");
    require(node.find("class=\"graph graph-node\"") != std::string::npos &&
            node.find("height=\"48\"") != std::string::npos && occurrences(node, "<circle ") == 1 &&
            node.find("<path ") == std::string::npos && node.find("preserveAspectRatio=\"none\"") == std::string::npos,
            "the fixed-size node layer keeps one round commit marker");
  }
  const auto page_boundary = ckgit::renderGraphRows({graph.front()});
  require(graphBoundaries(page_boundary.front()).second.size() == 3,
          "parents beyond the displayed page retain continuation lanes");
  ckgit::WebCommit unrelated; unrelated.id = "unrelated-root";
  const auto independent = ckgit::renderGraphRows({graph.front(), unrelated});
  require(graphBoundaries(independent.front()).second == graphBoundaries(independent.back()).first,
          "introducing an unrelated root does not invent another incoming lane");
  testClassicTreeLimits(root);
}
