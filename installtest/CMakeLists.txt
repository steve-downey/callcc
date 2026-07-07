# installtest/CMakeLists.txt                                        -*-CMake-*-
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

cmake_minimum_required(VERSION 3.27)
project(TestInstalledCallcc)

set(CMAKE_CXX_STANDARD 23)

# Enable testing in this separate project
enable_testing()

# Find the installed package
find_package(callcc.callcc REQUIRED)

# Add the test executable
add_executable(TestInstalledLibrary test.cpp)

# Link against the imported target
target_link_libraries(TestInstalledLibrary smd::callcc)

# Register the test with CTest
add_test(NAME RunInstalledTest COMMAND TestInstalledLibrary)

# Ensure 'make test' first builds the 'all' target
set(CMAKE_SKIP_TEST_ALL_DEPENDENCY FALSE)
