cmake_minimum_required(VERSION 3.28)
project(GB4ME CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wpedantic -Werror")

# compile_commands.json: consumed by clang-tidy (see .clang-tidy) and every
# editor's C++ tooling. Harmless for the build itself.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ccache: auto-detected, speeds up rebuilds. No hard dependency —
# configure with -DCMAKE_CXX_COMPILER_LAUNCHER=ccache to force it.
find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
	set(CMAKE_C_COMPILER_LAUNCHER ${CCACHE_PROGRAM})
	set(CMAKE_CXX_COMPILER_LAUNCHER ${CCACHE_PROGRAM})
endif()

# Default output is bin/ (the project's single canonical binary location).
# Overridable so a release or verification build can go to its own directory
# instead of clobbering the development binary in bin/.
if(NOT CMAKE_RUNTIME_OUTPUT_DIRECTORY)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)
endif()

# Find SDL3, three ways, best first. Either way SDL3_LIBS/SDL3_LIBRARY_DIRS/
# SDL3_CFLAGS_OTHER are set for the one consumer below.
#   1. find_package(SDL3 CONFIG) - the upstream package (what vcpkg and distro
#      -dev packages install).
#   2. pkg-config - for older layouts that ship no CMake config.
#   3. FetchContent - last resort for a machine with neither; builds SDL3 from
#      a pinned tag. This is a real fallback, not a stub, but it needs the
#      network and several minutes, so it is opt-in.
# Overrides: -DGB4ME_SDL3_PKGCONFIG=ON forces (2), -DGB4ME_SDL3_FETCH=ON forces
# (3). Off by default so a normal configure never reaches for the network.
option(GB4ME_SDL3_PKGCONFIG "Force the pkg-config SDL3 lookup" OFF)
option(GB4ME_SDL3_FETCH "Build SDL3 from source when it is not installed" OFF)
set(GB4ME_SDL3_TAG "release-3.2.24" CACHE STRING "SDL3 tag to fetch")

find_package(SDL3 CONFIG QUIET)
if(SDL3_FOUND AND NOT GB4ME_SDL3_PKGCONFIG AND NOT GB4ME_SDL3_FETCH)
    set(SDL3_LIBS SDL3::SDL3)
    set(SDL3_LIBRARY_DIRS "")
    set(SDL3_CFLAGS_OTHER "")
    message(STATUS "SDL3: found via find_package (SDL3::SDL3)")
elseif(NOT GB4ME_SDL3_FETCH)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SDL3 REQUIRED sdl3)
    list(APPEND SDL3_LIBS ${SDL3_LIBRARIES})
    message(STATUS "SDL3: found via pkg-config (${SDL3_VERSION})")
else()
    message(STATUS "SDL3: fetching ${GB4ME_SDL3_TAG} (no system package found)")
    include(FetchContent)
    # Only the parts we do not want to build. SDL_SHARED/SDL_STATIC are left
    # alone on purpose: SDL gates them with cmake_dependent_option, which
    # rewrites the cache entry, so forcing them here silently loses the
    # argument (its SDL3::SDL3 alias then names a target that was never
    # created). Pass -DSDL_SHARED=OFF -DSDL_STATIC_DEFAULT=ON for a static
    # fetch; the alias points at whichever of the two SDL built.
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    set(SDL_TESTS OFF CACHE BOOL "" FORCE)
    set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(SDL3
        GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
        GIT_TAG ${GB4ME_SDL3_TAG}
        GIT_SHALLOW ON)
    FetchContent_MakeAvailable(SDL3)
    set(SDL3_LIBS SDL3::SDL3)
    set(SDL3_LIBRARY_DIRS "")
    set(SDL3_CFLAGS_OTHER "")
endif()

# Find Vulkan
find_package(Vulkan REQUIRED)

# Release builds strip every developer facility: the selftest harness, the
# headless ROM/frame dump tools, all printf tracing and all debug env vars.
# The resulting binary produces NO terminal output of any kind.
#   cmake .. -DCMAKE_BUILD_TYPE=Release -DGB4ME_RELEASE=ON
option(GB4ME_RELEASE "Strip selftest, dumps and all debug output" OFF)
# Keeps the selftest harness in an otherwise-release build, so the suite can be
# run against release flags and prove the stripping changed no behaviour.
option(GB4ME_KEEP_SELFTEST "Keep the selftest in a release build (verification)" OFF)
# AddressSanitizer + UndefinedBehaviorSanitizer for the first-party target only
# (third-party ImGui is built without them, so their warnings stay out of the
# way). Pair with the `dev-asan` CMake preset; the selftest is the intended
# workload. See .github/workflows/ci.yml `sanitizers`.
option(GB4ME_SANITIZE "Build with ASan + UBSan (verification)" OFF)

