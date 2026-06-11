// call_cc.hpp                                                        -*-C++-*-
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// A C++26 std::execution sender adaptor implementing the semantics of
// Haskell's callCC (call-with-current-continuation), built *directly* on
// beman::execution (not through the fw::exec facade).
#ifndef INCLUDED_SMD_CALLCC_CALL_CC
#define INCLUDED_SMD_CALLCC_CALL_CC

#include <beman/execution/execution.hpp>
#include <beman/execution/stop_token.hpp>

#include <atomic>
#include <exception>
#include <functional>
#include <optional>
#include <type_traits>
#include <utility>

namespace smd {

namespace ex = ::beman::execution;

// callcc_detail — ADL-isolated home for the custom senders, receivers, and
// operation states so they never leak into beman::execution's namespace.
namespace callcc_detail {

}  // namespace callcc_detail

}  // namespace smd

#endif  // INCLUDED_SMD_CALLCC_CALL_CC
