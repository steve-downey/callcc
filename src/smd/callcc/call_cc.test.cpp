// call_cc.test.cpp                                                   -*-C++-*-
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <smd/callcc/call_cc.hpp>

#include <catch2/catch_test_macros.hpp>

#include <exception>
#include <utility>

namespace ex = ::beman::execution;

TEST_CASE("call_cc header includes cleanly", "[callcc][scaffold]") {
    STATIC_REQUIRE(ex::sender<decltype(ex::just(1))>);
}

namespace {
// A minimal receiver that records which completion channel fired.
struct probe_receiver {
    using receiver_concept = ex::receiver_tag;
    int* value;
    bool* errored;
    bool* stopped;
    void set_value(int v) && noexcept { *value = v; }
    void set_error(std::exception_ptr) && noexcept { *errored = true; }
    void set_stopped() && noexcept { *stopped = true; }
};
}  // namespace

TEST_CASE("shared_state completes the outer receiver exactly once", "[callcc][shared_state]") {
    int  value   = 0;
    bool errored = false;
    bool stopped = false;

    smd::callcc_detail::call_cc_shared_state<probe_receiver, int> ss{
        probe_receiver{&value, &errored, &stopped}};

    REQUIRE_FALSE(ss.stop_source.stop_requested());

    ss.complete_value(7);
    REQUIRE(value == 7);
    REQUIRE(ss.stop_source.stop_requested());  // completion requests stop

    // A second completion attempt is dropped (exactly-one-completion rule).
    ss.complete_value(99);
    REQUIRE(value == 7);
}
