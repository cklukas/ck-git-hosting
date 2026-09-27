// libcworks — shared utilities for CK Office
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
#pragma once

#include <memory>
#include <stdexcept>
#include <string>

namespace cworks {

struct AppError; // full definition in app_error.hpp

/// Base error type of CK Office. Components may derive their
/// own
/// (e.g. ctable::Error) so callers can catch per-component or suite-wide.
///
/// Message convention (binding, see the suite charter): name the
/// location, show the context, suggest the fix where possible. Never
/// throw with a bare "invalid argument".
///
/// An Error may optionally carry a structured AppError. Any component
/// error that inherits these constructors can be thrown with a structured
/// payload, so a frontend branches on structured()->code instead of
/// parsing what(). Plain string errors leave structured() null.
class Error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;

    /// Throw with a machine-readable payload; what() becomes its summary.
    explicit Error(AppError structured);

    /// The structured payload, or nullptr for a plain message-only error.
    const AppError* structured() const noexcept { return structured_.get(); }

private:
    // shared (not unique) so the exception object stays copyable.
    std::shared_ptr<const AppError> structured_;
};

} // namespace cworks
