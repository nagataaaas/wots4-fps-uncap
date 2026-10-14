cmake_minimum_required(VERSION 3.25)
project(wots4_mod_fps_uncap LANGUAGES C CXX)

# To build this Mod alone, pass -DWOTS4_SDK_DIR=<path to wots4-mods>
if(NOT COMMAND wots4_add_mod)
    set(WOTS4_SDK_DIR "" CACHE PATH "Path to wots4-mods (the ModLoader)")
    if(NOT EXISTS "${WOTS4_SDK_DIR}/cmake/Wots4.cmake")
        message(FATAL_ERROR "Set WOTS4_SDK_DIR to the path of wots4-mods")
    endif()
    include(${WOTS4_SDK_DIR}/cmake/Wots4.cmake)
    enable_testing()
    set(WOTS4_BUILD_TESTS ON)
endif()

wots4_use_minhook()

wots4_add_mod(fps_uncap SOURCES src/mod.cpp DEF src/mod.def
    LIBS wots4_minhook)
target_include_directories(mod_fps_uncap PRIVATE src)

if(WOTS4_BUILD_TESTS)
    # Checks, with the game's per-update constants as numbers, the values at 30 and 60 updates
    wots4_add_probe(step_values_probe SOURCES tests/step_values_probe.cpp
        INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/src)
    # Loads mod.dll into a copy of the exe and calls the hooked frame limiter in and out of a
    # script (the game is not started)
    set(WOTS4_UNPACKED_EXE "${WOTS4_ROOT}/research/bin/WayOfTheSamurai4.unpacked.exe" CACHE FILEPATH "Decrypted exe")
    file(TO_NATIVE_PATH "${WOTS4_UNPACKED_EXE}" unpacked_native)
    wots4_add_probe(install_native_probe SOURCES tests/install_native_probe.cpp
        LINK_OPTIONS /BASE:0x10000000 /DYNAMICBASE
        ARGS ${unpacked_native} $<TARGET_FILE:mod_fps_uncap> NEEDS_EXE)
    add_dependencies(install_native_probe mod_fps_uncap)
endif()
