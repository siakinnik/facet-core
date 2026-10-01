# facet_sdk: JSON, i18n and the plugin protocol client. Used by the core and by plugins
# (add_subdirectory(<facet-core>/sdk facet_sdk) from a plugin project).
cmake_minimum_required(VERSION 3.16)

if(NOT TARGET facet_sdk)
    # The SDK version is the core version it comes with ("0.3.0-alpha").
    file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/../CMakeLists.txt _facet_project REGEX "^project\\(facet VERSION")
    file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/../CMakeLists.txt _facet_suffix REGEX "^set\\(FACET_VERSION_SUFFIX")
    string(REGEX MATCH "VERSION ([0-9.]+)" _m "${_facet_project}")
    set(_facet_sdk_version "${CMAKE_MATCH_1}")
    string(REGEX MATCH "\"([^\"]*)\"" _m "${_facet_suffix}")
    if(CMAKE_MATCH_1)
        set(_facet_sdk_version "${_facet_sdk_version}-${CMAKE_MATCH_1}")
    endif()
    set(FACET_SDK_VERSION "${_facet_sdk_version}" CACHE INTERNAL "Version of the Facet plugin SDK")

    add_library(facet_sdk STATIC
        src/i18n.cpp
        src/i18n/keyboard_layouts.cpp
        src/keyboard.cpp
        src/json.cpp
        src/plugin.cpp
    )
    target_include_directories(facet_sdk PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_include_directories(facet_sdk PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_features(facet_sdk PUBLIC cxx_std_17)
    target_compile_definitions(facet_sdk PRIVATE FACET_SDK_VERSION="${FACET_SDK_VERSION}")
endif()

# facet_plugin_manifest(<manifest.json> <output file>)
# Copies a plugin manifest and records the SDK it is built with ("sdk"), so
# the core can tell which SDK an incompatible plugin was made for.
function(facet_plugin_manifest input output)
    file(READ "${input}" _json)
    string(REGEX REPLACE "\n?[ \t]*\"sdk\"[ \t]*:[ \t]*\"[^\"]*\",?" "" _json "${_json}")
    string(REGEX REPLACE "^([^{]*){" "\\1{\n  \"sdk\": \"${FACET_SDK_VERSION}\"," _json "${_json}")
    file(WRITE "${output}.tmp" "${_json}")
    configure_file("${output}.tmp" "${output}" COPYONLY)
    file(REMOVE "${output}.tmp")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${input}")
endfunction()
