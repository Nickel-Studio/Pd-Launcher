include_guard(GLOBAL)

# The optional input carries public Microsoft client identifiers only; secrets stay in the OS
# credential store. Without input the generator still emits a header with empty configuration.
set(NICKEL_PUBLIC_BUILD_CONFIG "" CACHE FILEPATH "Optional public Microsoft JSON input; never a secrets file")

function(nickel_generate_public_build_config output_directory)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(_script "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../scripts/native/public-build-config.py")
    set(_header "${output_directory}/PublicBuildConfig.hpp")
    set(_arguments --output "${_header}")
    if(NICKEL_PUBLIC_BUILD_CONFIG)
        if(NOT EXISTS "${NICKEL_PUBLIC_BUILD_CONFIG}" OR IS_DIRECTORY "${NICKEL_PUBLIC_BUILD_CONFIG}")
            message(FATAL_ERROR "The explicitly selected public build configuration is not a readable file")
        endif()
        list(APPEND _arguments --input "${NICKEL_PUBLIC_BUILD_CONFIG}")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${NICKEL_PUBLIC_BUILD_CONFIG}")
    endif()
    execute_process(COMMAND "${Python3_EXECUTABLE}" -B "${_script}" ${_arguments}
        RESULT_VARIABLE _result ERROR_VARIABLE _error OUTPUT_QUIET)
    if(NOT _result EQUAL 0)
        message(FATAL_ERROR "Public Microsoft build configuration generation failed: ${_error}")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_script}")
endfunction()
