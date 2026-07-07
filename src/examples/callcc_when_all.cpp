// callcc_when_all.cpp                                                -*-C++-*-
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Demonstrates escape.value(v) — the value-channel escape — inside a when_all
// arm. The stopped-channel escape (the default) cannot be a when_all child
// because beman's when_all requires each child to have exactly one value
// completion. escape.value(v) advertises set_value_t(ValueType) instead,
// satisfying when_all's constraint while still triggering the cooperative
// escape and cancelling the other arms.

#include <smd/callcc/call_cc.hpp>

#include <beman/execution/execution.hpp>

#include <print>

namespace ex = ::beman::execution;

int main() {
    bool arm_b_ran = false;

    // Two concurrent arms via when_all. Arm A escapes immediately with 42 via
    // escape.value(); arm B computes 99. Both arms are synchronous so arm B
    // completes before the stop request propagates, but the escape value (42)
    // is still delivered because inner_receiver substitutes the stashed value.
    // With genuinely asynchronous arms, the stop request would cancel arm B
    // before it finishes.
    auto work = smd::call_cc<int>([&](auto escape) {
        return ex::when_all(escape.value(42),
                            ex::just(99) | ex::then([&](int x) {
                                arm_b_ran = true;
                                return x;
                            })) |
               ex::then([](int a, int /*b*/) { return a; });
    });

    auto [result] = ex::sync_wait(std::move(work)).value();

    std::println("when_all: result = {} (arm B ran = {})", result, arm_b_ran);
    // Expected: result = 42 (arm B ran = true with sync senders)
}
