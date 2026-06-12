# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this project is

`callcc` ("Sender CallCC") is a header-only C++23 **facade over a P2300
`std::execution` reference implementation**. The single facade header
`src/smd/callcc/execution_fw.hpp` is the *only* place that includes a backend
(`beman::execution`); all other code must use `fw::exec::*` and never reach into
the backend namespace directly. This is deliberately a "metaprogramming
firewall" that insulates business code from vendor-specific headers and ADL.

Key consequence for routine work: when spec text, tests, or a directive
reference a `std::` P2300 symbol that libstdc++ doesn't yet ship (e.g.
`std::inplace_stop_source`, `std::execution::sync_wait`), route it through
`fw::exec::` backed by beman — adding the alias in `execution_fw.hpp` if absent.
Treating `beman::execution` as the canonical P2300 implementation is the
intended design, not a workaround.

## Architecture

- **`fw::exec::`** — the public facade. Mostly `using _impl::X;` passthroughs
  to `::beman::execution` (aliased internally as `_impl`), plus name-bridging
  for divergences between the WG21 draft / Northstar API and beman:
  - `empty_env` → `_impl::env<>`
  - `transfer` → `continues_on` (P2300 rename)
  - `let_value` is **not** a passthrough — it's reimplemented to add a
    stop-token QoI patch (`detail::let_value_qoi::guarded_sender`) so stop
    requests issued inside the factory body are honored before the inner
    sender starts.
  - `static_thread_pool` is hand-implemented (`detail::thread_pool_impl`)
    because beman ships no thread pool.
- **`fw::diag::`** — diagnostic toolkit (Phase 2). `display<...>` is an
  intentionally-undefined template; `dump_signatures_t` and the
  `*_v` predicates force the compiler to print deduced completion signatures
  in error output.
- **Custom Senders/Receivers/OperationStates** must live in a private,
  ADL-isolated namespace. The facade's own customs go under
  `fw::exec::detail::*`; tests put theirs in `my_domain::`. This blocks ADL
  from leaking into the backend namespace.

Backend selection is by macro: `FW_USE_BEMAN` (default) or `FW_USE_STDEXEC`
(reserved, not yet wired — `#error`s if selected).

This is built in **phases; do not advance until the prior phase's tests pass**:
1. Facade header + concept re-exports
2. Diagnostic toolkit
3. Linear chains (`just | transfer | then | sync_wait`)
4. Structured concurrency & cancellation (`when_all`, `let_value`,
   `inplace_stop_source`, no operation-state leaks)

## Layout

Merged/Pitchfork layout: headers, source, and tests are **co-located** under
`src/`. Tests are `*.test.cpp` next to the header they cover (e.g.
`src/smd/callcc/execution_fw.test.cpp`). The include root is `src`, so headers
are included as `<smd/callcc/execution_fw.hpp>`. Tests use **Catch2 v3**
(`TEST_CASE`/`REQUIRE`).

## Build & test

The top-level `Makefile` drives the workflow (preferred over CMake presets for
local dev). It uses `uv` to provision tools into a local `.venv` (cmake, ninja,
ctest, pre-commit, gcovr) — no system changes. CMake is "Ninja Multi-Config".

```sh
make                       # default: build + run all tests (CONFIG=Asan)
make compile               # build only
make test                  # rebuild and run ctest
make ctest                 # run ctest on the current build (no rebuild)
make compile-headers       # verify the interface header set compiles standalone
make lint                  # run all pre-commit hooks (clang-format, cmake-format, codespell, ...)
make coverage              # build+test with Gcov profile, process with gcovr
make view-coverage         # open the coverage HTML report
make install               # install the library
make testinstall           # build + install + verify the installed package consumes
```

Toolchain / config selection:
- `make TOOLCHAIN=gcc-15` uses `etc/gcc-15-toolchain.cmake` (compilers are
  expected on PATH under versioned names like `g++-15`, `clang++-21`). Bare
  `make` uses the system `c++`.
- `make CONFIG=RelWithDebInfo` (or `Debug`, `Tsan`, `Asan`, `Gcov`). Default is
  `Asan` (address sanitizer + compatible sanitizers).
- Build dirs: `.build/build-system/` (default toolchain) or
  `.build/build-<TOOLCHAIN>/`.

Run a single test (Catch2 tag/name filtering):
```sh
make compile && .build/build-system/src/smd/callcc/<config>/callcc_test "<test name or tag>"
# or via ctest by name regex:
make ctest && ctest --test-dir .build/build-system -C Asan -R "<regex>"
```

`make compile_commands.json` symlinks the current build's compile DB to the
repo root for tooling.

## Conventions

- Every source file starts with an emacs mode line and
  `// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception`.
- Headers use `INCLUDED_SMD_CALLCC_*` include guards (not `#pragma once`).
- clang-format and cmake-format are enforced via pre-commit; run `make lint`
  before committing rather than formatting by hand.
- The `infra/` directory is vendored from the Beman Project via `git subtree`
  (Apache-2.0); avoid editing it directly.
- `callcc_test` opts into the `mold`/`lld` linker when available — completion-
  signature template expansion produces large symbol tables.
