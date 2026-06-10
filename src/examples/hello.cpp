// hello.cpp                                                          -*-C++-*-
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <smd/callcc/execution_fw.hpp>

#include <print>

// Phase 1 smoke example: pipeline through the facade only.
int main() {
    auto work = fw::exec::just(21) | fw::exec::then([](int i) { return i * 2; });
    auto [result] = fw::exec::sync_wait(std::move(work)).value();
    std::println("hello, callcc: {}", result);
}
