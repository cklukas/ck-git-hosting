// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "ckgit/ci_dependency_lock.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>

#include "ckgit/hash.hpp"
#include "ckgit/validation.hpp"
#include "ckgit/yaml_subset.hpp"

namespace ckgit {
namespace {
constexpr YamlDialect dialect{
    {256 * 1024, 4096, 8192, 128, 1024, 1024, 256, 1024, 16, 10}, "dependency lock", "the dependency lock"};
using Node = YamlNode;
const Node& field(const Node& node, std::string_view name, Node::Kind kind) {
  const auto* value = yamlFindEntry(node, name);
  if (!value) yamlMalformed(dialect, "missing required field '" + std::string(name) + "'", node.line);
  return yamlRequireKind(dialect, *value, kind, "the field to have the expected type");
}
std::string scalar(const Node& node, std::string_view name) {
  return field(node, name, Node::Kind::Scalar).scalar;
}
bool safeToken(std::string_view value, std::size_t limit) {
  if (value.empty() || value.size() > limit) return false;
  return std::all_of(value.begin(), value.end(), [](unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
           c == '_' || c == '-' || c == '+';
  });
}
void digest(const std::string& value, std::size_t line) {
  if (!isValidSha256Hex(value))
    yamlMalformed(dialect, "SHA-256 must be 64 lowercase hexadecimal digits", line);
}
}  // namespace
CiDependencyLock parseCiDependencyLock(std::string_view content) {
  const auto root = parseYamlSubset(content, dialect);
  yamlRejectUnknownKeys(dialect, root, {"version", "platform", "architecture", "abi", "inputs", "packages"});
  if (scalar(root, "version") != "1") yamlMalformed(dialect, "version must be 1", root.line);
  CiDependencyLock lock;
  lock.sha256 = sha256Hex(content);
  lock.platform = scalar(root, "platform");
  lock.architecture = scalar(root, "architecture");
  lock.abi = scalar(root, "abi");
  if ((lock.platform != "windows" && lock.platform != "macos" && lock.platform != "linux") ||
      (lock.architecture != "x64" && lock.architecture != "arm64") || !safeToken(lock.abi, 128))
    yamlMalformed(dialect, "invalid target platform, architecture or ABI", root.line);
  const auto& inputs = field(root, "inputs", Node::Kind::Sequence);
  if (inputs.items.empty() || inputs.items.size() > 128)
    yamlMalformed(dialect, "inputs must contain 1 to 128 entries", inputs.line);
  std::set<std::string> paths;
  for (const auto& node : inputs.items) {
    yamlRequireKind(dialect, node, Node::Kind::Mapping, "an input to be a mapping");
    yamlRejectUnknownKeys(dialect, node, {"path", "sha256"});
    CiDependencyInput input{scalar(node, "path"), scalar(node, "sha256")};
    const bool printable_ascii = std::all_of(input.path.begin(), input.path.end(), [](unsigned char c) {
      return c >= 0x20 && c <= 0x7e;
    });
    if (!printable_ascii || !isSafeRelativePath(input.path) || input.path.back() == '/' ||
        !paths.insert(input.path).second)
      yamlMalformed(dialect, "invalid or duplicate input path", node.line);
    digest(input.sha256, node.line);
    lock.inputs.push_back(std::move(input));
  }
  const auto& packages = field(root, "packages", Node::Kind::Sequence);
  if (packages.items.empty() || packages.items.size() > 1024)
    yamlMalformed(dialect, "packages must contain 1 to 1024 entries", packages.line);
  std::map<std::string, std::size_t> names;
  for (const auto& node : packages.items) {
    yamlRequireKind(dialect, node, Node::Kind::Mapping, "a package to be a mapping");
    yamlRejectUnknownKeys(dialect, node,
                          {"name", "version", "registry", "sha256", "recipe_sha256", "direct", "needs"});
    CiDependencyPackage pkg;
    pkg.name = scalar(node, "name");
    pkg.version = scalar(node, "version");
    pkg.registry = scalar(node, "registry");
    pkg.sha256 = scalar(node, "sha256");
    pkg.recipe_sha256 = scalar(node, "recipe_sha256");
    if (!safeToken(pkg.name, 128) || !safeToken(pkg.version, 128) || !isValidCiLabel(pkg.registry) ||
        !names.emplace(pkg.name, lock.packages.size()).second)
      yamlMalformed(dialect, "invalid or duplicate package identity", node.line);
    digest(pkg.sha256, node.line);
    digest(pkg.recipe_sha256, node.line);
    const auto direct = scalar(node, "direct");
    if (direct != "true" && direct != "false")
      yamlMalformed(dialect, "direct must be true or false", node.line);
    pkg.direct = direct == "true";
    const auto& needs = field(node, "needs", Node::Kind::Sequence);
    if (needs.items.size() > 256) yamlMalformed(dialect, "too many package dependencies", node.line);
    std::set<std::string> unique;
    for (const auto& item : needs.items) {
      const auto& name =
          yamlRequireKind(dialect, item, Node::Kind::Scalar, "a dependency name to be a scalar").scalar;
      if (!safeToken(name, 128) || !unique.insert(name).second)
        yamlMalformed(dialect, "invalid or duplicate dependency name", item.line);
      pkg.needs.push_back(name);
    }
    lock.packages.push_back(std::move(pkg));
  }
  std::vector<unsigned char> state(lock.packages.size(), 0);
  const auto visit = [&](auto&& self, std::size_t index) -> void {
    if (state[index] == 1) throw std::runtime_error("dependency lock: dependency cycle");
    if (state[index] == 2) return;
    state[index] = 1;
    for (const auto& name : lock.packages[index].needs) {
      const auto it = names.find(name);
      if (it == names.end()) throw std::runtime_error("dependency lock: unresolved dependency");
      self(self, it->second);
    }
    state[index] = 2;
  };
  for (std::size_t i = 0; i < lock.packages.size(); ++i)
    if (lock.packages[i].direct) visit(visit, i);
  if (std::any_of(state.begin(), state.end(), [](auto value) { return value != 2; }))
    throw std::runtime_error("dependency lock: packages must be reachable from a direct dependency");
  return lock;
}
void verifyCiDependencyInputs(const CiDependencyLock& lock,
                              const std::function<std::string(const std::string&)>& read_input) {
  if (!read_input || lock.inputs.empty()) throw std::invalid_argument("dependency lock: no input verifier");
  for (const auto& input : lock.inputs) {
    const bool printable_ascii = std::all_of(input.path.begin(), input.path.end(), [](unsigned char c) {
      return c >= 0x20 && c <= 0x7e;
    });
    if (!printable_ascii || !isSafeRelativePath(input.path) || !isValidSha256Hex(input.sha256))
      throw std::invalid_argument("dependency lock: invalid input identity");
    if (sha256Hex(read_input(input.path)) != input.sha256)
      throw std::runtime_error("dependency lock: source input is missing or changed: " + input.path);
  }
}
}  // namespace ckgit
