# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this project is

`callcc` ("Sender CallCC") is a header-only C++23 library implementing
`call_cc` — **call-with-current-continuation** — as a P2300 `std::execution`
sender adaptor built directly on [`beman::execution`](https://github.com/bemanproject/execution).

`smd::call_cc<ValueType>(f)` returns a sender. `f` is invoked with an escape
factory; calling that factory with a `ValueType` value produces an escape sender
which, when started anywhere in the graph, performs a cooperative early exit
and makes the whole `call_cc` block complete with that value.

When spec text, tests, or a directive reference a `std::` P2300 symbol that
libstdc++ doesn't yet ship (e.g. `std::inplace_stop_source`,
`std::execution::sync_wait`), use the equivalent from `beman::execution`
directly — that is the canonical P2300 implementation, not a workaround.

## Architecture

- **`smd::call_cc<V>(f)`** — the single public entry point in `call_cc.hpp`.
- **`smd::callcc_detail::`** — ADL-isolated home for all custom senders,
  receivers, and operation states. Nothing in here leaks into
  `beman::execution`'s namespace.
- **`beman::execution`** — included directly in `call_cc.hpp`; aliased locally
  as `ex`. There is no intermediate facade layer.

Key design properties of the implementation:
- **Drain-then-complete:** an escape stashes its value and issues a cooperative
  stop request; the block only completes once the inner operation has drained,
  so the operation-state tree is never torn down while work is in flight.
- **Derived completion signatures:** computed from the inner sender (through
  the local stop-token environment), unioned with `set_value_t(V)` and
  `set_error_t(exception_ptr)`.
- **No heap allocation:** shared state lives by value in the operation state.

## Layout

Merged/Pitchfork layout: headers, source, and tests are **co-located** under
`src/`. Tests are `*.test.cpp` next to the header they cover (e.g.
`src/smd/callcc/call_cc.test.cpp`). The include root is `src`, so the header
is included as `<smd/callcc/call_cc.hpp>`. Tests use **Catch2 v3**
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
make compile && .build/build-system/src/smd/callcc/<config>/call_cc_test "<test name or tag>"
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
- `call_cc_test` opts into the `mold`/`lld` linker when available — completion-
  signature template expansion produces large symbol tables.
