// call_cc.test.cpp                                                   -*-C++-*-
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <smd/callcc/call_cc.hpp>

#include <catch2/catch_test_macros.hpp>

#include <exception>
#include <stdexcept>
#include <string>
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

namespace {
// A throwaway local receiver for the escape sender; the escape sender never
// completes it, so its members need only exist to satisfy the receiver concept.
struct discard_receiver {
    using receiver_concept = ex::receiver_tag;
    void set_value(int) && noexcept {}
    void set_error(std::exception_ptr) && noexcept {}
    void set_stopped() && noexcept {}
};
}  // namespace

TEST_CASE("escape sender completes the outer receiver, not its local one", "[callcc][escape]") {
    int  value   = 0;
    bool errored = false;
    bool stopped = false;

    using shared_t = smd::callcc_detail::call_cc_shared_state<probe_receiver, int>;
    shared_t ss{probe_receiver{&value, &errored, &stopped}};

    smd::callcc_detail::escape_factory<shared_t, int> factory{&ss};
    auto sndr = factory(55);
    STATIC_REQUIRE(ex::sender<decltype(sndr)>);

    auto op = ex::connect(std::move(sndr), discard_receiver{});
    ex::start(op);

    REQUIRE(value == 55);                      // outer receiver got the escaped value
    REQUIRE(ss.stop_source.stop_requested());  // escape requested downward stop
}

TEST_CASE("inner receiver injects the local stop token and forwards value", "[callcc][inner]") {
    int  value   = 0;
    bool errored = false;
    bool stopped = false;

    using shared_t = smd::callcc_detail::call_cc_shared_state<probe_receiver, int>;
    shared_t ss{probe_receiver{&value, &errored, &stopped}};

    smd::callcc_detail::inner_receiver<shared_t> rcvr{&ss};
    STATIC_REQUIRE(ex::receiver<decltype(rcvr)>);

    // The environment hands out the shared state's local stop token, not the
    // outer one: requesting stop on the shared source is observable here.
    auto token = ex::get_stop_token(ex::get_env(rcvr));
    REQUIRE_FALSE(token.stop_requested());
    ss.stop_source.request_stop();
    REQUIRE(token.stop_requested());

    // set_value forwards into the shared state and out to the outer receiver.
    std::move(rcvr).set_value(33);
    REQUIRE(value == 33);
}

#include <stdexcept>

TEST_CASE("call_cc normal completion forwards inner value", "[callcc]") {
    auto work = smd::call_cc<int>([](auto /*escape*/) { return ex::just(42); });
    auto [v]  = ex::sync_wait(std::move(work)).value();
    REQUIRE(v == 42);
}

TEST_CASE("call_cc escape completes outer with escaped value", "[callcc][escape]") {
    auto work = smd::call_cc<int>([](auto escape) { return escape(99); });
    auto [v]  = ex::sync_wait(std::move(work)).value();
    REQUIRE(v == 99);
}

TEST_CASE("call_cc forwards inner errors", "[callcc][error]") {
    auto work = smd::call_cc<int>([](auto /*escape*/) {
        return ex::just(0) | ex::then([](int) -> int {
                   throw std::runtime_error{"boom"};
               });
    });
    REQUIRE_THROWS_AS(ex::sync_wait(std::move(work)), std::runtime_error);
}

TEST_CASE("call_cc reports a throwing factory via set_error (no terminate)", "[callcc][error]") {
    auto work = smd::call_cc<int>([](auto /*escape*/) -> decltype(ex::just(0)) {
        throw std::runtime_error{"factory boom"};
    });
    try {
        ex::sync_wait(std::move(work));
        FAIL("expected sync_wait to throw");
    } catch (const std::runtime_error& e) {
        REQUIRE(std::string{e.what()} == "factory boom");
    }
}

TEST_CASE("call_cc propagates upward cancellation to inner work", "[callcc][cancel]") {
    ex::inplace_stop_source outer_src;
    bool inner_saw_stop = false;

    auto work = smd::call_cc<int>([&](auto /*escape*/) {
        return ex::read_env(ex::get_stop_token) | ex::then([&](auto tok) {
                   inner_saw_stop = tok.stop_requested();
                   return 0;
               });
    });

    auto stoppable = ex::write_env(
        std::move(work),
        ex::prop{ex::get_stop_token, outer_src.get_token()});

    outer_src.request_stop();  // before start: callback fires during start()
    ex::sync_wait(std::move(stoppable));
    REQUIRE(inner_saw_stop == true);
}
