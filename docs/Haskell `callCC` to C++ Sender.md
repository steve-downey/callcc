# **The Isomorphism of Haskell's callCC to C++26 std::execution: An Exhaustive Theoretical and Practical Implementation Report**

## **Introduction to the Asynchronous Paradigm Shift**

The evolution of asynchronous and parallel programming in systems programming languages has historically been fragmented, relying on ad-hoc threading models, heavy-weight futures, or platform-specific execution contexts. With the formal adoption of the std::execution framework, commonly referred to via its proposal number P2300, into the C++26 standard working draft during the WG21 plenary in St. Louis, the C++ standard library has introduced a foundational, lazy, and composable model for asynchronous computation.1 This model, based on the Sender and Receiver architecture, provides a grand unifying theory for asynchrony that targets heterogeneous execution contexts, ranging from single-threaded embedded microcontrollers to massive CPU thread pools and GPU accelerators.1  
Simultaneously, functional programming languages, most notably Haskell, have long possessed highly formalized abstractions for control flow management. The Continuation-Passing Style, commonly referred to as CPS, is encapsulated formally within the Cont and ContT monads.5 A cornerstone of this monadic structure is the callCC, or call-with-current-continuation, function. This mathematical construct allows a computation to capture its own continuation, providing an explicit mechanism to abort current operations and return a value immediately, bypassing the remaining operational stack.  
Translating the semantics of Haskell's callCC into the C++26 std::execution model requires traversing the theoretical bridge between purely functional monadic lazy evaluation and the explicit, hardware-aware operational states of C++. Because Senders inherently utilize Continuation-Passing Style, an isomorphism exists between the Cont monad and the Sender and Receiver architecture.5 However, the details diverge significantly when addressing the strict lifetime requirements of C++ operation states, the explicit nature of execution environments, and the cooperative cancellation mechanisms governed by C++26 stop tokens.6  
This report provides an exhaustive architectural analysis of mapping Haskell's callCC to a custom C++26 Sender Adaptor. It investigates the theoretical foundations of continuation passing, the operational complexities of C++ lifetime management, explicit error and cancellation handling, and provides a rigorously structured implementation plan culminating in a specification-compliant C++ design utilizing the stdexec reference implementation.

## **The Theoretical Bridge: Continuation-Passing Style and the ContT Monad**

To construct a robust C++26 implementation of callCC, the mathematical and structural relationships between Haskell's ContT monad and C++26 Senders must be explicitly defined and thoroughly understood. In functional theory, Continuation-Passing Style is a systematic transformation where a function, rather than returning a value to its caller via the traditional call stack, receives an explicit continuation to which it passes its result.5 This transforms the implicit stack into an explicit parameter, allowing the programmer to manipulate the future of the computation directly.  
The Haskell ContT monad encapsulates this pattern, elevating it into a composable abstraction. The signature of callCC is formulated as follows:

