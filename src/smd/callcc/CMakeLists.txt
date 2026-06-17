include(GNUInstallDirs)

target_sources(
    callcc.callcc
    INTERFACE FILE_SET callcc_callcc_headers TYPE HEADERS FILES call_cc.hpp
)

## Tests
if(CALLCC_ENABLE_TESTING)
    add_executable(call_cc_test)
    target_sources(call_cc_test PRIVATE call_cc.test.cpp)
    target_link_libraries(
        call_cc_test
        PRIVATE callcc.callcc Catch2::Catch2WithMain
    )

    # Completion-signature template expansion blows up symbol tables; use a
    # high-concurrency linker if one is available.
    foreach(_fw_linker mold lld)
        if(NOT _fw_linker_found)
            find_program(_fw_linker_path NAMES "${_fw_linker}")
            if(_fw_linker_path)
                string(TOUPPER "${_fw_linker}" _fw_linker_upper)
                set_target_properties(
                    call_cc_test
                    PROPERTIES LINKER_TYPE "${_fw_linker_upper}"
                )
                message(STATUS "call_cc_test: using ${_fw_linker} linker")
                set(_fw_linker_found TRUE)
            endif()
        endif()
    endforeach()

    catch_discover_tests(call_cc_test)
endif()
