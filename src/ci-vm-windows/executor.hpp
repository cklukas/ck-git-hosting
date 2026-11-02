// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#pragma once
#include "storage.hpp"
#include "ckgit/ci_runner.hpp"
namespace ckgit {
// Throws on unconfirmed VM shutdown or cleanup; the caller must stop claiming
// jobs. A normal build failure is returned only after confirmed containment.
CiRunRecord executeWindowsHcsJob(const CiPreparedJob&,const CiExecutionOptions&,
 const WindowsVmImages&,CiSourceProvider&,CiResultSink&);
}
