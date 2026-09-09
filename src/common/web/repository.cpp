// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "ckgit/web_repository.hpp"
#include <algorithm>
#include <charconv>
#include <cctype>
#include <ctime>
#include <sstream>
#include "ckgit/http_router.hpp"
#include "ckgit/validation.hpp"

namespace ckgit {
namespace {
constexpr std::size_t kHistoryLimit = 200000;
std::string trim(std::string s) {
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
  return s;
}
std::vector<std::string> split(std::string_view s, char separator) {
  std::vector<std::string> result;
  for (std::size_t i = 0; i < s.size();) {
    auto end = s.find(separator, i);
    if (end == std::string_view::npos) end = s.size();
    result.emplace_back(s.substr(i, end - i));
    i = end + 1;
  }
  return result;
}
std::int64_t number(std::string_view s) {
  std::int64_t n{};
  auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), n);
  if (ec != std::errc{} || end != s.data() + s.size()) throw WebError(503, "Invalid Git numeric data.");
  return n;
}
std::vector<WebCommit> parseCommits(std::string_view data) {
  std::vector<WebCommit> result;
  // -z terminates records; the seven internal fields are also NUL-delimited.
  while (!data.empty()) {
    while (!data.empty() && (data.front() == '\n' || data.front() == '\0')) data.remove_prefix(1);
    if (data.empty()) break;
    std::vector<std::string> fields;
    for (int i = 0; i < 8; ++i) {
      auto end = data.find('\0');
      if (end == std::string_view::npos) throw WebError(503, "Incomplete commit data.");
      fields.emplace_back(data.substr(0, end));
      data.remove_prefix(end + 1);
    }
    if (!isObjectId(fields[0])) throw WebError(503, "Invalid commit data.");
    WebCommit commit;
    commit.id = fields[0]; commit.parents = split(fields[1], ' ');
    if (commit.parents.size() > kMaximumCommitParents) throw WebError(503, "Commit exceeds the 128-parent viewing limit.");
    for (const auto& parent : commit.parents) if (!isObjectId(parent)) throw WebError(503, "Invalid parent.");
    commit.author = fields[2]; commit.epoch = number(fields[3]); commit.subject = fields[4];
    commit.committer = fields[5]; commit.author_epoch = number(fields[6]); commit.message = fields[7];
    result.push_back(std::move(commit));
  }
  return result;
}
const std::string kFormat = "--format=%H%x00%P%x00%an%x00%ct%x00%s%x00%cn%x00%at%x00%B%x00";
std::string lower(std::string s) {
  for (char& c : s) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
  return s;
}
}  // namespace

