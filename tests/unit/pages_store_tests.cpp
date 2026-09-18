// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#include "ckgit/pages_store.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error("pages store: " + message);
}

class PagesFixture {
 public:
  PagesFixture() {
    const char* configured_tmp = std::getenv("TMPDIR");
    require(configured_tmp && *configured_tmp, "TMPDIR must be explicitly selected");
    auto pattern = (std::filesystem::path(configured_tmp) / "ckgit-pages-XXXXXX").string();
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    require(mkdtemp(writable.data()) != nullptr, "could not create isolated fixture");
    root = writable.data();
    pages_root = root / "pages";
  }
  ~PagesFixture() {
    std::error_code error;
    std::filesystem::remove_all(root, error);
  }
  std::filesystem::path root;
  std::filesystem::path pages_root;
};

void writeFile(const std::filesystem::path& path, const std::string& content) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary) << content;
}

void testPublishAndServe() {
  PagesFixture fixture;
  const std::filesystem::path src = fixture.root / "site";
  writeFile(src / "index.html", "<h1>home</h1>");
  writeFile(src / "assets/app.css", "body{color:red}");
  ckgit::publishPagesSite(fixture.pages_root, "demo", "00000000000000000001-aaaaaaaa", src, 3);

  require(ckgit::currentPagesVersion(fixture.pages_root, "demo") == "00000000000000000001-aaaaaaaa",
          "the current version is set");
  const auto home = ckgit::readCurrentPage(fixture.pages_root, "demo", "", 1u << 20);
  require(home.has_value() && home->content == "<h1>home</h1>", "an empty path serves index.html");
  require(home->content_type == "text/html; charset=utf-8", "index.html has an HTML content type");
  const auto slashed = ckgit::readCurrentPage(fixture.pages_root, "demo", "index.html", 1u << 20);
  require(slashed.has_value() && slashed->content == "<h1>home</h1>", "an explicit path serves the file");
  const auto css = ckgit::readCurrentPage(fixture.pages_root, "demo", "assets/app.css", 1u << 20);
  require(css.has_value() && css->content == "body{color:red}" &&
              css->content_type.rfind("text/css", 0) == 0,
          "a nested asset serves with its content type");
  require(!ckgit::readCurrentPage(fixture.pages_root, "demo", "missing.html", 1u << 20).has_value(),
          "an absent file yields nothing");
  require(!ckgit::readCurrentPage(fixture.pages_root, "demo", "../secret", 1u << 20).has_value(),
          "a traversal path is rejected");
  require(!ckgit::readCurrentPage(fixture.pages_root, "absent", "", 1u << 20).has_value(),
          "an unknown project yields nothing");
}

void testRepublishAndPrune() {
  PagesFixture fixture;
  for (int index = 1; index <= 4; ++index) {
    const std::filesystem::path src = fixture.root / ("s" + std::to_string(index));
    writeFile(src / "index.html", "v" + std::to_string(index));
    const std::string id = "0000000000000000000" + std::to_string(index) + "-aaaaaaaa";
    ckgit::publishPagesSite(fixture.pages_root, "demo", id, src, 2);
  }
  const auto home = ckgit::readCurrentPage(fixture.pages_root, "demo", "", 1u << 20);
  require(home.has_value() && home->content == "v4", "the newest publish is current");
  std::size_t versions = 0;
  for (const auto& entry : std::filesystem::directory_iterator(fixture.pages_root / "demo" / "versions")) {
    if (entry.is_directory()) ++versions;
  }
  require(versions == 2, "keep_versions prunes older site versions");
}

void testSymlinkSkipped() {
  PagesFixture fixture;
  const std::filesystem::path src = fixture.root / "site";
  writeFile(src / "index.html", "home");
  std::error_code error;
  std::filesystem::create_symlink("/etc/hostname", src / "evil", error);
  require(!error, "the fixture symlink was created");
  ckgit::publishPagesSite(fixture.pages_root, "demo", "00000000000000000001-aaaaaaaa", src, 3);
  require(!ckgit::readCurrentPage(fixture.pages_root, "demo", "evil", 1u << 20).has_value(),
          "a symlink in the source is not published or served");
}

