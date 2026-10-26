// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#include "ckgit/control_rpc.hpp"

#include <array>
#include <algorithm>
#include <charconv>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>

#include "ckgit/ci_store.hpp"
#include "ckgit/validation.hpp"
#include "ckgit/metadata_store.hpp"

namespace ckgit {
namespace {

// The kinds of positional argument an operation takes, each with one validator.
enum class ArgumentKind { kProject, kBranch, kCheckoutPath, kCiRunId, kStepIndex, kByteOffset };

struct OperationGrammar {
  std::string_view name;
  std::size_t arity;
  std::array<ArgumentKind, kMaximumControlArguments> arguments;
};

// Every version-1 operation and its exact positional arguments. Documented in
// docs/protocol/01-ssh-and-control-v1.md; an operation is added here only
// after its grammar, limits, authorization, and negative tests are there.
constexpr ArgumentKind kProject = ArgumentKind::kProject;
constexpr ArgumentKind kRunId = ArgumentKind::kCiRunId;
constexpr std::array kOperations{
    OperationGrammar{"ping", 0, {}},
    OperationGrammar{"version", 0, {}},
    OperationGrammar{"versions", 0, {}},
    OperationGrammar{"list-projects", 0, {}},
    OperationGrammar{"checkouts", 0, {}},
    OperationGrammar{"ci-overview", 0, {}},
    OperationGrammar{"refs", 1, {kProject}},
    OperationGrammar{"refresh", 1, {kProject}},
    OperationGrammar{"forget-checkout", 1, {kProject}},
    OperationGrammar{"releases", 1, {kProject}},
    OperationGrammar{"ci-status", 1, {kProject}},
    OperationGrammar{"default-branch", 1, {kProject}},
    OperationGrammar{"create", 2, {kProject, ArgumentKind::kBranch}},
    OperationGrammar{"set-default-branch", 2, {kProject, ArgumentKind::kBranch}},
    OperationGrammar{"register", 2, {kProject, ArgumentKind::kCheckoutPath}},
    OperationGrammar{"replace-checkout", 2, {kProject, ArgumentKind::kCheckoutPath}},
    OperationGrammar{"ci-run", 2, {kProject, kRunId}},
    OperationGrammar{"ci-cancel", 2, {kProject, kRunId}},
    OperationGrammar{"ci-placement", 2, {kProject, kRunId}},
    OperationGrammar{"ci-log", 4, {kProject, kRunId, ArgumentKind::kStepIndex, ArgumentKind::kByteOffset}},
};

// A canonical unsigned decimal: digits only, no sign, and no leading zero
// unless the value is exactly 0, so every number has one spelling.
bool isCanonicalDecimal(std::string_view value, std::size_t maximum_digits) {
  return !value.empty() && value.size() <= maximum_digits && (value.size() == 1 || value.front() != '0') &&
         std::all_of(value.begin(), value.end(), [](unsigned char character) {
           return character >= '0' && character <= '9';
         });
}

bool isValidArgument(ArgumentKind kind, std::string_view value) {
  switch (kind) {
    case ArgumentKind::kProject: return isValidProjectName(value);
    case ArgumentKind::kBranch: return isValidBranchName(value);
    case ArgumentKind::kCheckoutPath: return isValidCheckoutPathToken(value);
    case ArgumentKind::kCiRunId: return isValidCiId(value);
    case ArgumentKind::kStepIndex: {
      std::size_t step = 0;
      return isCanonicalDecimal(value, 4) &&
             std::from_chars(value.data(), value.data() + value.size(), step).ec == std::errc{} &&
             step <= kMaximumCiSteps;
    }
    case ArgumentKind::kByteOffset: {
      std::uint64_t offset = 0;
      const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), offset);
      return isCanonicalDecimal(value, 20) && error == std::errc{} && end == value.data() + value.size();
    }
  }
  return false;
}

}  // namespace

bool isValidControlOperation(std::string_view operation, const std::vector<std::string>& arguments) {
  const auto grammar = std::find_if(kOperations.begin(), kOperations.end(),
                                    [&](const OperationGrammar& entry) { return entry.name == operation; });
  if (grammar == kOperations.end() || arguments.size() != grammar->arity) return false;
  for (std::size_t index = 0; index < grammar->arity; ++index) {
    if (!isValidArgument(grammar->arguments[index], arguments[index])) return false;
  }
  return true;
}

std::optional<ControlRequest> parseControlRequest(std::string_view line) {
  if (line.empty() || line.size() > kMaximumControlRequestBytes || line.back() != '\n' ||
      line.find('\n') != line.size() - 1) {
    return std::nullopt;
  }
  for (const unsigned char character : line.substr(0, line.size() - 1)) {
    if (character < 0x20 || character > 0x7e) return std::nullopt;
  }
  constexpr std::string_view kPrefix{"CKGIT-CONTROL/1 "};
  const std::string_view content = line.substr(0, line.size() - 1);
  if (content.rfind(kPrefix, 0) != 0) return std::nullopt;
  std::vector<std::string> tokens;
  const std::string_view rest = content.substr(kPrefix.size());
  std::size_t start = 0;
  while (start <= rest.size()) {
    const std::size_t end = rest.find(' ', start);
    const std::string_view token =
        rest.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
    if (token.empty() || tokens.size() == 2 + kMaximumControlArguments) return std::nullopt;
    tokens.emplace_back(token);
    if (end == std::string_view::npos) break;
    start = end + 1;
  }
  if (tokens.size() < 2 || !isValidClientId(tokens[0])) return std::nullopt;
  ControlRequest request{tokens[0], tokens[1], std::vector<std::string>(tokens.begin() + 2, tokens.end())};
  if (!isValidControlOperation(request.operation, request.arguments)) return std::nullopt;
  return request;
}

}  // namespace ckgit
