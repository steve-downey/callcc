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
// Shared state — the synchronization node of a call_cc block. Holds the outer
// receiver, an inplace_stop_source for downward cancellation, and an atomic
// completed flag enforcing the exactly-one-completion rule.
// ---------------------------------------------------------------------------
template <class OuterReceiver, class ValueType>
struct call_cc_shared_state {
    OuterReceiver           outer_receiver;
    ex::inplace_stop_source stop_source;
    std::atomic<bool>       completed{false};

    explicit call_cc_shared_state(OuterReceiver&& rcvr)
        : outer_receiver(std::move(rcvr)) {}

    template <class... Args>
    void complete_value(Args&&... args) {
        if (!completed.exchange(true, std::memory_order_release)) {
            stop_source.request_stop();
            ex::set_value(std::move(outer_receiver), std::forward<Args>(args)...);
        }
    }

    template <class Error>
    void complete_error(Error&& err) {
        if (!completed.exchange(true, std::memory_order_release)) {
            stop_source.request_stop();
            ex::set_error(std::move(outer_receiver), std::forward<Error>(err));
        }
    }

    void complete_stopped() {
        if (!completed.exchange(true, std::memory_order_release)) {
            stop_source.request_stop();
            ex::set_stopped(std::move(outer_receiver));
        }
    }
};

// ---------------------------------------------------------------------------
// Escape sender — the reified captured continuation. When started it completes
// the *outer* receiver (via the shared state) and abandons its own local
// receiver, acting as an execution sink.
// ---------------------------------------------------------------------------
template <class SharedState, class ValueType, class LocalReceiver>
struct escape_op_state {
    using operation_state_concept = ex::operation_state_tag;

    SharedState*  shared_state;
    ValueType     value;
    LocalReceiver local_receiver;  // intentionally never completed

    void start() & noexcept { shared_state->complete_value(std::move(value)); }
};

template <class SharedState, class ValueType>
struct escape_sender {
    using sender_concept = ex::sender_tag;

    SharedState* shared_state;
    ValueType    value;

    template <typename, typename... Env>
    static consteval auto get_completion_signatures() noexcept {
        return ex::completion_signatures<ex::set_value_t(ValueType)>{};
    }

    template <ex::receiver LocalReceiver>
    auto connect(LocalReceiver rcvr) &&
        -> escape_op_state<SharedState, ValueType, LocalReceiver> {
        return {shared_state, std::move(value), std::move(rcvr)};
    }

    auto get_env() const noexcept -> ex::env<> { return {}; }
};

template <class SharedState, class ValueType>
struct escape_factory {
    SharedState* shared_state;

    auto operator()(ValueType val) const
        -> escape_sender<SharedState, ValueType> {
        return {shared_state, std::move(val)};
    }
};

// ---------------------------------------------------------------------------
// Inner receiver — intercepts the non-escaped completion of the user's inner
// computation, forwarding it to the shared state, and injects the local stop
// token into the environment seen by the inner sender. The environment is
// composed from beman's own env utilities (the pattern beman's let adaptor
// uses): a prop overriding get_stop_token with the local inplace_stop_token,
// joined over a forwarding view of the outer environment.
// ---------------------------------------------------------------------------
template <class SharedState>
struct inner_receiver {
    using receiver_concept = ex::receiver_tag;

    SharedState* shared_state;

    template <class... Args>
    void set_value(Args&&... args) && noexcept {
        shared_state->complete_value(std::forward<Args>(args)...);
    }

    template <class Error>
    void set_error(Error&& err) && noexcept {
        shared_state->complete_error(std::forward<Error>(err));
    }

    void set_stopped() && noexcept { shared_state->complete_stopped(); }

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
    using InnerSenderType   = std::invoke_result_t<F&, escape_factory<SharedStateType, ValueType>>;
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

        escape_factory<SharedStateType, ValueType> factory{&shared_state};
        ::new (&inner_storage.op) InnerOpStateType(
            ex::connect(user_func(factory), InnerReceiverType{&shared_state}));
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
