# Example plugin: video (mpv) on a free-form screen with its own controls over
# it. A template for modules that play video. Built with facet-core.
add_executable(video main.cpp i18n/ru.cpp)
target_include_directories(video PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(video PRIVATE facet_sdk)
target_compile_options(video PRIVATE -Wall -Wextra)
if(FACET_STATIC)
    target_link_options(video PRIVATE -static -s)
endif()

set(VIDEO_OUT ${CMAKE_BINARY_DIR}/examples/video)
set_target_properties(video PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${VIDEO_OUT})
facet_plugin_manifest(${CMAKE_CURRENT_SOURCE_DIR}/manifest.json ${VIDEO_OUT}/manifest.json)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/logo.png ${VIDEO_OUT}/logo.png COPYONLY)
