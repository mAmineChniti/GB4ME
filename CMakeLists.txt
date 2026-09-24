cmake_minimum_required(VERSION 3.28)
project(GB4ME CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wpedantic -Werror")

# Default output is bin/ (the project's single canonical binary location).
# Overridable so a release or verification build can go to its own directory
# instead of clobbering the development binary in bin/.
if(NOT CMAKE_RUNTIME_OUTPUT_DIRECTORY)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)
endif()

# Find SDL3
find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL3 REQUIRED sdl3)

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

target_link_directories(${PROJECT_NAME} PRIVATE ${SDL3_LIBRARY_DIRS})
target_link_libraries(${PROJECT_NAME} PRIVATE ${SDL3_LIBRARIES} ${Vulkan_LIBRARIES})
target_compile_options(${PROJECT_NAME} PRIVATE ${SDL3_CFLAGS_OTHER})

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
