cmake_minimum_required(VERSION 3.20)
project(thistle LANGUAGES C CXX)

# Emit compile_commands.json so editors (clangd / VS Code C++ / CLion) get full
# IntelliSense — autocomplete and signatures for the whole thistle API.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

# Debug build: enables the in-app draggable log overlay. Configure a consumer
# project with -DTHISTLE_DEBUG=ON to turn it on.
option(THISTLE_DEBUG "Build with the in-app debug log overlay" OFF)

include(FetchContent)

# sokol — windowing + graphics (header-only).
FetchContent_Declare(
    sokol
    GIT_REPOSITORY https://github.com/floooh/sokol.git
    GIT_TAG        master
)
# stb — image loading (header-only).
FetchContent_Declare(
    stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG        master
)
# miniaudio — audio playback (header-only).
FetchContent_Declare(
    miniaudio
    GIT_REPOSITORY https://github.com/mackron/miniaudio.git
    GIT_TAG        master
)
# fontstash — TTF text rendering (used via sokol_fontstash.h).
FetchContent_Declare(
    fontstash
    GIT_REPOSITORY https://github.com/memononen/fontstash.git
    GIT_TAG        master
)
# Box2D — 2D physics (real CMake library target).
set(BOX2D_BUILD_UNIT_TESTS OFF CACHE BOOL "" FORCE)
set(BOX2D_BUILD_TESTBED OFF CACHE BOOL "" FORCE)
set(BOX2D_BUILD_DOCS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    box2d
    GIT_REPOSITORY https://github.com/erincatto/box2d.git
    GIT_TAG        v2.4.1
)
# nlohmann/json (for Tiled .tmj tilemaps) is vendored as a single header in
# third_party/nlohmann/json.hpp — the full repo is huge, so we don't fetch it.
# We only use miniaudio's header (our audio_impl.c holds the implementation), but
# MakeAvailable is the non-deprecated path (FetchContent_Populate is a hard error
# under newer CMake, e.g. via emcmake); its extra targets just build unused.
FetchContent_MakeAvailable(sokol stb fontstash box2d miniaudio)

add_library(thistle STATIC
    src/thistle.cpp
    src/sokol_impl.c
    src/stb_impl.c
    src/audio_impl.c
    src/text_impl.c
)

target_include_directories(thistle
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${sokol_SOURCE_DIR}
        ${sokol_SOURCE_DIR}/util
        ${stb_SOURCE_DIR}
        ${miniaudio_SOURCE_DIR}
        ${fontstash_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party
)

target_link_libraries(thistle PUBLIC box2d)

if(THISTLE_DEBUG)
    target_compile_definitions(thistle PUBLIC THISTLE_DEBUG)
endif()

if(APPLE)
    # sokol's Metal backend must be compiled as Objective-C.
    set_source_files_properties(src/sokol_impl.c PROPERTIES COMPILE_FLAGS "-x objective-c")
    # miniaudio's iOS backend touches AVAudioSession, so compile it as Obj-C too.
    set_source_files_properties(src/audio_impl.c PROPERTIES COMPILE_FLAGS "-x objective-c")
    target_sources(thistle PRIVATE src/ios_support.mm)

    if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
        target_link_libraries(thistle PRIVATE
            "-framework Foundation"
            "-framework UIKit"
            "-framework Metal"
            "-framework MetalKit"
            "-framework QuartzCore"
            "-framework CoreGraphics"
            "-framework AudioToolbox"
            "-framework AVFoundation"
            "-framework GameController"
        )
    else()
        target_link_libraries(thistle PRIVATE
            "-framework Cocoa"
            "-framework QuartzCore"
            "-framework Metal"
            "-framework MetalKit"
            "-framework AudioToolbox"
            "-framework AudioUnit"
            "-framework CoreAudio"
            "-framework CoreFoundation"
            "-framework GameController"
        )
    endif()
elseif(WIN32)
    # sokol's #pragma comment(lib, ...) auto-linking is an MSVC-ism that MinGW's
    # linker doesn't honor, so d3d11/dxgi need to be listed explicitly here (MSVC
    # builds just get them twice, which is harmless).
    target_sources(thistle PRIVATE src/win32_support.cpp)
    target_link_libraries(thistle PRIVATE winhttp d3d11 dxgi)
elseif(EMSCRIPTEN)
    # sokol uses WebGL2 (SOKOL_GLES3); the executable sets the emcc link flags.
    # Nothing to link into the engine library itself here.
elseif(ANDROID)
    target_link_libraries(thistle PRIVATE
        GLESv3 EGL android log OpenSLES
    )
elseif(UNIX)
    find_package(OpenGL REQUIRED)
    target_link_libraries(thistle PRIVATE
        OpenGL::GL X11 Xi Xcursor GL EGL dl m pthread
    )
endif()

# A minimal executable that links the engine into a real program (window +
# one frame of drawing). Off by default — this isn't part of any game, it's
# just how a platform (e.g. Windows) actually gets verified end-to-end,
# since a static library alone doesn't prove an executable can link against it.
option(THISTLE_BUILD_SMOKETEST "Build a tiny example executable for manual platform verification" OFF)
if(THISTLE_BUILD_SMOKETEST)
    add_executable(thistle_smoketest examples/smoketest.cpp)
    target_link_libraries(thistle_smoketest PRIVATE thistle)
    if(WIN32)
        set_target_properties(thistle_smoketest PROPERTIES WIN32_EXECUTABLE ON)
        if(MSVC)
            # WIN32_EXECUTABLE (subsystem:windows) makes link.exe look for
            # WinMain, but SOKOL_NO_ENTRY (sokol_impl.c) means we provide a
            # plain main() ourselves — point the linker at the CRT startup
            # that calls main() instead.
            target_link_options(thistle_smoketest PRIVATE "/ENTRY:mainCRTStartup")
        endif()
    endif()
endif()
