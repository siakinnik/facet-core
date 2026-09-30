# facet_sdk: JSON, i18n and the plugin protocol client. Used by the core and by plugins
# (add_subdirectory(<facet-core>/sdk facet_sdk) from a plugin project).
cmake_minimum_required(VERSION 3.16)

if(NOT TARGET facet_sdk)
    add_library(facet_sdk STATIC
        src/i18n.cpp
        src/json.cpp
        src/plugin.cpp
    )
    target_include_directories(facet_sdk PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_compile_features(facet_sdk PUBLIC cxx_std_17)
endif()
