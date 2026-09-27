// libcworks — structured application-boundary error model
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
#include "cworks/app_error.hpp"

#include <algorithm>
#include <utility>

#include "cworks/error.hpp"

namespace cworks {

bool AppError::suggests(RecoveryAction a) const noexcept {
    return std::find(actions.begin(), actions.end(), a) != actions.end();
}

std::string_view category_name(ErrorCategory c) noexcept {
    switch (c) {
    case ErrorCategory::InvalidInput: return "invalid-input";
    case ErrorCategory::NotFound: return "not-found";
    case ErrorCategory::Conflict: return "conflict";
    case ErrorCategory::Permission: return "permission";
    case ErrorCategory::AuthRequired: return "auth-required";
    case ErrorCategory::Stale: return "stale";
    case ErrorCategory::Io: return "io";
    case ErrorCategory::Unsupported: return "unsupported";
    case ErrorCategory::Cancelled: return "cancelled";
    case ErrorCategory::Internal: return "internal";
    }
    return "internal";
}

std::string_view code_name(ErrorCode c) noexcept {
    switch (c) {
    case ErrorCode::Unknown: return "unknown";
    case ErrorCode::ValidationFailed: return "validation-failed";
    case ErrorCode::FileNotFound: return "file-not-found";
    case ErrorCode::ObjectNotFound: return "object-not-found";
    case ErrorCode::FileExists: return "file-exists";
    case ErrorCode::ConstraintViolated: return "constraint-violated";
    case ErrorCode::ReadOnly: return "read-only";
    case ErrorCode::AccessDenied: return "access-denied";
    case ErrorCode::PasswordRequired: return "password-required";
    case ErrorCode::WrongPassword: return "wrong-password";
    case ErrorCode::StaleOperation: return "stale-operation";
    case ErrorCode::IoFailed: return "io-failed";
    case ErrorCode::NetworkFailed: return "network-failed";
    case ErrorCode::InvalidFormat: return "invalid-format";
    case ErrorCode::Unsupported: return "unsupported";
    case ErrorCode::Cancelled: return "cancelled";
    case ErrorCode::InternalError: return "internal-error";
    }
    return "unknown";
}

std::string_view action_name(RecoveryAction a) noexcept {
    switch (a) {
    case RecoveryAction::Retry: return "retry";
    case RecoveryAction::Overwrite: return "overwrite";
    case RecoveryAction::ChooseDifferentName: return "choose-different-name";
    case RecoveryAction::EnterPassword: return "enter-password";
    case RecoveryAction::SaveACopy: return "save-a-copy";
    case RecoveryAction::Reload: return "reload";
    case RecoveryAction::OpenReadOnly: return "open-read-only";
    }
    return "retry";
}

namespace {

// A quoted subject, or empty if there is none — so summaries read well
// whether or not a path/id was supplied.
std::string quoted(const std::string& subject) {
    return subject.empty() ? std::string() : "“" + subject + "”";
}

} // namespace

AppError password_required(std::string subject) {
    AppError e;
    e.category = ErrorCategory::AuthRequired;
    e.code = ErrorCode::PasswordRequired;
    const std::string who = quoted(subject);
    e.summary = (who.empty() ? "the file" : who) + " is encrypted — a password is required";
    e.subject = std::move(subject);
    e.recoverable = true;
    e.actions = {RecoveryAction::EnterPassword};
    return e;
}

AppError wrong_password(std::string subject) {
    AppError e;
    e.category = ErrorCategory::AuthRequired;
    e.code = ErrorCode::WrongPassword;
    const std::string who = quoted(subject);
    e.summary = "the password for " + (who.empty() ? "the file" : who) + " is not correct";
    e.subject = std::move(subject);
    e.recoverable = true;
    e.actions = {RecoveryAction::EnterPassword};
    return e;
}

AppError file_exists(std::string path) {
    AppError e;
    e.category = ErrorCategory::Conflict;
    e.code = ErrorCode::FileExists;
    const std::string who = quoted(path);
    e.summary = (who.empty() ? "that file" : who) + " already exists";
    e.subject = std::move(path);
    e.recoverable = true;
    e.actions = {RecoveryAction::Overwrite, RecoveryAction::ChooseDifferentName};
    return e;
}

AppError read_only(std::string subject) {
    AppError e;
    e.category = ErrorCategory::Permission;
    e.code = ErrorCode::ReadOnly;
    const std::string who = quoted(subject);
    e.summary = (who.empty() ? "this document" : who) + " is read-only";
    e.subject = std::move(subject);
    e.recoverable = true;
    e.actions = {RecoveryAction::SaveACopy};
    return e;
}

AppError access_denied(std::string subject, std::string detail) {
    AppError e;
    e.category = ErrorCategory::Permission;
    e.code = ErrorCode::AccessDenied;
    const std::string who = quoted(subject);
    e.summary = who.empty() ? std::string("access was denied") : "access to " + who + " was denied";
    e.subject = std::move(subject);
    e.detail = std::move(detail);
    // The permission lives outside the document, so no button the frontend
    // could draw would change the answer — unlike read_only, there is nowhere
    // else to save it to. Reported, not recovered from.
    return e;
}

AppError stale_operation(std::string detail) {
    AppError e;
    e.category = ErrorCategory::Stale;
    e.code = ErrorCode::StaleOperation;
    e.summary = "the document changed since this operation started";
    e.detail = std::move(detail);
    e.recoverable = true;
    e.actions = {RecoveryAction::Reload};
    return e;
}

AppError file_not_found(std::string path) {
    AppError e;
    e.category = ErrorCategory::NotFound;
    e.code = ErrorCode::FileNotFound;
    // Same shape as object_not_found's: the colon introduces a name, so it has
    // no business appearing when there is no name to introduce.
    const std::string who = quoted(path);
    e.summary = "no such file" + (who.empty() ? std::string() : ": " + who);
    e.subject = std::move(path);
    return e;
}

AppError object_not_found(std::string kind, std::string subject) {
    AppError e;
    e.category = ErrorCategory::NotFound;
    e.code = ErrorCode::ObjectNotFound;
    const std::string who = quoted(subject);
    e.summary = "no such " + kind + (who.empty() ? std::string() : ": " + who);
    e.subject = std::move(subject);
    return e;
}

AppError invalid_format(std::string subject, std::string detail) {
    AppError e;
    e.category = ErrorCategory::InvalidInput;
    e.code = ErrorCode::InvalidFormat;
    const std::string who = quoted(subject);
    e.summary = (who.empty() ? "the data" : who) + " is not in the expected format";
    e.subject = std::move(subject);
    e.detail = std::move(detail);
    return e;
}

AppError validation_failed(std::string summary) {
    AppError e;
    e.category = ErrorCategory::InvalidInput;
    e.code = ErrorCode::ValidationFailed;
    e.summary = std::move(summary);
    return e;
}

AppError validation_failed(std::string summary, SourceSpan at) {
    AppError e = validation_failed(std::move(summary));
    e.source_span = at;
    return e;
}

AppError constraint_violated(std::string subject, std::string detail) {
    AppError e;
    e.category = ErrorCategory::Conflict;
    e.code = ErrorCode::ConstraintViolated;
    const std::string who = quoted(subject);
    e.summary = (who.empty() ? "the value" : who) + " breaks a rule the data must keep";
    e.subject = std::move(subject);
    e.detail = std::move(detail);
    // The value is the caller's to change, so a retry with a different one is
    // the whole recovery — unlike file_exists, nothing here can be overwritten.
    e.recoverable = true;
    e.actions = {RecoveryAction::Retry};
    return e;
}

AppError io_failed(std::string subject, std::string detail) {
    AppError e;
    e.category = ErrorCategory::Io;
    e.code = ErrorCode::IoFailed;
    e.summary = "could not read or write " + (quoted(subject).empty() ? "the file"
                                                                      : quoted(subject));
    e.subject = std::move(subject);
    e.detail = std::move(detail);
    e.recoverable = true;
    e.actions = {RecoveryAction::Retry};
    return e;
}

AppError network_failed(std::string subject, std::string detail) {
    AppError e;
    e.category = ErrorCategory::Io;
    e.code = ErrorCode::NetworkFailed;
    const std::string who = quoted(subject);
    e.summary = "could not reach " + (who.empty() ? "the server" : who);
    e.subject = std::move(subject);
    e.detail = std::move(detail);
    // A network is the one Io failure that is routinely transient, so the retry
    // io_failed offers is if anything more apt here.
    e.recoverable = true;
    e.actions = {RecoveryAction::Retry};
    return e;
}

AppError unsupported(std::string summary) {
    AppError e;
    e.category = ErrorCategory::Unsupported;
    e.code = ErrorCode::Unsupported;
    e.summary = std::move(summary);
    return e;
}

AppError cancelled() {
    AppError e;
    e.category = ErrorCategory::Cancelled;
    e.code = ErrorCode::Cancelled;
    e.summary = "the operation was cancelled";
    return e;
}

AppError internal_error(std::string detail) {
    AppError e;
    e.category = ErrorCategory::Internal;
    e.code = ErrorCode::InternalError;
    e.summary = "an unexpected internal error occurred";
    e.detail = std::move(detail);
    return e;
}

AppError from_exception(const std::exception& ex) {
    AppError e = internal_error({});
    e.exception_text = ex.what();
    return e;
}

// The structured constructor of the suite-wide base error. Defined here
// (not in error.hpp) because it needs the full AppError type: what() is
// the summary, and the payload is stored for structured() to expose.
Error::Error(AppError structured)
    : std::runtime_error(structured.summary),
      structured_(std::make_shared<const AppError>(std::move(structured))) {}

} // namespace cworks
