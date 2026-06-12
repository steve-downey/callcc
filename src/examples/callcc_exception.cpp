// callcc_exception.cpp                                               -*-C++-*-
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Classic callCC use #2: EXCEPTION-LIKE NON-LOCAL EXIT.
//
// Before structured exceptions, callCC was the standard way to model
// throw/catch: the `call_cc` boundary is the `catch`, and invoking the escape
// is the `throw` — it abandons the local continuation and jumps to the
// boundary with a recovery value.
//
// Here the escape is taken *conditionally*. The decision rides the error
// completion channel: a stage throws on bad input, `let_error` is the handler,
// and the handler escapes with a recovery value. On good input the value
// channel simply flows through untouched. (The branches stay type-stable
// because they live on different completion channels — value vs. error — rather
// than being two different sender types out of one factory.)

#include <smd/callcc/call_cc.hpp>

#include <beman/execution/execution.hpp>

#include <exception>
#include <print>
#include <stdexcept>

namespace ex = ::beman::execution;

// safe_double(n) doubles n, but "throws" for negatives and recovers via an
// escape to -1 — exactly the throw/catch shape, expressed with call_cc.
int safe_double(int n) {
    auto work = smd::call_cc<int>([n](auto raise) {
        return ex::just(n) |
               ex::then([](int x) -> int {
                   if (x < 0) {
                       throw std::domain_error{"negative input"};
                   }
                   return x * 2;
               })
               // `let_error` is the handler; raising escapes to the boundary.
               |
               ex::let_error([raise](std::exception_ptr) { return raise(-1); });
    });
    auto [result] = ex::sync_wait(std::move(work)).value();
    return result;
}

int main() {
    std::println("safe_double(21)  = {}", safe_double(21)); // 42 (value path)
    std::println("safe_double(-5)  = {}", safe_double(-5)); // -1 (escaped)
    std::println("safe_double(0)   = {}", safe_double(0));  // 0  (value path)
}
