// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#include "ckgit/cli_help.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error("CLI help: " + message);
}

void expectHelp(const std::vector<std::string>& arguments, const std::string& topic) {
  const auto result = ckgit::prepareClientInvocation(arguments);
  require(result.exit_code == 0, "help should exit successfully: " + topic);
  require(result.standard_error.empty(), "help should not write stderr: " + topic);
  require(result.standard_output.find("Usage:") != std::string::npos, "help should include usage: " + topic);
  require(result.standard_output.find("Examples:") != std::string::npos, "help should include examples: " + topic);
  require(result.standard_output.find("Scope and defaults:") != std::string::npos, "help should explain scope: " + topic);
  require(result.standard_output.find("Effects:") != std::string::npos, "help should explain consequences: " + topic);
  require(result.standard_output.find("Exit codes:") != std::string::npos, "help should explain recovery: " + topic);
}

void expectError(const std::vector<std::string>& arguments, const std::string& detail,
                 const std::string& help_topic) {
  const auto result = ckgit::prepareClientInvocation(arguments);
  require(result.exit_code == 2, "bad arguments should exit 2: " + detail);
  require(result.standard_output.empty(), "bad arguments should not write stdout: " + detail);
  require(result.standard_error.find(detail) != std::string::npos, "error should identify " + detail);
  require(result.standard_error.find("Try 'ckgit help" + help_topic + "'.") != std::string::npos,
          "error should point to relevant help: " + help_topic);
}

}  // namespace

