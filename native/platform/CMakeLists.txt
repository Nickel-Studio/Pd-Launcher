if(NOT TARGET nickel_foundation)
    message(FATAL_ERROR "Add native/core/config before native/platform")
endif()
get_target_property(NICKEL_FOUNDATION_PLATFORM_ADDED nickel_foundation NICKEL_FOUNDATION_PLATFORM_ADDED)
if(NOT NICKEL_FOUNDATION_PLATFORM_ADDED)
    target_sources(nickel_foundation PRIVATE
        "${CMAKE_CURRENT_LIST_DIR}/process.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/gpu_preferences.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/java.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/memory.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/services.cpp"
    )
    set_target_properties(nickel_foundation PROPERTIES NICKEL_FOUNDATION_PLATFORM_ADDED TRUE)
endif()
