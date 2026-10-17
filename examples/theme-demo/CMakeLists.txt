# Example theme: a module that is only data (no "exec"). The generator runs at
# build time and writes the theme file next to the manifest. Also a template
# for new themes. Built as part of facet-core (FACET_BUILD_EXAMPLES).
add_executable(theme-demo main.cpp)
target_link_libraries(theme-demo PRIVATE facet_sdk)
target_compile_options(theme-demo PRIVATE -Wall -Wextra)

# <build>/examples is a plugin search dir: FACET_PLUGIN_PATH=<build>/examples
set(THEME_DEMO_OUT ${CMAKE_BINARY_DIR}/examples/theme-demo)
file(MAKE_DIRECTORY ${THEME_DEMO_OUT})
facet_plugin_manifest(${CMAKE_CURRENT_SOURCE_DIR}/manifest.json ${THEME_DEMO_OUT}/manifest.json)
add_custom_command(
    OUTPUT ${THEME_DEMO_OUT}/aurora.json
    COMMAND theme-demo ${THEME_DEMO_OUT}/aurora.json
    DEPENDS theme-demo
    COMMENT "Writing aurora.json")
add_custom_target(theme-demo-file ALL DEPENDS ${THEME_DEMO_OUT}/aurora.json)
