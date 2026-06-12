// execution_fw.hpp                                                   -*-C++-*-
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// The Metaprogramming Firewall: the single header that ever includes a
// std::execution reference implementation. All downstream code must use
// fw::exec::*, never the underlying namespace directly.
#ifndef INCLUDED_SMD_CALLCC_EXECUTION_FW
#define INCLUDED_SMD_CALLCC_EXECUTION_FW

#if !defined(FW_USE_BEMAN) && !defined(FW_USE_STDEXEC)
#define FW_USE_BEMAN 1
#endif

#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#if defined(FW_USE_BEMAN)
#include <beman/execution/execution.hpp>
#include <beman/execution/stop_token.hpp>
#elif defined(FW_USE_STDEXEC)
#error "stdexec backend not yet wired up; only FW_USE_BEMAN is implemented"
#else
#error "execution_fw.hpp: pick a backend (FW_USE_BEMAN or FW_USE_STDEXEC)"
#endif

namespace fw::exec {

#if defined(FW_USE_BEMAN)

namespace _impl = ::beman::execution;

using _impl::completion_signatures;
using _impl::completion_signatures_of_t;
using _impl::env;
using _impl::get_env;
using _impl::prop;

// beman::execution spells the empty environment as `env<>`; the P2300 draft
// and the Northstar API use `empty_env`. Map the name here.
using empty_env = _impl::env<>;

using _impl::operation_state;
using _impl::receiver;
using _impl::scheduler;
using _impl::sender;
using _impl::sender_in;
using _impl::sender_to;

// Tag types — required by user-authored Senders/Receivers/OperationStates
// to spell their concept tags without reaching into the underlying namespace.
using _impl::operation_state_tag;
using _impl::receiver_tag;
using _impl::scheduler_tag;
using _impl::sender_tag;

using _impl::inline_scheduler;

using _impl::schedule;

using _impl::just;
using _impl::just_error;
using _impl::just_stopped;
using _impl::let_error;
using _impl::let_stopped;
// `fw::exec::let_value` is *not* a passthrough; it's redefined below with a
// stop-token QoI patch absent from beman. See detail::let_value_qoi.
using _impl::on;
using _impl::read_env;
using _impl::starts_on;
using _impl::sync_wait;
using _impl::then;
using _impl::upon_error;
using _impl::upon_stopped;
using _impl::when_all;
using _impl::write_env;

// Standard P2300 renamed `transfer` to `continues_on`. The Northstar API
// still spells it `transfer`; route it through the facade so business code
// is insulated from the rename churn.
using _impl::continues_on;
inline constexpr auto& transfer = continues_on;

using _impl::connect;
using _impl::set_error;
using _impl::set_error_t;
using _impl::set_stopped;
using _impl::set_stopped_t;
using _impl::set_value;
using _impl::set_value_t;
using _impl::start;

using _impl::get_stop_token;
using _impl::get_completion_scheduler;
using _impl::get_scheduler;

// Stop-token primitives. libstdc++ has not yet shipped the P2300 additions,
// so the facade routes inplace_stop_source/inplace_stop_token through beman.
using _impl::inplace_stop_source;
using _impl::inplace_stop_token;

// ---------------------------------------------------------------------------
// let_value with stop-token QoI patch
//
// P2300 does not strictly require `let_value` to check the env's stop_token
// at the upstream-completion→factory-output handoff; beman::execution does
// not. The Northstar API expects stop requests issued inside the factory
// body to be honoured before the inner sender starts. We wrap the user's
// factory so its return value flows through a `guarded_sender` that
// performs the stop check at start().
// ---------------------------------------------------------------------------
namespace detail::let_value_qoi {

template <typename CS>
struct ensure_set_stopped_sig;

template <typename... Sigs>
struct ensure_set_stopped_sig<_impl::completion_signatures<Sigs...>> {
    static constexpr bool has_stopped =
        (::std::is_same_v<Sigs, _impl::set_stopped_t()> || ...);
    using type = ::std::conditional_t<
        has_stopped,
        _impl::completion_signatures<Sigs...>,
        _impl::completion_signatures<Sigs..., _impl::set_stopped_t()>>;
};

template <typename Inner, typename Receiver>
class guarded_op {
  public:
    using operation_state_concept = _impl::operation_state_tag;

    template <typename I, typename R>
    guarded_op(I&& i, R&& r)
        : inner_(::std::forward<I>(i)), receiver_(::std::forward<R>(r)) {}

