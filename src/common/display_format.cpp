// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "ckgit/web_renderer.hpp"
#include "ckgit/text.hpp"
#include <algorithm>
#include <ctime>
#include <iomanip>
#include <sstream>
namespace ckgit {
std::string formatUtcTimestamp(std::uint64_t epoch) {
  const auto timestamp = static_cast<std::time_t>(epoch); std::tm utc{}; char buffer[32]{};
#ifdef _WIN32
  if (gmtime_s(&utc, &timestamp) != 0) return {};
#else
  if (!gmtime_r(&timestamp, &utc)) return {};
#endif
  if (!std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M UTC", &utc)) return {};
  return buffer;
}
std::string relativeTime(std::uint64_t epoch) {
  return relativeTimeAt(epoch, static_cast<std::uint64_t>(std::time(nullptr)));
}
std::string relativeTimeAt(std::uint64_t epoch, std::uint64_t now) {
  if (epoch > now) return "in the future";
  auto delta = now - epoch;
  if (delta < 60) return "just now";
  for (const auto& unit : {std::pair<std::uint64_t, const char*>{31536000, "year"}, {2592000, "month"},
                          {86400, "day"}, {3600, "hour"}, {60, "minute"}})
    if (delta >= unit.first) { auto n = delta / unit.first; return std::to_string(n) + " " + unit.second + (n == 1 ? " ago" : "s ago"); }
  return "just now";
}
std::string formatBytes(std::uint64_t bytes) {
  if (bytes < 1024) return std::to_string(bytes) + " B";
  double n = static_cast<double>(bytes); std::size_t unit = 0;
  while (n >= 1024 && unit < 3) { n /= 1024; ++unit; }
  const char* units[]{"B", "KiB", "MiB", "GiB"};
  std::ostringstream text; text << std::fixed << std::setprecision(1) << n << " " << units[unit]; return text.str();
}
CiRunDisplay ciRunDisplay(const CiRunRecord& run) {
  return ciRunDisplayAt(run, static_cast<std::uint64_t>(std::time(nullptr)));
}

CiRunDisplay ciRunDisplayAt(const CiRunRecord& run, std::uint64_t now) {
  CiRunDisplay display;
  display.icon = ciRunStatusIcon(run.status);
  display.name = ciRunStatusName(run.status);
  display.seconds = 0;
  display.active = false;
  if (run.status == CiRunStatus::Running) {
    const std::uint64_t beat =
        run.heartbeat_epoch_seconds ? run.heartbeat_epoch_seconds : run.started_epoch_seconds;
    if (beat != 0 && now > beat + kCiRunStaleSeconds) {
      // The runner stopped reporting: show it as interrupted, not a live clock.
      display.icon = "\xf0\x9f\x92\xa4";  // 💤
      display.name = "interrupted";
      display.seconds = beat > run.started_epoch_seconds ? beat - run.started_epoch_seconds : 0;
    } else {
      display.name = "running";
      display.seconds =
          run.started_epoch_seconds && now >= run.started_epoch_seconds ? now - run.started_epoch_seconds : 0;
      display.active = true;
    }
    return display;
  }
  if (run.status == CiRunStatus::Pending) {
    display.active = true;  // queued: keep the view live until it starts
    return display;
  }
  if (run.finished_epoch_seconds >= run.started_epoch_seconds && run.started_epoch_seconds != 0) {
    display.seconds = run.finished_epoch_seconds - run.started_epoch_seconds;
  }
  return display;
}

bool ciAnyActiveRun(const std::vector<CiRunRecord>& runs) {
  return std::any_of(runs.begin(), runs.end(), [](const CiRunRecord& run) { return ciRunDisplay(run).active; });
}

std::string ciRunTiming(const CiRunDisplay& display) {
  if (display.name == "pending") return {};
  if (display.seconds == 0 && !display.active) return {};
  return formatDuration(display.seconds) + (display.seconds < 3600 ? " min" : "");
}

}
