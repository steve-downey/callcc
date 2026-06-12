# example/CMakeLists.txt                                            -*-cmake-*-
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

cmake_minimum_required(VERSION 4.3)

project(callcc VERSION 0.0.0 LANGUAGES CXX)

# Instrumentation query
# find_package(Python COMPONENTS Interpreter)
cmake_instrumentation(
  API_VERSION 1
  DATA_VERSION 1
  HOOKS postGenerate preBuild postBuild preCMakeBuild postCMakeBuild postCMakeInstall postCTest
  OPTIONS trace
  CALLBACK ${CMAKE_SOURCE_DIR}/scripts/instrument.sh
)

# Includes
include(CTest)
include(FetchContent)

option(
    CALLCC_ENABLE_TESTING
    "Enable building tests and test infrastructure"
    ${PROJECT_IS_TOP_LEVEL}
)

set(CMAKE_VERIFY_INTERFACE_HEADER_SETS ON)

# beman.execution is vendored under vendored/ as a squashed git subtree (we do
# not modify or upstream it). Build it from source as a nested project rather
# than resolving an external package, so no network/lockfile fetch is needed.
# Consume it headers-only: modules off, and its own tests/examples disabled.
set(BEMAN_USE_MODULES OFF CACHE BOOL "callcc consumes beman.execution as headers" FORCE)
set(BEMAN_EXECUTION_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(BEMAN_EXECUTION_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory(vendored/beman.execution)

# Header-only facade library (the Metaprogramming Firewall).
add_library(callcc.callcc INTERFACE)

target_sources(
    callcc.callcc
    INTERFACE FILE_SET callcc_callcc_headers TYPE HEADERS BASE_DIRS src
)

target_link_libraries(callcc.callcc INTERFACE beman::execution_headers)
target_compile_features(callcc.callcc INTERFACE cxx_std_23)

if(CALLCC_ENABLE_TESTING)
    find_package(Catch2 3 REQUIRED)
    include(Catch)
endif()

add_subdirectory(src)

include(infra/cmake/beman-install-library.cmake)
beman_install_library(callcc.callcc TARGETS callcc.callcc NAMESPACE smd::)

# Coverage
configure_file("cmake/gcovr.cfg.in" gcovr.cfg @ONLY)

add_custom_target(
    process_coverage
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Running gcovr to process coverage results"
    COMMAND mkdir -p coverage
    COMMAND gcovr --config gcovr.cfg .
)