    guarded_op(const guarded_op&)            = delete;
    guarded_op(guarded_op&&)                 = delete;
    guarded_op& operator=(const guarded_op&) = delete;
    guarded_op& operator=(guarded_op&&)      = delete;

    ~guarded_op() {
        if (started_) {
            storage_.inner_op.~inner_op_t();
        }
    }

    void start() & noexcept {
        if (_impl::get_stop_token(_impl::get_env(receiver_)).stop_requested()) {
            _impl::set_stopped(::std::move(receiver_));
            return;
        }
        ::new (&storage_.inner_op) inner_op_t(
            _impl::connect(::std::move(inner_), ::std::move(receiver_)));
        started_ = true;
        _impl::start(storage_.inner_op);
    }

  private:
    using inner_op_t = decltype(_impl::connect(
        ::std::declval<Inner>(), ::std::declval<Receiver>()));

    union storage_t {
        storage_t() {}
        ~storage_t() {}
        inner_op_t inner_op;
    };

    Inner     inner_;
    Receiver  receiver_;
    storage_t storage_{};
    bool      started_ = false;
};

template <typename Inner>
struct guarded_sender {
    using sender_concept = _impl::sender_tag;

    Inner inner;

    template <typename, typename... Env>
    static consteval auto get_completion_signatures() noexcept {
        using inner_sigs = _impl::completion_signatures_of_t<Inner, Env...>;
        return typename ensure_set_stopped_sig<inner_sigs>::type{};
    }

    template <typename Receiver>
    auto connect(Receiver&& r)
        -> guarded_op<Inner, ::std::decay_t<Receiver>> {
        return {::std::move(inner), ::std::forward<Receiver>(r)};
    }
};

}  // namespace detail::let_value_qoi

// CPO that mimics _impl::let_value but inserts the stop-check by wrapping
// the user-supplied factory: the wrapped factory invokes the user's
// callable (so any side effects, including `request_stop`, still execute)
// and then wraps the returned sender in `guarded_sender`.
inline constexpr struct let_value_t {
    template <typename Factory>
    auto operator()(Factory f) const {
        return _impl::let_value(
            [user_f = ::std::move(f)]<typename... Args>(Args&&... args) mutable {
                auto inner = user_f(::std::forward<Args>(args)...);
                return detail::let_value_qoi::guarded_sender<decltype(inner)>{
                    ::std::move(inner)};
            });
    }
} let_value;

// ---------------------------------------------------------------------------
// static_thread_pool
//
// beman::execution does not ship a thread pool; the Northstar API requires
// one. Implementation lives entirely under `detail::thread_pool_impl` to
// keep custom Senders/Receivers/OperationStates out of any namespace the
// underlying library might apply ADL through.
// ---------------------------------------------------------------------------
namespace detail::thread_pool_impl {

struct op_base {
    op_base*               next{nullptr};
    void                   (*execute_fn)(op_base*) noexcept;
    void execute() noexcept { execute_fn(this); }
};

class thread_pool {
  public:
    explicit thread_pool(::std::size_t n) {
        if (n == 0) {
            n = 1;
        }
        workers_.reserve(n);
        for (::std::size_t i = 0; i < n; ++i) {
            workers_.emplace_back([this] { worker_loop(); });
        }
    }
    thread_pool(const thread_pool&)            = delete;
    thread_pool& operator=(const thread_pool&) = delete;
    thread_pool(thread_pool&&)                 = delete;
    thread_pool& operator=(thread_pool&&)      = delete;

    ~thread_pool() {
        {
            ::std::lock_guard guard(mutex_);
            stopping_ = true;
        }
        cv_.notify_all();
        for (auto& t : workers_) {
            if (t.joinable()) {
                t.join();
            }
        }
    }

    void enqueue(op_base* op) noexcept {
        {
            ::std::lock_guard guard(mutex_);
            if (back_ != nullptr) {
                back_->next = op;
            } else {
                front_ = op;
            }
            back_ = op;
        }
        cv_.notify_one();
    }

  private:
    void worker_loop() noexcept {
        for (;;) {
            op_base* op = nullptr;
            {
                ::std::unique_lock guard(mutex_);
                cv_.wait(guard, [this] { return front_ != nullptr || stopping_; });
                if (front_ == nullptr) {
                    return;
                }
                op = front_;
                front_ = front_->next;
                if (front_ == nullptr) {
                    back_ = nullptr;
                }
            }
            op->execute();
        }
    }

    ::std::mutex              mutex_;
    ::std::condition_variable cv_;
    op_base*                  front_{nullptr};
    op_base*                  back_{nullptr};
    bool                      stopping_{false};
    ::std::vector<::std::thread> workers_;
};

template <typename Receiver>
struct thread_pool_op : op_base {
    using operation_state_concept = _impl::operation_state_tag;