WebRepository::WebRepository(std::filesystem::path repository, std::chrono::steady_clock::time_point deadline)
    : repository_(std::move(repository)), deadline_(deadline) {
  std::error_code error;
  auto status = std::filesystem::symlink_status(repository_, error);
  if (error || !std::filesystem::is_directory(status) || std::filesystem::is_symlink(status))
    throw WebError(404, "Repository is no longer available.");
}
ProcessResult WebRepository::git(std::vector<std::string> arguments, std::size_t limit,
                                 bool allow_failure, bool allow_truncation) const {
  const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline_ - std::chrono::steady_clock::now());
  if (remaining.count() <= 0) throw WebError(503, "Repository request exceeded its time limit.");
  std::vector<std::string> command{"git", "--no-optional-locks", "--git-dir", repository_.string(),
      "-c", "core.quotePath=false", "-c", "core.commitGraph=false", "-c", "log.showSignature=false"};
  command.insert(command.end(), arguments.begin(), arguments.end());
  auto result = runProcess(command, std::min(remaining, std::chrono::milliseconds(10000)), limit);
  if (result.timed_out || (result.output_truncated && !allow_truncation))
    throw WebError(503, "Repository data exceeded the time or output limit.");
  if (result.exit_code != 0 && !allow_failure) throw WebError(503, "Repository data could not be read.");
  return result;
}
ResolvedRef WebRepository::resolve(const std::string& ref) const {
  if (!isObjectId(ref) && !isValidBranchName(ref)) throw WebError(404, "Invalid ref.");
  auto resolveOne = [&](const std::string& name) {
    auto r = git({"rev-parse", "--verify", "--end-of-options", name + "^{commit}"}, 4096, true);
    auto id = trim(r.output);
    return r.exit_code == 0 && isObjectId(id) ? id : std::string{};
  };
  if (isObjectId(ref) || ref.rfind("heads/", 0) == 0 || ref.rfind("tags/", 0) == 0) {
    auto id = resolveOne(isObjectId(ref) ? ref : "refs/" + ref);
    if (id.empty()) throw WebError(404, "Ref or commit was not found.");
    return {id, ref, false};
  }
  const auto branch = resolveOne("refs/heads/" + ref), tag = resolveOne("refs/tags/" + ref);
  if (branch.empty() && tag.empty()) throw WebError(404, "Branch or tag was not found.");
  return {branch.empty() ? tag : branch, (branch.empty() ? "tags/" : "heads/") + ref,
          !branch.empty() && !tag.empty()};
}
std::vector<TreeEntry> WebRepository::tree(const std::string& id, const std::string& path) const {
  if (!isObjectId(id)) throw WebError(404, "Invalid commit.");
  auto result = git({"ls-tree", "-z", "--long", id + ":" + path}, 4 * 1024 * 1024, true);
  if (result.exit_code != 0) throw WebError(404, "Directory was not found.");
  std::vector<TreeEntry> entries;
  for (const auto& line : split(result.output, '\0')) {
    auto tab = line.find('\t');
    if (tab == std::string::npos) throw WebError(503, "Invalid tree data.");
    TreeEntry entry;
    std::string size;
    std::istringstream fields(line.substr(0, tab));
    fields >> entry.mode >> entry.type >> entry.id >> size;
    if (!isObjectId(entry.id) || size.empty()) throw WebError(503, "Invalid tree entry.");
    entry.name = line.substr(tab + 1);
    entry.size = size == "-" ? 0 : static_cast<std::uint64_t>(number(size));
    entries.push_back(std::move(entry));
    if (entries.size() > kMaximumTreeEntries) throw WebError(503, "Directory exceeds the 5000-entry limit.");
  }
  std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
    if ((a.type == "tree") != (b.type == "tree")) return a.type == "tree";
    return a.name < b.name;
  });
  return entries;
}
std::vector<TreeEntry> WebRepository::fileTree(const std::string& id) const {
  if (!isObjectId(id)) throw WebError(404, "Invalid commit.");
  const auto result = git({"ls-tree", "-r", "-t", "-z", id}, 4 * 1024 * 1024, false, true);
  if (result.output_truncated) throw TreeLimitError("File tree exceeds the sidebar output limit.");
  std::vector<TreeEntry> entries;
  for (const auto& line : split(result.output, '\0')) {
    const auto tab = line.find('\t');
    if (tab == std::string::npos) throw WebError(503, "Invalid tree data.");
    TreeEntry entry;
    std::istringstream fields(line.substr(0, tab));
    fields >> entry.mode >> entry.type >> entry.id;
    if (!isObjectId(entry.id) || entry.mode.empty() ||
        (entry.type != "tree" && entry.type != "blob" && entry.type != "commit"))
      throw WebError(503, "Invalid tree entry.");
    entry.name = line.substr(tab + 1);
    if (static_cast<std::size_t>(std::count(entry.name.begin(), entry.name.end(), '/')) + 1 > kMaximumTreeDepth)
      throw TreeLimitError("File tree exceeds the 32-level sidebar limit.");
    entries.push_back(std::move(entry));
    if (entries.size() > kMaximumTreeEntries) throw TreeLimitError("File tree exceeds the 5000-entry sidebar limit.");
  }
  return entries;
}
TreeEntry WebRepository::entry(const std::string& id, const std::string& path) const {
  const auto slash = path.rfind('/');
  auto entries = tree(id, slash == std::string::npos ? "" : path.substr(0, slash));
  const auto name = slash == std::string::npos ? path : path.substr(slash + 1);
  auto found = std::find_if(entries.begin(), entries.end(), [&](const auto& e) { return e.name == name; });
  if (found == entries.end()) throw WebError(404, "File was not found.");
  return *found;
}
std::string WebRepository::blob(const TreeEntry& entry, std::size_t limit) const {
  if (entry.type != "blob" || !isObjectId(entry.id)) throw WebError(404, "This entry is not a file.");
  if (entry.size > limit) throw WebError(413, "File exceeds this view's size limit.");
  auto result = git({"cat-file", "blob", entry.id}, limit);
  if (result.output.size() != entry.size) throw WebError(503, "Incomplete file data.");
  return result.output;
}
WebCommit WebRepository::commit(const std::string& id) const {
  auto commits = parseCommits(git({"show", "-s", "--no-notes", kFormat, "--end-of-options", id}, 1024 * 1024).output);
  if (commits.size() != 1) throw WebError(404, "Commit was not found.");
  return commits.front();
}
CommitPage WebRepository::commits(const std::string& id, const std::string& cursor) const {
  std::size_t skip = 0;
  if (!cursor.empty()) {
    // A cursor's own ancestry omits the other side of a merge. Locate it in
    // the pinned tip's bounded date-order traversal before selecting a page.
    auto ids = split(git({"rev-list", "--date-order", "--max-count=" + std::to_string(kHistoryLimit + 1), id, "--"},
                         (kHistoryLimit + 1) * 65).output, '\n');
    auto found = std::find(ids.begin(), ids.end(), cursor);
    if (found == ids.end()) throw WebError(404, "Paging cursor is not in this history.");
    if (static_cast<std::size_t>(found - ids.begin()) >= kHistoryLimit)
      throw WebError(503, "History exceeds the 200000-commit paging limit.");
    skip = static_cast<std::size_t>(found - ids.begin()) + 1;
  }
  if (skip + kCommitPageSize > kHistoryLimit) throw WebError(503, "History exceeds the 200000-commit paging limit.");
  auto rows = parseCommits(git({"log", "--date-order", "--no-notes", "-n", "51", "--skip=" + std::to_string(skip),
                                kFormat, "--end-of-options", id, "--"}, 1024 * 1024).output);
  bool more = rows.size() > kCommitPageSize;
  if (more) rows.resize(kCommitPageSize);
  return {std::move(rows), more};
}
std::string utcDate(std::int64_t epoch) {
  auto t = static_cast<std::time_t>(epoch); std::tm utc{}; char buffer[32]{};
  if (!gmtime_r(&t, &utc) || !std::strftime(buffer, sizeof(buffer), "%Y-%m-%d", &utc)) return {};
  return buffer;
}
ActivityData WebRepository::activity(const std::string& id) const {
  auto rows = split(git({"log", "--format=%ct", "--max-count=" + std::to_string(kHistoryLimit + 1),
                         "--end-of-options", id, "--"}, 4 * 1024 * 1024).output, '\n');
  ActivityData data; data.truncated = rows.size() > kHistoryLimit;
  if (data.truncated) rows.resize(kHistoryLimit);
  for (const auto& epoch : rows) ++data.counts[utcDate(number(epoch))];
  return data;
}
CommitPage WebRepository::day(const std::string& id, const std::string& date) const {
  auto rows = parseCommits(git({"log", "--date-order", "--no-notes", "--since-as-filter=" + date + "T00:00:00Z",
      "--until=" + date + "T23:59:59Z", "-n", "201", kFormat, "--end-of-options", id, "--"}, 4 * 1024 * 1024).output);
  const bool more = rows.size() > kMaximumDayCommits;
  if (more) rows.resize(kMaximumDayCommits);
  return {std::move(rows), more};
}
std::string WebRepository::asOf(const std::string& id, const std::string& date) const {
  // Walk the first-parent timeline: a side branch committed earlier but
  // merged later must not become the project's historic tree.
  return trim(git({"rev-list", "--first-parent", "-1", "--before=" + date + "T23:59:59Z", id, "--"}, 4096).output);
}
ProcessResult WebRepository::diff(const std::string& id, bool stat) const {
  auto c = commit(id);
  std::vector<std::string> args{"diff-tree", "--root", "--no-commit-id", "-r", "--no-ext-diff", "--no-textconv", "--no-color"};
  if (stat) { args.push_back("--stat"); args.push_back("--stat-width=100"); }
  else args.push_back("-p");
  if (!c.parents.empty()) args.push_back(c.parents.front());
  args.push_back(id); args.push_back("--");
  return git(std::move(args), stat ? 128 * 1024 : kMaximumDiffBytes, false, !stat);
}
std::string rawContentType(const std::string& path) {
  const auto dot = path.rfind('.'); auto extension = dot == std::string::npos ? "" : lower(path.substr(dot));
  if (extension == ".png") return "image/png";
  if (extension == ".jpg" || extension == ".jpeg") return "image/jpeg";
  if (extension == ".gif") return "image/gif";
  if (extension == ".webp") return "image/webp";
  if (extension == ".svg") return "image/svg+xml";
  for (auto e : {".txt", ".md", ".markdown", ".cpp", ".hpp", ".c", ".h", ".py", ".js", ".ts", ".css", ".html", ".xml", ".json", ".yml", ".yaml", ".sh", ".ini", ".toml", ".rs", ".go"})
    if (extension == e) return "text/plain; charset=utf-8";
  return "application/octet-stream";
}
bool isImageType(const std::string& type) { return type.rfind("image/", 0) == 0; }
bool isTextBlob(const std::string& content) {
  return content.find('\0') == std::string::npos && isValidUtf8(content);
}
std::string chooseReadme(const std::vector<TreeEntry>& entries) {
  for (auto name : {"readme.md", "readme.markdown", "readme", "readme.txt"})
    for (const auto& entry : entries)
      if (entry.type == "blob" && entry.mode != "120000" && lower(entry.name) == name) return entry.name;
  return {};
}
}  // namespace ckgit
