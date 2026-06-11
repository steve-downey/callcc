// call_cc.hpp                                                        -*-C++-*-
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// A C++26 std::execution sender adaptor implementing the semantics of
// Haskell's callCC (call-with-current-continuation), built *directly* on
// beman::execution (not through the fw::exec facade).
#ifndef INCLUDED_SMD_CALLCC_CALL_CC
#define INCLUDED_SMD_CALLCC_CALL_CC

#include <beman/execution/execution.hpp>
#include <beman/execution/stop_token.hpp>

#include <atomic>
#include <exception>
#include <functional>
#include <optional>
#include <type_traits>
#include <utility>

namespace smd {

namespace ex = ::beman::execution;

// callcc_detail — ADL-isolated home for the custom senders, receivers, and
// operation states so they never leak into beman::execution's namespace.
namespace callcc_detail {

// ---------------------------------------------------------------------------
// Escape sink — a tiny interface type-erased on ValueType only (not on the
// outer receiver). This lets escape_factory/escape_sender depend solely on
// ValueType, so call_cc_sender can name the inner sender type (and thus derive
// its completion signatures) without knowing the outer receiver.
// ---------------------------------------------------------------------------
template <class ValueType>
struct escape_sink {
    virtual void do_escape(ValueType) noexcept = 0;

  protected:
    ~escape_sink() = default;
};

// ---------------------------------------------------------------------------
// Shared state — the synchronization node of a call_cc block. Holds the outer
// receiver and an inplace_stop_source for downward cancellation. It does NOT
// complete the outer receiver on escape; instead an escape stashes its value
// here and requests stop, and the *inner operation's single completion* is the
// sole point that completes the outer receiver (drain-then-complete). This
// guarantees no inner work is still in flight when the outer completes.
// ---------------------------------------------------------------------------
template <class OuterReceiver, class ValueType>
struct call_cc_shared_state : escape_sink<ValueType> {
    OuterReceiver            outer_receiver;
    ex::inplace_stop_source  stop_source;
    std::atomic<bool>        escape_claimed{false};  // one-shot winner latch
    std::atomic<bool>        escaped{false};         // publishes escape_value
    std::optional<ValueType> escape_value;

    explicit call_cc_shared_state(OuterReceiver&& rcvr)
        : outer_receiver(std::move(rcvr)) {}

    // Called (possibly from a worker thread) when an escape sender starts.
    void do_escape(ValueType v) noexcept override {
        if (!escape_claimed.exchange(true, std::memory_order_acq_rel)) {
            escape_value.emplace(std::move(v));
            escaped.store(true, std::memory_order_release);  // publish value
            stop_source.request_stop();                      // cancel siblings
        }
    }

    bool has_escaped() const noexcept {
        return escaped.load(std::memory_order_acquire);
    }
};

// ---------------------------------------------------------------------------
// Escape sender — the reified captured continuation. When started it stashes
// the escaped value into the sink and requests stop, then completes its OWN
// local receiver with set_stopped() so the enclosing graph cancels and drains.
// The escaped value is delivered to the outer receiver later, by the inner
// operation's single completion (see inner_receiver). Hence it advertises
// set_stopped_t().
// ---------------------------------------------------------------------------
template <class ValueType, class LocalReceiver>
struct escape_op_state {
    using operation_state_concept = ex::operation_state_tag;

    escape_sink<ValueType>* sink;
    ValueType               value;
    LocalReceiver           local_receiver;

    void start() & noexcept {
        sink->do_escape(std::move(value));
        ex::set_stopped(std::move(local_receiver));
    }
};

template <class ValueType>
struct escape_sender {
    using sender_concept = ex::sender_tag;

    escape_sink<ValueType>* sink;
    ValueType               value;

    template <typename, typename... Env>
    static consteval auto get_completion_signatures() noexcept {
        return ex::completion_signatures<ex::set_stopped_t()>{};
    }

    template <ex::receiver LocalReceiver>
    auto connect(LocalReceiver rcvr) &&
        -> escape_op_state<ValueType, LocalReceiver> {
        return {sink, std::move(value), std::move(rcvr)};
    }

    auto get_env() const noexcept -> ex::env<> { return {}; }
};

template <class ValueType>
struct escape_factory {
    escape_sink<ValueType>* sink;

    auto operator()(ValueType val) const -> escape_sender<ValueType> {
        return {sink, std::move(val)};
    }
};

// ---------------------------------------------------------------------------
// inner_env_t — the environment the inner sender sees. Built from beman's own
// env-composition utilities (the same pattern beman's let adaptor uses): a
// prop overriding get_stop_token with the local inplace_stop_token, joined
// over a forwarding view of the outer environment. Used both at runtime and
// for completion-signature derivation, so the two always agree.
// ---------------------------------------------------------------------------
template <class OuterEnv>
using inner_env_t = decltype(ex::detail::join_env(
    ex::prop{ex::get_stop_token, std::declval<ex::inplace_stop_token>()},
    ex::detail::fwd_env(std::declval<OuterEnv>())));

// ---------------------------------------------------------------------------
// Inner receiver — intercepts the user's inner computation. Its environment
// injects the local stop token. Its completion is the SOLE point that
// completes the outer receiver: if an escape was claimed, deliver the stashed
// value via set_value; otherwise forward the inner sender's own completion.
// ---------------------------------------------------------------------------
template <class SharedState>
struct inner_receiver {
    using receiver_concept = ex::receiver_tag;

