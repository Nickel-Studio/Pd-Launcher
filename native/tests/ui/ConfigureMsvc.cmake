include_guard(GLOBAL)
if(MSVC AND CMAKE_GENERATOR MATCHES "Ninja")
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    execute_process(
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/MsvcDependencies.py"
            --probe "${CMAKE_CXX_COMPILER}" "${CMAKE_BINARY_DIR}"
        RESULT_VARIABLE _ui_include_probe_result
        OUTPUT_VARIABLE _ui_include_state
        ERROR_VARIABLE _ui_include_probe_error
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ENCODING UTF-8
    )
    if(NOT _ui_include_probe_result EQUAL 0)
        message(FATAL_ERROR "MSVC UI dependency probe failed: ${_ui_include_probe_error}")
    endif()
    set(CMAKE_CXX_COMPILER_LAUNCHER
        "${Python3_EXECUTABLE};${CMAKE_CURRENT_LIST_DIR}/MsvcDependencies.py;--compile-probed;${_ui_include_state}")
    set(CMAKE_CXX_CL_SHOWINCLUDES_PREFIX "Note: including file: ")
    set(CMAKE_CL_SHOWINCLUDES_PREFIX "Note: including file: ")
endif()
