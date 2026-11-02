// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "result_relay.hpp"
#include "ckgit/archive.hpp"
#include "ckgit/pages_store.hpp"
#include "ckgit/temp_directory.hpp"
#include <charconv>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace ckgit {
namespace {
constexpr auto kTimeout = std::chrono::seconds(30);
template<class T> T number(std::string_view text) {
  T value{};
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
  if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
      std::to_string(value) != text) throw std::invalid_argument("invalid VM event number");
  return value;
}
std::vector<std::string_view> fields(std::string_view value) {
  std::vector<std::string_view> result;
  while (!value.empty()) {
    const auto space = value.find(' ');
    const auto field = value.substr(0, space);
    if (field.empty()) throw std::invalid_argument("invalid VM event fields");
    result.push_back(field);
    if (space == std::string_view::npos) break;
    value.remove_prefix(space + 1);
    if (value.empty()) throw std::invalid_argument("invalid VM event fields");
  }
  return result;
}
std::string unhex(std::string_view value, std::size_t cap) {
  auto result = fromHexOrDash(value, cap);
  if (!result || hexOrDash(*result) != value) throw std::invalid_argument("invalid VM event text");
  return *result;
}
bool sameStep(const CiStepResult& a, const CiStepResult& b) {
  return a.name == b.name && a.exit_code == b.exit_code && a.timed_out == b.timed_out &&
         a.output_truncated == b.output_truncated;
}
}
CiRunRecord relayCiVmResults(CiVmChannel& channel, const CiPreparedJob& assignment, const CiRemotePolicy& policy,
                             CiResultSink& sink, const std::filesystem::path& relay_root) {
  if (!relay_root.is_absolute()) throw std::invalid_argument("VM relay root must be absolute");
  CiRunRecord record;
  record.run_id = assignment.job.job_id;
  record.project_name = assignment.job.project_name;
  record.ref = assignment.job.ref;
  record.commit_id = assignment.job.commit_id;
  record.parent_run_id = assignment.job.parent_run_id;
  record.workflow_job = assignment.job.workflow_job;
  record.attempt = assignment.job.attempt;
  record.status = CiRunStatus::Running;
  CiVmOutputGuard limits(assignment, policy);
  std::vector<CiDeclaredOutput> outputs;
  std::vector<std::string> step_names;
  for (const auto& job : assignment.workflow.jobs)
    for (const auto& step : job.steps) step_names.push_back(step.name.empty() ? job.name : step.name);
  const auto total_steps = step_names.size();
  bool started = false, active_step = false, pages = false, cancel_sent = false;
  // Cancellation must be acknowledged to the guest so it can stop its step
  // and send a terminal cancelled report. A lost lease instead immediately
  // invalidates the channel and prevents any further server writes.
  const auto keep = [&] { sink.heartbeat(record); return !sink.leaseLost(); };
  const auto receive = [&] { return channel.receive(kTimeout, keep); };
  const auto ack = [&] {
    sink.tick();
    if (sink.leaseLost()) {
      channel.send(CiVmFrameType::Error, "lease-lost", kTimeout);
      throw std::runtime_error("VM job lease lost during acknowledgement");
    }
    else if (sink.cancelRequested()) { cancel_sent = true; channel.send(CiVmFrameType::Cancel, {}, kTimeout); }
    else channel.send(CiVmFrameType::Event, "ok", kTimeout);
  };
  try {
    for (;;) {
      const auto frame = receive();
      if (frame.type == CiVmFrameType::Error)
        throw std::runtime_error("VM guest error: " + frame.payload.substr(0, 200));
      if (frame.type == CiVmFrameType::Done) {
        const auto report = parseCiFinishReport(frame.payload);
        if (cancel_sent && report.status != CiRunStatus::Cancelled)
          throw std::invalid_argument("VM ignored cancellation in terminal report");
        if (active_step || limits.artifactOpen() || report.steps.size() != record.steps.size() ||
            report.outputs.size() != outputs.size() || (started && report.started_epoch_seconds != record.started_epoch_seconds))
          throw std::invalid_argument("inconsistent VM terminal report");
        for (std::size_t i = 0; i < report.steps.size(); ++i)
          if (!sameStep(report.steps[i], record.steps[i])) throw std::invalid_argument("VM step report changed");
        for (std::size_t i = 0; i < outputs.size(); ++i) {
          const auto& a = report.outputs[i]; const auto& b = outputs[i];
          if (a.name != b.name || a.release != b.release || a.retention_days != b.retention_days || a.note != b.note)
            throw std::invalid_argument("VM output report changed");
        }
        if (report.status == CiRunStatus::Success && (!started || record.steps.size() != total_steps))
          throw std::invalid_argument("incomplete VM success report");
        if (report.status == CiRunStatus::Success)
          for (const auto& step : record.steps)
            if (step.exit_code != 0 || step.timed_out) throw std::invalid_argument("failed VM step reported as success");
        record.started_epoch_seconds = report.started_epoch_seconds;
        record.status = report.status;
        record.detail = report.detail;
        // The guest's terminal report is provisional until its VM has
        // stopped and the disposable workspace has been removed.
        ack();
        // A cancellation can arrive in ack() after the guest sent its last
        // frame. It cannot amend that frame, so the host records cancellation.
        if (cancel_sent && record.status != CiRunStatus::Cancelled) {
          record.status = CiRunStatus::Cancelled;
          record.detail = "cancelled before the VM result was committed";
        }
        return record;
      }
      if (frame.type != CiVmFrameType::Event) throw std::invalid_argument("unexpected VM result frame");
      const auto part = fields(frame.payload);
      if (part.empty()) throw std::invalid_argument("empty VM event");
      if (part[0] == "started" && part.size() == 2 && !started) {
        record.started_epoch_seconds = number<std::uint64_t>(part[1]);
        if (!record.started_epoch_seconds) throw std::invalid_argument("invalid VM start time");
        started = true; sink.runStarted(record); ack();
      } else if (part[0] == "step-start" && part.size() == 2 && started && !active_step &&
                 number<std::size_t>(part[1]) == record.steps.size() && record.steps.size() < total_steps &&
                 (record.steps.empty() || (record.steps.back().exit_code == 0 && !record.steps.back().timed_out))) {
        active_step = true; sink.stepStarted(record, record.steps.size()); ack();
      } else if (part[0] == "log" && part.size() == 2 && active_step &&
                 number<std::size_t>(part[1]) == record.steps.size()) {
        const auto chunk = receive();
        if (chunk.type != CiVmFrameType::Chunk || chunk.payload.empty()) throw std::invalid_argument("invalid VM log chunk");
        limits.log(record.steps.size(), chunk.payload.size());
        sink.appendLog(record.steps.size(), chunk.payload); ack();
      } else if (part[0] == "step-end" && part.size() == 6 && active_step &&
                 number<std::size_t>(part[1]) == record.steps.size() && (part[3] == "0" || part[3] == "1") &&
                 (part[4] == "0" || part[4] == "1")) {
        CiStepResult step{unhex(part[5], kMaximumCiDetailBytes), number<int>(part[2]), part[3] == "1", part[4] == "1"};
        if (step.name != step_names[record.steps.size()]) throw std::invalid_argument("unexpected VM step name");
        record.steps.push_back(std::move(step)); active_step = false;
        sink.stepFinished(record, record.steps.size() - 1); ack();
      } else if ((part[0] == "heartbeat" || part[0] == "poll") && part.size() == 1 && started) {
        if (part[0] == "heartbeat") sink.heartbeat(record); else sink.tick();
        ack();
      } else if (part[0] == "progress" && part.size() == 2 && started) {
        record.detail = unhex(part[1], kMaximumCiDetailBytes); sink.progress(record); ack();
      } else if (part[0] == "artifact" && part.size() == 4 && started && !active_step) {
        const auto job = number<std::size_t>(part[1]);
        const auto size = number<std::uint64_t>(part[2]);
        const auto note = unhex(part[3], kMaximumCiDetailBytes);
        const auto& artifact = limits.beginArtifact(job, size);
        std::size_t required = 0;
        for (std::size_t i = 0; i <= job; ++i) required += assignment.workflow.jobs[i].steps.size();
        if (record.steps.empty() || record.steps.size() < required || record.steps.back().exit_code != 0 || record.steps.back().timed_out)
          throw std::invalid_argument("VM artifact before successful job steps");
        if (!note.empty() && size != 0) throw std::invalid_argument("VM artifact note with data");
        if (note.empty()) {
          const auto temp = createTempDirectory(relay_root, "vm-output-");
          const auto file = temp / "bundle.tar";
          try {
            std::ofstream stream(file, std::ios::binary | std::ios::trunc);
            if (!stream) throw std::runtime_error("cannot create VM output file");
            std::uint64_t remaining = size;
            while (remaining) {
              const auto chunk = receive();
              if (chunk.type != CiVmFrameType::Chunk || chunk.payload.empty()) throw std::invalid_argument("invalid VM artifact chunk");
              limits.artifactChunk(chunk.payload.size());
              remaining -= chunk.payload.size();
              stream.write(chunk.payload.data(), static_cast<std::streamsize>(chunk.payload.size()));
              if (!stream) throw std::runtime_error("cannot write VM output file");
            }
            const auto done = receive();
            if (done.type != CiVmFrameType::Done || !done.payload.empty()) throw std::invalid_argument("invalid VM artifact end");
            stream.close();
            if (!stream) throw std::runtime_error("cannot close VM output file");
            limits.endArtifact();
            sink.putArtifact(artifact, file, note, assignment.is_release);
            std::filesystem::remove(file); std::filesystem::remove(temp);
          } catch (...) {
            std::error_code error; std::filesystem::remove(file, error); std::filesystem::remove(temp, error); throw;
          }
        } else {
          limits.endArtifact(); sink.putArtifact(artifact, {}, note, assignment.is_release);
        }
        outputs.push_back({artifact.name, assignment.is_release, artifact.retention_days, note}); ack();
      } else if (part[0] == "pages" && part.size() == 2 && started && !active_step && !pages &&
                 assignment.is_default_branch && !assignment.is_release && assignment.workflow.pages_path &&
                 record.steps.size() == total_steps && total_steps > 0 && record.steps.back().exit_code == 0 &&
                 !record.steps.back().timed_out) {
        const auto size = number<std::uint64_t>(part[1]);
        constexpr std::uint64_t max_archive = static_cast<std::uint64_t>(kMaximumPagesSiteBytes) + (1ull << 30);
        if (size > max_archive) throw std::invalid_argument("VM Pages archive too large");
        const auto temp = createTempDirectory(relay_root, "vm-pages-");
        const auto file = temp / "site.tar";
        const auto site = temp / "site";
        try {
          std::ofstream stream(file, std::ios::binary | std::ios::trunc);
          if (!stream) throw std::runtime_error("cannot create VM Pages archive");
          auto remaining = size;
          while (remaining) {
            const auto chunk = receive();
            if (chunk.type != CiVmFrameType::Chunk || chunk.payload.empty() || chunk.payload.size() > remaining)
              throw std::invalid_argument("invalid VM Pages chunk");
            stream.write(chunk.payload.data(), static_cast<std::streamsize>(chunk.payload.size()));
            if (!stream) throw std::runtime_error("cannot write VM Pages archive");
            remaining -= chunk.payload.size();
          }
          const auto done = receive();
          if (done.type != CiVmFrameType::Done || !done.payload.empty()) throw std::invalid_argument("invalid VM Pages end");
          stream.close();
          if (!stream) throw std::runtime_error("cannot close VM Pages archive");
          TarExtractOptions options;
          options.max_archive_bytes = max_archive;
          options.max_bytes = kMaximumPagesSiteBytes;
          options.max_file_bytes = kMaximumPagesSiteBytes;
          options.max_entries = kMaximumPagesEntries + 1;
          options.skip_non_regular = true;
          options.should_continue = keep;
          extractRegularTreeTar(file, site, options);
          sink.putPagesSite(site);
          std::filesystem::remove_all(temp);
        } catch (...) {
          std::error_code error; std::filesystem::remove_all(temp, error); throw;
        }
        pages = true; ack();
      } else throw std::invalid_argument("unexpected or out-of-order VM event");
    }
  } catch (...) { channel.invalidate(); throw; }
}
}
