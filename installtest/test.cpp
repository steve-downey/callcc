// testinstall/test.cpp                                               -*-C++-*-
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <smd/callcc/call_cc.hpp>

#include <cassert>

namespace ex = beman::execution;

int main() {
    auto work = smd::call_cc<int>([](auto escape) { return escape(42); });

    auto result = ex::sync_wait(std::move(work));
    assert(result.has_value());
    assert(get<0>(result.value()) == 42);
    return 0;
}
