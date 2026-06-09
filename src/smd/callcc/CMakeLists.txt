include(GNUInstallDirs)

target_sources(
    callcc.callcc
    PUBLIC FILE_SET callcc_callcc_headers TYPE HEADERS FILES callcc.hpp
    PRIVATE callcc.cpp
)

## Tests
if(CALLCC_ENABLE_TESTING AND NOT GTest_FOUND)
    find_package(GTest QUIET)
    if(GTest_FOUND)
        include(GoogleTest)
    else()
        message(
            WARNING
            "
No provider for GTest. Unable to build tests.
Consider using CMAKE_PROJECT_TOP_LEVEL_INCLUDES=./cmake/use-fetch-content.cmake as documented in the README.md
"
        )
    endif()
endif()

if(CALLCC_ENABLE_TESTING)
    add_executable(callcc_test)

    target_sources(callcc_test PRIVATE callcc.test.cpp)

    target_link_libraries(callcc_test callcc.callcc)
    target_link_libraries(callcc_test GTest::gtest)
    target_link_libraries(callcc_test GTest::gtest_main)

    include(GoogleTest)
    gtest_discover_tests(callcc_test)
endif()