void testRemoveProjectPages() {
  PagesFixture fixture;
  const std::filesystem::path src = fixture.root / "site";
  writeFile(src / "index.html", "home");
  ckgit::publishPagesSite(fixture.pages_root, "demo", "00000000000000000001-aaaaaaaa", src, 3);
  ckgit::removeProjectPages(fixture.pages_root, "demo");
  require(!ckgit::currentPagesVersion(fixture.pages_root, "demo").has_value(),
          "removeProjectPages clears the site");
}

void testContentType() {
  require(ckgit::pagesContentType("a.js").rfind("text/javascript", 0) == 0, "js");
  require(ckgit::pagesContentType("a.png") == "image/png", "png");
  require(ckgit::pagesContentType("a.unknownext") == "application/octet-stream", "unknown falls back");
}

// readCurrentPage delegates to this directly (WP9), so `ckdocs serve` reads
// a plain build directory with the identical safety and index.html folding
// a published Pages site gets, with no <pages_root>/<project>/current
// indirection at all.
void testReadSiteFile() {
  PagesFixture fixture;
  const std::filesystem::path site = fixture.root / "site";
  writeFile(site / "index.html", "<h1>home</h1>");
  writeFile(site / "docs/guide.html", "<p>guide</p>");
  std::error_code error;
  std::filesystem::create_symlink("/etc/hostname", site / "evil.html", error);
  require(!error, "the fixture symlink was created");

  const auto empty = ckgit::readSiteFile(site, "", 1u << 20);
  require(empty.has_value() && empty->content == "<h1>home</h1>" && empty->content_type == "text/html; charset=utf-8",
          "an empty path serves index.html");
  const auto slash = ckgit::readSiteFile(site, "docs/", 1u << 20);
  require(!slash.has_value(), "a trailing slash resolves to index.html, which docs/ does not have");
  const auto nested = ckgit::readSiteFile(site, "docs/guide.html", 1u << 20);
  require(nested.has_value() && nested->content == "<p>guide</p>", "a nested file serves directly");
  require(!ckgit::readSiteFile(site, "missing.html", 1u << 20).has_value(), "an absent file yields nothing");
  require(!ckgit::readSiteFile(site, "../secret", 1u << 20).has_value(), "a traversal path is rejected");
  require(!ckgit::readSiteFile(site, "evil.html", 1u << 20).has_value(), "a symlink is never followed or served");
  require(!ckgit::readSiteFile(site, "index.html", 1).has_value(), "a file over the cap is rejected");
  require(!ckgit::readSiteFile(fixture.root / "absent", "", 1u << 20).has_value(), "a missing site directory yields nothing");
}

void testDecodeRequestPath() {
  require(ckgit::decodeRequestPath("/a/b") == "/a/b", "an already-plain path is unchanged");
  require(ckgit::decodeRequestPath("/a%20b") == "/a b", "a space escape decodes");
  require(ckgit::decodeRequestPath("/a%2Fb") == "/a/b", "an encoded slash decodes like any other byte");
  require(ckgit::decodeRequestPath("") == "", "an empty target decodes to empty");
  require(!ckgit::decodeRequestPath("/a%").has_value(), "a truncated escape is rejected");
  require(!ckgit::decodeRequestPath("/a%2").has_value(), "an incomplete escape is rejected");
  require(!ckgit::decodeRequestPath("/a%zz").has_value(), "a non-hex escape is rejected");
  require(!ckgit::decodeRequestPath("/a%00b").has_value(), "an escaped NUL is rejected");
  require(!ckgit::decodeRequestPath("/a%7fb").has_value(), "an escaped DEL is rejected");
}

}  // namespace

void testPagesStore() {
  testPublishAndServe();
  testRepublishAndPrune();
  testSymlinkSkipped();
  testRemoveProjectPages();
  testContentType();
  testReadSiteFile();
  testDecodeRequestPath();
}
