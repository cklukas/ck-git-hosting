// cplot — how a scale resolves across the pictures that share it
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
#pragma once

#include <optional>
#include <string>
#include <utility>

namespace cplot {

/// How a scale resolves across the pictures that share it.
///
/// THE ONE ANSWER TO "WHICH PICTURES SHARE A SCALE", and it is one answer
/// because the question is asked in more than one place. A figure's subplot
/// grid asks it of N panels; a cplot::Sequence asks it of N independently
/// produced scenes; a slide's build steps and a sheet of label cards ask it
/// of their own pictures. Every one of those would otherwise answer it
/// separately, and a caller who learned one answer would have to learn the
/// next — which is exactly what a Figure's `share_x`/`share_y` booleans were
/// before they became `Figure::x_extent`: a second vocabulary that could say
/// only union-or-not, beside a policy that named three outcomes.
///
/// WHAT IS SHARED IS THE POLICY, NOT THE RESOLUTION. A subplot grid resolves
/// its shared domain by unioning `Scale`s, so it can merge categories in
/// first-seen order and refuse panels whose scale kinds differ; a sequence
/// resolves by unioning `Extent`s, four numbers a producer can report without
/// building a scene. Both are the right instrument for their input, and
/// forcing one to use the other would cost the grid its categories or the
/// sequence its cheap probe. What must not differ — and no longer does — is
/// what the policies are called, which one is the default, and what happens
/// when a range is stated.
enum class ExtentPolicy {
    /// One scale spanning every picture. The policy under which two pictures
    /// can be compared at all — a bar that grows and an axis that grows look
    /// identical when each picture scales to itself.
    Union,
    /// Each picture scales to its own data. The default, because it is what a
    /// picture does when nothing has been said about the others; legitimate
    /// when they are not meant to be compared, and misleading whenever they
    /// are.
    Independent,
    /// A range the author states. The only policy whose result does not depend
    /// on the data at all, and therefore the only one that is stable when the
    /// data changes underneath it.
    Pinned,
};

/// "union", "independent", "pinned".
const char* extent_policy_name(ExtentPolicy policy) noexcept;

/// Throws cworks::Error naming the accepted spellings.
ExtentPolicy extent_policy_from_name(const std::string& name);

/// One axis's policy, and the range a pinned one pins to.
///
/// The range is present exactly when the policy is `Pinned`, which
/// validate_extent_policy() enforces wherever an author can set one.
struct AxisExtent {
    ExtentPolicy policy = ExtentPolicy::Independent;
    std::optional<std::pair<double, double>> range;

    /// Whether this axis has a domain the pictures hold in common — true for
    /// every policy but `Independent`, and the question a layout asks before
    /// it decides which axis furniture to draw.
    bool shared() const noexcept { return policy != ExtentPolicy::Independent; }
};

/// The rule every surface that accepts a policy keeps: a pinned policy needs
/// the range it pins to, and a range stated with any other policy is refused
/// rather than ignored.
///
/// One function rather than one check per surface, because the two mistakes
/// are made in YAML, in C++ and across the C ABI, and a caller who met the
/// refusal once should meet the same words the next time. `what` names the
/// surface — "figure.x_extent", "sequence" — and leads the message.
///
/// Throws cworks::Error. A range silently ignored is a picture with the wrong
/// axis and nothing in the output to say so.
void validate_extent_policy(ExtentPolicy policy, bool has_range, const std::string& what);

} // namespace cplot
