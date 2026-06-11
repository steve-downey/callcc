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

TEST_CASE("shared_state stashes the escaped value without completing outer", "[callcc][shared_state]") {
    int  value   = 0;
    bool errored = false;
    bool stopped = false;

    smd::callcc_detail::call_cc_shared_state<probe_receiver, int> ss{
        probe_receiver{&value, &errored, &stopped}};

    REQUIRE_FALSE(ss.has_escaped());
    REQUIRE_FALSE(ss.stop_source.stop_requested());

    ss.do_escape(7);
    REQUIRE(ss.has_escaped());
    REQUIRE(*ss.escape_value == 7);
    REQUIRE(ss.stop_source.stop_requested());  // escape requests downward stop
    // do_escape does NOT complete the outer receiver (drain-then-complete).
    REQUIRE(value == 0);
    REQUIRE_FALSE(errored);
    REQUIRE_FALSE(stopped);

    // A second escape is dropped (one-shot winner latch).
    ss.do_escape(99);
    REQUIRE(*ss.escape_value == 7);
}

namespace {
// A local receiver recording the channel the escape sender completes it with.
struct local_probe {
    using receiver_concept = ex::receiver_tag;
    bool* got_value;
    bool* got_stopped;
    void set_value(int) && noexcept { *got_value = true; }
    void set_error(std::exception_ptr) && noexcept {}
    void set_stopped() && noexcept { *got_stopped = true; }
};
}  // namespace

TEST_CASE("escape sender stops its local receiver and stashes the value", "[callcc][escape]") {
    int  value   = 0;
    bool errored = false;
    bool stopped = false;

    using shared_t = smd::callcc_detail::call_cc_shared_state<probe_receiver, int>;
    shared_t ss{probe_receiver{&value, &errored, &stopped}};

    smd::callcc_detail::escape_factory<int> factory{&ss};
    auto sndr = factory(55);
    STATIC_REQUIRE(ex::sender<decltype(sndr)>);

    bool got_value = false, got_stopped = false;
    auto op = ex::connect(std::move(sndr), local_probe{&got_value, &got_stopped});
    ex::start(op);

    REQUIRE(got_stopped);          // local receiver is stopped, not valued
    REQUIRE_FALSE(got_value);
    REQUIRE(ss.has_escaped());     // value delivered out-of-band to the sink
    REQUIRE(*ss.escape_value == 55);
    REQUIRE(ss.stop_source.stop_requested());
}

TEST_CASE("inner receiver injects the local stop token; forwards or substitutes", "[callcc][inner]") {
    int  value   = 0;
    bool errored = false;
    bool stopped = false;

    using shared_t = smd::callcc_detail::call_cc_shared_state<probe_receiver, int>;

    SECTION("no escape: forwards the inner value") {
        shared_t ss{probe_receiver{&value, &errored, &stopped}};
        smd::callcc_detail::inner_receiver<shared_t> rcvr{&ss};
        STATIC_REQUIRE(ex::receiver<decltype(rcvr)>);

        auto token = ex::get_stop_token(ex::get_env(rcvr));
        REQUIRE_FALSE(token.stop_requested());
        ss.stop_source.request_stop();
        REQUIRE(token.stop_requested());  // env hands out the local token

        std::move(rcvr).set_value(33);
        REQUIRE(value == 33);
    }

    SECTION("escaped: substitutes the stashed value") {
        shared_t ss{probe_receiver{&value, &errored, &stopped}};
        ss.do_escape(88);
        smd::callcc_detail::inner_receiver<shared_t> rcvr{&ss};
        std::move(rcvr).set_stopped();   // inner drained as stopped
        REQUIRE(value == 88);            // escaped value delivered instead
    }
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
