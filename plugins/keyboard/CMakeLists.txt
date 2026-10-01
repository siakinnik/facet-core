# Default keyboard plugin, shipped and installed with the core.
add_executable(keyboard main.cpp i18n/ru.cpp)
target_include_directories(keyboard PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(keyboard PRIVATE facet_sdk)
target_compile_options(keyboard PRIVATE -Wall -Wextra)
if(FACET_STATIC)
    target_link_options(keyboard PRIVATE -static -s)
endif()

# <build>/plugins/keyboard: found automatically next to <build>/facet.
set(KEYBOARD_OUT ${CMAKE_BINARY_DIR}/plugins/keyboard)
set_target_properties(keyboard PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${KEYBOARD_OUT})
facet_plugin_manifest(${CMAKE_CURRENT_SOURCE_DIR}/manifest.json ${KEYBOARD_OUT}/manifest.json)

install(TARGETS keyboard RUNTIME DESTINATION lib/facet/plugins/keyboard)
install(FILES ${KEYBOARD_OUT}/manifest.json DESTINATION lib/facet/plugins/keyboard)