    SharedState* shared_state;

    template <class Fallback>
    void finish(Fallback&& fallback) noexcept {
        if (shared_state->has_escaped()) {
            ex::set_value(std::move(shared_state->outer_receiver),
                          std::move(*shared_state->escape_value));
        } else {
            std::forward<Fallback>(fallback)();
        }
    }

    template <class... Args>
    void set_value(Args&&... args) && noexcept {
        finish([&] {
            ex::set_value(std::move(shared_state->outer_receiver),
                          std::forward<Args>(args)...);
        });
    }

    template <class Error>
    void set_error(Error&& err) && noexcept {
        finish([&] {
            ex::set_error(std::move(shared_state->outer_receiver),
                          std::forward<Error>(err));
        });
    }

    void set_stopped() && noexcept {
        finish([&] { ex::set_stopped(std::move(shared_state->outer_receiver)); });
    }

    auto get_env() const noexcept {
        return ex::detail::join_env(
            ex::prop{ex::get_stop_token, shared_state->stop_source.get_token()},
            ex::detail::fwd_env(ex::get_env(shared_state->outer_receiver)));
    }
};

// ---------------------------------------------------------------------------
// Outer operation state — weaves the pieces together. Owns the shared state by
// value (no heap allocation), then connects and starts the user's inner sender.
//
// The inner operation state is held in manual union storage rather than a
// std::optional because operation states are immovable; placement-new from the
// connect() prvalue constructs it in place via guaranteed copy elision.
// ---------------------------------------------------------------------------
template <class OuterReceiver, class F, class ValueType>
struct call_cc_op_state {
    using operation_state_concept = ex::operation_state_tag;

    using SharedStateType   = call_cc_shared_state<OuterReceiver, ValueType>;
    using InnerSenderType   = std::invoke_result_t<F&, escape_factory<ValueType>>;
    using InnerReceiverType = inner_receiver<SharedStateType>;
    using InnerOpStateType  = ex::connect_result_t<InnerSenderType, InnerReceiverType>;

    using OuterStopToken = decltype(ex::get_stop_token(
        ex::get_env(std::declval<const OuterReceiver&>())));
    using StopCallback   = ex::stop_callback_for_t<OuterStopToken, std::function<void()>>;

    SharedStateType             shared_state;
    F                           user_func;
    std::optional<StopCallback> stop_callback;

    union inner_storage_t {
        inner_storage_t() {}
        ~inner_storage_t() {}
        InnerOpStateType op;
    };
    inner_storage_t inner_storage{};
    bool            started{false};

    call_cc_op_state(OuterReceiver rcvr, F func)
        : shared_state(std::move(rcvr)), user_func(std::move(func)) {}

    call_cc_op_state(const call_cc_op_state&)            = delete;
    call_cc_op_state(call_cc_op_state&&)                 = delete;
    call_cc_op_state& operator=(const call_cc_op_state&) = delete;
    call_cc_op_state& operator=(call_cc_op_state&&)      = delete;

    ~call_cc_op_state() {
        if (started) {
            inner_storage.op.~InnerOpStateType();
        }
    }

    void start() & noexcept {
        // Upward cancellation: if the outer token is triggered, request stop on
        // our local source so the inner work is cancelled. Registered before
        // the inner op starts; if the outer token is already stopped the
        // callback fires immediately here.
        auto outer_token =
            ex::get_stop_token(ex::get_env(shared_state.outer_receiver));
        stop_callback.emplace(outer_token, [this]() {
            shared_state.stop_source.request_stop();
        });

        escape_factory<ValueType> factory{&shared_state};
        try {
            ::new (&inner_storage.op) InnerOpStateType(
                ex::connect(user_func(factory), InnerReceiverType{&shared_state}));
        } catch (...) {
            // A throwing user factory or connect must be reported through the
            // error channel, not escape this noexcept start() and terminate.
            ex::set_error(std::move(shared_state.outer_receiver),
                          std::current_exception());
            return;
        }
        started = true;
        ex::start(inner_storage.op);
    }
};

template <class F, class ValueType>
struct call_cc_sender {
    using sender_concept = ex::sender_tag;

    F user_func;

    template <typename, typename... Env>
    static consteval auto get_completion_signatures() noexcept {
        return ex::completion_signatures<ex::set_value_t(ValueType),
                                         ex::set_error_t(std::exception_ptr),
                                         ex::set_stopped_t()>{};
    }

    template <ex::receiver OuterReceiver>
    auto connect(OuterReceiver rcvr) &&
        -> call_cc_op_state<OuterReceiver, F, ValueType> {
        return {std::move(rcvr), std::move(user_func)};
    }

    auto get_env() const noexcept -> ex::env<> { return {}; }
};

}  // namespace callcc_detail

// ---------------------------------------------------------------------------
// call_cc<ValueType>(f) — the sender factory. `f` is invoked with an escape
// factory; calling that factory with a ValueType produces an escape sender
// which, when started, performs the early exit. ValueType is explicit because
// the escape factory's signature must be known before `f` is invoked.
// ---------------------------------------------------------------------------
template <class ValueType, class F>
auto call_cc(F func) -> callcc_detail::call_cc_sender<F, ValueType> {
    return {std::move(func)};
}

}  // namespace smd

#endif  // INCLUDED_SMD_CALLCC_CALL_CC
