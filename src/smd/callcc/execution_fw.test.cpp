// execution_fw.test.cpp                                              -*-C++-*-
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Phase 1 Northstar tests. Concept verification + diagnostic helper.
// Tests 2-4 (linear execution, cancellation, when_all lifecycle) belong to
// later phases and must not appear here until their prerequisites are built.

#include <smd/callcc/execution_fw.hpp>

#include <smd/callcc/execution_fw.hpp>  // 2nd include must be a no-op.

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <stdexcept>
#include <thread>
#include <utility>

// my_domain — the ADL-isolated namespace mandated by the directive for any
// custom senders/receivers/operation-states the facade's users author.
// Used by Northstar Test 4.
namespace my_domain {

template <typename Receiver>
class tracking_op {
  public:
    using operation_state_concept = fw::exec::operation_state_tag;

    template <typename R>
    tracking_op(::std::atomic<int>* counter, int value, R&& r)
        : counter_(counter), value_(value), receiver_(::std::forward<R>(r)) {
        counter_->fetch_add(1, ::std::memory_order_relaxed);
    }

    ~tracking_op() {
        counter_->fetch_sub(1, ::std::memory_order_relaxed);
    }

    tracking_op(const tracking_op&)            = delete;
    tracking_op(tracking_op&&)                 = delete;
    tracking_op& operator=(const tracking_op&) = delete;
    tracking_op& operator=(tracking_op&&)      = delete;

    void start() & noexcept {
        fw::exec::set_value(::std::move(receiver_), value_);
    }

  private:
    ::std::atomic<int>* counter_;
    int                 value_;
    Receiver            receiver_;
};

struct tracking_sender_t {
    using sender_concept = fw::exec::sender_tag;

    ::std::atomic<int>* counter;
    int                 value;

    template <typename, typename...>
    static consteval auto get_completion_signatures() noexcept {
        return fw::exec::completion_signatures<fw::exec::set_value_t(int)>{};
    }

    template <typename Receiver>
    auto connect(Receiver&& r) noexcept
        -> tracking_op<::std::decay_t<Receiver>> {
        return {counter, value, ::std::forward<Receiver>(r)};
    }
};

inline auto tracking_sender(::std::atomic<int>& counter, int value)
    -> tracking_sender_t {
    return {&counter, value};
}

}  // namespace my_domain

// Northstar Test 1: Concept Verification & Re-exporting -----------------------
static_assert(fw::is_sender<decltype(fw::exec::just(42))>);
static_assert(fw::is_scheduler<fw::exec::inline_scheduler>);

// Diagnostic trigger: instantiating this alias on a failing sender forces
// the compiler to print the generated completion signatures.
template <typename Sender, typename Env = fw::exec::empty_env>
using debug_sigs = fw::exec::completion_signatures_of_t<Sender, Env>;

// Sanity instantiation so the alias actually compiles for a known-good sender.
using _just_sigs = debug_sigs<decltype(fw::exec::just(42))>;
static_assert(sizeof(_just_sigs*) > 0);  // ODR-use the alias.

TEST_CASE("Phase 1 facade smoke", "[exec][phase1]") {
    SECTION("just is a sender") {
        STATIC_REQUIRE(fw::is_sender<decltype(fw::exec::just(1))>);
    }
    SECTION("inline_scheduler is a scheduler") {
        STATIC_REQUIRE(fw::is_scheduler<fw::exec::inline_scheduler>);
    }
}

// Phase 2 Northstar: Diagnostic Toolkit --------------------------------------
//
// The toolkit must (a) pass cleanly on well-formed senders, and (b) when
// invoked deliberately on a known sender, produce a compiler diagnostic
// that embeds the completion signatures verbatim. We can only exercise
// (a) in a passing build; (b) is verified by uncommenting the marked
// compile-fail probe below and inspecting the resulting error.

TEST_CASE("Phase 2 diagnostic toolkit", "[exec][phase2][diag]") {
    using just_sender = decltype(fw::exec::just(42));

    SECTION("is_sender_in_v accepts a well-formed sender") {
        STATIC_REQUIRE(fw::diag::is_sender_in_v<just_sender>);
    }

    SECTION("has_signatures_v matches expected set_value_t(int)") {
        using expected = fw::exec::completion_signatures<
            fw::exec::set_value_t(int)>;
        STATIC_REQUIRE(fw::diag::has_signatures_v<just_sender, expected>);
    }
}