file(GLOB_RECURSE SOURCES "src/*.cpp" "src/**/*.cpp")
file(GLOB_RECURSE HEADERS "include/*.h" "include/**/*.h" "src/*.h" "src/**/*.h")

if(GB4ME_RELEASE AND NOT GB4ME_KEEP_SELFTEST)
    # Drop the selftest translation unit entirely rather than relying on the
    # linker to strip it, so no test code or format strings reach the binary.
    list(REMOVE_ITEM SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/selftest.cpp")
    list(REMOVE_ITEM HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/src/selftest.h")
endif()

add_executable(${PROJECT_NAME} ${SOURCES} ${HEADERS})

if(GB4ME_RELEASE)
    target_compile_definitions(${PROJECT_NAME} PRIVATE GB4ME_RELEASE=1)
endif()
if(GB4ME_KEEP_SELFTEST)
    target_compile_definitions(${PROJECT_NAME} PRIVATE GB4ME_KEEP_SELFTEST=1)
endif()

target_include_directories(${PROJECT_NAME} PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

if(GB4ME_SANITIZE)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        message(WARNING "GB4ME_SANITIZE: unsupported compiler ${CMAKE_CXX_COMPILER_ID}")
    endif()
    target_compile_options(${PROJECT_NAME} PRIVATE
        -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined)
    target_link_options(${PROJECT_NAME} PRIVATE -fsanitize=address,undefined)
endif()

if(SDL3_LIBRARY_DIRS)
    target_link_directories(${PROJECT_NAME} PRIVATE ${SDL3_LIBRARY_DIRS})
endif()
target_link_libraries(${PROJECT_NAME} PRIVATE ${SDL3_LIBS} ${Vulkan_LIBRARIES})
if(SDL3_CFLAGS_OTHER)
    target_compile_options(${PROJECT_NAME} PRIVATE ${SDL3_CFLAGS_OTHER})
endif()

# Dear ImGui, find-or-fetch (system core when present, pinned source fallback).
# Produces gb4me_imgui; linked now so the wiring stays verified, first panels later.
include(cmake/SetupImGui.cmake)
target_link_libraries(${PROJECT_NAME} PRIVATE gb4me_imgui)

# ctest entry point: the selftest is the project's only test suite. The
# `selftest` / `selftest-asan` presets in CMakePresets.json run it (the latter
# under ASan + UBSan, see GB4ME_SANITIZE).
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/selftest.cpp")
    enable_testing()
    add_test(NAME selftest COMMAND $<TARGET_FILE:${PROJECT_NAME}> --selftest)
endif()

# Installation — only binary; desktop/icon/shaders/assets handled by PKGBUILD
install(TARGETS ${PROJECT_NAME} DESTINATION bin)

# Compile GLSL shaders to SPIR-V with glslc.
# REQUIRED: the renderer loads these .spv files at startup, so a build that
# silently skips them produces a binary that cannot create a window.
find_program(GLSLC glslc REQUIRED)
file(MAKE_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/shaders)
file(GLOB SHADER_SOURCES "shaders/*.vert" "shaders/*.frag")
list(LENGTH SHADER_SOURCES SHADER_SOURCE_COUNT)
if(SHADER_SOURCE_COUNT EQUAL 0)
    message(FATAL_ERROR "No shader sources found in ${CMAKE_CURRENT_SOURCE_DIR}/shaders")
endif()
foreach(SHADER_SRC ${SHADER_SOURCES})
    get_filename_component(SHADER_NAME ${SHADER_SRC} NAME)
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${GLSLC} "${SHADER_SRC}" -o
                "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/shaders/${SHADER_NAME}.spv"
    )
endforeach()

# UI typeface: Cousine (monospace, Apache-2.0) from the pinned ImGui source,
# staged next to the binary like the shaders. The renderer also falls back to
# system fonts (DejaVu/Noto) and finally the built-in 8x8 bitmap, so a missing
# copy degrades gracefully instead of failing the build.
file(MAKE_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/fonts)
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${imgui_SOURCE_DIR}/misc/fonts/Cousine-Regular.ttf"
            "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/fonts/Cousine-Regular.ttf"
)
install(FILES "${imgui_SOURCE_DIR}/misc/fonts/Cousine-Regular.ttf"
    DESTINATION share/GB4ME/fonts)
