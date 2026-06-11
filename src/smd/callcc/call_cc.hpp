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

}  // namespace callcc_detail

}  // namespace smd

#endif  // INCLUDED_SMD_CALLCC_CALL_CC
