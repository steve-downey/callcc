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

}  // namespace callcc_detail

}  // namespace smd

#endif  // INCLUDED_SMD_CALLCC_CALL_CC
