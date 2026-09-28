# Dear ImGui, find-or-fetch.
#
# Prefers the system package and falls back to a pinned FetchContent tag, so
# fresh machines and offline builds (FETCHCONTENT_SOURCE_DIR_IMGUI) keep
# working. Distro packages never ship the SDL3/Vulkan backends, so the backend
# sources ALWAYS come from the fetched tree; when the system core is used the
# fetch tag is derived from the system headers to keep core and backends at
# the exact same version.
#
# Provides: gb4me_imgui (static lib, core + SDL3/Vulkan backends).
# Options:  GB4ME_USE_SYSTEM_IMGUI (default ON), GB4ME_IMGUI_TAG (fallback tag).

option(GB4ME_USE_SYSTEM_IMGUI "Link the system Dear ImGui core when present (backends always built from pinned source)" ON)
set(GB4ME_IMGUI_TAG "v1.92.9" CACHE STRING "ImGui git tag used for FetchContent")

if(GB4ME_USE_SYSTEM_IMGUI)
	find_package(imgui CONFIG QUIET)
endif()

if(imgui_FOUND)
	# Derive the fetch tag from the system headers so backends match the core.
	find_path(GB4ME_IMGUI_INCLUDE_DIR NAMES imgui.h PATH_SUFFIXES imgui NO_CACHE)
	if(GB4ME_IMGUI_INCLUDE_DIR)
		file(STRINGS "${GB4ME_IMGUI_INCLUDE_DIR}/imgui.h" _imgui_ver_line
			REGEX "#define IMGUI_VERSION ")
		string(REGEX MATCH "[0-9]+\\.[0-9]+\\.[0-9]+" _imgui_ver "${_imgui_ver_line}")
		if(_imgui_ver)
			set(GB4ME_IMGUI_TAG "v${_imgui_ver}")
		endif()
	endif()
	message(STATUS "GB4ME: using system imgui core, backends from tag ${GB4ME_IMGUI_TAG}")
else()
	message(STATUS "GB4ME: fetching imgui ${GB4ME_IMGUI_TAG}")
endif()

include(FetchContent)
FetchContent_Declare(imgui
	GIT_REPOSITORY https://github.com/ocornut/imgui.git
	GIT_TAG ${GB4ME_IMGUI_TAG}
	GIT_SHALLOW TRUE
	EXCLUDE_FROM_ALL)
FetchContent_MakeAvailable(imgui)

set(_imgui_backends
	${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
	${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp)

if(imgui_FOUND)
	# System core + freshly built backends (version-matched by construction).
	add_library(gb4me_imgui STATIC ${_imgui_backends})
	target_link_libraries(gb4me_imgui PUBLIC imgui::imgui)
else()
	add_library(gb4me_imgui STATIC
		${imgui_SOURCE_DIR}/imgui.cpp
		${imgui_SOURCE_DIR}/imgui_draw.cpp
		${imgui_SOURCE_DIR}/imgui_tables.cpp
		${imgui_SOURCE_DIR}/imgui_widgets.cpp
		${_imgui_backends})
endif()

target_include_directories(gb4me_imgui PUBLIC
	${imgui_SOURCE_DIR}
	${imgui_SOURCE_DIR}/backends)
# Backend sources need the SDL3/Vulkan headers (found by the top-level build).
target_include_directories(gb4me_imgui PRIVATE ${SDL3_INCLUDE_DIRS} ${Vulkan_INCLUDE_DIRS})
# Third-party code: keep its warnings visible but never fail the build on them
# (the project builds its own sources with -Werror).
target_compile_options(gb4me_imgui PRIVATE -Wno-error -Wno-pedantic)
