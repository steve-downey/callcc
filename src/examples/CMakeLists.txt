include(GNUInstallDirs)

add_executable(hello)

# Add example source file.
target_sources(hello PRIVATE hello.cpp)

# Link example with the library.
target_link_libraries(hello PRIVATE callcc.callcc)

install(
    TARGETS hello
    COMPONENT callcc.callcc.examples
    DESTINATION ${CMAKE_INSTALL_BINDIR}
    EXCLUDE_FROM_ALL
)

# call_cc demonstration examples (built directly on beman.execution).
foreach(_callcc_example callcc_early_return callcc_exception callcc_deep_escape)
    add_executable(${_callcc_example})
    target_sources(${_callcc_example} PRIVATE ${_callcc_example}.cpp)
    target_link_libraries(${_callcc_example} PRIVATE callcc.callcc)
    install(
        TARGETS ${_callcc_example}
        COMPONENT callcc.callcc.examples
        DESTINATION ${CMAKE_INSTALL_BINDIR}
        EXCLUDE_FROM_ALL
    )
endforeach()
