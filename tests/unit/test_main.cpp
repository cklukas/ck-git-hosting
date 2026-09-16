// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "ckgit/authorized_keys.hpp"
#include "ckgit/client_config.hpp"
#include "ckgit/client_state.hpp"
#include "ckgit/git_repository.hpp"
#include "ckgit/server_config.hpp"
#include "ckgit/http_request.hpp"
#include "ckgit/metadata_store.hpp"
#include "ckgit/process.hpp"
#include "ckgit/ref_status.hpp"
#include "ckgit/repository_store.hpp"
#include "ckgit/remote_url.hpp"
#include "ckgit/server_identity.hpp"
#include "ckgit/ssh_command.hpp"
#include "ckgit/validation.hpp"
#include "ckgit/web_renderer.hpp"

namespace {

int failures = 0;

void expect(bool condition, const std::string& message) {
  if (!condition) {
    ++failures;
    std::cerr << "FAIL: " << message << "\n";
  }
}

void expectEqual(const std::string& actual, const std::string& expected,
                 const std::string& message) {
  expect(actual == expected, message + " (expected '" + expected + "', got '" + actual + "')");
}

void runGit(const std::filesystem::path& path, std::initializer_list<std::string> arguments) {
  std::vector<std::string> command{"git", "-C", path.string()};
  command.insert(command.end(), arguments.begin(), arguments.end());
  const auto result = ckgit::runProcess(command);
  if (result.exit_code != 0 || result.timed_out) {
    throw std::runtime_error("test Git command failed: " + result.output);
  }
}

std::filesystem::path testDirectory() {
  const auto temporary_root = std::filesystem::temp_directory_path();
  // The Makefile exports the approved build root; every test file must stay
  // beneath it so a workstation never receives scratch data elsewhere.
  const char* configured_root = std::getenv("CKGIT_TEST_ROOT");
  const std::string required_prefix = configured_root == nullptr || *configured_root == '\0'
                                          ? std::string("/Volumes/PRO-BLADE/tmp")
                                          : std::string(configured_root);
  const auto root_string = temporary_root.string();
  if (root_string.rfind(required_prefix, 0) != 0) {
    throw std::runtime_error("TMPDIR must be beneath " + required_prefix);
  }
  const auto directory = temporary_root / ("ckgit-unit-" + std::to_string(getpid()));
  std::error_code error;
  std::filesystem::remove_all(directory, error);
  std::filesystem::create_directories(directory / "outer" / "nested");
  return directory;
}

void testValidation() {
  expect(ckgit::isValidProjectName("cworks"), "ordinary name is valid");
  expect(ckgit::isValidProjectName("ck_git-hosting.2"), "portable punctuation is valid");
  expect(!ckgit::isValidProjectName(".hidden"), "dot-prefixed name is rejected");
  expect(!ckgit::isValidProjectName("a..b"), "traversal-like name is rejected");
  expect(!ckgit::isValidProjectName("project.git"), "git suffix is rejected");
  expect(!ckgit::isValidProjectName("../escape"), "path separator is rejected");
  expect(!ckgit::isValidProjectName("CON"), "reserved name is rejected");
  expect(ckgit::isValidClientId("mac-studio_2"), "client id is valid");
  expect(!ckgit::isValidClientId("-bad"), "option-like client id is rejected");
}

void testRemoteUrls() {
  const auto scp = ckgit::parseRemoteUrl("ckgit", "git@rpi4:project.git");
  expect(scp.transport == ckgit::RemoteTransport::kScpLikeSsh, "SCP SSH URL is recognized");
  expectEqual(scp.user, "git", "SCP user parsed");
  expectEqual(scp.host, "rpi4", "SCP host parsed");
  expectEqual(scp.path, "project.git", "SCP path parsed");

  const auto uri = ckgit::parseRemoteUrl("ckgit", "ssh://git@rpi4:2222/team/project.git");
  expect(uri.transport == ckgit::RemoteTransport::kSshUrl, "SSH URI is recognized");
  expect(uri.port.has_value() && *uri.port == 2222, "SSH URI port parsed");
  expectEqual(uri.path, "team/project.git", "SSH URI path parsed");

  const auto local = ckgit::parseRemoteUrl("origin", "/Volumes/repos/project.git");
  expect(local.transport == ckgit::RemoteTransport::kOther, "local path is not SSH");
  expectEqual(ckgit::redactRemoteUrl("https://user:secret@example.test/project.git"),
              "https://user:***@example.test/project.git", "password is redacted");
}

void testClientConfig() {
  const auto directory = testDirectory();
  try {
    const auto config_path = directory / "client.ini";
    {
      std::ofstream config(config_path);
      config << "schema_version=1\n"
             << "client_id=mac-studio\n"
             << "display_name=Mac Studio\n"
             << "server=ckgit@rpi4\n"
             << "remote_name=ckgit\n"
             << "scan_root=/Users/example/git\n"
             << "scan_root=/Volumes/PRO-BLADE/git\n"
             << "exclude=*/node_modules/*\n"
             << "public_path_mode=basename\n";
    }
    const auto config = ckgit::loadClientConfig(config_path);
    expect(config.client_id == "mac-studio", "client configuration preserves client ID");
    expect(config.scan_roots.size() == 2, "client configuration preserves repeated scan roots");
    expect(config.exclusions.size() == 1, "client configuration preserves exclusions");
    expect(!config.expose_full_paths, "client configuration applies basename privacy mode");
    expect(config.config_directory == directory,
           "client configuration records the absolute directory that owns its lock and selections");

    const auto invalid_path = directory / "invalid.ini";
    {
      std::ofstream invalid(invalid_path);
      invalid << "schema_version=1\nclient_id=mac\ndisplay_name=Mac\nserver=ckgit@rpi4\n"
              << "remote_name=ckgit\nunknown=value\n";
    }
    bool rejected_unknown = false;
    try {
      static_cast<void>(ckgit::loadClientConfig(invalid_path));
    } catch (const std::exception&) {
      rejected_unknown = true;
    }
    expect(rejected_unknown, "client configuration rejects unknown fields");
  } catch (...) {
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    throw;
  }
  std::error_code error;
  std::filesystem::remove_all(directory, error);
}

void testRefStatus() {
  const std::string first_id(40, 'a');
  const std::string second_id(40, 'b');
  const auto refs = ckgit::parseRefsControlResponse(
      "ok 2\nrefs/heads/main " + first_id + "\nrefs/tags/v1 " + second_id + "\n");
  expect(refs.size() == 2, "valid ref control response is parsed");

  const auto comparison = ckgit::compareRefTips(
      {ckgit::RefTip{"refs/heads/main", first_id}, ckgit::RefTip{"refs/heads/local", second_id}},
      {ckgit::RefTip{"refs/heads/main", first_id}, ckgit::RefTip{"refs/heads/remote", second_id}});
  expect(comparison.size() == 3, "ref comparison includes the union of names");
  expect(comparison[0].relation == ckgit::RefRelation::kLocalOnly,
         "local-only ref is explicitly classified");
  expect(comparison[1].relation == ckgit::RefRelation::kEqual,
         "equal ref is explicitly classified");
  expect(comparison[2].relation == ckgit::RefRelation::kServerOnly,
         "server-only ref is explicitly classified");

  std::string large = "ok 3000\n";
  for (int index = 0; index < 3000; ++index) {
    large += "refs/tags/release-" + std::to_string(index) + " " + first_id + "\n";
  }
  expect(large.size() > 4096 && ckgit::parseRefsControlResponse(large).size() == 3000,
         "a listing of thousands of refs is accepted within the bounded response");
  bool rejected_count = false;
  try {
    static_cast<void>(ckgit::parseRefsControlResponse("ok 2\nrefs/heads/main " + first_id + "\n"));
  } catch (const std::exception&) {
    rejected_count = true;
  }
  expect(rejected_count, "ref response rejects a mismatched count");

  ckgit::ClientConfig config;
  config.server = "ckgit@rpi4";
  const auto matched = ckgit::projectForConfiguredServerRemote(
      ckgit::parseRemoteUrl("ckgit", "ckgit@rpi4:cworks.git"), config);
  expect(matched.has_value() && *matched == "cworks", "paired server remote is recognized exactly");
  const auto rejected = ckgit::projectForConfiguredServerRemote(
      ckgit::parseRemoteUrl("ckgit", "ckgit@not-rpi4:cworks.git"), config);
  expect(!rejected.has_value(), "wrong SSH host is never treated as paired");
  expect(ckgit::hostedRepositoryUrl(config.server, "--config") == "ckgit@rpi4:./--config.git",
         "option-looking projects use a transport path accepted by Git");
  const auto dash = ckgit::projectForConfiguredServerRemote(
      ckgit::parseRemoteUrl("ckgit", "ckgit@rpi4:./--config.git"), config);
  expect(dash && *dash == "--config", "safe option-looking transport path retains project identity");
  expect(!ckgit::projectForConfiguredServerRemote(
      ckgit::parseRemoteUrl("ckgit", "ckgit@rpi4:./../escape.git"), config), "transport escape never admits traversal");
}

void testWebRenderer() {
  const std::string rendered = ckgit::renderProjectTable(
      {ckgit::ProjectSummary{"<script>alert(1)</script>", "main", true, 3, 2, 0, {}, {}},
       ckgit::ProjectSummary{"safe", "missing", false, 0, 0, 0, {}, {}}});
  expect(rendered.find("&lt;script&gt;alert(1)&lt;/script&gt;") != std::string::npos,
         "project table HTML-escapes project names");
  expect(rendered.find("<script>") == std::string::npos,
         "project table does not emit unescaped script markup");
  expect(rendered.find("class=\"project-default ok\">main") != std::string::npos,
         "project table shows valid HEAD state");
  expect(rendered.find("class=\"project-default warn\">missing") != std::string::npos,
         "project table flags invalid HEAD state");
  expect(rendered.find("href=\"/project/safe\"") != std::string::npos,
         "project table links valid project names to their detail page");
  const std::string detail = ckgit::renderProjectDetail(
      ckgit::ProjectSummary{"<project>", "main", true, 1, 0, 0, {}, {}});
  expect(detail.find("<h1>&lt;project&gt;</h1>") != std::string::npos,
         "project detail HTML-escapes project names");
  ckgit::ProjectSummary dated{"dated", "main", true, 1, 0, 0, {}, {}};
  dated.last_commit_epoch_seconds = 1700000000;
  const std::string dated_table = ckgit::renderProjectTable({dated});
  expect(dated_table.find(">Last commit</th>") != std::string::npos &&
             dated_table.find("2023-11-14 22:13 UTC") != std::string::npos,
         "project table shows the last commit time");
  expect(rendered.find("<span class=\"muted\">none</span>") != std::string::npos,
         "project table shows none for an empty repository's last commit");
  ckgit::ProjectSummary registered{"safe", "main", true, 1, 0, 0, {}, {}};
  registered.checkouts.push_back(ckgit::CheckoutMetadata{"mac-studio", "worktree", 1700000000});
  registered.events.push_back(ckgit::StateEvent{1700000000, "checkout-registered", "safe", "mac-studio"});
  const std::string registered_detail = ckgit::renderProjectDetail(registered);
  expect(registered_detail.find("<strong>mac-studio</strong> · <code>worktree</code>") != std::string::npos,
         "project detail renders the latest safe checkout registration");
  expect(registered_detail.find("Checkout registered · <strong>mac-studio</strong>") != std::string::npos,
         "project detail renders safe event metadata without a checkout path");
  expect(registered_detail.find("<code>ckgit clone safe</code>") != std::string::npos &&
             registered_detail.find("ssh_clone_target=user@host") != std::string::npos &&
             registered_detail.find("<code>git clone ") == std::string::npos,
         "an unconfigured clone target shows the ckgit command and precise administrator hint");
  registered.ssh_clone_target = "ckgit@rpi4";
  const std::string clone_detail = ckgit::renderProjectDetail(registered);
  expect(clone_detail.find("<code>ckgit clone safe</code>") != std::string::npos &&
             clone_detail.find("<code>git clone ckgit@rpi4:safe.git</code>") != std::string::npos,
         "project overview offers both ckgit and the configured SSH clone command");
  ckgit::ProjectSummary empty{"empty", "main", false, 0, 0, 0, {}, {}};
  empty.ssh_clone_target = "ckgit@git.example.test";
  const std::string empty_detail = ckgit::renderProjectDetail(empty);
  const auto clone_section = empty_detail.find("class=\"clone-info\"");
  expect(clone_section != std::string::npos &&
             empty_detail.find("class=\"clone-info\"", clone_section + 1) == std::string::npos &&
             empty_detail.find("<code>ckgit clone empty</code>") != std::string::npos &&
             empty_detail.find("<code>git clone ckgit@git.example.test:empty.git</code>") != std::string::npos,
         "empty projects have one consistent clone section with both commands");
  registered.ssh_clone_target = "ckgit@host<script>alert(1)</script>";
  const auto invalid_target_detail = ckgit::renderProjectDetail(registered);
  expect(invalid_target_detail.find("<script>") == std::string::npos &&
             invalid_target_detail.find("<code>git clone ") == std::string::npos &&
             invalid_target_detail.find("ssh_clone_target=user@host") != std::string::npos,
         "invalid presentation data cannot become HTML or a copyable shell command");
  expect(detail.find("<code>ckgit clone ") == std::string::npos,
         "invalid project names cannot become copyable clone commands");
  empty.name = "-dash-project";
  expect(ckgit::renderProjectDetail(empty).find("<code>ckgit clone -- -dash-project</code>") != std::string::npos,
         "dash-prefixed project names are protected from option parsing in copyable commands");
}

void testHttpRequestParser() {
  const auto get = ckgit::parseReadOnlyHttpRequest("GET /project/alpha HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
  expect(get.has_value() && get->method == ckgit::HttpMethod::kGet && get->target == "/project/alpha",
         "canonical loopback GET request is accepted");
  const auto head = ckgit::parseReadOnlyHttpRequest("HEAD / HTTP/1.1\r\nHost: example.test\r\nContent-Length: 0\r\n\r\n");
  expect(head.has_value() && head->method == ckgit::HttpMethod::kHead,
         "bodyless HEAD request is accepted");
  expect(!ckgit::parseReadOnlyHttpRequest("GET / HTTP/1.1\nHost: example.test\n\n").has_value(),
         "bare-LF request framing is rejected");
  expect(!ckgit::parseReadOnlyHttpRequest("GET / HTTP/1.1\r\nHost: example.test\r\nContent-Length: 1\r\n\r\n").has_value(),
         "requests with a body are rejected");
  expect(!ckgit::parseReadOnlyHttpRequest("GET / HTTP/1.1\r\nHost: one\r\nHost: two\r\n\r\n").has_value(),
         "duplicate HTTP Host headers are rejected");
  expect(!ckgit::parseReadOnlyHttpRequest("GET / HTTP/1.1\r\nHost: example.test\r\nTransfer-Encoding: chunked\r\n\r\n").has_value(),
         "transfer encoding is rejected");
  expect(!ckgit::parseReadOnlyHttpRequest("GET /?x=y HTTP/1.1\r\nHost: example.test\r\n\r\n").has_value(),
         "non-canonical query targets are rejected");
}

void testCheckoutMetadata() {
  const auto directory = testDirectory();
  try {
    const auto state_root = directory / "state";
    std::filesystem::create_directory(state_root);
    if (chmod(state_root.c_str(), 0700) != 0) {
      throw std::runtime_error("could not make metadata fixture private");
    }
    const std::string encoded_path = ckgit::encodeCheckoutPath("project-worktree");
    expect(encoded_path == "70726f6a6563742d776f726b74726565", "checkout paths use a stable hex token");
    expect(ckgit::isValidCheckoutPathToken(encoded_path), "checkout path token is validated after decoding");
    expect(!ckgit::isValidCheckoutPathToken("not-hex"), "invalid checkout token is rejected");
    ckgit::registerCheckout(state_root, "project", "mac-studio", encoded_path);
    const auto metadata = ckgit::loadCheckoutMetadata(state_root, "project");
    expect(metadata.size() == 1, "registered checkout is discoverable from state");
    expect(metadata.size() == 1 && metadata.front().client_id == "mac-studio" &&
               metadata.front().reported_path == "project-worktree" &&
               metadata.front().last_seen_epoch_seconds > 0,
           "checkout record preserves safe client, path, and timestamp fields");
    ckgit::registerCheckout(state_root, "project", "mac-studio", encoded_path);
    bool conflict = false;
    try {
      ckgit::registerCheckout(state_root, "project", "mac-studio", ckgit::encodeCheckoutPath("elsewhere"));
    } catch (const ckgit::CheckoutConflict&) {
      conflict = true;
    }
    expect(conflict, "a host cannot silently register a second checkout for a project");
    expect(ckgit::loadCheckoutMetadata(state_root, "project").front().reported_path == "project-worktree",
           "a refused registration leaves the main checkout unchanged");
    ckgit::registerCheckout(state_root, "project", "laptop", ckgit::encodeCheckoutPath("laptop-worktree"));
    ckgit::registerCheckout(state_root, "other", "mac-studio", ckgit::encodeCheckoutPath("other-worktree"));
    const auto mine = ckgit::loadClientCheckouts(state_root, "mac-studio");
    expect(mine.size() == 2 && mine[0].project_name == "other" && mine[0].reported_path == "other-worktree" &&
               mine[1].project_name == "project" && mine[1].reported_path == "project-worktree",
           "a host retrieves exactly its own registrations, sorted by project");
    expect(ckgit::loadClientCheckouts(state_root, "laptop").size() == 1,
           "another host sees only its own registration");
    ckgit::registerCheckout(state_root, "project", "mac-studio", ckgit::encodeCheckoutPath("/Users/me/git/project-worktree"));
    expect(ckgit::loadCheckoutMetadata(state_root, "project")[1].reported_path == "/Users/me/git/project-worktree",
           "a full path that ends in the registered folder name refreshes the same main checkout");
    expect(ckgit::sameReportedCheckout("work", "/a/b/work") && ckgit::sameReportedCheckout("/a/b/work/", "/a/b/work") &&
               !ckgit::sameReportedCheckout("/a/work", "/b/work") && !ckgit::sameReportedCheckout("work", "other"),
           "reported checkouts compare by folder name only across privacy modes");
    ckgit::registerCheckout(state_root, "project", "mac-studio", ckgit::encodeCheckoutPath("elsewhere"), true);
    expect(ckgit::loadClientCheckouts(state_root, "mac-studio")[1].reported_path == "elsewhere",
           "an explicit replacement moves the main checkout");
    ckgit::appendStateEvent(state_root, "project-created", "project", "mac-studio");
    ckgit::appendStateEvent(state_root, "checkout-registered", "project", "mac-studio");
    const auto events = ckgit::loadProjectEvents(state_root, "project");
    expect(events.size() == 2 && events.front().kind == "project-created" &&
               events.back().kind == "checkout-registered",
           "strict project events append and load in chronological order");
    bool unsafe_root_rejected = false;
    const auto unsafe_root = directory / "unsafe-state";
    std::filesystem::create_directory(unsafe_root);
    if (chmod(unsafe_root.c_str(), 0755) != 0) {
      throw std::runtime_error("could not make metadata fixture public");
    }
    try {
      static_cast<void>(ckgit::validatedMetadataRoot(unsafe_root));
    } catch (const std::exception&) {
      unsafe_root_rejected = true;
    }
    expect(unsafe_root_rejected, "metadata state root must not expose paths to group or world readers");
  } catch (...) {
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    throw;
  }
  std::error_code error;
  std::filesystem::remove_all(directory, error);
}

void testClientState() {
  const auto directory = testDirectory();
  try {
    const auto lock_path = directory / "sync.lock";
    auto first = ckgit::SyncLock::tryAcquire(lock_path);
    expect(first.has_value(), "first sync lock acquisition succeeds");
    const auto second = ckgit::SyncLock::tryAcquire(lock_path);
    expect(!second.has_value(), "second sync lock acquisition is refused while the first is held");
    first.reset();
    const auto third = ckgit::SyncLock::tryAcquire(lock_path);
    expect(third.has_value(), "sync lock can be reacquired after release");
    expect(std::filesystem::exists(lock_path), "sync lock file persists so its identity is stable");

    const auto selection = directory / "canonical.ini";
    expect(ckgit::loadCanonicalCheckouts(selection).empty(), "absent selection file means no selections");
    ckgit::saveCanonicalCheckout(selection, "ckmux", "/Volumes/PRO-BLADE/git/ckmux");
    ckgit::saveCanonicalCheckout(selection, "ckvision", "/Volumes/PRO-BLADE/git/ckvision");
    ckgit::saveCanonicalCheckout(selection, "ckmux", "/Users/example/git/ckmux");
    const auto loaded = ckgit::loadCanonicalCheckouts(selection);
    expect(loaded.size() == 2 && loaded.count("ckmux") == 1 && loaded.count("ckvision") == 1 &&
               loaded.at("ckmux") == std::filesystem::path("/Users/example/git/ckmux") &&
               loaded.at("ckvision") == std::filesystem::path("/Volumes/PRO-BLADE/git/ckvision"),
           "canonical selections replace one project and preserve the others");
    bool rejected_relative = false;
    try {
      ckgit::saveCanonicalCheckout(selection, "ckmux", "relative/checkout");
    } catch (const std::exception&) {
      rejected_relative = true;
    }
    expect(rejected_relative, "relative canonical checkout paths are rejected");
    expect(ckgit::loadCanonicalCheckouts(selection).size() == 2,
           "a rejected selection leaves the existing file untouched");
    {
      std::ofstream malformed(directory / "malformed.ini");
      malformed << "ckmux=/Volumes/PRO-BLADE/git/ckmux\n";
    }
    bool rejected_schema = false;
    try {
      static_cast<void>(ckgit::loadCanonicalCheckouts(directory / "malformed.ini"));
    } catch (const std::exception&) {
      rejected_schema = true;
    }
    expect(rejected_schema, "selection file without a leading schema_version is rejected");

    expect(ckgit::looksEphemeralCheckoutPath("/Volumes/PRO-BLADE/tmp/ckmux-v013-gate.Jthl4i"),
           "mktemp-style suffix beneath a scratch root is ephemeral");
    expect(!ckgit::looksEphemeralCheckoutPath("/Volumes/PRO-BLADE/tmp/ckmux"),
           "a scratch parent directory alone does not make a checkout ephemeral");
    expect(ckgit::looksEphemeralCheckoutPath("/Users/example/git/ckmux-v013-gate.Jthl4i"),
           "mktemp-style suffix is ephemeral");
    expect(!ckgit::looksEphemeralCheckoutPath("/Volumes/PRO-BLADE/git/ckmux"), "stable path is not ephemeral");
    expect(!ckgit::looksEphemeralCheckoutPath("/Volumes/PRO-BLADE/git/project.backup"),
           "lowercase dotted suffix is not mistaken for a mktemp suffix");
  } catch (...) {
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    throw;
  }
  std::error_code error;
  std::filesystem::remove_all(directory, error);
}

void testServerConfig() {
  const auto directory = testDirectory();
  try {
    const auto config_path = directory / "server.ini";
    {
      std::ofstream config(config_path);
      config << "# comment\nschema_version=1\nrepo_root=/srv/ck-git-hosting/repos\n"
             << "control_socket=/run/ck-git-hosting/control.sock\nstate_root=/var/lib/ck-git-hosting/state\n"
             << "hook_directory=/usr/lib/ck-git-hosting/hooks\nhttp_port=8420\nssh_clone_target=ckgit@rpi4\n";
    }
    const auto config = ckgit::loadServerConfig(config_path);
    expect(config.repo_root == std::filesystem::path("/srv/ck-git-hosting/repos") &&
               config.control_socket == std::filesystem::path("/run/ck-git-hosting/control.sock") &&
               config.state_root.has_value() && config.hook_directory.has_value() &&
               config.http_port.has_value() && *config.http_port == 8420 &&
               config.ssh_clone_target == "ckgit@rpi4",
           "server configuration preserves every documented field");
    expectEqual(ckgit::renderServerConfig(config),
                "schema_version=1\nrepo_root=/srv/ck-git-hosting/repos\ncontrol_socket=/run/ck-git-hosting/control.sock\n"
                "state_root=/var/lib/ck-git-hosting/state\nhook_directory=/usr/lib/ck-git-hosting/hooks\nhttp_port=8420\n"
                "ssh_clone_target=ckgit@rpi4\n",
                "server configuration renders in its own key=value form");
    const auto minimal_path = directory / "minimal.ini";
    {
      std::ofstream minimal(minimal_path);
      minimal << "schema_version=1\nrepo_root=/srv/repos\ncontrol_socket=/run/control.sock\n";
    }
    const auto minimal = ckgit::loadServerConfig(minimal_path);
    expect(!minimal.state_root.has_value() && !minimal.hook_directory.has_value() && !minimal.http_port.has_value() &&
               !minimal.ssh_clone_target.has_value(),
           "optional server fields stay unset when absent");
    expect(ckgit::renderServerConfig(minimal).find("ssh_clone_target=") == std::string::npos,
           "legacy configurations render without inventing an SSH destination");
    for (const auto* target : {"ckgit@rpi4", "git_user@git-server", "git.user@git.example.test", "ckgit@192.0.2.1"}) {
      expect(ckgit::isValidSshCloneTarget(target), "safe SSH aliases, DNS names, and IPv4 targets are accepted");
    }
    for (const auto* target : {"", "rpi4", "@rpi4", "ckgit@", "ckgit@@rpi4", "-git@rpi4", "git@-rpi4",
                               "git@host:2222", "ssh://git@host", "git@host/path", "git@host;whoami", "git@$(whoami)",
                               "git@host name", "git@host\nhttp_port=1", "git@host<script>", "git@[::1]"}) {
      expect(!ckgit::isValidSshCloneTarget(target), "ambiguous, option-like, and unsafe SSH targets are rejected");
    }
    expect(ckgit::isValidSshCloneTarget("ckgit@" + std::string(249, 'h')) &&
               !ckgit::isValidSshCloneTarget("ckgit@" + std::string(250, 'h')),
           "advertised SSH target length is bounded");
    const auto reject = [&](const std::string& content, const std::string& message) {
      const auto path = directory / "reject.ini";
      {
        std::ofstream file(path);
        file << content;
      }
      bool rejected = false;
      try {
        static_cast<void>(ckgit::loadServerConfig(path));
      } catch (const std::exception&) {
        rejected = true;
      }
      expect(rejected, message);
    };
    reject("schema_version=1\nrepo_root=relative\ncontrol_socket=/run/x.sock\n", "relative repo_root is rejected");
    reject("schema_version=1\nrepo_root=/srv/x\ncontrol_socket=/run/x.sock\nlisten=0.0.0.0\n", "unknown field is rejected");
    reject("schema_version=1\nrepo_root=/srv/x\ncontrol_socket=/run/x.sock\nhttp_port=70000\n", "out-of-range port is rejected");
    reject("schema_version=1\nrepo_root=/srv/x\nrepo_root=/srv/y\ncontrol_socket=/run/x.sock\n", "duplicate field is rejected");
    reject("schema_version=2\nrepo_root=/srv/x\ncontrol_socket=/run/x.sock\n", "unknown schema is rejected");
    reject("repo_root=/srv/x\ncontrol_socket=/run/x.sock\n", "missing schema is rejected");
    reject("schema_version=1\nrepo_root=/srv/x\ncontrol_socket=/run/x.sock\nssh_clone_target=git@host;whoami\n",
           "configuration rejects shell syntax in an advertised SSH target");
    reject("schema_version=1\nrepo_root=/srv/x\ncontrol_socket=/run/x.sock\nssh_clone_target=git@rpi4\nssh_clone_target=git@other\n",
           "configuration rejects conflicting advertised SSH targets");
  } catch (...) {
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    throw;
  }
  std::error_code error;
  std::filesystem::remove_all(directory, error);
}

void testAuthorizedKeys() {
  const std::string key_data = "AAAAC3NzaC1lZDI1NTE5AAAAIGb6Gz7pRt8bC1pT3VbwBgV6h4v2HkQx1y7z8Q0m8ZrA";
  const ckgit::ForcedCommandLayout layout{"/usr/bin/ck-git-shell", "/srv/ck-git-hosting/repos",
                                          "/run/ck-git-hosting/control.sock", "/var/lib/ck-git-hosting/state"};
  expectEqual(ckgit::renderAuthorizedKeyLine("mac-studio", "ssh-ed25519 " + key_data + " user@example\n", layout),
              "restrict,command=\"/usr/bin/ck-git-shell --client-id mac-studio --repo-root /srv/ck-git-hosting/repos "
              "--control-socket /run/ck-git-hosting/control.sock --state-root /var/lib/ck-git-hosting/state\" "
              "ssh-ed25519 " + key_data + " mac-studio\n",
              "authorized key line uses the fixed forced command and the client ID as comment");
  const auto reject = [&](const std::string& client, const std::string& key, const ckgit::ForcedCommandLayout& used,
                          const std::string& message) {
    bool rejected = false;
    try {
      static_cast<void>(ckgit::renderAuthorizedKeyLine(client, key, used));
    } catch (const std::exception&) {
      rejected = true;
    }
    expect(rejected, message);
  };
  reject("mac-studio", "ssh-dss " + key_data + "\n", layout, "unsupported key type is rejected");
  reject("mac-studio", "command=\"/bin/sh\" ssh-ed25519 " + key_data + "\n", layout, "option prefix is rejected");
  reject("mac-studio", "ssh-ed25519 not*base64*\n", layout, "malformed key data is rejected");
  reject("mac-studio", "ssh-ed25519 " + key_data + "\nssh-ed25519 " + key_data + "\n", layout,
         "multi-line key file is rejected");
  reject("-mac", "ssh-ed25519 " + key_data + "\n", layout, "option-like client ID is rejected");
  ckgit::ForcedCommandLayout unsafe = layout;
  unsafe.repo_root = "/srv/ck git";
  reject("mac-studio", "ssh-ed25519 " + key_data + "\n", unsafe, "path with whitespace is never embedded");
  unsafe.repo_root = "/srv/x\"y";
  reject("mac-studio", "ssh-ed25519 " + key_data + "\n", unsafe, "path with a quote is never embedded");
}

void testProcessTimeoutKillsTree() {
  // A child that hands its output pipe to a long-lived grandchild must not
  // keep the caller waiting after the timeout: the whole tree is killed.
  const auto started = std::chrono::steady_clock::now();
  const auto result = ckgit::runProcess({"sh", "-c", "sleep 30 & wait"}, std::chrono::milliseconds(300));
  const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - started);
  expect(result.timed_out, "process helper reports the timeout");
  expect(elapsed.count() < 5, "process helper returns promptly after killing the child tree");
  const auto quiet = ckgit::runProcess({"sh", "-c", "read line; echo got:$line"}, std::chrono::seconds(5));
  expect(!quiet.timed_out && quiet.exit_code == 0 && quiet.output == "got:\n",
         "children read end-of-file from /dev/null instead of waiting on the terminal");
  ckgit::ProcessOptions options;
  options.timeout = std::chrono::seconds(5);
  options.inherit_stderr = true;
  const auto split = ckgit::runProcess(
      {"sh", "-c", "echo out; echo '(expected stderr line from testProcessTimeoutKillsTree)' >&2"}, options);
  expect(split.exit_code == 0 && split.output == "out\n",
         "inherit_stderr captures standard output only");
}

void testSshCommandGrammar() {
  std::string reason;
  const auto upload = ckgit::parseSshOriginalCommand("git-upload-pack 'cworks.git'", &reason);
  expect(upload.has_value(), "standard Git upload command is accepted");
  expect(upload.has_value() && upload->kind == ckgit::SshCommandKind::kUploadPack,
         "upload command has the correct kind");
  expect(upload.has_value() && upload->project_name == "cworks", "upload project is derived safely");

  const auto receive = ckgit::parseSshOriginalCommand("git-receive-pack 'cworks.git'", &reason);
  expect(receive.has_value() && receive->kind == ckgit::SshCommandKind::kReceivePack,
         "standard Git receive command is accepted");
  const auto rpc = ckgit::parseSshOriginalCommand("ckgit-rpc 1 list-projects", &reason);
  expect(rpc.has_value() && rpc->kind == ckgit::SshCommandKind::kRpc &&
             rpc->rpc_operation == "list-projects",
         "documented version-1 RPC is accepted");
  const auto refs = ckgit::parseSshOriginalCommand("ckgit-rpc 1 refs cworks", &reason);
  expect(refs.has_value() && refs->kind == ckgit::SshCommandKind::kRpc &&
             refs->rpc_operation == "refs" && refs->rpc_argument == "cworks",
         "project ref RPC is accepted with a validated project name");
  const auto create = ckgit::parseSshOriginalCommand("ckgit-rpc 1 create new-project main", &reason);
  expect(create.has_value() && create->rpc_operation == "create" &&
             create->rpc_argument == "new-project" && create->rpc_second_argument == "main",
         "project create RPC is accepted with validated inputs");
  const auto registration = ckgit::parseSshOriginalCommand(
      "ckgit-rpc 1 register cworks 776f726b", &reason);
  expect(registration.has_value() && registration->rpc_operation == "register" &&
             registration->rpc_argument == "cworks" && registration->rpc_second_argument == "776f726b",
         "checkout registration RPC accepts a validated encoded path");

  const auto checkouts = ckgit::parseSshOriginalCommand("ckgit-rpc 1 checkouts", &reason);
  expect(checkouts.has_value() && checkouts->rpc_operation == "checkouts" && checkouts->rpc_argument.empty(),
         "checkout listing RPC is accepted without arguments");
  const auto version = ckgit::parseSshOriginalCommand("ckgit-rpc 1 version", &reason);
  expect(version.has_value() && version->rpc_operation == "version" && version->rpc_argument.empty(),
         "version RPC is accepted without arguments");
  expect(!ckgit::parseSshOriginalCommand("ckgit-rpc 1 version cworks", &reason).has_value(),
         "version RPC rejects an argument");
  const auto replacement = ckgit::parseSshOriginalCommand("ckgit-rpc 1 replace-checkout cworks 776f726b", &reason);
  expect(replacement.has_value() && replacement->rpc_operation == "replace-checkout" &&
             replacement->rpc_argument == "cworks" && replacement->rpc_second_argument == "776f726b",
         "checkout replacement RPC accepts a validated encoded path");
  expect(!ckgit::parseSshOriginalCommand("ckgit-rpc 1 checkouts cworks", &reason).has_value(),
         "checkout listing RPC rejects an argument");
  expect(!ckgit::parseSshOriginalCommand("git-upload-pack cworks.git", &reason).has_value(),
         "unquoted repository argument is rejected");
  expect(!ckgit::parseSshOriginalCommand("git-upload-pack '../cworks.git'", &reason).has_value(),
         "traversal repository argument is rejected");
  expect(!ckgit::parseSshOriginalCommand("git-upload-pack 'cworks.git'; id", &reason).has_value(),
         "shell metacharacters are rejected");
  expect(!ckgit::parseSshOriginalCommand("ckgit-rpc 2 ping", &reason).has_value(),
         "unknown RPC major version is rejected");
  expect(!ckgit::parseSshOriginalCommand("ckgit-rpc 1 refs ../cworks", &reason).has_value(),
         "ref RPC rejects traversal project arguments");
  expect(!ckgit::parseSshOriginalCommand("ckgit-rpc 1 create cworks ../main", &reason).has_value(),
         "create RPC rejects an unsafe branch");
  expect(!ckgit::parseSshOriginalCommand("ckgit-rpc 1 register cworks not-hex", &reason).has_value(),
         "registration RPC rejects unsafe metadata tokens");
  expect(!ckgit::parseSshOriginalCommand("sh -c id", &reason).has_value(),
         "arbitrary commands are rejected");
}

void testBareRepositoryCreation() {
  const auto directory = testDirectory();
  try {
    const auto root = directory / "repo-root";
    std::filesystem::create_directories(root);
    const auto dry_run = ckgit::createBareRepository(root, "preview", "main", true);
    expect(dry_run == root / "preview.git", "dry run returns the planned bare path");
    expect(!std::filesystem::exists(dry_run), "dry run does not create a repository");

    const auto repository = ckgit::createBareRepository(root, "cworks", "main");
    expect(std::filesystem::is_directory(repository), "bare repository is created");
    const auto deny_deletes = ckgit::runProcess(
        {"git", "--git-dir", repository.string(), "config", "--get", "receive.denyDeletes"});
    expect(deny_deletes.exit_code == 0 && deny_deletes.output == "true\n",
           "new bare repository denies ref deletion");
    const auto deny_non_fast_forwards = ckgit::runProcess(
        {"git", "--git-dir", repository.string(), "config", "--get", "receive.denyNonFastForwards"});
    expect(deny_non_fast_forwards.exit_code == 0 && deny_non_fast_forwards.output == "true\n",
           "new bare repository denies non-fast-forward updates");
    bool duplicate_rejected = false;
    try {
      static_cast<void>(ckgit::createBareRepository(root, "cworks", "main"));
    } catch (const std::exception&) {
      duplicate_rejected = true;
    }
    expect(duplicate_rejected, "existing repository is never overwritten");
  } catch (...) {
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    throw;
  }
  std::error_code error;
  std::filesystem::remove_all(directory, error);
}

void testDiscoveryAndAudit() {
  const auto directory = testDirectory();
  try {
    const auto outer = directory / "outer";
    const auto nested = outer / "nested";
    const auto linked = directory / "linked-worktree";
    runGit(outer.parent_path(), {"init", "outer"});
    runGit(nested.parent_path(), {"init", "nested"});
    runGit(outer, {"config", "user.email", "tests@example.test"});
    runGit(outer, {"config", "user.name", "Tests"});
    {
      std::ofstream file(outer / "tracked.txt");
      file << "one\n";
    }
    runGit(outer, {"add", "tracked.txt"});
    runGit(outer, {"commit", "-m", "initial"});
    runGit(outer, {"remote", "add", "ckgit", "git@rpi4:outer.git"});
    runGit(outer, {"worktree", "add", "--detach", linked.string()});
    {
      std::ofstream file(outer / "dirty.txt");
      file << "not committed\n";
    }

    // Debris: a .git directory holding only a file-monitor socket is ignored,
    // and a stale one inside a real tree must not masquerade as a checkout.
    std::filesystem::create_directories(outer / "stale" / ".git");
    {
      std::ofstream leftover(outer / "stale" / ".git" / "fsmonitor--daemon.ipc");
      leftover << "";
    }
    const auto discovered = ckgit::discoverWorkingTrees({directory});
    expect(discovered.warnings.empty(), "fixture has no discovery warnings");
    expect(discovered.repositories.size() == 3,
           "nested repositories and linked worktrees are found and .git debris is ignored");
    bool stale_rejected = false;
    try {
      static_cast<void>(ckgit::inspectDiscoveredRepository(outer / "stale"));
    } catch (const std::exception&) {
      stale_rejected = true;
    }
    expect(stale_rejected, "a stale .git inside a repository is not reported as a second checkout");
    expect(ckgit::inspectRepository(outer / "stale").path == outer,
           "plain inspection still resolves a nested path to its tree root like Git does");
    if (geteuid() != 0) {
      const auto locked = directory / "locked-root";
      std::filesystem::create_directory(locked);
      if (chmod(locked.c_str(), 0000) == 0) {
        const auto unreadable = ckgit::discoverWorkingTrees({locked});
        expect(unreadable.repositories.empty() && unreadable.warnings.size() == 1 &&
                   unreadable.warnings.front().rfind("unreadable scan root", 0) == 0,
               "an unreadable scan root is reported instead of looking empty");
        chmod(locked.c_str(), 0700);
      }
    }
    const auto excluded = ckgit::discoverWorkingTrees({directory}, {"*nested*"});
    expect(excluded.repositories.size() == 2, "configured exclusions suppress matching subtrees");
    const auto audit = ckgit::inspectRepository(outer);
    expect(!audit.linked_worktree, "main working tree is not a linked worktree");
    expect(ckgit::inspectRepository(linked).linked_worktree, "linked worktree is recognized from its .git file");
    expect(!audit.current_branch.empty(), "current branch is recorded");
    expect(audit.changed_entries >= 1, "untracked file is counted without reading content");
    expect(audit.branches.size() == 1, "branch tips are recorded");
    expect(audit.remotes.size() == 1, "remote is recorded");
    expectEqual(audit.remotes.front().display_url, "git@rpi4:outer.git", "remote is safe to display");
  } catch (...) {
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    throw;
  }
  std::error_code error;
  std::filesystem::remove_all(directory, error);
}

}  // namespace

