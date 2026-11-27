# Only build-resume registers this directory in the existing production root graph.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    message(FATAL_ERROR "Trusted export tests are not a standalone historical-library import portal")
endif()
if(NOT TARGET Nickel::AppServices)
    message(FATAL_ERROR "Trusted export tests require the actual complete production Nickel::AppServices target")
endif()
get_target_property(_pd_app_target Nickel::AppServices ALIASED_TARGET)
if(NOT _pd_app_target)
    set(_pd_app_target Nickel::AppServices)
endif()
get_target_property(_pd_app_imported "${_pd_app_target}" IMPORTED)
get_target_property(_pd_app_type "${_pd_app_target}" TYPE)
get_target_property(_pd_app_sources "${_pd_app_target}" SOURCES)
if(_pd_app_imported OR NOT _pd_app_type STREQUAL "STATIC_LIBRARY" OR NOT _pd_app_sources)
    message(FATAL_ERROR "No imported/old .lib or incomplete AppServices graph may register these tests")
endif()

set(PD_TRUSTED_EXPORT_SOURCE_COMMIT "" CACHE STRING "Exact immutable production source commit")
set(PD_TRUSTED_EXPORT_SOURCE_SHA256 "" CACHE STRING "Exact production consumed source manifest SHA256")
string(LENGTH "${PD_TRUSTED_EXPORT_SOURCE_COMMIT}" _pd_commit_length)
string(LENGTH "${PD_TRUSTED_EXPORT_SOURCE_SHA256}" _pd_source_length)
if(NOT _pd_commit_length EQUAL 40 OR NOT PD_TRUSTED_EXPORT_SOURCE_COMMIT MATCHES "^[0-9a-f]+$"
   OR NOT _pd_source_length EQUAL 64 OR NOT PD_TRUSTED_EXPORT_SOURCE_SHA256 MATCHES "^[0-9a-f]+$")
    message(FATAL_ERROR "Builder must bind the full current production commit and consumed source SHA256")
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)

add_executable(pd_trusted_export_security_tests trusted_export_tests.cpp)
target_compile_features(pd_trusted_export_security_tests PRIVATE cxx_std_20)
target_link_libraries(pd_trusted_export_security_tests PRIVATE Nickel::AppServices)
target_compile_definitions(pd_trusted_export_security_tests PRIVATE
    PD_TRUSTED_EXPORT_SOURCE_COMMIT="${PD_TRUSTED_EXPORT_SOURCE_COMMIT}"
    PD_TRUSTED_EXPORT_SOURCE_SHA256="${PD_TRUSTED_EXPORT_SOURCE_SHA256}")
set_target_properties(pd_trusted_export_security_tests PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
if(MSVC)
    target_compile_options(pd_trusted_export_security_tests PRIVATE /W4 /WX /utf-8 /EHsc /permissive- /Zc:__cplusplus)
else()
    target_compile_options(pd_trusted_export_security_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

set(_pd_graph "{}")
string(JSON _pd_graph SET "${_pd_graph}" schema "\"pd.trusted-export.graph.v1\"")
string(JSON _pd_graph SET "${_pd_graph}" sourceCommit "\"${PD_TRUSTED_EXPORT_SOURCE_COMMIT}\"")
string(JSON _pd_graph SET "${_pd_graph}" sourceSha256 "\"${PD_TRUSTED_EXPORT_SOURCE_SHA256}\"")
string(JSON _pd_graph SET "${_pd_graph}" appServicesTarget "\"${_pd_app_target}\"")
string(JSON _pd_graph SET "${_pd_graph}" appServicesImported false)
set(_pd_sources_json "[]")
set(_pd_index 0)
foreach(_pd_source IN LISTS _pd_app_sources)
    string(REPLACE "\\" "/" _pd_source "${_pd_source}")
    string(REPLACE "\"" "\\\"" _pd_source "${_pd_source}")
    string(JSON _pd_sources_json SET "${_pd_sources_json}" ${_pd_index} "\"${_pd_source}\"")
    math(EXPR _pd_index "${_pd_index} + 1")
endforeach()
string(JSON _pd_graph SET "${_pd_graph}" appServicesSources "${_pd_sources_json}")
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/graph-$<CONFIG>.json" CONTENT "${_pd_graph}\n")

# Tests use new run directories and record stdout/stderr bytes, hashes and NOT_RUNs.
add_test(NAME pd.trusted_export.tools
    COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/ctest_entry.py"
            tools --owner "${CMAKE_CURRENT_SOURCE_DIR}")
add_test(NAME pd.trusted_export.app_negative
    COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/ctest_entry.py"
            app-negative --owner "${CMAKE_CURRENT_SOURCE_DIR}"
            --exe "$<TARGET_FILE:pd_trusted_export_security_tests>"
            --graph "${CMAKE_CURRENT_BINARY_DIR}/graph-$<CONFIG>.json"
            --source-commit "${PD_TRUSTED_EXPORT_SOURCE_COMMIT}"
            --source-sha256 "${PD_TRUSTED_EXPORT_SOURCE_SHA256}")
add_test(NAME pd.trusted_export.actual_pd
    COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/ctest_entry.py"
            actual-pd-not-run --owner "${CMAKE_CURRENT_SOURCE_DIR}")
set_tests_properties(pd.trusted_export.tools pd.trusted_export.app_negative pd.trusted_export.actual_pd
    PROPERTIES TIMEOUT 240 RUN_SERIAL TRUE SKIP_RETURN_CODE 77)

add_custom_target(pd_trusted_export_tools_check
    COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/ctest_entry.py"
            tools --owner "${CMAKE_CURRENT_SOURCE_DIR}"
    VERBATIM)
# Actual Pd is deliberately a NOT_RUN CTest until delivery-entry exposes a real
# isolated production-process hook. Never make a fixture main.exe the positive.
