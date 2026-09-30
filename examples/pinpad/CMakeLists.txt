# Example plugin: a PIN pad drawn with the canvas widget. Also a template for
# new plugins. Built as part of facet-core (FACET_BUILD_EXAMPLES).
add_executable(pinpad main.cpp i18n/ru.cpp)
target_include_directories(pinpad PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(pinpad PRIVATE facet_sdk)
target_compile_options(pinpad PRIVATE -Wall -Wextra)
if(FACET_STATIC)
    target_link_options(pinpad PRIVATE -static -s)
endif()

# <build>/examples is a plugin search dir: FACET_PLUGIN_PATH=<build>/examples
set(PINPAD_OUT ${CMAKE_BINARY_DIR}/examples/pinpad)
set_target_properties(pinpad PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${PINPAD_OUT})
configure_file(manifest.json ${PINPAD_OUT}/manifest.json COPYONLY)
