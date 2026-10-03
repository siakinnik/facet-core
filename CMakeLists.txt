cmake_minimum_required(VERSION 3.16)
project(facet VERSION 0.7.1 LANGUAGES CXX)
set(FACET_VERSION_SUFFIX "alpha")  # pre-release tag; empty for final releases

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

option(FACET_WITH_X11 "Build the X11 development backend if libX11 is available" ON)
option(FACET_STATIC "Link fully static (portable release binaries)" OFF)
option(FACET_BUILD_EXAMPLES "Build the example plugins in examples/" ON)

add_subdirectory(sdk)

add_executable(facet
    src/main.cpp
    src/app/app.cpp
    src/app/keyboard.cpp
    src/app/overlays.cpp
    src/app/screens.cpp
    src/core/build_info.cpp
    src/core/config.cpp
    src/core/log.cpp
    src/core/netstatus.cpp
    src/core/timezone.cpp
    src/i18n/i18n.cpp
    src/i18n/ru.cpp
    src/gfx/canvas.cpp
    src/gfx/font.cpp
    src/gfx/raster.cpp
    src/gfx/recorder.cpp
    src/gpu/channel.cpp
    src/gpu/gl_package.cpp
    src/gpu/sha256.cpp
    src/platform/backlight.cpp
    src/platform/evdev_touch.cpp
    src/platform/fbdev.cpp
    src/platform/gpu.cpp
    src/platform/headless.cpp
    src/platform/platform.cpp
    src/plugins/notifications.cpp
    src/plugins/permissions.cpp
    src/plugins/plugin_host.cpp
    src/plugins/sandbox.cpp
    src/ui/canvas_ops.cpp
    src/ui/context.cpp
    src/ui/icons.cpp
    src/ui/theme.cpp
)
target_include_directories(facet PRIVATE src)
if(FACET_VERSION_SUFFIX)
    set(FACET_VERSION_STRING "${PROJECT_VERSION}-${FACET_VERSION_SUFFIX}")
else()
    set(FACET_VERSION_STRING "${PROJECT_VERSION}")
endif()
# Version, git commit, repository and channel, refreshed on every build.
set(FACET_GENERATED ${CMAKE_BINARY_DIR}/generated)
add_custom_target(facet_build_info
    COMMAND ${CMAKE_COMMAND} -DSRC_DIR=${CMAKE_SOURCE_DIR} -DOUT_FILE=${FACET_GENERATED}/build_info_gen.h
            -DVERSION=${FACET_VERSION_STRING} -P ${CMAKE_SOURCE_DIR}/cmake/build_info.cmake
    BYPRODUCTS ${FACET_GENERATED}/build_info_gen.h
    COMMENT "Collecting build info")
add_dependencies(facet facet_build_info)

# Release builds know their OpenGL package (built by scripts/build-gl.sh in the
# same release) by its SHA-256: only exactly that file is installed.
set(FACET_GL_SHA256 "" CACHE STRING "SHA-256 of this release's OpenGL package")
set(FACET_GL_SIZE "0" CACHE STRING "Size of this release's OpenGL package in bytes")
if(FACET_GL_SHA256)
    target_compile_definitions(facet PRIVATE FACET_GL_SHA256="${FACET_GL_SHA256}" FACET_GL_SIZE=${FACET_GL_SIZE})
endif()

# The GPU helper (facet-gpu): dynamically linked against the GPU drivers
# (Mesa), so it is not part of the static core. It ships in the downloadable
# OpenGL package; built when the development files are installed.
option(FACET_GPU_HELPER "Build the GPU helper (needs libdrm, gbm, EGL, GLESv2)" ON)
if(FACET_GPU_HELPER)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(FACET_GL QUIET libdrm gbm egl glesv2)
    endif()
    if(FACET_GL_FOUND)
        add_executable(facet-gpu src/gpu/helper/facet_gpu.cpp src/gpu/channel.cpp sdk/src/json.cpp)
        target_include_directories(facet-gpu PRIVATE src sdk/include ${FACET_GL_INCLUDE_DIRS})
        target_link_libraries(facet-gpu PRIVATE ${FACET_GL_LIBRARIES})
        target_compile_options(facet-gpu PRIVATE -Wall -Wextra)
        message(STATUS "Building the GPU helper (facet-gpu)")
    else()
        message(STATUS "GPU helper not built (libdrm/gbm/EGL/GLESv2 development files missing)")
    endif()
endif()
target_include_directories(facet PRIVATE ${FACET_GENERATED})
target_compile_options(facet PRIVATE -Wall -Wextra)
target_link_libraries(facet PRIVATE facet_sdk pthread)
if(FACET_STATIC)
    target_link_options(facet PRIVATE -static -s)  # stripped: release binaries
endif()

if(FACET_WITH_X11 AND NOT FACET_STATIC)  # no static libX11; release binaries are fbdev-only
    find_package(X11)
    if(X11_FOUND)
        target_sources(facet PRIVATE src/platform/x11.cpp)
        target_compile_definitions(facet PRIVATE FACET_HAVE_X11)
        target_link_libraries(facet PRIVATE X11::X11)
    endif()
endif()

# Bundled plugins, installed with the core.
add_subdirectory(plugins/keyboard)

if(FACET_BUILD_EXAMPLES)
    add_subdirectory(examples/pinpad)
    add_subdirectory(examples/showcase)
endif()

install(TARGETS facet RUNTIME DESTINATION bin)
set(FACET_BIN ${CMAKE_INSTALL_PREFIX}/bin/facet)
configure_file(deploy/facet.service.in ${CMAKE_BINARY_DIR}/facet.service @ONLY)
install(FILES ${CMAKE_BINARY_DIR}/facet.service DESTINATION lib/systemd/system)
install(FILES deploy/facet.env DESTINATION share/facet)
