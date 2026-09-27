// libcworks — shared utilities for CK Office
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
//
// The suite's two-tier limits: soft limits produce
// warnings, hard limits produce errors; both configurable from the
// central CK Office configuration; defaults are generous by
// design —
// millions of rows are routine, columns have no artificial cap below
// the hard runaway protection.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "yaml.hpp"

namespace cworks {

/// A diagnostic emitted by an engine: soft-limit warnings, data
/// quality notes, per-step statistics. Errors are exceptions, not
/// diagnostics; Severity::Error appears only in collected reports
/// (e.g. `check` runs that continue after failures).
struct Diagnostic {
    enum class Severity { Note, Warning, Error };
    Severity severity = Severity::Warning;
    std::string message;
};

using Diagnostics = std::vector<Diagnostic>;

struct Limits {
    struct Bound {
        std::uint64_t soft = 0; ///< exceeding → warning diagnostic
        std::uint64_t hard = 0; ///< exceeding → cworks::Error

        friend bool operator==(const Bound&, const Bound&) = default;
    };

    Bound rows{10'000'000ULL, 100'000'000ULL};
    Bound columns{100'000ULL, 1'000'000ULL};
    Bound cell_text_bytes{1ULL << 20, 16ULL << 20};
    Bound query_steps{200ULL, 10'000ULL};

    friend bool operator==(const Limits&, const Limits&) = default;

    static Limits defaults() { return {}; }

    /// One limit of the central `limits:` section, described well enough
    /// to drive a reader, a writer, a validator and a configuration
    /// editor without any of them keeping a list of its own.
    struct Field {
        const char* name;        ///< the YAML key, e.g. "cell_text_bytes"
        const char* title;       ///< human label, e.g. "Cell text"
        const char* unit;        ///< what the numbers count, e.g. "bytes"
        const char* description; ///< one line: what the limit governs
        /// Where this limit's two tiers live inside a Limits value.
        Bound Limits::*member;
    };

    /// Every limit the suite declares, in the order a config file writes
    /// them — the single source of truth. from_yaml, to_yaml, validate
    /// and any configuration editor iterate this table, so declaring a
    /// new limit is one member above plus one row here, and there is no
    /// second list anywhere that could drift out of step with it.
    static const std::vector<Field>& fields();

    /// The field called `name`, or nullptr — the reverse of Field::name.
    static const Field* find_field(const std::string& name);

    /// The two tiers `field` names in this value.
    Bound& bound(const Field& field) { return this->*field.member; }
    const Bound& bound(const Field& field) const { return this->*field.member; }

    /// Read overrides from a `limits:` mapping:
    ///   limits:
    ///     rows: {soft: 1000000, hard: 10000000}
    ///     columns: {hard: 500000}          # partial override is fine
    /// Unknown keys are errors (suite convention). A Null node returns
    /// the defaults unchanged.
    static Limits from_yaml(const YamlNode& node);

    /// The canonical `limits:` mapping — every declared limit written
    /// out with both tiers, in schema order. The inverse of from_yaml,
    /// so a saved configuration is complete and self-documenting.
    YamlNode to_yaml() const;

    /// Refuses one bound whose soft tier exceeds its hard tier, naming it
    /// as `name`. This is the *same* rule, with the same wording, that
    /// bound_from_yaml applies while reading a file, so a limit typed
    /// into a configuration editor and a hand-edited file can never
    /// disagree about what is legal.
    static void validate_bound(const Bound& bound, const std::string& name);

    /// validate_bound over every limit the schema declares.
    void validate() const;

    /// Parses one tier the way a human types it, with the grammar and
    /// the refusal the YAML reader uses (non-negative integers only).
    /// `what` names the limit and tier in the message, e.g. "rows.soft".
    static std::uint64_t parse_value(const std::string& text,
                                     const std::string& what);

    /// Apply one `{soft: N, hard: N}` mapping onto `bound`. Shared by
    /// Limits::from_yaml and component-specific limit structs (e.g.
    /// cplot::RenderLimits) so every limits block in the suite parses
    /// and fails identically: unknown keys are errors, values must be
    /// non-negative integers, soft must not exceed hard. `name` names
    /// the bound in error messages ("rows", "series_points").
    static void bound_from_yaml(Bound& bound, const YamlNode& node, const std::string& name);

    /// Enforce a bound: appends a Warning to `out` when `value` exceeds
    /// the soft bound, throws cworks::Error when it exceeds the hard
    /// bound. `subject` names what is being counted ("rows", "columns",
    /// "cell text bytes") and `context` names where ("CSV source
    /// 'data.csv'").
    static void check(std::uint64_t value, const Bound& bound, const std::string& subject,
                      const std::string& context, Diagnostics& out);
};

/// Load the central CK Office configuration. Search order:
///   1. $CKOFFICE_CONFIG (explicit file path)
///   2. $XDG_CONFIG_HOME/ckoffice/config.yaml
///   3. ~/.config/ckoffice/config.yaml
/// Returns a Null node when no file exists; throws cworks::Error when a
/// file exists but does not parse.
YamlNode load_central_config();

} // namespace cworks
