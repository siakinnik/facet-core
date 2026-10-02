# Example plugin: notifications, a call, a transient permission, badge, wake
# lock and background work (API 3). Built as part of facet-core (FACET_BUILD_EXAMPLES).
add_executable(showcase main.cpp i18n/ru.cpp)
target_include_directories(showcase PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(showcase PRIVATE facet_sdk)
target_compile_options(showcase PRIVATE -Wall -Wextra)
if(FACET_STATIC)
    target_link_options(showcase PRIVATE -static -s)
endif()

set(SHOWCASE_OUT ${CMAKE_BINARY_DIR}/examples/showcase)
set_target_properties(showcase PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${SHOWCASE_OUT})
facet_plugin_manifest(${CMAKE_CURRENT_SOURCE_DIR}/manifest.json ${SHOWCASE_OUT}/manifest.json)
