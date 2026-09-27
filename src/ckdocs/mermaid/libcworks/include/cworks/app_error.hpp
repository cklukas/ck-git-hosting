// libcworks — structured application-boundary error model
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
//
// C++ exceptions stay useful inside the engines, but an application
// boundary must expose more than an English sentence: a native frontend
// has to decide "prompt for a password" versus "offer to overwrite"
// without ever running `message.find("password")`. This header is that
// vocabulary — a stable category/code, a user-facing summary, technical
// detail, the affected subject, suggested recovery actions, and the raw
// underlying text kept only for logs. It is toolkit-free Layer-B code.
#pragma once

#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "cworks/source_span.hpp"

namespace cworks {

/// Coarse, stable classification. A frontend can branch on this alone for
/// generic handling (retry vs. give up); the code below is the precise
/// condition. Never localized — these names are machine identifiers.
enum class ErrorCategory {
    InvalidInput, // the request was malformed or failed validation
    NotFound,     // a named file or object does not exist
    Conflict,     // the target already exists / a constraint was violated
    Permission,   // read-only or access denied
    AuthRequired, // a password or credential is needed
    Stale,        // the operation was superseded by newer state
    Io,           // filesystem or network failure
    Unsupported,  // not available in this build or context
    Cancelled,    // the user or system cancelled the work
    Internal,     // an unexpected engine failure (a bug)
};

/// The precise condition a frontend switches on. This is the contract that
/// replaces string-matching: `err.is(ErrorCode::PasswordRequired)` instead
/// of `what().find("password")`. Extend, never renumber.
enum class ErrorCode {
    Unknown,
    ValidationFailed,   // InvalidInput
    FileNotFound,       // NotFound
    ObjectNotFound,     // NotFound
    FileExists,         // Conflict
    ConstraintViolated, // Conflict
    ReadOnly,           // Permission
    AccessDenied,       // Permission
    PasswordRequired,   // AuthRequired — encrypted, no password given
    WrongPassword,      // AuthRequired — password given but rejected
    StaleOperation,     // Stale
    IoFailed,           // Io
    NetworkFailed,      // Io
    InvalidFormat,      // InvalidInput — not the file type we expected
    Unsupported,        // Unsupported
    Cancelled,          // Cancelled
    InternalError,      // Internal
};

/// A next step the frontend may surface as a button or command. The core
/// suggests; the frontend renders it however it likes (a dialog button, a
/// CLI hint, a toolbar action).
enum class RecoveryAction {
    Retry,
    Overwrite,
    ChooseDifferentName,
    EnterPassword,
    SaveACopy,
    Reload, // re-read the current state, then retry (for stale operations)
    OpenReadOnly,
};

/// The structured error crossing an application boundary. Plain data:
/// copyable, serializable, and free of any exception machinery so it can
/// be returned as well as thrown.
struct AppError {
    ErrorCategory category = ErrorCategory::Internal;
    ErrorCode code = ErrorCode::Unknown;
    std::string summary;        // one user-facing line
    std::string detail;         // technical, shown on request
    std::string subject;        // affected object: a path or object id
    std::string exception_text; // raw underlying what(), for logs only
    bool recoverable = false;
    std::vector<RecoveryAction> actions;
    /// Where in the request's own input text the failure lies, when it is
    /// about text the caller wrote — a formula, an expression. Offsets are
    /// into that text exactly as the caller supplied it, so a frontend puts
    /// its caret on `offset` without knowing how the engine read the text.
    /// Empty when the failure is not about a place in a text.
    std::optional<SourceSpan> source_span;

    bool is(ErrorCode c) const noexcept { return code == c; }
    bool in(ErrorCategory c) const noexcept { return category == c; }
    bool suggests(RecoveryAction a) const noexcept;
};

/// Stable machine strings, for structured logs and serialization. Never
/// shown to users and never localized.
std::string_view category_name(ErrorCategory) noexcept;
std::string_view code_name(ErrorCode) noexcept;
std::string_view action_name(RecoveryAction) noexcept;

// Factories for the conditions a frontend has to recognize by name, plus
// the common neighbours. Each sets the category, recoverability, and
// suggested actions consistently so call sites cannot forget them; callers
// may still refine `summary`/`detail`/`exception_text` afterwards.
AppError password_required(std::string subject);
AppError wrong_password(std::string subject);
AppError file_exists(std::string path);
AppError read_only(std::string subject);
/// The system refused the caller outright — a path it may not enter, a
/// statement the database's authorizer rejected. Distinct from read_only:
/// that one opened the subject and only refuses writes, so saving elsewhere
/// still works; here the door never opened and nothing the frontend can offer
/// opens it. `subject` names what was refused; `detail` carries the system's
/// own reason.
AppError access_denied(std::string subject, std::string detail = {});
AppError stale_operation(std::string detail = {});
AppError file_not_found(std::string path);
/// A named object the caller asked for does not exist — a sheet, a chart, a
/// table, a saved query. Distinct from file_not_found: nothing was on disk to
/// look for and there is nothing to retry, so a frontend offers a chooser
/// rather than a file dialog. `kind` names what was looked for ("sheet",
/// "chart"), because "no such object" helps nobody; `subject` is the name that
/// did not resolve.
AppError object_not_found(std::string kind, std::string subject);
AppError invalid_format(std::string subject, std::string detail = {});
AppError validation_failed(std::string summary);
/// Text the caller wrote does not read, and `at` is where: the syntax errors of
/// a formula or an expression. The summary still names the problem in words;
/// the span is what a frontend acts on, so it never has to find the place by
/// reading the summary.
AppError validation_failed(std::string summary, SourceSpan at);
/// The store refused the write because it would break a rule the data must
/// keep — a duplicate key, a NOT NULL column left empty, a foreign key with no
/// parent. Distinct from validation_failed: the request was well-formed and the
/// engine got as far as trying it, so the fix is the value, not the syntax.
/// `subject` names what was refused (a table, a column); `detail` carries the
/// store's own reason.
AppError constraint_violated(std::string subject, std::string detail = {});
AppError io_failed(std::string subject, std::string detail);
/// The request never crossed the network — no route, a refused connection, a
/// server that answered with a failure. Distinct from io_failed: both are Io
/// and both retry, but only this one lets a frontend say "check your
/// connection" instead of "check the disk". `subject` names what could not be
/// reached (a URL); `detail` carries the transport's own reason.
AppError network_failed(std::string subject, std::string detail = {});
AppError unsupported(std::string summary);
AppError cancelled();
AppError internal_error(std::string detail);

/// Wrap an arbitrary exception as an Internal error. The original text is
/// preserved only in `exception_text` (for logs) — it is never the thing a
/// frontend branches on.
AppError from_exception(const std::exception& e);

} // namespace cworks
