// cworks — serial executor contracts for application-core ownership
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
#pragma once

#include <functional>
#include <stdexcept>
#include <thread>

namespace cworks {

/// Synchronously executes work on one serial owner.
///
/// Application sessions use this boundary for engines that require serial
/// ownership. Implementations must complete the callback before returning and
/// must execute a nested call inline when already on the executor. That
/// re-entrancy rule lets event observers safely query the session that emitted
/// their event. A native frontend may provide a worker-backed implementation;
/// the TUI uses InlineSerialExecutor and therefore keeps ownership on its event
/// thread.
class SerialExecutor {
public:
    virtual ~SerialExecutor() = default;

    virtual void execute_sync(std::function<void()> operation) = 0;
    virtual bool is_current() const noexcept = 0;
};

/// A serial executor bound to the thread on which it is constructed.
/// Calling it from another thread is an ownership error rather than an
/// accidental cross-thread engine call.
class InlineSerialExecutor final : public SerialExecutor {
public:
    InlineSerialExecutor() : owner_(std::this_thread::get_id()) {}

    void execute_sync(std::function<void()> operation) override {
        if (!is_current())
            throw std::logic_error(
                "serial executor called from outside its owning thread");
        operation();
    }

    bool is_current() const noexcept override {
        return std::this_thread::get_id() == owner_;
    }

private:
    std::thread::id owner_;
};

} // namespace cworks
