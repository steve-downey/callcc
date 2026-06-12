// callcc_deep_escape.cpp                                             -*-C++-*-
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Classic callCC use #3: ESCAPE FROM A DEEPLY NESTED CONTEXT.
//
// callCC's signature selling point is that the captured continuation can be
// invoked from *anywhere* inside the body — however many layers deep — and it
// unwinds straight back to the boundary, skipping every intervening
// continuation. This is the asynchronous analogue of a multi-level `break` or a
// jump to a labelled exit.

#include <smd/callcc/call_cc.hpp>

#include <beman/execution/execution.hpp>

#include <print>

namespace ex = ::beman::execution;

int main() {
    bool depth2_tail_ran = false;
    bool depth1_tail_ran = false;

    // Three nested let_value levels. We escape from the innermost one; neither
    // the depth-2 nor the depth-1 trailing stage runs.
    auto work = smd::call_cc<int>([&](auto out) {
        return ex::just(1) | ex::let_value([&, out](int a) { // depth 1
                   return ex::just(a + 10) |
                          ex::let_value([&, out](int b) { // depth 2
                              return ex::just(b + 100) |
                                     ex::then([](int c) { return c + 1000; })
                                     // escape from depth 3 with 1111
                                     | ex::let_value(
                                           [out](int c) { return out(c); });
                          }) |
                          ex::then([&](int x) { // depth-2 tail
                              depth2_tail_ran = true;
                              return x;
                          });
               }) |
               ex::then([&](int x) { // depth-1 tail
                   depth1_tail_ran = true;
                   return x;
               });
    });

    auto [result] = ex::sync_wait(std::move(work)).value();

    std::println("deep_escape: result = {}",
                 result); // 1 + 10 + 100 + 1000 = 1111
    std::println("  depth-2 tail ran = {}", depth2_tail_ran); // false
    std::println("  depth-1 tail ran = {}", depth1_tail_ran); // false
}
