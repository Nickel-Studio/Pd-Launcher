include_guard(GLOBAL)

# Ninja + MSVC does not inherit the IDE's dependency prefix; measure it from the real compiler,
# otherwise /showIncludes output is not parsed and incremental builds silently miss headers.
if(MSVC AND CMAKE_GENERATOR MATCHES "Ninja")
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    foreach(_language C CXX)
        if(NOT CMAKE_${_language}_COMPILER_LOADED)
            continue()
        endif()
        execute_process(
            COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_LIST_DIR}/../scripts/native/msvc-includes.py"
                "${CMAKE_${_language}_COMPILER}" "${CMAKE_BINARY_DIR}" --language "${_language}"
            RESULT_VARIABLE _nickel_prefix_result
            OUTPUT_VARIABLE _nickel_include_prefix
            ERROR_VARIABLE _nickel_prefix_error
            ENCODING UTF-8
            TIMEOUT 40
        )
        if(NOT _nickel_prefix_result EQUAL 0 OR NOT _nickel_include_prefix)
            message(FATAL_ERROR "The actual MSVC dependency prefix could not be measured: ${_nickel_prefix_error}")
        endif()
        set(CMAKE_${_language}_CL_SHOWINCLUDES_PREFIX "${_nickel_include_prefix}")
        set(CMAKE_CL_SHOWINCLUDES_PREFIX "${_nickel_include_prefix}")
    endforeach()
endif()
