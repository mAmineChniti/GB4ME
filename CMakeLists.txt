cmake_minimum_required(VERSION 3.28)
project(GB4ME CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wpedantic -Werror")

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Find SDL3
find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL3 REQUIRED sdl3)

# Find Vulkan
find_package(Vulkan REQUIRED)

file(GLOB_RECURSE SOURCES "src/*.cpp" "src/**/*.cpp")
file(GLOB_RECURSE HEADERS "include/*.h" "include/**/*.h" "src/*.h" "src/**/*.h")

add_executable(${PROJECT_NAME} ${SOURCES} ${HEADERS})

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
find_program(GLSLC glslc)
file(MAKE_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/shaders)
if(GLSLC)
    file(GLOB SHADER_SOURCES "shaders/*.vert" "shaders/*.frag")
    foreach(SHADER_SRC ${SHADER_SOURCES})
        get_filename_component(SHADER_NAME ${SHADER_SRC} NAME)
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND ${GLSLC} "${SHADER_SRC}" -o
                    "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/shaders/${SHADER_NAME}.spv"
        )
    endforeach()
else()
    message(WARNING "glslc not found; shaders will not be compiled")
endif()