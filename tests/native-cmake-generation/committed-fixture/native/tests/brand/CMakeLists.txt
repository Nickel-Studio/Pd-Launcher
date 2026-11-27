include("${CMAKE_CURRENT_LIST_DIR}/brand-resource-input.fragment.cmake")
add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/brand-resource-copy.qrc"
    COMMAND "${CMAKE_COMMAND}" -E copy
        "${_nickel_functional_qrc_path}"
        "${CMAKE_CURRENT_BINARY_DIR}/brand-resource-copy.qrc"
    DEPENDS "${_nickel_functional_qrc_path}"
    VERBATIM
)
add_custom_target(pd_brand_resource_copy ALL
    DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/brand-resource-copy.qrc"
)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/brand-configure-inputs.txt"
    "${_nickel_functional_template}\n${_nickel_functional_qrc_path}\n"
)
