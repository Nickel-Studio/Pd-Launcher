if(NOT TARGET nickel_transport)
    message(FATAL_ERROR "Add core/network before core/tasks; nickel_transport owns the local source aggregate")
endif()
target_sources(nickel_transport PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/tasks.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/secure_fs.cpp"
)