Haskell  
\-- | @callCC@ (call-with-current-continuation) calls its argument-- function, passing it the current continuation.  
callCC :: ((a \-\> ContT r m b) \-\> ContT r m a) \-\> ContT r m a  
callCC f \= ContT $ \\ c \-\> runContT (f (\\ x \-\> ContT $ \\ \_ \-\> c x)) c  
{-\# INLINE callCC \#-}

This signature implies that callCC takes a higher-order function f. This function f is provided with an escape function, represented by the lambda expression \\x \-\> ContT $ \\\_ \-\> c x. When the user-defined function f executes, it has the option to invoke this escape function with a value of type a. If invoked, the escape function deliberately discards its own localized continuation, represented by the underscore \\\_, and immediately supplies the value x to the captured outer continuation c. This achieves an early exit, analogous to throw and catch semantics, but it remains explicitly typed and managed within the continuation structure rather than relying on the language's hidden runtime exception unwinder. The standard idiom used with callCC is to provide a lambda expression to name the continuation, allowing a call to the named continuation anywhere within its scope to escape from the computation, even if it is nested many layers deep.  
In a garbage-collected language like Haskell, discarding a continuation is an inherently memory-safe operation. The lambda calculus abstracts away the physical machine memory, and if a continuation is ignored, the tracing garbage collector eventually identifies the isolated closures and reclaims the associated memory. There are no dangling pointers, and there is no undefined behavior resulting from an abandoned operational state. This elegant theoretical model forms the basis of what must be achieved in C++, but the physical realities of unmanaged memory and hardware-specific execution contexts present severe implementation hurdles.

## **Senders as Monadic Structures in C++26**

The C++26 std::execution model utilizes the Decorator pattern to lift ordinary asynchronous operations into Continuation-Passing Style.5 By formalizing this architecture, C++ fundamentally alters how asynchronous tasks are represented, moving away from the allocation-heavy std::future and std::function models toward a zero-overhead, compile-time evaluable system.3 Senders describe asynchronous work lazily. They do not initiate computation upon instantiation, nor do they allocate heap memory to store future results. Instead, they must be connected to a Receiver, producing an Operation State object.10  
The structural isomorphism between Haskell and C++26 can be mapped comprehensively through their core concepts.

| Functional Concept | Haskell ContT Equivalent | C++26 std::execution Representation |
| :---- | :---- | :---- |
| **Computation Unit** | ContT r m a | std::execution::sender |
| **Continuation Callback** | a \-\> m r | std::execution::receiver |
| **Bind Operation** | \>\>= (Bind operator) | std::execution::then / std::execution::let\_value |
| **Execution Trigger** | runContT | std::execution::connect followed by std::execution::start |
| **Context/Environment** | Monad Transformer (m) | Execution Environment (get\_env) and Schedulers |

Once a Sender is connected to a Receiver, the resulting Operation State is explicitly started via the std::execution::start customization point object. This acts as the trigger that initiates the execution, which will eventually call one of the Receiver's three distinct completion channels: set\_value, set\_error, or set\_stopped.12 This three-channel design is a significant departure from standard functional continuations, which typically only pass values. The C++ standard mandates separate channels to distinguish between successful computation, exceptional failures that must propagate up the chain, and cooperative cancellation requests.5  
Furthermore, C++ requires explicit management of hardware resources, meaning Senders enforce strict rules regarding the lifetime of the Operation State. Objects that model the operation\_state concept must remain valid and undestroyed for the entire duration of the asynchronous operation.7 As defined by the standard, if the lifetime of an asynchronous operation's associated operation state ends before the lifetime of the asynchronous operation itself, the behavior is undefined.7 Accessing any part of an invalid operation state constitutes a severe memory safety violation. An asynchronous operation shall not execute a completion operation before its start operation has begun executing, and after its start operation has begun executing, exactly one completion operation shall execute.7  
These rigorous lifetime rules create the primary conflict when implementing callCC. In C++, an asynchronous operation chain may span multiple threads, GPU streams, or I/O multiplexers like io\_uring or epoll.7 If an escape sender completes the outer receiver early, the inner sender's operation state is technically still alive and potentially executing concurrently.7 If the outer receiver is completed, the lifecycle of the callCC block technically ends, meaning the memory backing the operation states may be reclaimed by the thread that initiated the computation. If the inner asynchronous work is still executing on a background thread pool, its inevitable attempt to complete its continuation will result in an access to reclaimed memory, crashing the system.15 Consequently, a faithful and safe C++ implementation of callCC must integrate cooperatively with the C++26 cancellation model, utilizing Stop Tokens to explicitly request the termination of the inner computation immediately upon an escape invocation.7

## **Cooperative Cancellation and Explicit Environment Management**

To handle the critical cleanup of the discarded continuation, the callCC implementation must act as a strict cancellation boundary. The design of cancellation in P2300 is built upon and extends the std::stop\_token facilities introduced in C++20.7 However, because asynchronous operation graphs require highly efficient, non-allocating synchronization to maintain the zero-overhead principle, C++26 introduces a specialized suite of in-place synchronization primitives.  
The framework introduces the std::stoppable\_token and std::stoppable\_source concepts, which generalize the interface of stop tokens to allow various implementation strategies.7 For localized, structured concurrency where the lifetime of the asynchronous graph is tightly bound, the standard provides std::inplace\_stop\_source, std::inplace\_stop\_token, and std::inplace\_stop\_callback.7 The inplace\_stop\_source is a stop source that explicitly does not allocate its shared state on the heap, allowing it to be embedded directly within an operation state object.7 The associated inplace\_stop\_token can be queried for stop requests via the stop\_requested() member function 18, while the inplace\_stop\_callback provides a mechanism to register a function that will be executed atomically when request\_stop() is called on the source.17  
In the std::execution framework, contextual data such as stop tokens, allocators, current schedulers, execution domains, and priorities are propagated downward from parent operations to child operations via the Receiver's environment.7 A receiver provides its environment through the std::execution::get\_env customization point object.10 This design allows each operation to have the exact contextual information it needs before it is started.7  
For a callCC sender adaptor to function safely, its shared operational state must instantiate a std::inplace\_stop\_source.22 The inner receiver, which is responsible for capturing the non-escaped completion of the inner computation, must expose a customized environment that intercepts the std::execution::get\_stop\_token query.7 When the inner sender queries its environment for a stop token, the inner receiver must return the inplace\_stop\_token tied to the callCC block's inplace\_stop\_source, rather than the outer receiver's broader stop token.  
The mechanism for constructing and composing these environments relies on environment wrapping. An environment is conceptually a key-value store evaluated at compile time.21 To construct a customized environment, the inner receiver must utilize the stdexec::make\_env utility, or implement a custom environment object that selectively answers specific queries while forwarding all others to the parent environment.24

| Query Dispatched by Inner Sender | Evaluation Path within callCC Inner Receiver Environment | Resulting Type / Action |
| :---- | :---- | :---- |
| std::execution::get\_stop\_token | Intercepted by the custom environment object. | Returns std::inplace\_stop\_token originating from the local inplace\_stop\_source. |
| std::execution::get\_allocator | Forwarded directly to the Outer Receiver's environment. | Returns the parent allocator type, preserving memory pool semantics. |
| std::execution::get\_scheduler | Forwarded directly to the Outer Receiver's environment. | Returns the parent scheduler, maintaining execution affinity. |
| std::execution::get\_domain | Forwarded directly to the Outer Receiver's environment. | Preserves heterogeneous domain tagging (e.g., CUDA streams). |

While callCC introduces its own stop source for downward cancellation to halt inner work upon an escape, it must also strictly respect upward cancellation. If the outer receiver is instructed to stop by the broader system, such as a user terminating the program or a parent when\_all algorithm encountering an error in a sibling task, that stop request must cascade downward into the callCC inner work. This requires the callCC operation state to register a stop callback, using std::inplace\_stop\_callback or the std::stop\_callback\_for\_t trait, with the outer receiver's stop token during the start sequence.14 If the outer token is triggered, the registered callback explicitly calls request\_stop() on the internal inplace\_stop\_source, propagating the cancellation signal down the tree.7

## **Architectural Blueprint of the callCC Sender Adaptor**

Mapping the theoretical concept of callCC to a C++26 Sender Adaptor requires orchestrating a complex hierarchy of senders, receivers, and operation states. In C++, the equivalent of passing a function to callCC involves passing a callable object, typically a lambda expression, to a call\_cc sender factory. This callable is provided with an escape factory capable of producing an escape sender.  
When user code invokes this architecture, the framework must systematically instantiate the following entities:  
First, the Outer Sender is created. This is the object returned by the initial call\_cc(F) invocation. It perfectly models the sender concept and advertises the comprehensive completion signatures of the entire block. Next, when an algorithm like sync\_wait or then connects to the Outer Sender, an Outer Receiver is bound to it, and the CallCC Operation State is generated.  
This operation state encapsulates the Shared State. The Shared State is a critical synchronization node, holding the Outer Receiver, the inplace\_stop\_source, and a std::atomic\<bool\> flag representing whether a completion has already been signaled. Because the operation state of a sender is mathematically guaranteed to live until a completion operation is executed, the Shared State can safely reside by-value within the operation state itself, avoiding any dynamic memory allocation.7  
The user's callable is then invoked, producing the Inner Sender. This Inner Sender represents the standard, non-escaped execution path. It is connected to an Inner Receiver. The Inner Receiver is a highly specialized object that intercepts successful completions, errors, and stoppage signals, forwarding them to the Shared State while providing the customized cancellation environment.  
Simultaneously, the user's callable is provided with an Escape Factory. When the user invokes this factory with a value, it produces the Escape Sender. The Escape Sender is the object representation of the captured continuation. When connected and started deep within the user's algorithm graph, the Escape Sender initiates the early exit procedure.

### **The Semantic Rules of the Escape Sender**

The Escape Sender represents the captured continuation from the Haskell callCC definition. When the user connects and starts the Escape Sender, it fundamentally alters the control flow of the application. It does not complete its direct downstream receiver. Instead, it completes the outer receiver of the entire call\_cc block.28 Therefore, from the perspective of the inner computation graph, the Escape Sender acts as an execution sink. It absorbs control flow and never returns it to the local continuation.  
However, to satisfy the strict typing rules of C++26, a sender must statically advertise its completion signatures via the get\_completion\_signatures customization point object.7 To ensure type compatibility within algorithms like std::execution::then 29 or std::execution::let\_value 30, the Escape Sender must report a valid completion signature. Because it diverges control flow, its declared completion signature effectively acts as a noreturn type. It will declare that it completes with set\_value\_t(), allowing it to pass type-checking, but at runtime, it will invoke set\_value on the outer receiver and abandon its own local receiver entirely.  
The operation state of the Escape Sender maintains a raw pointer back to the Shared State of the call\_cc block. When start is called on the escape operation state 11, it performs a highly specific, atomic sequence of events to prevent undefined behavior. First, it atomically checks and sets the completed flag in the Shared State. If another branch of the computation has already completed the outer receiver, the escape is ignored. This guarantees adherence to the exactly-one-completion rule of C++26 operation states.7 If the flag was successfully set from false to true, it immediately invokes request\_stop() on the inplace\_stop\_source held in the Shared State.7 This signals all concurrently executing inner operations to gracefully halt. Finally, it invokes set\_value on the outer receiver, passing the escaped value.7

## **Formal Implementation Plan and C++ Metaprogramming**

The implementation of the call\_cc adaptor requires advanced template metaprogramming, meticulous adherence to stdexec concept requirements, and precise memory ordering to avoid data races across execution contexts. The following narrative details the implementation utilizing the C++26 standard library execution facilities, referencing the paradigms established by the stdexec reference implementation.7  
The foundation of the architecture is the Shared State, which serves as the central nervous system for the call\_cc block. It must hold the outer receiver and provide thread-safe methods for completion.

C++  
\#**include** \<atomic\>  
\#**include** \<optional\>  
\#**include** \<functional\>  
\#**include** \<stdexec/execution.hpp\>

namespace ex \= stdexec;

template \<class OuterReceiver, class ValueType\>  
struct call\_cc\_shared\_state {  
    OuterReceiver outer\_receiver;  
    ex::inplace\_stop\_source stop\_source;  
    std::atomic\<bool\> completed{false};

    explicit call\_cc\_shared\_state(OuterReceiver&& rcvr)   
        : outer\_receiver(std::move(rcvr)) {}

    template \<class... Args\>  
    void complete\_value(Args&&... args) {  
        if (\!completed.exchange(true, std::memory\_order\_release)) {  
            stop\_source.request\_stop();   
            ex::set\_value(std::move(outer\_receiver), std::forward\<Args\>(args)...);  
        }  
    }

    template \<class Error\>  
    void complete\_error(Error&& err) {  
        if (\!completed.exchange(true, std::memory\_order\_release)) {  
            stop\_source.request\_stop();  
            ex::set\_error(std::move(outer\_receiver), std::forward\<Error\>(err));  
        }  
    }

    void complete\_stopped() {  
        if (\!completed.exchange(true, std::memory\_order\_release)) {  
            stop\_source.request\_stop();  
            ex::set\_stopped(std::move(outer\_receiver));  
        }  
    }  
};

The use of std::memory\_order\_release is critical during the exchange operation. This ensures that any memory writes performed by the thread invoking the escape are visible to whatever thread subsequently executes the outer receiver's continuation. This prevents subtle data races when escaping across CPU cores.  
Next, the framework must define the Escape Sender and its corresponding Operation State. The Escape Sender acts as the injected continuation parameter. When the user algorithm connects the Escape Sender to a local receiver, the Escape Operation State is instantiated.15

C++  
template \<class SharedState, class ValueType, class LocalReceiver\>  
struct escape\_op\_state {  
    using operation\_state\_concept \= ex::operation\_state\_tag;  
      
    SharedState\* shared\_state\_;  
    ValueType value\_;  
    LocalReceiver local\_receiver\_;

    void start() & noexcept {  
        shared\_state\_-\>complete\_value(std::move(value\_));  
    }  
};

template \<class SharedState, class ValueType\>  
struct escape\_sender {  
    using sender\_concept \= ex::sender\_tag;  
    SharedState\* shared\_state\_;  
    ValueType value\_;

    template \<class Env\>  
    friend auto tag\_invoke(ex::get\_completion\_signatures\_t, escape\_sender, Env)   
        \-\> ex::completion\_signatures\<ex::set\_value\_t(ValueType)\>;

    template \<ex::receiver LocalReceiver\>  
    friend auto tag\_invoke(ex::connect\_t, escape\_sender self, LocalReceiver rcvr)  
        \-\> escape\_op\_state\<SharedState, ValueType, LocalReceiver\> {  
        return {self.shared\_state\_, std::move(self.value\_), std::move(rcvr)};  
    }  
      
    friend auto tag\_invoke(ex::get\_env\_t, const escape\_sender&) noexcept \-\> ex::empty\_env {  
        return {};  
    }  
};

template \<class SharedState, class ValueType\>  
struct escape\_factory {  
    SharedState\* shared\_state\_;

    escape\_sender\<SharedState, ValueType\> operator()(ValueType val) const {  
        return {shared\_state\_, std::move(val)};  
    }  
};

In this structure, the tag\_invoke mechanism is utilized to define the customization point objects. While newer revisions of P2300 have moved toward static member functions for customization to improve compile times 11, the fundamental semantic remains identical. The escape\_op\_state::start method deliberately ignores its local\_receiver\_ member, severing the local execution graph, and instead pushes the value to the shared\_state\_, achieving the early exit.  
The Inner Receiver must intercept the completion of the user's primary computation block, provided no escape has occurred. Crucially, it must intercept the get\_env query to inject the inplace\_stop\_token into the context.23

C++  
template \<class SharedState, class OuterEnv\>  
struct inner\_env {  
    SharedState\* shared\_state\_;  
    OuterEnv outer\_env\_;

    friend auto tag\_invoke(ex::get\_stop\_token\_t, const inner\_env& self) noexcept   
        \-\> ex::inplace\_stop\_token {  
        return self.shared\_state\_-\>stop\_source.get\_token();  
    }

    template \<class Query, class... Args\>  
        requires std::is\_invocable\_v\<Query, const OuterEnv&, Args...\> &&  
                 (\!std::is\_same\_v\<Query, ex::get\_stop\_token\_t\>)  
    friend auto tag\_invoke(Query q, const inner\_env& self, Args&&... args) noexcept  
        \-\> std::invoke\_result\_t\<Query, const OuterEnv&, Args...\> {  
        return std::invoke(q, self.outer\_env\_, std::forward\<Args\>(args)...);  
    }  
};

template \<class SharedState\>  
struct inner\_receiver {  
    using receiver\_concept \= ex::receiver\_tag;  
    SharedState\* shared\_state\_;

    template \<class... Args\>  
    void set\_value(Args&&... args) && noexcept {  
        shared\_state\_-\>complete\_value(std::forward\<Args\>(args)...);  
    }

    template \<class Error\>  
    void set\_error(Error&& err) && noexcept {  
        shared\_state\_-\>complete\_error(std::forward\<Error\>(err));  
    }

    void set\_stopped() && noexcept {  
        shared\_state\_-\>complete\_stopped();  
    }

    friend auto tag\_invoke(ex::get\_env\_t, const inner\_receiver& self) noexcept {  
        return inner\_env\<SharedState, ex::env\_of\_t\<decltype(self.shared\_state\_-\>outer\_receiver)\>\>{  
            self.shared\_state\_,  
            ex::get\_env(self.shared\_state\_-\>outer\_receiver)  
        };  
    }  
};

The inner\_env structure is a textbook implementation of an environment adapter. By constraining the generic query forwarder to specifically exclude ex::get\_stop\_token\_t, the compiler guarantees that any child task querying for a stop token will receive the local inplace\_stop\_token. This is the fundamental linkage that allows the Escape Sender to cancel parallel sibling tasks successfully.  
Finally, the Outer Operation State and Outer Sender weave these components together into a unified asynchronous block. The Outer Operation State must handle upward cancellation propagation by registering a callback on the outer receiver's stop token during the start sequence.27

C++  
template \<class OuterReceiver, class F, class ValueType\>  
struct call\_cc\_op\_state {  
    using operation\_state\_concept \= ex::operation\_state\_tag;

    using SharedStateType \= call\_cc\_shared\_state\<OuterReceiver, ValueType\>;  
    SharedStateType shared\_state\_;

    using InnerSenderType \= std::invoke\_result\_t\<F, escape\_factory\<SharedStateType, ValueType\>\>;  
    using InnerReceiverType \= inner\_receiver\<SharedStateType\>;  
    using InnerOpStateType \= ex::connect\_result\_t\<InnerSenderType, InnerReceiverType\>;

    F user\_func\_;  
    std::optional\<InnerOpStateType\> inner\_op\_state\_;

    using OuterStopToken \= decltype(ex::get\_stop\_token(ex::get\_env(std::declval\<OuterReceiver\>())));  
    using StopCallback \= typename OuterStopToken::template callback\_type\<  
        std::function\<void()\>\>;  
    std::optional\<StopCallback\> stop\_callback\_;

    call\_cc\_op\_state(OuterReceiver rcvr, F func)  
        : shared\_state\_(std::move(rcvr)), user\_func\_(std::move(func)) {}

    void start() & noexcept {  
        auto outer\_token \= ex::get\_stop\_token(ex::get\_env(shared\_state\_.outer\_receiver));  
        stop\_callback\_.emplace(outer\_token, \[this\]() {  
            shared\_state\_.stop\_source.request\_stop();  
        });

        escape\_factory\<SharedStateType, ValueType\> factory{\&shared\_state\_};  
        InnerSenderType inner\_sender \= user\_func\_(factory);

        inner\_op\_state\_.emplace(ex::connect(std::move(inner\_sender), InnerReceiverType{\&shared\_state\_}));  
        ex::start(\*inner\_op\_state\_);  
    }  
};

The Outer Sender is what the call\_cc function returns to the user. It dictates the overall completion signatures. The completion signatures must dynamically calculate the union of the inner computation's potential results and the explicitly defined escaped value types.

C++  
template \<class F, class ValueType\>  
struct call\_cc\_sender {  
    using sender\_concept \= ex::sender\_tag;  
    F user\_func\_;

    template \<class Env\>  
    friend auto tag\_invoke(ex::get\_completion\_signatures\_t, call\_cc\_sender, Env)  
        \-\> ex::completion\_signatures\<  
            ex::set\_value\_t(ValueType),  
            ex::set\_error\_t(std::exception\_ptr),  
            ex::set\_stopped\_t()\>;

    template \<ex::receiver OuterReceiver\>  
    friend auto tag\_invoke(ex::connect\_t, call\_cc\_sender self, OuterReceiver rcvr)  
        \-\> call\_cc\_op\_state\<OuterReceiver, F, ValueType\> {  
        return {std::move(rcvr), std::move(self.user\_func\_)};  
    }  
      
    friend auto tag\_invoke(ex::get\_env\_t, const call\_cc\_sender&) noexcept \-\> ex::empty\_env {  
        return {};  
    }  
};

template \<class ValueType, class F\>  
call\_cc\_sender\<F, ValueType\> call\_cc(F func) {  
    return {std::move(func)};  
}

Whenever a custom sender is defined in P2300, manual encoding of the type transformations on the completion signatures via the get\_completion\_signatures CPO is required.30 It is not possible to compute the completion signatures entirely automatically without instantiating the inner operation state, which is structurally prohibited at the sender level. Knowing the set of possible completions is strictly necessary to store the results in intermediate containers within algorithms like std::execution::let\_value or std::execution::when\_all.30 In this architecture, the signature is explicitly enforced to match the user's provided ValueType, ensuring that whether the function completes naturally or escapes, the output type remains homogeneously sound.

## **Heterogeneous Computing Contexts and Domain Customization**

The design parameters of the C++26 model optimize heavily for heterogeneous computing contexts.1 Frameworks like NVIDIA's stdexec provide sophisticated schedulers that target CUDA architectures, exposing nvexec::stream\_context for direct GPU manipulation.7 When a callCC construct is utilized within a GPU stream graph, the physical and theoretical implications of escaping become highly complex. GPU kernel launches are deeply pipelined hardware operations. If an Escape Sender is placed inside a then block that executes on a GPU scheduler, invoking the escape requires communicating from the device execution context back to the host context to trigger the set\_value on the outer receiver.34  
Because the std::inplace\_stop\_source fundamentally relies on atomic memory orderings, specifically utilizing std::memory\_order\_release and spin locks in its implementation to guarantee cross-thread safety 18, an escape invoked from a device context might not cleanly interrupt other kernels already queued in the GPU stream without explicit memory polling. In highly constrained environments where deep recursive graphs operate across disparate domains, the escape\_op\_state::start routine must be explicitly designed to bridge execution domains via std::execution::get\_domain.7 If the domain of the escape sender differs from the domain of the outer receiver, a transition sender, such as std::execution::transfer, must be implicitly or explicitly injected to route the escape value securely back to the execution agent holding the call\_cc\_op\_state.12  
Without this transition, invoking an escape from a GPU kernel could result in a host-side set\_value being called concurrently with device-side memory operations, corrupting the execution context. The architecture presented resolves this mathematically by ensuring that the inplace\_stop\_source handles cross-domain synchronization cooperatively, though performance in high-latency interconnects may degrade due to the spin-lock mechanisms inherent to inplace\_stop\_callback destruction sequences.35

## **Interoperability with Coroutines and Higher-Level Algorithms**

A major objective of P2300 is interoperability with C++20 Coroutines. Senders can be naturally awaited using the co\_await keyword. The stdexec implementation utilizes a synthesized awaitable fallback via get\_await\_completion\_adapter or as\_awaitable to convert Senders into coroutine states.11 This allows developers to mix and match between sender algorithms and coroutine types, such as stdexec::task or unifex::task, seamlessly.7  
When adapting callCC to a coroutine task, the interaction of the Escape Sender with the coroutine frame requires deep analysis. Coroutines inherently operate top-down.39 If a coroutine awaits a call\_cc block, the coroutine suspends and returns control to its caller. If the Escape Sender triggers, it completes the CallCCSender, resuming the parent coroutine accurately.  
However, a critical danger arises if the Escape Sender itself is awaited inside an inner coroutine. The stack unwinding rules apply uniquely to coroutines. It is common to have coroutine awaiters in the wild that unconditionally destroy handles in destructors, performing a bottom-up destruction sequence.39 If an escape occurs, the outer block terminates, which begins destroying the inner operation state. If this destruction sequence is bottom-up, the lowest level awaiter is destroyed, which destroys its handle, resuming the next stack level for unwinding, creating an inherent race between the asynchronous completion and the destruction cascade.39  
The call\_cc architecture protects against this undefined behavior by ensuring that the Escape Sender never attempts to unwind the inner coroutine stack via an exception. Instead, the Escape Sender permanently suspends the inner coroutine by never completing its local receiver, and independently fulfills the outer promise. The inner coroutine will only be safely destroyed when the call\_cc\_op\_state drops its inner\_op\_state\_ std::optional, allowing a controlled, top-down frame destruction managed by the parent operation state, completely bypassing the hazards of bottom-up unwinding.16 Furthermore, the error reporting utilized for unifex and stdexec natively turns an exception escaping from a coroutine into a set\_error\_t(std::exception\_ptr) completion.40 When unhandled\_exception() is called on the promise type, the coroutine is suspended and the function calls set\_error on the receiver. The call\_cc Shared State naturally intercepts this via the Inner Receiver, propagating the error upward effectively and safely locking out any concurrent escape attempts through its atomic barrier.

## **Advanced Error Propagations and Alternative Mappings**

Within the broader sender and receiver algorithms ecosystem, alternative mechanisms exist to handle early termination, though none provide the true continuation-passing semantics of callCC. The C++26 standard includes algorithms such as stopped\_as\_error and stopped\_as\_optional.41 The stopped\_as\_error algorithm maps an input sender's stopped completion operation into an error completion operation utilizing a custom error type.42 The result is a sender that never completes with stopped, instead reporting cancellation by completing with an error.  
While stopped\_as\_error allows a computation to be aborted and converted into an exception-like path, it requires the underlying task to actively issue a stop request or fail. It does not provide the mechanism to capture the continuation and execute an immediate value-yielding jump. Haskell's ContT effectively achieves both throwE and catchE mechanics seamlessly because values and continuations are functionally identical.5 In the C++26 call\_cc module, the set\_error channel is inherently supported independently of the escape mechanism. The Inner Receiver correctly forwards exceptions or explicit errors bubbling up from the inner sender directly to the outer receiver.11  
If multiple execution paths attempt to escape simultaneously, such as a scenario where an error is thrown on a worker thread while a parallel worker thread invokes the Escape Sender, a severe data race on the completion channel is circumvented entirely by the std::atomic\<bool\> completed lock-free guarantee in the Shared State.18 The first thread to transition the flag from false to true irrevocably claims the exclusive right to complete the outer receiver. The loser of the race will observe the true flag during its atomic exchange and silently drop its payload. This behavior aligns perfectly with the "serendipitous success" cancellation model defined in P1677R2 and integrated into P2300.7 Under serendipitous success, if a task successfully completes precisely as a cancellation request is issued, the success is prioritized, and the cancellation is safely ignored. Similarly, an escape attempt is prioritized over a parallel error, provided the escape wins the atomic race, ensuring deterministic structural resolution without application crashes.

## **Conclusion**

The conceptual leap required to translate Haskell's Continuation Monad ContT into C++26's std::execution framework illuminates the shared mathematical underpinnings of Continuation-Passing Style across radically divergent programming paradigms. Haskell's callCC thrives in an isolated environment of immutable state and tracing garbage collection, relying on the sophisticated runtime to discard abandoned computational branches cleanly without developer intervention.  
Conversely, C++26 mandates a systems-level approach where zero-allocation steady-states and explicitly constrained operation lifetimes dominate the architectural design.3 By structurally deconstructing the callCC mechanism into a composite Sender graph comprising an Outer Sender, an Inner Receiver intercepting environment queries, and a terminal Escape Sender, the exact semantics of Haskell's early-exit continuation can be achieved in C++26 without violating dynamic allocation constraints or memory safety rules.  
The critical insight distinguishing the C++26 implementation from its functional counterpart is the mandatory inclusion of the cooperative cancellation mechanism. To satisfy the rigorous constraints of C++ object lifetimes, abandoning a continuation in C++ intrinsically requires injecting a std::inplace\_stop\_source into the environment tree.7 When the Escape Sender is invoked, it must proactively request the termination of all parallel operations operating within the abandoned scope.7 Through this highly intricate integration of custom receivers, atomic shared states, and zero-overhead cancellation tokens, the theoretical elegance of functional control flow is mapped faithfully into the highly constrained, performance-portable ecosystem of modern C++ systems programming.

#### **Works cited**

1. C++26 std::execution vs. Rust's async/rayon: Two different philosophies for the future of concurrency? \- Reddit, accessed June 9, 2026, [https://www.reddit.com/r/rust/comments/1opyaun/c26\_stdexecution\_vs\_rusts\_asyncrayon\_two/](https://www.reddit.com/r/rust/comments/1opyaun/c26_stdexecution_vs_rusts_asyncrayon_two/)  
2. Senders/Receivers: An Introduction \- ACCU, accessed June 9, 2026, [https://accu.org/journals/overload/32/184/teodorescu/](https://accu.org/journals/overload/32/184/teodorescu/)  
3. std::execution from the metal up \- Paul Bendixen \- Meeting C++ 2022 \- YouTube, accessed June 9, 2026, [https://www.youtube.com/watch?v=hLbhNTRKafo](https://www.youtube.com/watch?v=hLbhNTRKafo)  
4. Unifying the HPC ecosystem with std::execution \- Sched, accessed June 9, 2026, [https://static.sched.com/hosted\_files/hpsf2025/d2/hpsf-conference-2025-std-execution-simberg.pdf](https://static.sched.com/hosted_files/hpsf2025/d2/hpsf-conference-2025-std-execution-simberg.pdf)  
5. std::execution, Sender/Receiver, and the Continuation Monad | What Comes to Mind, accessed June 9, 2026, [https://sdowney.org/posts/index.php/2021/10/03/stdexecution-sender-receiver-and-the-continuation-monad/](https://sdowney.org/posts/index.php/2021/10/03/stdexecution-sender-receiver-and-the-continuation-monad/)  
6. wg21\_p2300\_execution/execution.bs at main \- GitHub, accessed June 9, 2026, [https://github.com/brycelelbach/wg21\_p2300\_execution/blob/main/execution.bs](https://github.com/brycelelbach/wg21_p2300_execution/blob/main/execution.bs)  
7. P2300R10: \`std::execution\` \- WG21 Links, accessed June 9, 2026, [https://wg21.link/P2300](https://wg21.link/P2300)  
8. Optional Types and Lightweight Continuation Passing in C++ : r/cpp \- Reddit, accessed June 9, 2026, [https://www.reddit.com/r/cpp/comments/2mou4t/optional\_types\_and\_lightweight\_continuation/](https://www.reddit.com/r/cpp/comments/2mou4t/optional_types_and_lightweight_continuation/)  
9. How is the new asynchronous model in C++26 different from existing models?, accessed June 9, 2026, [https://stackoverflow.com/questions/78829184/how-is-the-new-asynchronous-model-in-c26-different-from-existing-models](https://stackoverflow.com/questions/78829184/how-is-the-new-asynchronous-model-in-c26-different-from-existing-models)  
10. Writing Senders \- ACCU, accessed June 9, 2026, [https://accu.org/journals/overload/33/186/teodorescu/](https://accu.org/journals/overload/33/186/teodorescu/)  
11. Reference — stdexec dev documentation \- GitHub Pages, accessed June 9, 2026, [https://nvidia.github.io/stdexec/reference/index.html](https://nvidia.github.io/stdexec/reference/index.html)  
12. std::execution and pika at CSCS \- Indico \- CERN, accessed June 9, 2026, [https://indico.cern.ch/event/1194848/attachments/2524253/4341115/cscs-cern-std-execution-pika.pdf](https://indico.cern.ch/event/1194848/attachments/2524253/4341115/cscs-cern-std-execution-pika.pdf)  
13. P2300R5: \`std::execution\`, accessed June 9, 2026, [https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2022/p2300r5.html](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2022/p2300r5.html)  
14. P2300R7: \`std::execution\`, accessed June 9, 2026, [https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2023/p2300r7.html](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2023/p2300r7.html)  
15. Sender Intuition: Senders Don't Send · Ben FrantzDale's Blog, accessed June 9, 2026, [https://benfrantzdale.github.io/blog/2024/10/01/sender-intuition-senders-dont-send.html](https://benfrantzdale.github.io/blog/2024/10/01/sender-intuition-senders-dont-send.html)  
16. libunifex/doc/cancellation.md at main \- GitHub, accessed June 9, 2026, [https://github.com/facebookexperimental/libunifex/blob/main/doc/cancellation.md](https://github.com/facebookexperimental/libunifex/blob/main/doc/cancellation.md)  
17. Concurrency support library (since C++11) \- cppreference.com, accessed June 9, 2026, [https://cppreference-45864d.gitlab-pages.liu.se/en/cpp/thread.html](https://cppreference-45864d.gitlab-pages.liu.se/en/cpp/thread.html)  
18. stop\_token.hpp \- NVIDIA/stdexec \- GitHub, accessed June 9, 2026, [https://github.com/NVIDIA/stdexec/blob/main/include/stdexec/stop\_token.hpp](https://github.com/NVIDIA/stdexec/blob/main/include/stdexec/stop_token.hpp)  
19. std::stop\_token \- cppreference.com \- DMCS Pages for Students, accessed June 9, 2026, [https://neo.dmcs.pl/oopc/reference/en/cpp/thread/stop\_token.html](https://neo.dmcs.pl/oopc/reference/en/cpp/thread/stop_token.html)  
20. Contracts for C++ \- Open-std.org, accessed June 9, 2026, [https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2025/p2900r14.pdf](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2025/p2900r14.pdf)  
21. C++ Baremetal Senders & Receivers, accessed June 9, 2026, [https://intel.github.io/cpp-baremetal-senders-and-receivers/](https://intel.github.io/cpp-baremetal-senders-and-receivers/)  
22. GitHub \- Caturra000/uring\_exec: Porting liburing/io\_uring to C++26 std::execution (stdexec), accessed June 9, 2026, [https://github.com/Caturra000/uring\_exec](https://github.com/Caturra000/uring_exec)  
23. Make when\_all a Ronseal Algorithm, accessed June 9, 2026, [https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2025/p3887r0.pdf](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2025/p3887r0.pdf)  
24. The Sender Sub-Language For Beginners \- Standard C++, accessed June 9, 2026, [https://isocpp.org/files/papers/P4014R1.pdf](https://isocpp.org/files/papers/P4014R1.pdf)  
25. stdexec/include/nvexec/stream/split.cuh at main \- GitHub, accessed June 9, 2026, [https://github.com/NVIDIA/stdexec/blob/main/include/nvexec/stream/split.cuh](https://github.com/NVIDIA/stdexec/blob/main/include/nvexec/stream/split.cuh)  
26. accessed June 9, 2026, [https://raw.githubusercontent.com/cplusplus/sender-receiver/12fde4af201017e49efd39178126f661a04dbb94/execution.bs](https://raw.githubusercontent.com/cplusplus/sender-receiver/12fde4af201017e49efd39178126f661a04dbb94/execution.bs)  
27. P2300R3: \`std::execution\`, accessed June 9, 2026, [https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2021/p2300r3.html](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2021/p2300r3.html)  
28. C++ 26 四大新特性 \- AtomGit开源社区, accessed June 9, 2026, [https://gitcode.csdn.net/69cc8c0d54b52172bc66269b.html](https://gitcode.csdn.net/69cc8c0d54b52172bc66269b.html)  
29. stdexec/examples/algorithms/then.hpp at main · NVIDIA/stdexec, accessed June 9, 2026, [https://github.com/NVIDIA/stdexec/blob/main/examples/algorithms/then.hpp](https://github.com/NVIDIA/stdexec/blob/main/examples/algorithms/then.hpp)  
30. What does the \_\_completion\_signature concept in stdexec (senders/ receivers) do?, accessed June 9, 2026, [https://stackoverflow.com/questions/76657859/what-does-the-completion-signature-concept-in-stdexec-senders-receivers-do](https://stackoverflow.com/questions/76657859/what-does-the-completion-signature-concept-in-stdexec-senders-receivers-do)  
31. GitHub \- NVIDIA/stdexec: \`std::execution\`, the standard C++ framework for asynchronous and parallel programming., accessed June 9, 2026, [https://github.com/nvidia/stdexec](https://github.com/nvidia/stdexec)  
32. wg21\_p2300\_std\_execution/std\_execution.bs at main · huzifa-terkawi ... \- GitHub, accessed June 9, 2026, [https://github.com/huzifa-terkawi/wg21\_p2300\_std\_execution/blob/main/std\_execution.bs](https://github.com/huzifa-terkawi/wg21_p2300_std_execution/blob/main/std_execution.bs)  
33. how to safely register std::stop\_callback with std::jthread \- Stack Overflow, accessed June 9, 2026, [https://stackoverflow.com/questions/67302926/how-to-safely-register-stdstop-callback-with-stdjthread](https://stackoverflow.com/questions/67302926/how-to-safely-register-stdstop-callback-with-stdjthread)  
34. User's Guide \- NVIDIA Documentation Hub, accessed June 9, 2026, [https://docs.nvidia.com/hpc-sdk/archive/24.3/pdf/hpc243ug.pdf](https://docs.nvidia.com/hpc-sdk/archive/24.3/pdf/hpc243ug.pdf)  
35. finite\_inplace\_stop\_token benchmarks \- Github-Gist, accessed June 9, 2026, [https://gist.github.com/lewissbaker/d95b3a001650c509570af4968b0d00c5](https://gist.github.com/lewissbaker/d95b3a001650c509570af4968b0d00c5)  
36. A demonstration of how execution domains ensure the correct sender algorithm implementation is selected \- GitHub Gist, accessed June 9, 2026, [https://gist.github.com/ericniebler/3c810343d153af4c0edaeb4f4751bc39](https://gist.github.com/ericniebler/3c810343d153af4c0edaeb4f4751bc39)  
37. P3570R1: optional variants in sender/receiver \- Open-std.org, accessed June 9, 2026, [https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2025/p3570r1.html](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2025/p3570r1.html)  
38. What are Senders Good For, Anyway? \- Eric Niebler, accessed June 9, 2026, [https://ericniebler.com/2024/02/04/what-are-senders-good-for-anyway/](https://ericniebler.com/2024/02/04/what-are-senders-good-for-anyway/)  
39. snaury/coroactors: Experimental actors with C++ coroutines \- GitHub, accessed June 9, 2026, [https://github.com/snaury/coroactors](https://github.com/snaury/coroactors)  
40. Add a Coroutine Task Type \- Open-std.org, accessed June 9, 2026, [https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2025/p3552r1.pdf](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2025/p3552r1.pdf)  
41. Remove std::execution::split, accessed June 9, 2026, [https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2025/p3682r0.pdf](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2025/p3682r0.pdf)  
42. Fix Sender Algorithm Customization \- Open Standards, accessed June 9, 2026, [https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2026/p3826r3.html](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2026/p3826r3.html)