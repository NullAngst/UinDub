cmake_minimum_required(VERSION 3.22)

# Version comes from `git describe`, falling back when built outside a checkout.
find_package(Git QUIET)
set(UINDUB_VERSION "0.0.0-unknown")
if(GIT_FOUND AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.git")
    execute_process(COMMAND ${GIT_EXECUTABLE} describe --tags --always --dirty
                    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                    OUTPUT_VARIABLE UINDUB_GIT_DESCRIBE OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(UINDUB_GIT_DESCRIBE)
        string(REGEX REPLACE "^v" "" UINDUB_VERSION "${UINDUB_GIT_DESCRIBE}")
    endif()
endif()
if(DEFINED ENV{UINDUB_VERSION})
    set(UINDUB_VERSION "$ENV{UINDUB_VERSION}")
endif()

project(UinDub LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

option(UINDUB_BUILD_GUI "Build the Qt GUI" ON)
option(UINDUB_BUILD_TESTS "Build the test suite" ON)
option(UINDUB_LTO "Link-time optimization" ON)

if(UINDUB_LTO AND CMAKE_BUILD_TYPE STREQUAL "Release")
    include(CheckIPOSupported)
    check_ipo_supported(RESULT ipo_ok OUTPUT ipo_msg)
    if(ipo_ok)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
    endif()
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavcodec libavformat libavfilter libavutil)
pkg_check_modules(ZSTD REQUIRED IMPORTED_TARGET libzstd)
find_package(Threads REQUIRED)

# VapourSynth headers only: the library is opened at run time, so a build
# without the bundled stack still runs and falls back to bwdif.
find_path(VAPOURSYNTH_INCLUDE_DIR NAMES VSScript4.h PATH_SUFFIXES vapoursynth include
          HINTS $ENV{VAPOURSYNTH_INCLUDE} ${VAPOURSYNTH_INCLUDE})
if(NOT VAPOURSYNTH_INCLUDE_DIR)
    message(FATAL_ERROR "VapourSynth headers (VSScript4.h) not found. Set VAPOURSYNTH_INCLUDE to the folder that holds them.")
endif()
message(STATUS "VapourSynth headers: ${VAPOURSYNTH_INCLUDE_DIR}")

# x86-64-v2 is the floor; AVX2 kernels are picked at run time.
add_compile_options(-Wall -Wextra -Wno-unused-parameter -march=x86-64-v2 -mtune=generic)

# --- core: no Qt, linked by the app, the tests and --self-test ---------------
file(GLOB UINDUB_CORE_SOURCES CONFIGURE_DEPENDS src/core/*.cpp)
add_library(uindub_core STATIC
    ${UINDUB_CORE_SOURCES}
    src/shared/Json.cpp
    src/shared/Checksums.cpp
)
target_include_directories(uindub_core PUBLIC src)
target_include_directories(uindub_core SYSTEM PUBLIC ${VAPOURSYNTH_INCLUDE_DIR})
target_compile_definitions(uindub_core PUBLIC UINDUB_VERSION="${UINDUB_VERSION}")
target_link_libraries(uindub_core PUBLIC PkgConfig::FFMPEG PkgConfig::ZSTD Threads::Threads ${CMAKE_DL_LIBS})

# --- GUI ---------------------------------------------------------------------
if(UINDUB_BUILD_GUI)
    find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets OpenGL OpenGLWidgets)
    file(GLOB UINDUB_THEMES RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} src/ui/themes/*.theme)
    file(GLOB UINDUB_UI_SOURCES CONFIGURE_DEPENDS src/ui/*.cpp)
    add_library(uindub_ui STATIC
        ${UINDUB_UI_SOURCES}
        src/shared/Theme.cpp
        src/shared/Scopes.cpp
        src/shared/Histogram.cpp
    )
    set_target_properties(uindub_ui PROPERTIES AUTOMOC ON)
    target_include_directories(uindub_ui PUBLIC src)
    target_link_libraries(uindub_ui PUBLIC uindub_core Qt6::Core Qt6::Gui Qt6::Widgets Qt6::OpenGL Qt6::OpenGLWidgets)
    qt_add_executable(uindub src/main.cpp)
    qt_add_resources(uindub "themes"
        PREFIX "/"
        BASE src/ui
        FILES ${UINDUB_THEMES} src/ui/themes/style.qss)
    qt_add_resources(uindub "icons"
        PREFIX "/"
        BASE packaging
        FILES packaging/uindub.svg)
    target_link_libraries(uindub PRIVATE uindub_ui)
    set_target_properties(uindub PROPERTIES OUTPUT_NAME uindub AUTOMOC ON AUTORCC ON)
    install(TARGETS uindub RUNTIME DESTINATION bin)
    install(FILES packaging/uindub.desktop DESTINATION share/applications RENAME io.github.nullangst.UinDub.desktop)
    install(FILES packaging/uindub.svg DESTINATION share/icons/hicolor/scalable/apps RENAME io.github.nullangst.UinDub.svg)
    install(FILES packaging/io.github.nullangst.UinDub.metainfo.xml DESTINATION share/metainfo)
endif()

# --- tests -------------------------------------------------------------------
if(UINDUB_BUILD_TESTS)
    enable_testing()
    add_library(uindub_testutil INTERFACE)
    target_include_directories(uindub_testutil INTERFACE tests src)
    target_link_libraries(uindub_testutil INTERFACE uindub_core)
    set(UINDUB_TESTS neutral jitter_synth real_motion dark occlusion cut subpixel field_order pingdub_inputs output color
        offsets_store cuts qtgmc pal warble)
    foreach(t ${UINDUB_TESTS})
        if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_${t}.cpp)
            message(WARNING "tests/test_${t}.cpp is missing")
            continue()
        endif()
        add_executable(test_${t} tests/test_${t}.cpp)
        target_link_libraries(test_${t} PRIVATE uindub_testutil)
        add_test(NAME ${t} COMMAND test_${t})
        set_tests_properties(${t} PROPERTIES TIMEOUT 1800)
    endforeach()
endif()