    thread_pool* pool_;
    Receiver     receiver_;

    template <typename R>
    thread_pool_op(thread_pool* p, R&& r)
        : op_base{nullptr, &thread_pool_op::execute_impl},
          pool_(p),
          receiver_(::std::forward<R>(r)) {}

    void start() & noexcept { pool_->enqueue(this); }

    static void execute_impl(op_base* base) noexcept {
        auto* self = static_cast<thread_pool_op*>(base);
        using token_t = decltype(_impl::get_stop_token(_impl::get_env(self->receiver_)));
        if constexpr (!_impl::unstoppable_token<token_t>) {
            if (_impl::get_stop_token(_impl::get_env(self->receiver_)).stop_requested()) {
                _impl::set_stopped(::std::move(self->receiver_));
                return;
            }
        }
        _impl::set_value(::std::move(self->receiver_));
    }
};

struct thread_pool_sender {
    using sender_concept = _impl::sender_tag;

    thread_pool* pool_;

    template <typename, typename... Env>
    static consteval auto get_completion_signatures() noexcept {
        if constexpr (
            _impl::unstoppable_token<decltype(_impl::get_stop_token(::std::declval<Env>()...))>) {
            return _impl::completion_signatures<_impl::set_value_t()>{};
        } else {
            return _impl::completion_signatures<_impl::set_value_t(), _impl::set_stopped_t()>{};
        }
    }

    template <typename Receiver>
    auto connect(Receiver&& r) noexcept
        -> thread_pool_op<::std::decay_t<Receiver>> {
        return {pool_, ::std::forward<Receiver>(r)};
    }
};

class thread_pool_scheduler {
  public:
    using scheduler_concept = _impl::scheduler_tag;

    explicit thread_pool_scheduler(thread_pool* p) noexcept : pool_(p) {}

    auto schedule() const noexcept -> thread_pool_sender { return {pool_}; }

    friend bool operator==(thread_pool_scheduler a, thread_pool_scheduler b) noexcept {
        return a.pool_ == b.pool_;
    }

  private:
    thread_pool* pool_;
};

}  // namespace detail::thread_pool_impl

class static_thread_pool {
  public:
    explicit static_thread_pool(::std::size_t thread_count) : pool_(thread_count) {}

    auto get_scheduler() noexcept -> detail::thread_pool_impl::thread_pool_scheduler {
        return detail::thread_pool_impl::thread_pool_scheduler{&pool_};
    }

  private:
    detail::thread_pool_impl::thread_pool pool_;
};

#endif  // FW_USE_BEMAN

}  // namespace fw::exec

namespace fw {

// Variable-template concept aliases for the directive's Northstar Test 1.
template <typename S>
inline constexpr bool is_sender = fw::exec::sender<S>;

template <typename Sch>
inline constexpr bool is_scheduler = fw::exec::scheduler<Sch>;

}  // namespace fw

// Phase 2: diagnostic toolkit. Helpers that force the compiler to surface
// completion signatures in its error output, so that custom-sender breakage
// can be diagnosed without relying on vendor-specific concepts.
namespace fw::diag {

// `display<...>` is intentionally undefined. Instantiating it (e.g. via the
// `dump_signatures_t` alias) triggers an "incomplete type" error whose
// message embeds the template arguments verbatim — the most reliable way
// to coax a compiler into printing deduced sender signatures.
template <typename...>
struct display;

// Type alias whose mere mention forces the compiler to evaluate the
// sender's completion signatures and then choke on the incomplete
// `display`. Use as a manual diagnostic probe in a TU.
template <typename Sender, typename Env = fw::exec::empty_env>
using dump_signatures_t =
    display<fw::exec::completion_signatures_of_t<Sender, Env>>;

// Predicates suitable for use inside `static_assert` so the build aborts
// at the call site instead of deep inside a CPO. Pair them with a
// `dump_signatures_t` line above the failing assertion to get the sigs
// printed alongside the diagnostic.
template <typename Sender, typename Env = fw::exec::empty_env>
inline constexpr bool is_sender_in_v = fw::exec::sender_in<Sender, Env>;

template <typename Sender, typename Expected, typename Env = fw::exec::empty_env>
inline constexpr bool has_signatures_v = ::std::is_same_v<
    fw::exec::completion_signatures_of_t<Sender, Env>,
    Expected>;

}  // namespace fw::diag

#endif  // INCLUDED_SMD_CALLCC_EXECUTION_FW
