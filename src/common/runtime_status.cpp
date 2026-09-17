// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#include "ckgit/runtime_status.hpp"

#include <algorithm>
#include <array>
#include <csignal>
#include <ctime>
#include <fstream>
#include <sstream>
#include <system_error>
#include <unistd.h>

namespace ckgit {
namespace {

constexpr std::size_t kMaxVersionLength = 128;

// The runtime store holds only our own daemons' version/pid/start lines, but
// one of the writers (ck-pagesd) also serves untrusted project content, so a
// value read back is sanitised before it reaches the control response or the
// dashboard: a single compact token cannot smuggle a space or newline into the
// line-based control protocol, or markup into the page (which still escapes it
// too).
std::string sanitizeVersion(const std::string& raw) {
  std::string out;
  for (const char c : raw) {
    if (out.size() >= kMaxVersionLength) break;
    const bool ok = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                    c == '.' || c == '+' || c == '~' || c == '-' || c == '_' || c == ':';
    if (ok) out.push_back(c);
  }
  return out;
}

bool validComponentName(const std::string& name) {
  if (name.empty() || name.size() > 64) return false;
  for (const char c : name) {
    const bool ok = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                    c == '-' || c == '_' || c == '.';
    if (!ok || c == '/') return false;
  }
  return name != "." && name != "..";
}

std::filesystem::path runtimeDir(const std::filesystem::path& state_root) {
  return state_root / "runtime";
}

// A fixed order so the operator always reads the suite top-down; anything not
// named here (a future daemon) follows, alphabetically.
int suiteRank(const std::string& name) {
  static const std::array<const char*, 3> kOrder = {"ck-git-hostingd", "ck-ci-runnerd", "ck-pagesd"};
  for (std::size_t i = 0; i < kOrder.size(); ++i) {
    if (name == kOrder[i]) return static_cast<int>(i);
  }
  return static_cast<int>(kOrder.size());
}

}  // namespace

void recordRuntimeComponent(const std::filesystem::path& state_root, const std::string& name,
                            const std::string& version) {
  if (state_root.empty() || !validComponentName(name)) return;
  try {
    const std::filesystem::path dir = runtimeDir(state_root);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) return;
    std::ostringstream body;
    body << "version=" << sanitizeVersion(version) << "\n"
         << "pid=" << static_cast<long>(::getpid()) << "\n"
         << "started=" << static_cast<std::uint64_t>(std::time(nullptr)) << "\n";
    const std::string text = body.str();
    // Write to a same-directory temp and rename, so a reader never sees a
    // half-written file. Best-effort: a torn write self-heals on next startup.
    const std::filesystem::path staging = dir / (name + ".tmp");
    {
      std::ofstream out(staging, std::ios::binary | std::ios::trunc);
      if (!out) return;
      out.write(text.data(), static_cast<std::streamsize>(text.size()));
      if (!out) return;
    }
    std::filesystem::rename(staging, dir / (name + ".txt"), ec);
    if (ec) std::filesystem::remove(staging, ec);
  } catch (const std::exception&) {
    // Recording a version is never worth failing to serve.
  }
}

std::vector<RuntimeComponent> readRuntimeComponents(const std::filesystem::path& state_root) {
  std::vector<RuntimeComponent> components;
  if (state_root.empty()) return components;
  try {
    const std::filesystem::path dir = runtimeDir(state_root);
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
      if (ec) break;
      if (!entry.is_regular_file()) continue;
      const std::filesystem::path& path = entry.path();
      if (path.extension() != ".txt") continue;
      const std::string name = path.stem().string();
      if (!validComponentName(name)) continue;
      std::ifstream in(path, std::ios::binary);
      if (!in) continue;
      RuntimeComponent component;
      component.name = name;
      std::string line;
      while (std::getline(in, line)) {
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq);
        const std::string value = line.substr(eq + 1);
        if (key == "version") {
          component.version = sanitizeVersion(value);
        } else if (key == "pid") {
          try { component.pid = std::stol(value); } catch (const std::exception&) { component.pid = 0; }
        } else if (key == "started") {
          try { component.started_epoch_seconds = std::stoull(value); } catch (const std::exception&) {}
        }
      }
      // A live pid tells the reader the recorded version is what is running now,
      // not a stale file from a crashed or replaced-but-not-restarted daemon.
      component.running = component.pid > 0 && ::kill(static_cast<pid_t>(component.pid), 0) == 0;
      components.push_back(std::move(component));
    }
  } catch (const std::exception&) {
    return components;
  }
  std::sort(components.begin(), components.end(), [](const RuntimeComponent& a, const RuntimeComponent& b) {
    const int ra = suiteRank(a.name);
    const int rb = suiteRank(b.name);
    return ra != rb ? ra < rb : a.name < b.name;
  });
  return components;
}

}  // namespace ckgit
