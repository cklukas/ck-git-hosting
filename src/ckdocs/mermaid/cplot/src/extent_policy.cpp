// cplot — how a scale resolves across the pictures that share it
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
#include <cplot/extent_policy.hpp>

#include <cplot/error.hpp>

#include <cworks/app_error.hpp>
#include <cworks/error.hpp>

namespace cplot {

const char* extent_policy_name(ExtentPolicy policy) noexcept {
    switch (policy) {
    case ExtentPolicy::Union: return "union";
    case ExtentPolicy::Independent: return "independent";
    case ExtentPolicy::Pinned: return "pinned";
    }
    return "independent";
}

ExtentPolicy extent_policy_from_name(const std::string& name) {
    if (name == "union") return ExtentPolicy::Union;
    if (name == "independent") return ExtentPolicy::Independent;
    if (name == "pinned") return ExtentPolicy::Pinned;
    throw Error(cworks::validation_failed("unknown extent policy '" + name +
                                          "'; write union, independent or pinned"));
}

void validate_extent_policy(ExtentPolicy policy, bool has_range, const std::string& what) {
    // cplot::Error rather than cworks::Error, and structured rather than
    // message-only: this is a refusal the library's own callers catch, and one
    // reached from a YAML key, a C++ setter, the C ABI and a Sequence — four
    // surfaces whose frontends all read the error code.
    if (policy == ExtentPolicy::Pinned && !has_range)
        throw Error(cworks::validation_failed(
            what + ": the pinned extent policy needs the range to pin to"));
    if (policy != ExtentPolicy::Pinned && has_range)
        throw Error(cworks::validation_failed(
            what + ": a range was given with the " + std::string(extent_policy_name(policy)) +
            " extent policy, which does not use one; a range that is silently ignored is a "
            "chart with the wrong axis and no signal"));
}

} // namespace cplot
