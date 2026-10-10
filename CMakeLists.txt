cmake_minimum_required(VERSION 3.25)
project(wots4_mod_smooth_fps LANGUAGES C CXX)

# 単体でビルドするときは -DWOTS4_SDK_DIR=<wots4-mods のパス> を渡す
if(NOT COMMAND wots4_add_mod)
    set(WOTS4_SDK_DIR "" CACHE PATH "wots4-mods（ModLoader）のパス")
    if(NOT EXISTS "${WOTS4_SDK_DIR}/cmake/Wots4.cmake")
        message(FATAL_ERROR "WOTS4_SDK_DIR に wots4-mods のパスを指定してください")
    endif()
    include(${WOTS4_SDK_DIR}/cmake/Wots4.cmake)
    enable_testing()
    set(WOTS4_BUILD_TESTS ON)
endif()

# まだ中身はない。配布はしない。
wots4_add_mod(smooth_fps SOURCES src/mod.cpp DEF src/mod.def NO_RELEASE)