void testProjectIndex();
void testRouterMarkdown();
void testDeletion();
void testDashboard();
void testBulkPublishDiscovery();
void testCliHelp();
void testClientManagement();
void testSetupConfig();
void testCiWorkflow();
void testCiStore();
void testCiRunner();
void testCiWeb();
void testPagesStore();
int testRecovery();

int main() {
  try {
    testProjectIndex();
    testRouterMarkdown();
    testDeletion();
    testDashboard();
    testBulkPublishDiscovery();
    testCliHelp();
    testClientManagement();
    testSetupConfig();
    testCiWorkflow();
    testCiStore();
    testCiRunner();
    testCiWeb();
    testPagesStore();
    failures += testRecovery();
    testValidation();
    testRemoteUrls();
    testClientConfig();
    testRefStatus();
    testWebRenderer();
    testHttpRequestParser();
    testCheckoutMetadata();
    testClientState();
    testServerConfig();
    testAuthorizedKeys();
    testProcessTimeoutKillsTree();
    testSshCommandGrammar();
    testBareRepositoryCreation();
    testDiscoveryAndAudit();
  } catch (const std::exception& error) {
    ++failures;
    std::cerr << "FAIL: unexpected exception: " << error.what() << "\n";
  }
  if (failures != 0) {
    std::cerr << failures << " test assertion(s) failed\n";
    return 1;
  }
  std::cout << "all unit tests passed\n";
  return 0;
}
