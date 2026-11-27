add_library(nickel_minecraft STATIC
    java.cpp
    memory.cpp
    rules.cpp
    maven.cpp
    loader_policy.cpp
    metadata.cpp
    services.cpp
    runtime.cpp
    catalog.cpp
    loader.cpp
    fullinstaller.cpp
    launch_plan.cpp
    launcher.cpp
)
target_compile_features(nickel_minecraft PUBLIC cxx_std_20)
target_include_directories(nickel_minecraft PUBLIC "${CMAKE_CURRENT_LIST_DIR}/../..")
target_link_libraries(nickel_minecraft PUBLIC nickel_common)
if(TARGET nickel_foundation AND TARGET nickel_transport)
    target_sources(nickel_minecraft PRIVATE services_native.cpp)
    target_link_libraries(nickel_minecraft PUBLIC nickel_foundation nickel_transport)
    target_compile_definitions(nickel_minecraft PUBLIC NICKEL_MINECRAFT_NATIVE_SERVICES=1)
elseif(NICKEL_MINECRAFT_REQUIRE_NATIVE_SERVICES)
    message(FATAL_ERROR "Minecraft production services require nickel_foundation and nickel_transport")
endif()
if(MSVC)
    target_compile_options(nickel_minecraft PRIVATE /W4 /permissive- /utf-8 /EHsc)
    set_property(TARGET nickel_minecraft PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
    target_compile_options(nickel_minecraft PRIVATE -Wall -Wextra -Wpedantic)
endif()
