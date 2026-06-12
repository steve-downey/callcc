// callcc_early_return.cpp                                            -*-C++-*-
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Classic callCC use #1: EARLY RETURN.
//
// The defining behaviour of call-with-current-continuation is that the body is
// handed an escape function ("the current continuation"); invoking it returns
// immediately from the whole `call_cc` block, *discarding* the rest of the
// computation. This is the asynchronous analogue of an early `return` from the
// middle of a function.

#include <smd/callcc/call_cc.hpp>

#include <beman/execution/execution.hpp>

#include <print>

namespace ex = ::beman::execution;

int main() {
    bool tail_ran = false;

    // The escape `ret` is the captured continuation. We reach it half-way
    // through the pipeline and jump straight out with the value 42; the
    // trailing `then` (which would multiply by 1000) is never run.
    auto work = smd::call_cc<int>([&](auto ret) {
        return ex::just(21) | ex::then([](int x) { return x * 2; }) // 21 -> 42
               |
               ex::let_value([ret](int x) { return ret(x); }) // early return 42
               | ex::then([&](int x) {                        // *** skipped ***
                     tail_ran = true;
                     return x * 1000;
                 });
    });

    auto [result] = ex::sync_wait(std::move(work)).value();

    std::println("early_return: result = {} (tail ran = {})", result, tail_ran);
    // Expected: result = 42 (tail ran = false)

    // For contrast, the same shape *without* taking the escape runs to the end:
    auto full = smd::call_cc<int>([](auto /*ret*/) {
        return ex::just(21) | ex::then([](int x) { return x * 2; }) // 42
               | ex::then([](int x) { return x + 1; }); // 43 — runs normally
    });
    auto [full_result] = ex::sync_wait(std::move(full)).value();
    std::println("no_escape:    result = {}", full_result);
    // Expected: result = 43
}
