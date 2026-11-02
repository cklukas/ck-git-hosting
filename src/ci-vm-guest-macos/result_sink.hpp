// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#pragma once
#include "ckgit/ci_vm_protocol.hpp"

namespace ckgit {
// Synchronous guest side of the VM result protocol. The host owns the lease;
// each acknowledgement communicates cancellation or loss of that lease.
class CiVmGuestSink : public CiResultSink {
 public:
  CiVmGuestSink(CiVmChannel& channel, const CiPreparedJob& assignment, const CiRemotePolicy& policy,
                std::filesystem::path relay_root);
  void runStarted(const CiRunRecord&) override;
  void stepStarted(const CiRunRecord&, std::size_t) override;
  void appendLog(std::size_t, std::string_view) override;
  void stepFinished(const CiRunRecord&, std::size_t) override;
  void heartbeat(const CiRunRecord&) override;
  void tick() override;
  void progress(const CiRunRecord&) override;
  bool cancelRequested() override { return cancelled_; }
  bool leaseLost() override { return lease_lost_; }
  CiArtifactRecord putArtifact(const CiArtifact&, const std::filesystem::path&, const std::string&, bool) override;
  void putPagesSite(const std::filesystem::path&) override;
  CiRunRecord finish(CiRunRecord) override;
 private:
  void request(CiVmFrameType type, std::string_view payload);
  void acknowledge();
  void upload(std::string_view header, const std::filesystem::path& file, std::uint64_t size);
  CiVmChannel& channel_;
  const CiPreparedJob& assignment_;
  const CiRemotePolicy& policy_;
  std::filesystem::path relay_root_;
  std::vector<bool> artifacts_;
  std::vector<CiDeclaredOutput> outputs_;
  std::chrono::steady_clock::time_point last_poll_{};
  bool cancelled_ = false;
  bool lease_lost_ = false;
};
}
