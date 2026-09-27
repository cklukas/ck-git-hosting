// cworks — background task runner (operation & task model)
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
//
// A reusable, frontend-neutral executor for long-running work, factored
// out of cpdf's PreviewCache so every app core shares one contract. Task
// bodies run on one owned WORKER thread; their results are delivered back
// on the OWNING (UI/session) thread through poll(), so a ckVision app
// applies each result on its event loop and never touches a widget — or a
// non-thread-safe engine like csheet — off thread. Each task carries the
// same contract: a stable id, an operation kind and description, the
// source-document revision it targets (to drop stale results), a
// cancellation token, measured progress, and a result or structured
// error. Executor affinity is explicit: the body is Worker-side, the
// completion callback is Session-side.
#ifndef CWORKS_TASK_HPP
#define CWORKS_TASK_HPP

#include <cworks/app_error.hpp>
#include <cworks/executor.hpp>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace cworks {

/// Cooperative cancellation, shared between the submitter and the worker
/// running a task body: the body polls cancelled() at safe points and
/// returns early. Copyable (it shares one flag); cancelling is
/// idempotent and thread-safe.
class CancelToken {
public:
    CancelToken() : state_(std::make_shared<std::atomic<State>>(State::Active)) {}

    void request() const noexcept {
        State expected = State::Active;
        state_->compare_exchange_strong(expected, State::Requested,
                                        std::memory_order_relaxed);
    }

    bool cancelled() const noexcept {
        return state_->load(std::memory_order_relaxed) == State::Requested;
    }

    /// Mark the operation's result committed. A later cancellation request is
    /// ignored; an already-requested cancellation remains requested. Bodies
    /// with an irreversible commit point call this immediately afterwards.
    bool complete() const noexcept {
        State expected = State::Active;
        return state_->compare_exchange_strong(expected, State::Completed,
                                               std::memory_order_relaxed) ||
               expected == State::Completed;
    }

private:
    enum class State : unsigned char { Active, Requested, Completed };
    std::shared_ptr<std::atomic<State>> state_;
};

/// A task's lifecycle state; the last three are terminal.
enum class TaskStatus { Queued, Running, Succeeded, Failed, Cancelled };

/// The metadata and outcome of one task. `revision` is the source
/// document revision the task targets, so a caller can drop a result the
/// document has since outrun. `progress` is in [0, 1], or negative when
/// indeterminate. `error` is set only when the body throws.
struct TaskInfo {
    std::uint64_t id = 0;
    std::string kind;
    std::string description;
    long revision = 0;
    TaskStatus status = TaskStatus::Queued;
    double progress = -1.0;
    /// The structured failure of a Failed task: the AppError set at the throw
    /// site, so a frontend's progress UI can switch on error.code — offer a
    /// retry, name the missing file — instead of running `error.find("...")`.
    /// Default (Unknown) on a task that did not fail. The QueryRefreshFailure
    /// template (`c037333`): the field carries the code, message() keeps the
    /// string call sites reading.
    AppError error;

    /// The user-facing summary of `error` — ready to show, and byte-for-byte
    /// what the flat string field carried before the code was threaded
    /// through (the body's exception message for an unstructured throw).
    [[nodiscard]] const std::string& message() const noexcept { return error.summary; }
};

/// Reports progress from inside a task body (thread-safe via the runner).
/// A value outside [0, 1] marks progress indeterminate.
using ProgressFn = std::function<void(double)>;

struct TaskHandle {
    std::uint64_t id = 0;
    CancelToken cancel;
};

/// A serial executor that also accepts observable, cancellable work.
/// Synchronous and submitted operations share one queue and therefore one
/// execution owner. This is the required boundary for engines such as the
/// suite's single-threaded SQLite build: adding responsiveness must never add
/// a second, concurrent route to the same engine object.
class SerialTaskExecutor : public SerialExecutor {
public:
    /// The work to run on the worker thread: it receives a cancel token to
    /// poll and a progress sink to report to. Throwing marks the task
    /// Failed with the exception's message. Capture the result in the
    /// closure and read it in the completion callback.
    using Body = std::function<void(const CancelToken&, const ProgressFn&)>;
    /// Runs on the OWNING thread from poll(), exactly once, after the body
    /// ends (Succeeded / Failed / Cancelled) — the place to apply a result
    /// or report an error, on the session thread.
    using Completion = std::function<void(const TaskInfo&)>;

    /// A submitted task's id paired with its own cancel token.
    using Handle = TaskHandle;

    virtual Handle submit(std::string kind, std::string description,
                          long revision, Body body, Completion on_done) = 0;
    virtual void cancel(std::uint64_t id) = 0;
    virtual void cancel_all() = 0;
    virtual std::size_t poll() = 0;
    virtual void discard_completions() = 0;
    virtual bool has_completions() const = 0;
    virtual std::vector<TaskInfo> active() const = 0;
    /// Registers `notify`, called whenever a task's observable state moves:
    /// it was submitted or cancelled (on the owner's thread), started,
    /// reported progress or landed in the completion queue (on the worker's).
    /// An event-driven owner has no idle tick to poll on; from the notice it
    /// posts one poll() and one repaint of whatever shows active() onto its
    /// own thread, coalescing bursts — a body reporting progress in a tight
    /// loop notifies as often as it reports. The notice runs without the
    /// executor's lock held, is never called once the executor is destroyed
    /// (the destructor joins the workers first), and an empty function
    /// clears it — a clear waits for a notice in flight.
    virtual void set_task_notifier(std::function<void()> notify) = 0;
};

