# GBA unit tests (Phase 1). Self-contained, no external framework — the
# repo avoids extra dependencies, so each test is a plain executable
# returning 0 on success (mirrors the --debug exit-code convention).

function(gba_test name)
    add_executable(${name} gba/${name}.cpp ${ARGN})
    target_include_directories(${name} PRIVATE
        "${CMAKE_SOURCE_DIR}/include"
        "${CMAKE_SOURCE_DIR}/src"
    )
    add_test(NAME ${name} COMMAND ${name})
endfunction()

gba_test(test_gba_cartridge "${CMAKE_SOURCE_DIR}/src/gba/cartridge.cpp")
gba_test(test_gba_bus "${CMAKE_SOURCE_DIR}/src/gba/bus.cpp")
gba_test(test_gba_scheduler "${CMAKE_SOURCE_DIR}/src/gba/scheduler.cpp")
