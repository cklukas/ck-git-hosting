// ckdiagram — Diagram Renderer
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
//
// The `placement` front-matter extension: authored positions that survive a
// save and a reopen.
//
// ## Why this exists
//
// Mermaid's syntax has no coordinates. Layout is computed, which is a feature —
// it is why a diagram stays readable as it grows and why two people editing the
// same file do not fight over pixels. But there is a real class of diagrams
// where the author knows better than the algorithm: a node that belongs on the
// left because that is where the reader's eye starts, two boxes that should
// share a column because they are the same kind of thing. Without somewhere to
// record that, an editor either refuses the request or — worse — accepts it and
// loses it on reload.
//
// This extension is that somewhere. It is deliberately *narrow*:
//
//   * it lives in YAML front matter, which Mermaid already parses and whose
//     unknown keys every other renderer already ignores — so a diagram carrying
//     placement still renders correctly, just with computed layout, in any tool
//     that does not know this key;
//   * it is applied **after** the deterministic layout, never instead of it, so
//     the layout engine remains the single owner of everything not explicitly
//     placed, and removing the block restores exactly the original drawing;
//   * it is versioned, so the vocabulary can grow without breaking files.
//
// ## The format
//
// ```yaml
// ---
// placement:
//   version: 1
//   objects:
//     A: {pin: {x: 120, y: 40}}
//     B: {align: {to: A, axis: x}}
// ---
// ```
//
// `pin` puts an object's centre exactly there, in the diagram's own units
// (the same units the SVG viewBox uses, origin top-left). `align` makes an
// object share one coordinate with another, which is the constraint people
// actually mean when they drag something into a column — it keeps holding as
// the diagram grows, where a pin would not.
//
// Anything else — ranks, lanes, explicit ordering — is deliberately *not* in
// version 1. Those are constraints on the layout's input rather than
// corrections to its output, so they belong inside the layout engine, and
// shipping them as post-layout approximations would produce drawings that
// disagree with the layout that claims to own them.
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <cworks/limits.hpp>

namespace cdiagram {

/// The version of the vocabulary this build writes and understands.
inline constexpr int kPlacementVersion = 1;

/// An exact position for an object's centre, in diagram units.
struct PlacementPin {
    double x = 0.0;
    double y = 0.0;

    friend bool operator==(const PlacementPin&, const PlacementPin&) = default;
};

/// Which coordinate an alignment shares.
enum class PlacementAxis {
    X, ///< same horizontal position — a column
    Y, ///< same vertical position — a row
};

/// Share one coordinate with another object.
struct PlacementAlign {
    std::string to;
    PlacementAxis axis = PlacementAxis::X;

    friend bool operator==(const PlacementAlign&, const PlacementAlign&) = default;
};

/// What an author declared about one object. Both parts are optional and may
/// be combined: a pin fixes both coordinates, and an alignment then overrides
/// one of them.
struct ObjectPlacement {
    std::optional<PlacementPin> pin;
    std::optional<PlacementAlign> align;

    bool empty() const { return !pin.has_value() && !align.has_value(); }
    friend bool operator==(const ObjectPlacement&, const ObjectPlacement&) = default;
};

/// The whole `placement` block, keyed by object key (a flowchart node id).
///
/// Entries are kept in insertion order and written back in sorted key order, so
/// the serialised block is deterministic regardless of how it was built.
class Placement {
public:
    /// Read the block out of a diagram source's front matter. Malformed
    /// entries are reported and skipped — placement never breaks a diagram,
    /// exactly as front matter never does. A source with no block yields an
    /// empty Placement, which is not an error.
    static Placement parse(std::string_view source, cworks::Diagnostics* diagnostics = nullptr);

    bool empty() const { return entries_.empty(); }
    std::size_t size() const { return entries_.size(); }
    int version() const { return version_; }

    const ObjectPlacement* find(std::string_view key) const;

    /// Set, replace, or (with an empty value) drop one object's placement.
    void set(std::string_view key, const ObjectPlacement& value);
    void erase(std::string_view key);

    /// Rename a key, keeping its placement — the placement half of a rename.
    void rename(std::string_view from, std::string_view to);

    using Entry = std::pair<std::string, ObjectPlacement>;
    const std::vector<Entry>& entries() const { return entries_; }

    /// The block as YAML, starting at `placement:` and ending with a newline.
    /// Empty when there is nothing to write.
    std::string to_yaml() const;

    /// Write this placement into `source`'s front matter, adding, replacing or
    /// removing the `placement:` block as needed and leaving every other byte
    /// of the source — including the rest of the front matter — untouched.
    std::string write_into(std::string_view source) const;

private:
    std::vector<Entry> entries_;
    int version_ = kPlacementVersion;
};

} // namespace cdiagram
