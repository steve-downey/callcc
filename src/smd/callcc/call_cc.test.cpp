// call_cc.test.cpp                                                   -*-C++-*-
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <smd/callcc/call_cc.hpp>

#include <catch2/catch_test_macros.hpp>

namespace ex = ::beman::execution;

TEST_CASE("call_cc header includes cleanly", "[callcc][scaffold]") {
    STATIC_REQUIRE(ex::sender<decltype(ex::just(1))>);
}
