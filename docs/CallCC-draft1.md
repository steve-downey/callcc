# Agent Directive: P2300 Facade Implementation[cite: 1, 2]

This document contains the structural prompts, grounding instructions, and Northstar test suite designed to guide an LLM agent (Claude/Codex) through building a robust domain adaptation layer for C++26 asynchronous execution.[cite: 1, 2]

> **[INSTRUCTION TO AGENT] Primary Persona & Mission**[cite: 1, 2]
> You are an Expert C++ Systems Engineer tasked with implementing an asynchronous execution architecture.[cite: 1, 2] You will be bridging standard C++26 P2300 signatures with a reference implementation (`beman::execution` or `stdexec`).[cite: 1, 2] Your code must act as a strict Metaprogramming Firewall.[cite: 1, 2] You will prioritize correct compilation, lifecycle safety, and strict alignment with WG21 draft standards to ensure forwards-compatibility.[cite: 1, 2]

## 1. Grounding Instructions & Guardrails[cite: 1, 2]

Provide the following constraints to the agent to prevent hallucinations and architectural drift:[cite: 1, 2]

* **Strict Namespace Isolation:** Never include `<beman/execution.hpp>` or `<stdexec/execution.hpp>` outside of the single facade header (`execution_fw.hpp`).[cite: 1, 2] All business logic must use the `fw::exec::` namespace alias.[cite: 1, 2]
* **CPO Hallucination Prevention:** Do not invent Customization Point Objects.[cite: 1, 2] If a standard CPO (like `let_value` or `bulk`) fails to compile, do not write a custom workaround immediately.[cite: 1, 2] First, explicitly define the `get_env` queries for the sender, and utilize `fw::exec::completion_signatures_of_t` to diagnose the divergence.[cite: 1, 2]
* **Build System Constraints:** Assume the environment utilizes CMake.[cite: 1, 2] Because completion signatures generate massive symbol sizes, you must explicitly instruct CMake to use high-concurrency linkers (`mold` or `lld`).[cite: 1, 2] Standard `ld` will cause unacceptable latency during the iterative testing cycle.[cite: 1, 2]
* **ADL Protection:** Define all custom Senders, Receivers, and Operation States in a dedicated internal namespace to prevent Argument-Dependent Lookup (ADL) from accidentally pulling in implementation-specific overloads.[cite: 1, 2]

## 2. Iterative Challenge Roadmap[cite: 1, 2]

Guide the agent to execute the implementation in the following discrete phases.[cite: 1, 2] Do not allow the agent to proceed to the next phase until the tests for the current phase pass.[cite: 1, 2]

* **Phase 1: The Facade Header.** Establish `execution_fw.hpp`.[cite: 1, 2] Conditionally compile the aliases for the underlying library.[cite: 1, 2] Re-export core concepts (`sender`, `scheduler`, `receiver`) into the `fw::` namespace.[cite: 1, 2]
* **Phase 2: The Diagnostic Toolkit.** Implement static assertion helpers to force the compiler to output completion signatures on failure.[cite: 1, 2] This is mandatory before writing complex chains.[cite: 1, 2]
* **Phase 3: Linear Chains.** Implement and test basic linear workflows: `just() | transfer() | then() | sync_wait()`.[cite: 1, 2]
* **Phase 4: Structured Concurrency & Cancellation.** Implement branched execution using `when_all`.[cite: 1, 2] Inject an `in_place_stop_source` and verify that `set_stopped()` propagates correctly through a `let_value` expansion without triggering `set_error()` or leaking operation states.[cite: 1, 2]

## 3. Northstar Test Suite (User-Facing API)[cite: 1, 2]

Provide these exact tests to the agent.[cite: 1, 2] The agent's generated facade and implementation MUST make this code compile and pass.[cite: 1, 2] This defines the target end-state API.[cite: 1, 2]

> **Agent Instruction:** Do not modify the Northstar tests.[cite: 1, 2] Your implementation must conform to this API.[cite: 1, 2] Use Catch2 or GTest for assertions.[cite: 1, 2]

### Test 1: Concept Verification & Re-exporting[cite: 1, 2]
```cpp
// Ensures the facade correctly isolates concepts from the implementation
static_assert(fw::is_sender<decltype(fw::exec::just(42))>);
static_assert(fw::is_scheduler<fw::exec::inline_scheduler>);

// Diagnostic trigger: if a custom sender fails, the agent must use this
// to print the generated signatures during compilation debugging.
template <typename Sender, typename Env = fw::exec::empty_env>
using debug_sigs = fw::exec::completion_signatures_of_t<Sender, Env>;
```

### Test 2: Linear Execution and Context Switching[cite: 1, 2]
```cpp
TEST_CASE("Linear Execution Graph", "[exec][linear]") {
    fw::exec::static_thread_pool pool(4);
    auto sch = pool.get_scheduler();

    auto work = fw::exec::just(21)
              | fw::exec::transfer(sch)
              | fw::exec::then([](int i) { return i * 2; });

    auto [result] = fw::exec::sync_wait(std::move(work)).value();
    REQUIRE(result == 42);
}
```

### Test 3: Cancellation Propagation (Crucial for stdexec/beman divergence)[cite: 1, 2]
```cpp
TEST_CASE("Cancellation Propagation via let_value", "[exec][cancel]") {
    std::inplace_stop_source stop_source;
    bool was_cancelled = false;

    auto work = fw::exec::just()
        | fw::exec::let_value([&]() {
            stop_source.request_stop(); // Trigger cancellation midway
            return fw::exec::just() 
                 | fw::exec::then([](){ return 1; });
        })
        | fw::exec::upon_stopped([&]() {
            was_cancelled = true;
        });

    // Attach the stop token to the execution environment
    auto stoppable_work = fw::exec::write_env(
        std::move(work),
        fw::exec::prop{fw::exec::get_stop_token, stop_source.get_token()}
    );

    fw::exec::sync_wait(std::move(stoppable_work));
    REQUIRE(was_cancelled == true);
}
```

### Test 4: Structured Concurrency & Operation State Lifecycles[cite: 1, 2]
```cpp
TEST_CASE("When_All Memory Lifecycle", "[exec][concurrency]") {
    std::atomic<int> active_states{0};

    // Agent must implement `tracking_sender` to increment/decrement `active_states`
    // in its operation_state constructor/destructor.
    auto branch1 = my_domain::tracking_sender(active_states, 10);
    auto branch2 = my_domain::tracking_sender(active_states, 20);

    auto graph = fw::exec::when_all(std::move(branch1), std::move(branch2))
               | fw::exec::then([](int a, int b) { return a + b; });

    auto [result] = fw::exec::sync_wait(std::move(graph)).value();
    
    REQUIRE(result == 30);
    // Critical: Ensure no operation_state objects leaked after sync_wait returns.
    REQUIRE(active_states.load() == 0); 
}
```

````</Sender,>