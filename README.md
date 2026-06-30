# Sender CallCC

[![OpenSSF Baseline](https://www.bestpractices.dev/projects/12577/baseline)](https://www.bestpractices.dev/projects/12577)

This project implements `call_cc` — **call-with-current-continuation** — as a
C++26 `std::execution` (P2300) sender adaptor, built directly on
[`beman::execution`](https://github.com/bemanproject/execution).

## What callCC is

`callCC` captures *the current continuation* — "the rest of the computation" —
and hands it to your code as a first-class **escape function**. Calling that
escape abandons whatever work would normally have followed and resumes at the
point where the continuation was captured, carrying along the value you passed.
It is the most general of the classical control operators: early return,
exceptions, generators, coroutines, and backtracking are all special cases of
it. It comes from Scheme (`call/cc`); in Haskell it is the cornerstone of the
continuation monad:

```haskell
callCC :: ((a -> ContT r m b) -> ContT r m a) -> ContT r m a
```

Senders and receivers *are* continuation-passing style — a receiver is the
continuation a sender will eventually invoke — so there is a natural
isomorphism between the continuation monad and the sender/receiver model. That
correspondence is what makes a sender-based `call_cc` possible at all. The
long-form derivation in `docs/` is an AI-generated research report; I used it to
explore the theory and sharpen the plan before writing the code, not as a spec
the code was checked against. Read it as background, not gospel.

## What it can be used for

- **Early return** from the middle of an asynchronous pipeline.
- **Exception-style non-local exit**: the `call_cc` boundary is the `catch`,
  invoking the escape is the `throw`.
- **Multi-level break**: jump straight out of a deeply nested composition,
  skipping every intervening stage.
- In languages with *re-invocable* continuations, also generators, coroutines,
  and backtracking search — see *Limits* below for what this implementation
  does and does not give you.

## What this implementation does

`smd::call_cc<ValueType>(f)` returns a sender. `f` is invoked with an **escape
factory**; calling `escape(v)` yields a sender that, when started anywhere in
your graph, performs the early exit and makes the whole `call_cc` block complete
with `v`. Concretely it:

- is built **directly on `beman::execution`** and keeps its internals in the
  ADL-isolated `smd::callcc_detail`;
- uses **drain-then-complete** semantics: an escape stashes its value and issues
  a cooperative stop request to the abandoned inner work, and the block
  completes only once that inner operation has actually drained — so the
  operation-state tree is never torn down while work is still in flight (no
  use-after-free);
- **derives its completion signatures from the inner sender** (unioned with the
  escape's `set_value_t(ValueType)` and a `set_error_t(exception_ptr)` for a
  throwing factory/connect), so it reports what it can really complete with;
- reports a throwing user factory or `connect` through `set_error` rather than
  terminating; and
- allocates nothing on the heap (the shared state lives by value in the
  operation state).

## Limits

- **One-shot escape.** The captured continuation is single-use: it is a sender
  you connect and start once, not a value you can store and re-invoke later.
  This is inherent to P2300: operation states are immovable (`start()` takes
  `&`, not `&&`) and senders are consumed on `connect`. Multi-shot
  continuations — generators, coroutine resumption, backtracking — require
  fibers or stackful coroutines, a fundamentally different mechanism.
- **The default escape lives on the stopped channel.** `escape(v)` completes
  its local receiver with `set_stopped()` and delivers its value out-of-band,
  so it is not a valid `when_all` child (beman's `when_all` requires each child
  to have exactly one value completion). For positions that require a value
  completion, use `escape.value(v)` instead — it advertises
  `set_value_t(ValueType)`, satisfying the constraint while still triggering
  the cooperative escape. Use the stopped-channel `escape(v)` inside
  `let_value`/`let_error`/`let_stopped` handlers; use `escape.value(v)` as a
  `when_all` child (see `callcc_when_all.cpp`).
- **`ValueType` is explicit** (`call_cc<int>(...)`): the escape factory's
  signature must be known before `f` runs — the factory type depends on
  `ValueType`, and `f` receives the factory, so the type cannot be deduced from
  how `f` uses the factory. Two convenience forms reduce the ergonomic burden:
  - `call_cc_void(f)` — for early exit with no return value (`monostate`).
  - `call_cc_from(f)` — deduces `ValueType` from a typed parameter:
    `call_cc_from([](smd::escape_fn<int> escape) { ... })`. Only works for
    non-generic lambdas.
- **Cancellation is cooperative.** Abandoned work is asked to stop via stop
  tokens; inner work that ignores its stop token is not forcibly interrupted —
  the block waits for it to finish. This matches P2300's design philosophy:
  `when_all`'s internal cancellation has the same property.
- **Value type consistency.** The escaped value type and the inner sender's
  normal value completion must be consistent for single-value consumers (e.g.
  `sync_wait`) to accept the block. Ensure both paths produce the same type,
  or use different completion channels (see `callcc_exception.cpp`, where the
  escape fires from `let_error` and the normal path flows through `set_value`).

## Examples

Five runnable demonstrations live in [`src/examples/`](./src/examples). Build
them with `make TOOLCHAIN=gcc-16` (or your toolchain of choice) and run the
binaries from the build tree.

- **`callcc_early_return.cpp` — early return.** Reaches the escape half-way
  through a pipeline and jumps straight out; the trailing stages never run. A
  second graph that does not take the escape runs to completion, for contrast.
- **`callcc_exception.cpp` — exception-like non-local exit.** A stage throws on
  bad input; an `ex::let_error` handler is the `catch` and escapes with a
  recovery value. Good input flows through the value channel untouched — a
  conditional escape expressed across completion channels.
- **`callcc_deep_escape.cpp` — escape from deep nesting.** Escapes from the
  innermost of three nested `let_value` levels straight back to the boundary,
  bypassing both outer trailing stages (verified with side-effect flags).
- **`callcc_when_all.cpp` — value-channel escape in `when_all`.** Uses
  `escape.value(v)` as a `when_all` child, demonstrating the value-channel
  escape that satisfies `when_all`'s completion-signature constraint.
- **`callcc_void_escape.cpp` — void escape with `call_cc_void`.** Early exit
  carrying no value (`std::monostate`), using the `call_cc_void` convenience.

## Project scaffolding

The repository also serves as a working C++ project scaffold: CI, linting,
packaging, docs, presentation export. It is generated from my
[steve-downey/example](https://github.com/steve-downey/example) Copier template,
so much of what follows is boilerplate shared with every project I start that
way — `.copier-answers.yml` records the answers, and `copier update` pulls in
later template changes.

The C++ src is all in the ./src directory, including the headers and tests. Take a look at [The Pitchfork Layout Spec](https://www.w3.org/publications/spec-generator/?type=bikeshed-spec&output=html&die-on=fatal&md-date=&url=https%3A%2F%2Fraw.githubusercontent.com%2Fvector-of-bool%2Fpitchfork%2Fdevelop%2Fdata%2Fspec.bs&file=) for some discussion about merged layouts. Short answer is that include directories are an install location, not a source location, but that the directory layouts must still be coherent. Tests are co-located because tests are important and the further away they are, the more they will be dropped.

The CMake is contemporary, post-modern, so not just target oriented, it is also file set oriented.

GitHub Actions are set up to make sure everything I expect to work actually does.

There is a top-level Makefile to drive workflow. Its default is to build and run all tests for the project.

The project leverages `uv` and PyPI to install the tools that it requires. It installs them into a local virtual environment so as not to make system wide changes.

The [pre-commit](https://github.com/pre-commit/pre-commit) framework is used to drive linters both locally and in GitHub Actions. Clang format is enforced, as is a CMake format. I've given up doing this by hand. Yaml is even worse. Spellcheck, for code, also.

The infra directory is vendored in from the [Beman Project](https://github.com/bemanproject/infra) supporting [infra](https://github.com/bemanproject/infra) project. Right now for install of the project. Many of the GitHub actions in .github/workflows/ also use Beman scripts and tools. The CMakePresets.json exists largely to support those tools. I find the workflow [Makefile](./Makefile) easier to extend with less combinatorial explosion.

Compilers are expected to be available on PATH with versioned names, such as `g++-15` or `clang++-21`. Toolchains are in the ./etc/ directory.

`make` by itself uses the system `c++` compiler. For others, e.g., `make TOOLCHAIN=gcc-15` will use the etc/gcc-15-toolchain.cmake toolchain, which sets CXX to be gcc-15. By default the build and test is address sanitized, plus some compatible sanitizers. Alternatives are specified with CONFIG, e.g. `make TOOLCHAIN=gcc-15 CONFIG=RelWithDebInfo`.


## Presentations with Emacs and org-transclusion

`make presentation` builds and tests the project, then runs [nobiot's org-transclusion](https://github.com/nobiot/org-transclusion) and org-export over the `.org` files to produce HTML for use in presentations. The point of transcluding the real source is that the code in a talk is the code that actually compiled and passed its tests, rather than a snippet that has since drifted.

## The workflow Makefile

The Makefile provides a variety of tools. It installs most of them from PyPI, as long as `uv` is available, into a local `.venv` so as not to mess up the rest of your environment.

```shell
(callcc) sdowney@pwyll:~/src/callcc/callcc (main ±)
$ make help
clean                          Clean the build artifacts
clean-venv                     Delete python virtual env
compile                        Compile the project
compile_commands.json          symlink the current compile commands db
compile-headers                Compile the headers
coverage                       Build and run the tests with the GCOV profile and process the results
ctest                          Run CTest on current build
dev-shell                      Shell with the venv activated
docs                           Build the docs with Doxygen
help                           Show this help.
install                        Install the project
install-uv                     install uv via `pipx install uv`
lint                           Run all configured tools in pre-commit
lint-manual                    Run all manual tools in pre-commit
mrdocs                         Build the docs with MrDocs
realclean                      Delete the build directory
show-venv                      Debugging target - show venv details
test                           Rebuild and run tests
testinstall                    Test the installed package
venv                           Create python virtual env
view-coverage                  View the coverage report
```

`lint` uses pre-commit to drive the various lint tools.

The code in infra is Apache 2.0 licensed, see https://github.com/bemanproject/infra for more details. The CMakeLists.txt is derived from https://github.com/bemanproject/exemplar the purpose of which is to be a concrete but boring example of a well behaved CMake C++ project using the current tools and practices.

The css in `etc/`  is exported from emacs based on the modus tinted themes via `org-html-htmlize-generate-css` .

The Makefile that drives the workflow is mine, is Apache 2.0 licensed, and take what you need from it. No part of it is interesting enough to be protected.
