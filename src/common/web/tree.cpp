// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "tree.hpp"
#include <algorithm>
#include <initializer_list>
#include <map>
#include <set>
#include "ckgit/http_router.hpp"
#include "ckgit/web_renderer.hpp"
namespace ckgit {
namespace {
std::string parent(const std::string& path) {
  const auto slash = path.rfind('/');
  return slash == std::string::npos ? "" : path.substr(0, slash);
}
std::string basename(const std::string& path) {
  const auto slash = path.rfind('/');
  return slash == std::string::npos ? path : path.substr(slash + 1);
}
std::string iconKind(const TreeEntry& entry) {
  if (entry.type == "tree") return "folder";
  if (entry.type == "commit") return "submodule";
  if (entry.mode == "120000") return "link";
  auto name = basename(entry.name);
  for (auto& c : name) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
  const auto dot = name.rfind('.');
  const auto extension = dot == std::string::npos ? "" : name.substr(dot);
  const auto oneOf = [&](std::initializer_list<const char*> extensions) {
    return std::any_of(extensions.begin(), extensions.end(), [&](const auto* item) { return extension == item; });
  };
  if (oneOf({".md", ".markdown", ".rst", ".adoc"})) return "markdown";
  if (oneOf({".svg", ".png", ".jpg", ".jpeg", ".gif", ".webp", ".avif", ".ico", ".bmp", ".tiff"})) return "image";
  if (oneOf({".zip", ".gz", ".tgz", ".bz2", ".xz", ".tar", ".7z", ".rar", ".deb", ".dmg", ".pkg"})) return "archive";
  if (oneOf({".json", ".yml", ".yaml", ".toml", ".ini", ".conf", ".config", ".xml", ".plist", ".properties"}) ||
      name.starts_with(".git") || name == ".editorconfig" || name == ".env" || name.starts_with(".env.") ||
      name == "makefile" || name == "dockerfile" || name == "cmakelists.txt") return "config";
  if (oneOf({".c", ".cpp", ".cc", ".cxx", ".h", ".hpp", ".m", ".mm", ".swift", ".py", ".js", ".jsx", ".ts", ".tsx", ".rs", ".go", ".java", ".kt", ".rb", ".php", ".sh", ".bash", ".zsh", ".html", ".css", ".scss", ".sql", ".vue", ".svelte"})) return "code";
  return "file";
}
std::string icon(const std::string& kind) {
  std::string shape;
  if (kind == "folder") {
    shape = "<g class=\"folder-closed\"><path d=\"M3 6h7l2 2h9v12H3z\"/></g>"
            "<g class=\"folder-open\"><path d=\"M3 6h7l2 2h8v3H3z\"/><path d=\"M3 11h19l-3 9H3z\"/></g>";
  } else if (kind == "submodule") {
    shape = "<path d=\"m12 3 9 5v9l-9 5-9-5V8zm-9 5 9 5 9-5M12 13v9m-4-17 9 5\"/>";
  } else if (kind == "link") {
    shape = "<path d=\"m9 15 6-6m-5-3 2-2a4 4 0 0 1 6 6l-2 2m-2 6-2 2a4 4 0 0 1-6-6l2-2\"/>";
  } else {
    shape = "<path d=\"M5 3h9l5 5v13H5zM14 3v5h5\"/>";
    if (kind == "code") shape += "<path d=\"m10 11-3 3 3 3m4-6 3 3-3 3\"/>";
    else if (kind == "markdown") shape += "<path d=\"M8 17v-6l4 4 4-4v6\"/>";
    else if (kind == "image") shape += "<circle cx=\"9\" cy=\"11\" r=\"1\"/><path d=\"m7 18 4-4 2 2 2-3 3 5\"/>";
    else if (kind == "config") shape += "<circle cx=\"12\" cy=\"14\" r=\"2.5\"/><path d=\"M12 9v2m0 6v2m-5-5h2m6 0h2m-8.5-3.5 1.5 1.5m4 4 1.5 1.5m0-7L14 12m-4 4-1.5 1.5\"/>";
    else if (kind == "archive") shape += "<path d=\"M11 9h2v2h-2zm0 4h2v2h-2zm0 4h2v2h-2z\"/>";
    else shape += "<path d=\"M8 11h5m-5 4h8m-8 3h6\"/>";
  }
  return "<svg class=\"tree-icon icon-" + kind + "\" width=\"18\" height=\"18\" viewBox=\"0 0 24 24\" aria-hidden=\"true\" focusable=\"false\">" + shape + "</svg>";
}
std::string entryLink(const std::string& url, const std::string& name, bool selected) {
  return "<a class=\"tree-label\" href=\"" + htmlEscape(url) + "\"" + (selected ? " aria-current=\"page\"" : "") + ">" + htmlEscape(name) + "</a>";
}
std::string renderTree(WebRepository& repository, const ProjectSummary& project,
                          const std::string& commit, const std::string& ref,
                          const std::string& directory, const std::string& selected, bool partial) {
  std::map<std::string, std::vector<TreeEntry>> children;
  std::set<std::string> loaded{""};
  if (!partial) {
    for (auto entry : repository.fileTree(commit)) {
      if (entry.type == "tree") loaded.insert(entry.name);
      children[parent(entry.name)].push_back(std::move(entry));
    }
  } else {
    // Large repositories retain the old, bounded per-directory browsing path.
    // Opening any folder loads it and its ancestors on the next page.
    std::vector<std::string> ancestors{""};
    for (std::size_t i = 0; i < directory.size(); ++i)
      if (directory[i] == '/') ancestors.push_back(directory.substr(0, i));
    if (!directory.empty()) ancestors.push_back(directory);
    if (ancestors.size() - 1 > kMaximumTreeDepth) throw TreeLimitError("Tree exceeds the 32-level browsing limit.");
    std::size_t total = 0;
    for (const auto& path : ancestors) {
      loaded.insert(path);
      for (auto entry : repository.tree(commit, path)) {
        entry.name = path.empty() ? entry.name : path + '/' + entry.name;
        children[path].push_back(std::move(entry));
        if (++total > kMaximumTreeEntries) throw TreeLimitError("Expanded tree exceeds the 5000-entry viewing limit.");
      }
    }
  }
  for (auto& [path, entries] : children) {
    static_cast<void>(path);
    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
      if ((a.type == "tree") != (b.type == "tree")) return a.type == "tree";
      return a.name < b.name;
    });
  }
  const auto root_url = sourceUrl(project.name, "tree", ref);
  std::string out = "<aside class=\"tree-pane\" aria-label=\"Repository files\"><div class=\"tree-row tree-root" +
      std::string(selected.empty() ? " selected" : "") + "\">" + icon("folder") + entryLink(root_url, project.name, selected.empty()) + "</div>";
  const auto render = [&](auto&& self, const std::string& path) -> void {
    out += "<ul class=\"tree-list\">";
    for (const auto& entry : children[path]) {
      const bool folder = entry.type == "tree";
      const bool current = entry.name == selected;
      const auto url = sourceUrl(project.name, folder ? "tree" : "blob", ref, entry.name);
      const bool browsable = entry.type != "commit" && parseHttpRoute(url).kind != RouteKind::kNotFound;
      const auto label = browsable ? entryLink(url, basename(entry.name), current) : "<span class=\"tree-label\">" + htmlEscape(basename(entry.name)) + "</span>";
      const auto row_class = std::string("tree-row") + (current ? " selected" : "");
      out += "<li class=\"tree-node " + std::string(folder ? "tree-directory" : "tree-leaf") + "\">";
      if (folder) {
        const bool open = directory == entry.name || directory.starts_with(entry.name + '/');
        out += "<details class=\"tree-folder\"" + std::string(open ? " open" : "") + "><summary class=\"" + row_class + "\">" + icon("folder") + label + "</summary>";
        if (!browsable) out += "<p class=\"tree-note\">Unsupported folder name.</p>";
        else if (!loaded.contains(entry.name)) out += "<p class=\"tree-note\"><a href=\"" + htmlEscape(url) + "\">Open folder to load contents</a></p>";
        else if (children[entry.name].empty()) out += "<p class=\"tree-note\">Empty folder</p>";
        else self(self, entry.name);
        out += "</details>";
      } else {
        out += "<div class=\"" + row_class + "\"><span class=\"tree-spacer\" aria-hidden=\"true\"></span>" + icon(iconKind(entry)) + label;
        if (entry.type == "commit") out += "<span class=\"badge\">submodule " + htmlEscape(entry.id.substr(0, 8)) + "</span>";
        else if (entry.mode == "120000") out += "<span class=\"badge\">symlink</span>";
        else if (!browsable) out += "<span class=\"badge\">unsupported filename</span>";
        out += "</div>";
      }
      out += "</li>";
      if (out.size() > 4 * 1024 * 1024) throw TreeLimitError("File tree exceeds the sidebar viewing limit.");
    }
    out += "</ul>";
  };
  render(render, "");
  if (partial) out += "<p class=\"tree-limit muted\">Large repository: expand a folder, then open it to load its contents.</p>";
  return out + "</aside>";
}
}  // namespace
std::string renderFileTree(WebRepository& repository, const ProjectSummary& project,
                          const std::string& commit, const std::string& ref,
                          const std::string& directory, const std::string& selected) {
  try {
    return renderTree(repository, project, commit, ref, directory, selected, false);
  } catch (const TreeLimitError&) {
    // Encoded links and row markup can exceed the sidebar budget even when
    // the raw tree fits. Retry once with only the current path's listings.
    return renderTree(repository, project, commit, ref, directory, selected, true);
  }
}
}  // namespace ckgit
