add_library(nickel_common INTERFACE)
target_compile_features(nickel_common INTERFACE cxx_std_20)
target_include_directories(nickel_common INTERFACE "${CMAKE_CURRENT_LIST_DIR}/../..")
target_link_libraries(nickel_common INTERFACE nlohmann_json::nlohmann_json)
