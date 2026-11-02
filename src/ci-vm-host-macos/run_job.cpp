// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "run_job.hpp"
#include "result_relay.hpp"
#include "ckgit/ci_vm_source.hpp"
#include "ckgit/macos_vm_process.hpp"
#include "ckgit/macos_vm_workspace.hpp"
#include <chrono>
#include <exception>
#include <memory>
#include <stdexcept>

namespace ckgit {
CiRunRecord runCiMacosVmJob(const CiPreparedJob& assignment, const CiRemotePolicy& policy,
                            CiSourceProvider& source, CiResultSink& sink, const CiMacosVmOptions& options) {
  MacosVmWorkspace workspace(options.base, options.work_root, options.min_free_gib);
  std::unique_ptr<MacosVmProcess> vm;
  CiRunRecord heartbeat;
  heartbeat.run_id = assignment.job.job_id;
  heartbeat.project_name = assignment.job.project_name;
  const auto keep = [&] { sink.heartbeat(heartbeat); return !sink.leaseLost() && !sink.cancelRequested(); };
  std::exception_ptr shutdown_heartbeat_failure;
  const auto shutdownHeartbeat = [&] {
    if (shutdown_heartbeat_failure) return;
    try { if (!sink.leaseLost()) sink.heartbeat(heartbeat); }
    catch (...) { shutdown_heartbeat_failure = std::current_exception(); }
  };
  CiRunRecord record;
  bool guest_ready = false;
  try {
    stageCiVmSources(assignment, source, workspace.exchange(), keep);
    if (!keep()) throw std::runtime_error("VM job cancelled before startup");
    vm = std::make_unique<MacosVmProcess>(options.helper, workspace.bundle(), workspace.exchange());
    CiVmChannel channel(vm->socket());
    ciVmHostHandshake(channel, assignment, policy, std::chrono::minutes(4), keep);
    guest_ready = true;
    record = relayCiVmResults(channel, assignment, policy, sink, options.relay_root);
  } catch (...) {
    const auto failure = std::current_exception();
    // Without a started helper no VM can be running. Otherwise only a clean
    // helper exit permits deletion; an uncertain stop preserves the clone.
    if (vm && !vm->stop(std::chrono::seconds(30), shutdownHeartbeat))
      throw std::runtime_error("VM shutdown was not confirmed; workspace retained and runner must stop");
    workspace.removeAfterConfirmedStop(shutdownHeartbeat);
    shutdownHeartbeat();
    if (shutdown_heartbeat_failure) std::rethrow_exception(shutdown_heartbeat_failure);
    // A cancellation before the guest is ready is an ordinary terminal job
    // outcome. A lost lease takes precedence and must never publish a result.
    if (!guest_ready && !sink.leaseLost() && sink.cancelRequested()) {
      const auto now = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::system_clock::now().time_since_epoch()).count());
      CiRunRecord cancelled;
      cancelled.run_id = assignment.job.job_id;
      cancelled.project_name = assignment.job.project_name;
      cancelled.ref = assignment.job.ref;
      cancelled.commit_id = assignment.job.commit_id;
      cancelled.parent_run_id = assignment.job.parent_run_id;
      cancelled.workflow_job = assignment.job.workflow_job;
      cancelled.attempt = assignment.job.attempt;
      cancelled.status = CiRunStatus::Cancelled;
      cancelled.started_epoch_seconds = now;
      cancelled.finished_epoch_seconds = now;
      cancelled.heartbeat_epoch_seconds = now;
      cancelled.detail = "cancelled before the VM guest was ready";
      return sink.finish(std::move(cancelled));
    }
    std::rethrow_exception(failure);
  }
  if (!vm->stop(std::chrono::seconds(30), shutdownHeartbeat))
    throw std::runtime_error("VM shutdown was not confirmed; workspace retained and runner must stop");
  workspace.removeAfterConfirmedStop(shutdownHeartbeat);
  shutdownHeartbeat();
  if (shutdown_heartbeat_failure) std::rethrow_exception(shutdown_heartbeat_failure);
  if (sink.leaseLost()) throw std::runtime_error("VM job lease lost during shutdown");
  if (sink.cancelRequested() && record.status != CiRunStatus::Cancelled) {
    record.status = CiRunStatus::Cancelled;
    record.detail = "cancelled before the VM result was committed";
  }
  return sink.finish(std::move(record));
}
}
