set(_resource_source "${CMAKE_SOURCE_DIR}/native/assets/brand/functional-resources.qrc")
file(READ "${_resource_source}" _resource_contents)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/functional-resources.qrc" "${_resource_contents}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_resource_source}")

add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/app-resource-copy.qrc"
    COMMAND "${CMAKE_COMMAND}" -E copy
        "${CMAKE_CURRENT_BINARY_DIR}/functional-resources.qrc"
        "${CMAKE_CURRENT_BINARY_DIR}/app-resource-copy.qrc"
    DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/functional-resources.qrc"
    VERBATIM
)
add_custom_target(pd_app_fixture ALL
    DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/app-resource-copy.qrc"
)
