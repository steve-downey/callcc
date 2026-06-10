include(GNUInstallDirs)

target_sources(
    callcc.callcc
    INTERFACE FILE_SET callcc_callcc_headers TYPE HEADERS FILES execution_fw.hpp
)

## Tests
if(CALLCC_ENABLE_TESTING)
    add_executable(callcc_test)

    target_sources(callcc_test PRIVATE execution_fw.test.cpp)

    target_link_libraries(callcc_test PRIVATE callcc.callcc Catch2::Catch2WithMain)

    # Completion-signature template expansion blows up symbol tables; use a
    # high-concurrency linker if one is available.
    foreach(_fw_linker mold lld)
        if(NOT _fw_linker_found)
            find_program(_fw_linker_path NAMES "${_fw_linker}")
            if(_fw_linker_path)
                string(TOUPPER "${_fw_linker}" _fw_linker_upper)
                set_target_properties(
                    callcc_test
                    PROPERTIES LINKER_TYPE "${_fw_linker_upper}"
                )
                message(STATUS "callcc_test: using ${_fw_linker} linker")
                set(_fw_linker_found TRUE)
            endif()
        endif()
    endforeach()

    catch_discover_tests(callcc_test)
endif()