// Phase 3 prereq: static_thread_pool scheduler concept + actual execution --
TEST_CASE("static_thread_pool scheduler", "[exec][phase3][pool]") {
    fw::exec::static_thread_pool pool(2);
    auto sch = pool.get_scheduler();

    SECTION("get_scheduler returns a scheduler") {
        STATIC_REQUIRE(fw::is_scheduler<decltype(sch)>);
    }

    SECTION("schedule(sch) returns a sender") {
        STATIC_REQUIRE(fw::is_sender<decltype(fw::exec::schedule(sch))>);
    }

    SECTION("body of then runs on a worker, not the main thread") {
        auto main_id = ::std::this_thread::get_id();
        ::std::thread::id worker_id{};

        auto work = fw::exec::schedule(sch)
                  | fw::exec::then([&] {
                        worker_id = ::std::this_thread::get_id();
                        return 7;
                    });

        auto [v] = fw::exec::sync_wait(::std::move(work)).value();
        REQUIRE(v == 7);
        REQUIRE(worker_id != ::std::thread::id{});
        REQUIRE(worker_id != main_id);
    }
}

// Phase 3 Northstar Test 2: Linear Execution Graph --------------------------
TEST_CASE("Linear Execution Graph", "[exec][linear]") {
    fw::exec::static_thread_pool pool(4);
    auto sch = pool.get_scheduler();

    auto work = fw::exec::just(21)
              | fw::exec::transfer(sch)
              | fw::exec::then([](int i) { return i * 2; });

    auto [result] = fw::exec::sync_wait(::std::move(work)).value();
    REQUIRE(result == 42);
}

// Phase 3 robustness: value forwarding across multiple thens + error path ---
TEST_CASE("Linear chain: multi-then value forwarding", "[exec][linear]") {
    fw::exec::static_thread_pool pool(2);
    auto sch = pool.get_scheduler();

    auto work = fw::exec::just(1)
              | fw::exec::transfer(sch)
              | fw::exec::then([](int i) { return i + 10; })
              | fw::exec::then([](int i) { return i * 3; })
              | fw::exec::then([](int i) { return i - 2; });

    auto [result] = fw::exec::sync_wait(::std::move(work)).value();
    REQUIRE(result == 31);  // ((1 + 10) * 3) - 2
}

TEST_CASE("Linear chain: thrown exception surfaces via sync_wait", "[exec][linear][error]") {
    fw::exec::static_thread_pool pool(2);
    auto sch = pool.get_scheduler();

    auto work = fw::exec::just(0)
              | fw::exec::transfer(sch)
              | fw::exec::then([](int) -> int { throw ::std::runtime_error{"boom"}; });

    REQUIRE_THROWS_AS(fw::exec::sync_wait(::std::move(work)), ::std::runtime_error);
}

// Phase 4 Northstar Test 3: Cancellation Propagation via let_value ----------
//
// Deviations from the directive's literal text, both documented:
//   * `std::inplace_stop_source` → `fw::exec::inplace_stop_source`
//     (libstdc++ has not yet shipped P2300 stop tokens; the facade is the
//     intended migration path until it does)
//   * `upon_stopped([&](){ ... })` → `upon_stopped([&]() -> int { ...; return 0; })`
//     The inner sender's set_value(int) signature must unify with
//     upon_stopped's translation of set_stopped for `sync_wait` to deduce
//     a single value-tuple type. The cancellation-propagation semantics
//     under test are unaffected.
TEST_CASE("Cancellation Propagation via let_value", "[exec][cancel]") {
    fw::exec::inplace_stop_source stop_source;
    bool was_cancelled = false;

    auto work = fw::exec::just()
        | fw::exec::let_value([&]() {
            stop_source.request_stop();
            return fw::exec::just()
                 | fw::exec::then([]() { return 1; });
        })
        | fw::exec::upon_stopped([&]() -> int {
            was_cancelled = true;
            return 0;
        });

    auto stoppable_work = fw::exec::write_env(
        ::std::move(work),
        fw::exec::prop{fw::exec::get_stop_token, stop_source.get_token()});

    fw::exec::sync_wait(::std::move(stoppable_work));
    REQUIRE(was_cancelled == true);
}

// Phase 4 Northstar Test 4: When_All Memory Lifecycle -----------------------
TEST_CASE("When_All Memory Lifecycle", "[exec][concurrency]") {
    ::std::atomic<int> active_states{0};

    auto branch1 = my_domain::tracking_sender(active_states, 10);
    auto branch2 = my_domain::tracking_sender(active_states, 20);

    auto graph = fw::exec::when_all(::std::move(branch1), ::std::move(branch2))
               | fw::exec::then([](int a, int b) { return a + b; });

    auto [result] = fw::exec::sync_wait(::std::move(graph)).value();

    REQUIRE(result == 30);
    REQUIRE(active_states.load() == 0);
}

// ---------------------------------------------------------------------------
// Manual compile-fail probe. Uncomment to verify the toolkit prints
// completion signatures in the diagnostic. Expected behaviour: the build
// fails with a message naming `fw::diag::display<completion_signatures<...>>`
// as an incomplete type.
//
//   using PROBE = fw::diag::dump_signatures_t<decltype(fw::exec::just(42))>;
//   inline constexpr auto force_instantiation = sizeof(PROBE);
// ---------------------------------------------------------------------------
