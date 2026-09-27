// libcworks — shared utilities for CK Office
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
#include "cworks/limits.hpp"

#include <cstdlib>
#include <fstream>
#include <sstream>

#include "cworks/app_error.hpp"
#include "cworks/error.hpp"
#include "cworks/text.hpp"

namespace cworks {

namespace {

std::uint64_t need_uint(const YamlNode& node, const std::string& what) {
    bool ok = false;
    const double v = node.as_double(&ok);
    if (!ok || v < 0 || v != static_cast<double>(static_cast<std::uint64_t>(v))) {
        throw Error(validation_failed("limits: " + what +
                                      " must be a non-negative integer, got '" +
                                      node.scalar + "'"));
    }
    return static_cast<std::uint64_t>(v);
}

/// "rows, columns, cell_text_bytes, query_steps" — the known limits,
/// listed from the schema so the message cannot go stale.
std::string known_limits() {
    std::string list;
    for (const Limits::Field& field : Limits::fields()) {
        if (!list.empty()) list += ", ";
        list += field.name;
    }
    return list;
}

} // namespace

const std::vector<Limits::Field>& Limits::fields() {
    // Declaring a limit means adding a member to Limits and a row here.
    // Everything else — parsing, writing, validation, the configuration
    // editor's dialog — is generated from this table.
    static const std::vector<Field> table = {
        {"rows", "Rows", "rows",
         "Rows in one table, sheet, or query result.", &Limits::rows},
        {"columns", "Columns", "columns",
         "Columns in one table, sheet, or query result.", &Limits::columns},
        {"cell_text_bytes", "Cell text", "bytes",
         "UTF-8 bytes of text in a single cell.", &Limits::cell_text_bytes},
        {"query_steps", "Query steps", "steps",
         "Steps in one query or transformation pipeline.",
         &Limits::query_steps},
    };
    return table;
}

const Limits::Field* Limits::find_field(const std::string& name) {
    for (const Field& field : fields())
        if (name == field.name) return &field;
    return nullptr;
}

void Limits::validate_bound(const Bound& bound, const std::string& name) {
    if (bound.soft > bound.hard)
        throw Error(validation_failed(
            "limits: " + name + ".soft (" + std::to_string(bound.soft) +
            ") must not exceed " + name + ".hard (" + std::to_string(bound.hard) +
            ")"));
}

void Limits::validate() const {
    for (const Field& field : fields()) validate_bound(this->bound(field), field.name);
}

std::uint64_t Limits::parse_value(const std::string& text, const std::string& what) {
    // Literally the reader's own parser, over a scalar node built from the
    // typed text: a value a dialog accepts is a value the file accepts.
    return need_uint(YamlNode::make_scalar(text), what);
}

void Limits::bound_from_yaml(Bound& bound, const YamlNode& node, const std::string& name) {
    if (!node.is_map()) {
        throw Error(validation_failed(
            "limits: '" + name + "' must be a mapping with soft/hard, e.g. " +
            name + ": {soft: 1000000, hard: 10000000}"));
    }
    for (const auto& [key, value] : node.map) {
        if (key == "soft") bound.soft = need_uint(value, name + ".soft");
        else if (key == "hard") bound.hard = need_uint(value, name + ".hard");
        else
            throw Error(validation_failed("limits: unknown key '" + key + "' in '" +
                                          name + "' (soft, hard)"));
    }
    validate_bound(bound, name);
}

Limits Limits::from_yaml(const YamlNode& node) {
    Limits limits;
    if (node.is_null()) return limits;
    if (!node.is_map())
        throw Error(validation_failed("limits: expected a mapping of bounds"));
    for (const auto& [key, value] : node.map) {
        const Field* field = find_field(key);
        if (field == nullptr) {
            std::string message =
                "limits: unknown limit '" + key + "' (" + known_limits() + ")";
            std::vector<std::string> names;
            for (const Field& known : fields()) names.emplace_back(known.name);
            const std::string suggestion = closest_match(key, names);
            if (!suggestion.empty())
                message += " — did you mean '" + suggestion + "'?";
            throw Error(validation_failed(message));
        }
        bound_from_yaml(limits.bound(*field), value, field->name);
    }
    return limits;
}

YamlNode Limits::to_yaml() const {
    YamlNode node;
    for (const Field& field : fields()) {
        YamlNode& entry = node.insert(field.name);
        entry.insert("soft") =
            YamlNode::make_scalar(std::to_string(bound(field).soft));
        entry.insert("hard") =
            YamlNode::make_scalar(std::to_string(bound(field).hard));
    }
    return node;
}

void Limits::check(std::uint64_t value, const Bound& bound, const std::string& subject,
                   const std::string& context, Diagnostics& out) {
    if (value > bound.hard) {
        // The hard tier of the two-tier limits IS validation by design: the
        // input is refused against a configured bound, and the fix is the
        // input or the config — not a retry, not a bug report. cplot's limit
        // passthroughs (figure/raster) carry this code out unchanged.
        throw Error(validation_failed(
            "limit exceeded: " + std::to_string(value) + " " + subject + " in " +
            context + " (hard limit " + std::to_string(bound.hard) +
            "; raise it in the central config under limits: if this is intended)"));
    }
    if (value > bound.soft) {
        out.push_back({Diagnostic::Severity::Warning,
                       std::to_string(value) + " " + subject + " in " + context +
                           " exceed the soft limit of " + std::to_string(bound.soft)});
    }
}

} // namespace cworks