class TaskRunner final : public SerialTaskExecutor {
public:
    using Handle = TaskHandle;

    TaskRunner();
    ~TaskRunner();

    TaskRunner(const TaskRunner&) = delete;
    TaskRunner& operator=(const TaskRunner&) = delete;

    /// Submit a task; the worker runs `body`, then poll() runs `on_done`
    /// on the owning thread. Returns the id and this task's cancel token.
    Handle submit(std::string kind, std::string description, long revision,
                  Body body, Completion on_done) override;

    /// Queue ordinary engine work behind any earlier submitted task and wait
    /// for it. A call made by the worker itself executes inline, which keeps
    /// nested session operations deadlock-free.
    void execute_sync(std::function<void()> operation) override;
    bool is_current() const noexcept override;

    /// Request cooperative cancellation of one task / every task. A queued
    /// task that is cancelled before it starts never runs its body.
    void cancel(std::uint64_t id) override;
    void cancel_all() override;

    /// Run — on the OWNING thread (e.g. the idle loop) — the completion
    /// callback of every task that has finished since the last call, then
    /// forget it. Returns how many completed this call.
    std::size_t poll() override;
    void discard_completions() override;

    /// True when poll() has completions waiting — a cheap idle-loop guard.
    bool has_completions() const override;

    /// A snapshot of the queued and running tasks, for a progress UI.
    std::vector<TaskInfo> active() const override;
    void set_task_notifier(std::function<void()> notify) override;

private:
    struct Task {
        TaskInfo info;
        Body body;
        Completion on_done;
        CancelToken cancel;
        bool tracked = true;
    };

    void worker_loop();
    void notify_task_change();

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool stop_ = false;
    std::uint64_t next_id_ = 1;
    std::deque<std::shared_ptr<Task>> queue_;      ///< waiting to run (FIFO)
    std::shared_ptr<Task> running_;                ///< on the worker now (for active())
    std::vector<std::shared_ptr<Task>> finished_;  ///< done, awaiting poll()
    std::thread worker_;
    std::mutex notifier_mutex_;
    std::function<void()> notifier_;
};

/// A pooled executor for observable, cancellable work whose bodies are
/// independent of any single-owner engine: N worker threads run bodies
/// concurrently, and completions are still delivered exactly once on
/// the OWNING thread through poll(). Tasks start in submission order
/// but finish in any order.
///
/// This is deliberately NOT a SerialTaskExecutor: pooled execution has
/// no ordering guarantee and no execute_sync boundary, so it must never
/// front a single-owner engine (the suite's SQLite build, a csheet
/// workbook). It exists for embarrassingly parallel pure work — e.g.
/// rasterizing independent cplot scenes, which the chart engine's
/// thread model (<cplot/cplot.hpp>) supports without locks.
class TaskPool final {
public:
    using Body = SerialTaskExecutor::Body;
    using Completion = SerialTaskExecutor::Completion;
    using Handle = TaskHandle;

    /// `workers` is clamped to at least one.
    explicit TaskPool(unsigned workers);
    ~TaskPool();

    TaskPool(const TaskPool&) = delete;
    TaskPool& operator=(const TaskPool&) = delete;

    unsigned worker_count() const noexcept;

    /// Submit a task; a free worker runs `body`, then poll() runs
    /// `on_done` on the owning thread. Returns the id and this task's
    /// cancel token. When the pool outlives the submitter, `on_done`
    /// must guard against the submitter being gone (capture a
    /// weak_ptr life token, not a raw this).
    Handle submit(std::string kind, std::string description, long revision,
                  Body body, Completion on_done);

    /// Request cooperative cancellation of one task / every task. A
    /// queued task that is cancelled before it starts never runs its
    /// body.
    void cancel(std::uint64_t id);
    void cancel_all();

    /// Run — on the OWNING thread (e.g. the idle loop) — the completion
    /// callback of every task that has finished since the last call,
    /// then forget it. Returns how many completed this call.
    std::size_t poll();
    void discard_completions();

    /// True when poll() has completions waiting — a cheap idle-loop guard.
    bool has_completions() const;

    /// A snapshot of the queued and running tasks, for a progress UI.
    std::vector<TaskInfo> active() const;
    /// As SerialTaskExecutor::set_task_notifier: called on whichever thread
    /// moves a task's observable state, never with the pool's lock held,
    /// never after the pool is destroyed.
    void set_task_notifier(std::function<void()> notify);

private:
    struct Task {
        TaskInfo info;
        Body body;
        Completion on_done;
        CancelToken cancel;
    };

    void worker_loop();
    void notify_task_change();

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool stop_ = false;
    std::uint64_t next_id_ = 1;
    std::deque<std::shared_ptr<Task>> queue_;      ///< waiting to run (FIFO)
    std::vector<std::shared_ptr<Task>> running_;   ///< on a worker now (for active())
    std::vector<std::shared_ptr<Task>> finished_;  ///< done, awaiting poll()
    std::vector<std::thread> workers_;
    std::mutex notifier_mutex_;
    std::function<void()> notifier_;
};

} // namespace cworks

#endif // CWORKS_TASK_HPP
