// testinstall/test.cpp                                               -*-C++-*-
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <iostream>
#include <smd/callcc/callcc.hpp>

int main() {
    std::cout << "callcc: |" << callcc::callcc() << '|' << '\n';
    return 0;
}
