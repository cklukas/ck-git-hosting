// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "../../src/ci-vm-guest-macos/staged_source.hpp"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>
#include <unistd.h>

namespace fs = std::filesystem;
namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class F> bool rejects(F f) { try { f(); return false; } catch (const std::exception&) { return true; } }
}
int main() {
  const char* tmp = std::getenv("TMPDIR");
  if (!tmp) return 1;
  std::string pattern = (fs::path(tmp) / "vm-guest-source-XXXXXX").string();
  std::vector<char> name(pattern.begin(), pattern.end());
  name.push_back('\0');
  if (!mkdtemp(name.data())) return 1;
  const fs::path root(name.data());
  struct Cleanup { fs::path path; ~Cleanup() { std::error_code e; fs::remove_all(path, e); } } cleanup{root};
  try {
    fs::create_directories(root / "share/source/0/dir");
    fs::create_directories(root / "share/source/1");
    fs::create_directory(root / "out");
    fs::create_directory(root / "sister");
    fs::create_directory(root / "cancelled");
    std::ofstream(root / "share/source/0/dir/file") << "contents";
    std::ofstream(root / "share/source/0/large", std::ios::binary) << std::string(700000, 'a');
    fs::create_symlink("../../outside", root / "share/source/0/link");
    std::ofstream(root / "share/source/1/file") << "sister";
    ckgit::CiPreparedJob job;
    job.job.project_name = "project"; job.job.commit_id = std::string(40, 'a'); job.job.ref = "refs/heads/main";
    job.sisters.push_back({"sister", std::string(40, 'b'), {}});
    unsigned checks = 0;
    ckgit::CiVmStagedSource source(job, root / "share", [&] { ++checks; return true; });
    ckgit::CiCheckoutRequest primary{job.job.project_name, job.job.commit_id, job.workflow.checkout, job.job.ref};
    source.checkout(primary, root / "out");
    require(fs::exists(root / "out/dir/file") && fs::is_symlink(root / "out/link") &&
                fs::read_symlink(root / "out/link") == "../../outside" &&
                fs::file_size(root / "out/large") == 700000 && checks > 5, "VM source copy failed");
    ckgit::CiCheckoutRequest sister{"sister", std::string(40, 'b'), {}, {}};
    source.checkout(sister, root / "sister");
    require(fs::exists(root / "sister/file"), "VM sister source copy failed");
    sister.commit = std::string(40, 'c');
    require(rejects([&] { source.checkout(sister, root / "sister"); }), "VM accepted another commit");
    require(rejects([&] { source.checkout(primary, root / "out"); }), "VM accepted a nonempty destination");
    unsigned attempts = 0;
    ckgit::CiVmStagedSource cancelling(job, root / "share", [&] { return ++attempts < 3; });
    require(rejects([&] { cancelling.checkout(primary, root / "cancelled"); }), "VM checkout ignored cancellation");
    std::cout << "VM staged source passed\n";
    return 0;
  } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
