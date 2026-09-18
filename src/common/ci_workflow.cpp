// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#include "ckgit/ci_workflow.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <initializer_list>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ckgit/validation.hpp"
#include "ckgit/yaml_subset.hpp"

namespace ckgit {
namespace {

// The syntax lives in yaml_subset.cpp; this file interprets the parsed tree
// against the workflow schema. The wrappers keep every error in the same
// "ci workflow: ..." wording the reference document quotes.
using Node = YamlNode;

[[noreturn]] void malformed(const std::string& message, std::size_t line) {
  yamlMalformed(kCiWorkflowYamlDialect, message, line);
}

[[noreturn]] void tooLarge(const std::string& message) {
  yamlTooLarge(kCiWorkflowYamlDialect, message);
}

const Node& requireKind(const Node& node, Node::Kind kind, const char* what) {
  return yamlRequireKind(kCiWorkflowYamlDialect, node, kind, what);
}

const Node* findEntry(const Node& mapping, std::string_view key) {
  return yamlFindEntry(mapping, key);
}

void rejectUnknownKeys(const Node& mapping, std::initializer_list<std::string_view> allowed) {
  yamlRejectUnknownKeys(kCiWorkflowYamlDialect, mapping, allowed);
}

bool isValidEnvName(std::string_view name) {
  if (name.empty() || name.size() > kMaximumCiKeyBytes) return false;
  const unsigned char first = name.front();
  const bool first_ok = (first >= 'A' && first <= 'Z') || (first >= 'a' && first <= 'z') || first == '_';
  if (!first_ok) return false;
  return std::all_of(name.begin(), name.end(), [](unsigned char character) {
    return (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z') ||
           (character >= '0' && character <= '9') || character == '_';
  });
}

// A tag trigger pattern: an exact name or a single trailing '*' wildcard, over
// the same safe characters release tags are stored under (no '/').
bool isValidTagPattern(std::string_view pattern) {
  if (pattern.empty() || pattern.size() > 128) return false;
  std::string_view body = pattern;
  if (body.back() == '*') body.remove_suffix(1);  // one trailing wildcard is allowed
  return std::all_of(body.begin(), body.end(), [](unsigned char character) {
    return (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z') ||
           (character >= '0' && character <= '9') || character == '.' || character == '_' ||
           character == '-';
  });
}

// A pinned sister ref: a branch, tag, or commit id. Validated so it cannot be
// taken for a git option (no leading '-') or escape the object namespace (no
// '..', no leading '/'), and restricted to the characters those names use.
bool isValidSisterRef(std::string_view ref) {
  if (ref.empty() || ref.size() > 255 || ref.front() == '-' || ref.front() == '/' ||
      ref.find("..") != std::string_view::npos) {
    return false;
  }
  return std::all_of(ref.begin(), ref.end(), [](unsigned char character) {
    return (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z') ||
           (character >= '0' && character <= '9') || character == '.' || character == '_' ||
           character == '-' || character == '/';
  });
}

bool isValidCiName(std::string_view name) {
  return !name.empty() && name.size() <= kMaximumCiNameBytes &&
         std::all_of(name.begin(), name.end(), [](unsigned char character) {
           return (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z') ||
                  (character >= '0' && character <= '9') || character == '.' || character == '_' ||
                  character == '-';
         });
}

CiEnv interpretEnv(const Node& node) {
  requireKind(node, Node::Kind::Mapping, "env to be a mapping of name: value");
  if (node.entries.size() > kMaximumCiEnvEntries) tooLarge("env has too many entries");
  CiEnv env;
  for (const auto& entry : node.entries) {
    if (!isValidEnvName(entry.first)) malformed("invalid environment name '" + entry.first + "'", node.line);
    const Node& value = requireKind(entry.second, Node::Kind::Scalar, "an env value to be a scalar");
    env.emplace_back(entry.first, value.scalar);
  }
  return env;
}

// A path that is safe to collect from the checkout: relative, within the tree,
// no control bytes or backslashes. A single trailing '/' (a directory) is
// accepted; empty, absolute, '.'/'..' or empty components are not.
bool isSafeRelativePath(std::string_view path) {
  while (path.size() > 1 && path.back() == '/') path.remove_suffix(1);
  if (path.empty() || path.size() > kMaximumCiPathBytes || path.front() == '/') return false;
  std::size_t start = 0;
  while (start <= path.size()) {
    const std::size_t slash = path.find('/', start);
    const std::string_view component =
        path.substr(start, slash == std::string_view::npos ? std::string_view::npos : slash - start);
    if (component.empty() || component == "." || component == "..") return false;
    for (const unsigned char character : component) {
      if (character < 0x20 || character == 0x7f || character == '\\') return false;
    }
    if (slash == std::string_view::npos) break;
    start = slash + 1;
  }
  return true;
}

CiArtifact interpretArtifact(const Node& node, const std::string& job_name) {
  requireKind(node, Node::Kind::Mapping, "artifacts to be a mapping");
  rejectUnknownKeys(node, {"paths", "name", "retention_days"});
  CiArtifact artifact;
  artifact.name = job_name;
  if (const Node* name = findEntry(node, "name")) {
    artifact.name = requireKind(*name, Node::Kind::Scalar, "an artifact name to be a scalar").scalar;
    if (!isValidCiName(artifact.name)) malformed("invalid artifact name '" + artifact.name + "'", node.line);
  }
  // "release" is reserved: a tag build's release record is stored as
  // release.ini beside each asset's own <name>.ini sidecar in the same
  // directory (see writeCiReleaseRecord/writeCiReleaseArtifactRecord), so an
  // artifact named "release" would silently overwrite the release record
  // instead of getting its own sidecar. Caught here (whether the name came
  // from an explicit `name:` or defaulted from the job name) rather than
  // discovered later as a release with no visible assets.
  if (artifact.name == "release") {
    malformed("artifact name 'release' is reserved for the release record", node.line);
  }
  const Node* paths = findEntry(node, "paths");
  if (paths == nullptr) malformed("artifacts needs a 'paths' list", node.line);
  requireKind(*paths, Node::Kind::Sequence, "artifact paths to be a list");
  if (paths->items.empty()) malformed("artifact paths is empty", node.line);
  if (paths->items.size() > kMaximumCiArtifactPaths) tooLarge("too many artifact paths");
  for (const Node& item : paths->items) {
    const std::string& value = requireKind(item, Node::Kind::Scalar, "each artifact path to be a scalar").scalar;
    if (!isSafeRelativePath(value)) malformed("unsafe or absolute artifact path '" + value + "'", node.line);
    artifact.paths.push_back(value);
  }
  if (const Node* retention = findEntry(node, "retention_days")) {
    const std::string& text =
        requireKind(*retention, Node::Kind::Scalar, "retention_days to be a number").scalar;
    unsigned value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || value == 0 ||
        value > kMaximumCiRetentionDaysCap) {
      malformed("retention_days must be a positive number of days", node.line);
    }
    artifact.retention_days = value;
  }
  return artifact;
}

CiStep interpretStep(const Node& node) {
  requireKind(node, Node::Kind::Mapping, "a step to be a mapping");
  rejectUnknownKeys(node, {"name", "run", "script"});
  CiStep step;
  if (const Node* name = findEntry(node, "name")) {
    step.name = requireKind(*name, Node::Kind::Scalar, "a step name to be a scalar").scalar;
    if (step.name.empty() || step.name.size() > kMaximumCiNameBytes) {
      malformed("a step name is empty or too long", node.line);
    }
  }
  const Node* run = findEntry(node, "run");
  const Node* script = findEntry(node, "script");
  if ((run != nullptr) == (script != nullptr)) {
    malformed("a step needs exactly one of 'run' or 'script'", node.line);
  }
  if (script != nullptr) {
    step.script = requireKind(*script, Node::Kind::Scalar, "script to be text").scalar;
    if (step.script.empty()) malformed("a script is empty", node.line);
    if (step.script.size() > kMaximumCiScriptBytes) tooLarge("a script exceeds its length limit");
    return step;
  }
  if (run->kind == Node::Kind::Scalar) {
    step.script = run->scalar;  // scalar run: is a shell command line
    if (step.script.empty()) malformed("a run command is empty", node.line);
    return step;
  }
  requireKind(*run, Node::Kind::Sequence, "run to be a command line or an argv list");
  if (run->items.empty()) malformed("a run argv list is empty", node.line);
  if (run->items.size() > kMaximumCiArgvItems) tooLarge("a run argv list is too long");
  for (const Node& item : run->items) {
    const Node& argument = requireKind(item, Node::Kind::Scalar, "each argv item to be a scalar");
    if (argument.scalar.empty()) malformed("an argv item is empty", node.line);
    step.argv.push_back(argument.scalar);
  }
  return step;
}

CiJob interpretJob(const Node& node) {
  requireKind(node, Node::Kind::Mapping, "each job to be a mapping");
  rejectUnknownKeys(node, {"name", "env", "steps", "artifacts"});
  CiJob job;
  const Node* name = findEntry(node, "name");
  if (name == nullptr) malformed("a job is missing 'name'", node.line);
  job.name = requireKind(*name, Node::Kind::Scalar, "a job name to be a scalar").scalar;
  if (!isValidCiName(job.name)) malformed("invalid job name '" + job.name + "'", node.line);
  if (const Node* env = findEntry(node, "env")) job.env = interpretEnv(*env);
  const Node* steps = findEntry(node, "steps");
  if (steps == nullptr) malformed("job '" + job.name + "' has no steps", node.line);
  requireKind(*steps, Node::Kind::Sequence, "steps to be a list");
  if (steps->items.empty()) malformed("job '" + job.name + "' has no steps", node.line);
  if (steps->items.size() > kMaximumCiStepsPerJob) tooLarge("a job has too many steps");
  for (const Node& step : steps->items) job.steps.push_back(interpretStep(step));
  if (const Node* artifacts = findEntry(node, "artifacts")) job.artifact = interpretArtifact(*artifacts, job.name);
  return job;
}

CiWorkflow interpret(const Node& root) {
  requireKind(root, Node::Kind::Mapping, "the workflow to be a mapping");
  rejectUnknownKeys(root, {"version", "on", "env", "sisters", "cache", "jobs", "pages"});

  const Node* version = findEntry(root, "version");
  if (version == nullptr) malformed("the workflow is missing 'version'", root.line);
  const std::string& version_text = requireKind(*version, Node::Kind::Scalar, "version to be a number").scalar;
  int version_value = 0;
  const auto [end, error] = std::from_chars(version_text.data(), version_text.data() + version_text.size(), version_value);
  if (error != std::errc{} || end != version_text.data() + version_text.size() || version_value != 1) {
    malformed("version must be 1", root.line);
  }

  CiWorkflow workflow;
  workflow.version = 1;

  if (const Node* on = findEntry(root, "on")) {
    requireKind(*on, Node::Kind::Mapping, "'on' to be a mapping");
    rejectUnknownKeys(*on, {"branches", "tags"});
    if (const Node* branches = findEntry(*on, "branches")) {
      requireKind(*branches, Node::Kind::Sequence, "branches to be a list");
      if (branches->items.size() > kMaximumCiBranches) tooLarge("too many branches");
      for (const Node& branch : branches->items) {
        const std::string& value = requireKind(branch, Node::Kind::Scalar, "each branch to be a scalar").scalar;
        if (!isValidBranchName(value)) malformed("invalid branch name '" + value + "'", on->line);
        workflow.branches.push_back(value);
      }
    }
    if (const Node* tags = findEntry(*on, "tags")) {
      requireKind(*tags, Node::Kind::Sequence, "tags to be a list");
      if (tags->items.size() > kMaximumCiBranches) tooLarge("too many tags");
      for (const Node& tag : tags->items) {
        const std::string& value = requireKind(tag, Node::Kind::Scalar, "each tag to be a scalar").scalar;
        if (!isValidTagPattern(value)) malformed("invalid tag pattern '" + value + "'", on->line);
        workflow.tags.push_back(value);
      }
    }
  } else {
    // No `on:` — build the default branch and cut a release on any tag.
    workflow.triggers_default_branch = true;
    workflow.tags.push_back("*");
  }

  if (const Node* env = findEntry(root, "env")) workflow.env = interpretEnv(*env);

  if (const Node* sisters = findEntry(root, "sisters")) {
    requireKind(*sisters, Node::Kind::Sequence, "sisters to be a list");
    if (sisters->items.size() > kMaximumCiSisters) tooLarge("too many sisters");
    std::set<std::string> seen;
    for (const Node& item : sisters->items) {
      // A sister is either a bare project name, or a { name, ref } mapping that
      // pins it to a branch, tag, or commit.
      CiSister sister;
      if (item.kind == Node::Kind::Scalar) {
        sister.name = item.scalar;
      } else if (item.kind == Node::Kind::Mapping) {
        rejectUnknownKeys(item, {"name", "ref"});
        const Node* name = findEntry(item, "name");
        if (name == nullptr) malformed("a sister needs a 'name'", item.line);
        sister.name = requireKind(*name, Node::Kind::Scalar, "a sister name to be a scalar").scalar;
        if (const Node* ref = findEntry(item, "ref")) {
          sister.ref = requireKind(*ref, Node::Kind::Scalar, "a sister ref to be a scalar").scalar;
          if (!isValidSisterRef(sister.ref)) malformed("invalid sister ref '" + sister.ref + "'", item.line);
        }
      } else {
        malformed("each sister to be a name or a { name, ref } mapping", item.line);
      }
      if (!isValidProjectName(sister.name)) malformed("invalid sister project name '" + sister.name + "'", sisters->line);
      if (!seen.insert(sister.name).second) malformed("duplicate sister project '" + sister.name + "'", sisters->line);
      workflow.sisters.push_back(std::move(sister));
    }
  }

  if (const Node* caches = findEntry(root, "cache")) {
    requireKind(*caches, Node::Kind::Sequence, "cache to be a list");
    if (caches->items.size() > kMaximumCiCaches) tooLarge("too many caches");
    std::set<std::string> seen;
    for (const Node& item : caches->items) {
      // A cache is either a bare name, or a { name, env } mapping that also
      // binds environment variables (e.g. CCACHE_DIR) to the cache directory.
      CiCache cache;
      if (item.kind == Node::Kind::Scalar) {
        cache.name = item.scalar;
      } else if (item.kind == Node::Kind::Mapping) {
        rejectUnknownKeys(item, {"name", "env"});
        const Node* name = findEntry(item, "name");
        if (name == nullptr) malformed("a cache needs a 'name'", item.line);
        cache.name = requireKind(*name, Node::Kind::Scalar, "a cache name to be a scalar").scalar;
        if (const Node* env = findEntry(item, "env")) {
          requireKind(*env, Node::Kind::Sequence, "cache env to be a list");
          if (env->items.size() > kMaximumCiCacheEnv) tooLarge("a cache has too many env bindings");
          for (const Node& var : env->items) {
            const std::string& value = requireKind(var, Node::Kind::Scalar, "a cache env name to be a scalar").scalar;
            if (!isValidEnvName(value)) malformed("invalid cache env name '" + value + "'", item.line);
            cache.env.push_back(value);
          }
        }
      } else {
        malformed("each cache to be a name or a { name, env } mapping", item.line);
      }
      if (!isValidCiName(cache.name)) malformed("invalid cache name '" + cache.name + "'", caches->line);
      if (!seen.insert(cache.name).second) malformed("duplicate cache '" + cache.name + "'", caches->line);
      workflow.caches.push_back(std::move(cache));
    }
  }

  if (const Node* pages = findEntry(root, "pages")) {
    requireKind(*pages, Node::Kind::Mapping, "pages to be a mapping");
    rejectUnknownKeys(*pages, {"path"});
    const Node* path = findEntry(*pages, "path");
    if (path == nullptr) malformed("pages needs a 'path'", pages->line);
    const std::string& value = requireKind(*path, Node::Kind::Scalar, "pages path to be a scalar").scalar;
    if (!isSafeRelativePath(value)) malformed("unsafe or absolute pages path '" + value + "'", pages->line);
    workflow.pages_path = value;
  }

  const Node* jobs = findEntry(root, "jobs");
  if (jobs == nullptr) malformed("the workflow is missing 'jobs'", root.line);
  requireKind(*jobs, Node::Kind::Sequence, "jobs to be a list");
  if (jobs->items.empty()) malformed("the workflow has no jobs", root.line);
  if (jobs->items.size() > kMaximumCiJobs) tooLarge("too many jobs");
  std::set<std::string> job_names;
  for (const Node& job_node : jobs->items) {
    CiJob job = interpretJob(job_node);
    if (!job_names.insert(job.name).second) malformed("duplicate job name '" + job.name + "'", job_node.line);
    workflow.jobs.push_back(std::move(job));
  }
  return workflow;
}

}  // namespace

CiWorkflow parseCiWorkflow(std::string_view content) {
  return interpret(parseYamlSubset(content, kCiWorkflowYamlDialect));
}

}  // namespace ckgit
