// callcc_void_escape.cpp                                             -*-C++-*-
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Demonstrates call_cc_void — a convenience for early-exit with no return
// value. The escape factory's operator() takes no arguments; the underlying
// value type is std::monostate.

#include <smd/callcc/call_cc.hpp>

#include <beman/execution/execution.hpp>

#include <print>
#include <variant>

namespace ex = ::beman::execution;

int main() {
    int steps_run = 0;

    // call_cc_void: the escape carries no value (monostate). Useful when you
    // only want to short-circuit, not deliver a specific result.
    auto work = smd::call_cc_void([&](auto bail) {
        return ex::just(std::monostate{}) | ex::then([&](std::monostate m) {
                   ++steps_run;
                   return m;
               }) |
               ex::let_value([bail](std::monostate) {
                   return bail(); // escape — no value needed
               }) |
               ex::then([&](std::monostate m) { // *** skipped ***
                   ++steps_run;
                   return m;
               });
    });

    ex::sync_wait(std::move(work));

    std::println("void_escape: steps_run = {} (expected 1)", steps_run);
    // Expected: steps_run = 1
}