void testCliHelp() {
  const std::vector<std::vector<std::string>> command_paths{
      {"setup"}, {"doctor"}, {"projects"}, {"fetch"}, {"update"}, {"checkout", "forget"},
      {"publish"}, {"sync"}, {"status"}, {"clone"}, {"scan"}, {"web"},
      {"checkout"}, {"checkout", "list"}, {"checkout", "set-canonical"},
      {"checkout", "migrate"}, {"register"}, {"create"}, {"config"},
      {"config", "show"}, {"completion"}};
  for (const auto& path : command_paths) {
    const auto topic = path.front();
    auto long_help = path;
    long_help.push_back("--help");
    expectHelp(long_help, topic);
    auto short_help = path;
    short_help.push_back("-h");
    expectHelp(short_help, topic);
    auto help_command = path;
    help_command.insert(help_command.begin(), "help");
    expectHelp(help_command, topic);
    require(ckgit::prepareClientInvocation(long_help).standard_output ==
                ckgit::prepareClientInvocation(short_help).standard_output &&
            ckgit::prepareClientInvocation(long_help).standard_output ==
                ckgit::prepareClientInvocation(help_command).standard_output,
            "all help spellings should share a definition");
  }
  const auto global = ckgit::prepareClientInvocation({"--help"});
  require(global.exit_code == 0 && global.standard_error.empty(), "global help should use stdout");
  require(global.standard_output == ckgit::prepareClientInvocation({"-h"}).standard_output &&
              global.standard_output == ckgit::prepareClientInvocation({"help"}).standard_output,
          "global aliases should agree");
  require(global.standard_output.find("[--config PATH]") != std::string::npos,
          "configuration should be shown as optional");
  require(global.standard_output.find("private managed-checkout inventory") != std::string::npos,
          "global help should describe shared management");
  require(ckgit::prepareClientInvocation({"publish", "--config", "/nonexistent/config", "--help"}).exit_code == 0,
          "help must never load even an explicit configuration path");
  require(ckgit::prepareClientInvocation({"checkout", "--config", "/nonexistent/config", "list", "-h"}).exit_code == 0,
          "nested help should allow options before the subcommand");
  const auto version = ckgit::prepareClientInvocation({"--version"});
  require(version.exit_code == 0 && version.standard_output.rfind("ckgit ", 0) == 0 &&
              version.standard_output.size() > 7 && version.standard_error.empty(),
          "version should identify the build on stdout");
  const auto build = ckgit::buildVersion();
  require(!build.empty() && build.find_first_of("\r\n") == std::string::npos && !build.starts_with("ckgit "),
          "shared build identifier should omit the CLI product prefix and line terminator");
  require(version.standard_output == "ckgit " + build + "\n" &&
              ckgit::clientVersion() == version.standard_output,
          "CLI and dashboard should use the same running build identifier");
  // Every daemon prints "<program> <build>\n" from the same helper, so the four
  // suite versions the operator sees are all rooted in one build identifier.
  require(ckgit::versionLine("ck-ci-runnerd") == "ck-ci-runnerd " + build + "\n" &&
              ckgit::versionLine("ckgit") == ckgit::clientVersion(),
          "daemon --version lines should share the CLI's build identifier");

  expectError({}, "a command is required", "");
  expectError({"puslish"}, "unknown command 'puslish'", "");
  expectError({"--unknown"}, "unknown option '--unknown'", "");
  expectError({"help", "unknown"}, "unknown help topic", "");
  expectError({"checkout", "unknown"}, "unknown checkout command", " checkout");
  expectError({"checkout"}, "subcommand is required", " checkout");
  expectError({"publish", "--wat"}, "unknown option '--wat'", " publish");
  expectError({"sync", "--repo"}, "--repo requires PATH", " sync");
  expectError({"sync", "--repo", "--dry-run"}, "--repo requires PATH", " sync");
  expectError({"publish", "--branch="}, "--branch must not be empty", " publish");
  expectError({"publish", "--branch=bad..name"}, "valid local Git branch name", " publish");
  expectError({"create", "project.git"}, "NAME must not end in .git", " create");
  expectError({"clone"}, "missing required argument NAME", " clone");
  expectError({"clone", "--all"}, "requires --into DIR", " clone");
  expectError({"clone", "--all", "--project", "one", "--into", "."}, "choose --all or --project", " clone");
  expectError({"clone", "--all", "--into", ".", "one"}, "no positional arguments", " clone");
  expectError({"clone", "one", "--dry-run"}, "apply to bulk clone", " clone");
  expectError({"fetch", "--all", "--repo", "."}, "choose one scope", " fetch");
  expectError({"update", "--project", "one", "--repo", "."}, "choose one scope", " update");
  require(!ckgit::prepareClientInvocation({"clone", "--all", "--into", ".", "--yes"}).exit_code,
          "bulk clone omits the single-project positional argument");
  const auto selected_clone = ckgit::prepareClientInvocation({"clone", "--project=one", "--project", "one", "--project", "two", "--into", "."});
  require(!selected_clone.exit_code && selected_clone.arguments ==
              std::vector<std::string>{"--project", "one", "--project", "two", "--into", "."},
          "bulk selection is repeatable and deduplicated");
  const auto dash_forget = ckgit::prepareClientInvocation({"checkout", "forget", "--yes", "--", "--config"});
  require(!dash_forget.exit_code && dash_forget.arguments ==
              std::vector<std::string>{"forget", "--yes", "--", "--config"},
          "forget preserves a project name that looks like an option");
  expectError({"clone", "project", "folder", "extra"}, "unexpected argument 'extra'", " clone");
  expectError({"web", "--port", "65536"}, "--port must be a port between 1 and 65535", " web");
  expectError({"web", "--remote-port=0"}, "--remote-port must be a port", " web");
  expectError({"web", "bad@@host"}, "ADMIN-HOST must be an SSH host", " web");
  expectError({"publish", "--yes=false"}, "--yes does not take a value", " publish");
  expectError({"sync", "--repo", ".", "--scan"}, "--repo and --scan", " sync");
  expectError({"status", "--repo", ".", "--scan"}, "--repo and --scan", " status");
  expectError({"sync", "--replace-checkout"}, "--replace-checkout requires --repo PATH", " sync");
  expectError({"completion", "fish"}, "SHELL must be bash or zsh", " completion");

  const auto normalized = ckgit::prepareClientInvocation(
      {"--config=/path/to/client.ini", "publish", "--branch=main", "--branch", "main", "--branch", "topic", "--yes", "--yes", "--", "-folder"});
  require(!normalized.exit_code && normalized.command == "publish", "valid arguments should continue to dispatch");
  require(normalized.arguments == std::vector<std::string>{"--config", "/path/to/client.ini", "--branch", "main", "--branch", "topic", "--yes", "./-folder"},
          "normalization should preserve scope, deduplicate repeated selectors, and protect positional paths");
  const auto nested = ckgit::prepareClientInvocation(
      {"--config", "/path/to/client.ini", "checkout", "--scan", "list", "project"});
  require(!nested.exit_code && nested.command == "checkout" &&
              nested.arguments == std::vector<std::string>{"list", "--config", "/path/to/client.ini", "--scan", "project"},
          "nested commands should normalize options consistently");
  const auto show = ckgit::prepareClientInvocation({"config", "show"});
  require(!show.exit_code && show.arguments == std::vector<std::string>{"show"},
          "config show must permit the default configuration");
  const auto scan = ckgit::prepareClientInvocation({"scan", "--", "-one", "-two"});
  require(!scan.exit_code && scan.arguments == std::vector<std::string>{"./-one", "./-two"},
          "repeatable path positionals should preserve -- semantics");
  const auto dash_clone = ckgit::prepareClientInvocation({"clone", "--config", "/path/to/client.ini", "--", "--config", "-folder"});
  require(!dash_clone.exit_code && dash_clone.arguments ==
              std::vector<std::string>{"--config", "/path/to/client.ini", "--", "--config", "./-folder"},
          "clone retains the project argument boundary even when a project is named after an option");
  require(!ckgit::prepareClientInvocation({"checkout", "migrate", "--dry-run"}).exit_code,
          "migration should be documented and accepted by the same schema");
  require(!ckgit::prepareClientInvocation({"publish", "--verbose", "--dry-run"}).exit_code &&
              !ckgit::prepareClientInvocation({"sync", "--verbose", "--dry-run"}).exit_code,
          "both ref plans should offer explicit expansion");
  for (const auto* shell : {"bash", "zsh"}) {
    const auto completion = ckgit::prepareClientInvocation({"completion", shell});
    require(completion.exit_code == 0 && completion.standard_error.empty(), "completion should work offline");
    for (const auto& path : command_paths) {
      require(completion.standard_output.find(path.back()) != std::string::npos,
              "completion should include all declared commands");
    }
    for (const auto* option : {"--verbose", "--dry-run", "--remote-port", "--no-tags", "--replace-checkout"})
      require(completion.standard_output.find(option) != std::string::npos,
              "completion should share current option definitions");
  }
}
