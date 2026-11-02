// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#pragma once
#include "ckgit/ci_runner.hpp"

namespace ckgit {
// The share is attached read-only. The assignment and share path are supplied
// by the guest bootstrap, not by workflow commands.
class CiVmStagedSource : public CiSourceProvider {
 public:
  CiVmStagedSource(const CiPreparedJob& assignment, std::filesystem::path exchange,
                   std::function<bool()> should_continue = {});
  void checkout(const CiCheckoutRequest& request, const std::filesystem::path& destination) override;
  void checkout(const CiCheckoutRequest& request, const std::filesystem::path& destination,
                const std::function<bool()>& should_continue) override;
 private:
  const CiPreparedJob& assignment_;
  std::filesystem::path exchange_;
  std::function<bool()> should_continue_;
};
}
