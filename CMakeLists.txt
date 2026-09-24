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

# Gets an assets/ directory next to your executable, on every platform, so
# relative paths like "assets/font.ttf" resolve the same way whether you're
# on macOS, Windows, or Linux. Call once per executable target, after
# add_executable() — see tools/thistle-cli's generated CMakeLists.txt for the
# canonical example. Defined here (not in a consumer project) specifically so
# every project that adds this engine as a subdirectory gets it for free,
# instead of every game hand-rolling its own per-platform bundling logic —
# which is exactly the gap that existed before this: only the Apple half of
# asset-copying had ever been written anywhere in this project's history.
function(thistle_bundle_assets TARGET ASSETS_DIR)
    if(NOT IS_DIRECTORY "${ASSETS_DIR}")
        message(WARNING "thistle_bundle_assets: '${ASSETS_DIR}' is not a directory, skipping")
        return()
    endif()

    if(APPLE)
        # MACOSX_PACKAGE_LOCATION only takes effect on a bundle target
        # (add_executable(... MACOSX_BUNDLE ...) on macOS, or any iOS app
        # target) — a plain non-bundle executable ignores it harmlessly.
        file(GLOB_RECURSE _thistle_asset_files "${ASSETS_DIR}/*")
        foreach(_thistle_asset_file ${_thistle_asset_files})
            file(RELATIVE_PATH _thistle_asset_rel "${ASSETS_DIR}" "${_thistle_asset_file}")
            get_filename_component(_thistle_asset_reldir "${_thistle_asset_rel}" DIRECTORY)
            set_source_files_properties("${_thistle_asset_file}" PROPERTIES
                MACOSX_PACKAGE_LOCATION "Resources/assets/${_thistle_asset_reldir}")
        endforeach()
        target_sources(${TARGET} PRIVATE ${_thistle_asset_files})
    else()
        # Windows/Linux: a plain post-build copy next to the executable.
        add_custom_command(TARGET ${TARGET} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                    "${ASSETS_DIR}" "$<TARGET_FILE_DIR:${TARGET}>/assets"
            COMMENT "thistle_bundle_assets: copying assets next to ${TARGET}"
        )
    endif()
endfunction()

include(FetchContent)

# sokol — windowing + graphics (header-only). Pinned, unlike the other
# header-only deps: the 3D renderer's shaders are pre-compiled by sokol-shdc
# into src/shaders/*.glsl.h, and that generated code has to match the
# sg_shader_desc layout of the exact sokol_gfx.h it's built against. Bump
# this together with SOKOL_TOOLS_COMMIT in tools/shaders/build_shaders.py,
# then rerun that script.
FetchContent_Declare(
    sokol
    GIT_REPOSITORY https://github.com/floooh/sokol.git
    GIT_TAG        2e75443dbd4940b5aa8d76a8e479f8e4b270b9a3
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
# cgltf — glTF 2.0 / GLB parsing for three::load_model() (header-only).
FetchContent_Declare(
    cgltf
    GIT_REPOSITORY https://github.com/jkuhlmann/cgltf.git
    GIT_TAG        v1.15
)
# Jolt Physics — opt-in 3D rigid bodies for three::Physics3D. Off by default
# (it's a big library: a few minutes of extra first build). Turn it on with
# -DTHISTLE_PHYSICS3D=ON, or `thistle enable physics3d` in a CLI project,
# which sets "modules": {"physics3d": true} in thistle.json. Off, Physics3D still
# compiles and links; it just logs that it's disabled and does nothing.
option(THISTLE_PHYSICS3D "Build three::Physics3D on Jolt Physics" OFF)
if(THISTLE_PHYSICS3D)
    # Jolt's own defaults that would break or surprise a consumer build:
    # a static MSVC runtime (/MT) that won't link with ours (/MD), LTO,
    # AVX-512 (binaries that crash on many players' CPUs), and its tests/samples.
    set(USE_STATIC_MSVC_RUNTIME_LIBRARY OFF CACHE BOOL "" FORCE)
    set(INTERPROCEDURAL_OPTIMIZATION OFF CACHE BOOL "" FORCE)
    set(USE_AVX512 OFF CACHE BOOL "" FORCE)
    # Jolt's defaults also assume AVX2/FMA (Haswell, 2013+), and its flags
    # for that reach every engine source that links it. Some CPUs still in
    # use (older Pentiums/Celerons, some VMs) lack them, and a game built
    # that way crashes on start there. SSE4.2 runs everywhere that matters;
    # these aren't FORCEd, so -DUSE_AVX2=ON etc. still opts back in.
    set(USE_AVX OFF CACHE BOOL "")
    set(USE_AVX2 OFF CACHE BOOL "")
    set(USE_LZCNT OFF CACHE BOOL "")
    set(USE_TZCNT OFF CACHE BOOL "")
    set(USE_F16C OFF CACHE BOOL "")
    set(USE_FMADD OFF CACHE BOOL "")
    set(ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    set(TARGET_UNIT_TESTS OFF CACHE BOOL "" FORCE)
    set(TARGET_HELLO_WORLD OFF CACHE BOOL "" FORCE)
    set(TARGET_PERFORMANCE_TEST OFF CACHE BOOL "" FORCE)
    set(TARGET_SAMPLES OFF CACHE BOOL "" FORCE)
    set(TARGET_VIEWER OFF CACHE BOOL "" FORCE)
    set(GENERATE_DEBUG_SYMBOLS OFF CACHE BOOL "" FORCE)
    # Jolt 5.x also builds GPU compute backends (needing the DX12/Vulkan/
    # Metal SDKs), treats every warning as an error on whatever compiler
    # you have, traps floating-point exceptions, and keeps its profiler and
    # debug renderer in release builds. None of that belongs in a game build.
    set(JPH_USE_DX12 OFF CACHE BOOL "" FORCE)
    set(JPH_USE_VK OFF CACHE BOOL "" FORCE)
    set(JPH_USE_MTL OFF CACHE BOOL "" FORCE)
    set(ENABLE_ALL_WARNINGS OFF CACHE BOOL "" FORCE)
    set(OVERRIDE_CXX_FLAGS OFF CACHE BOOL "" FORCE)
    set(FLOATING_POINT_EXCEPTIONS_ENABLED OFF CACHE BOOL "" FORCE)
    set(DEBUG_RENDERER_IN_DEBUG_AND_RELEASE OFF CACHE BOOL "" FORCE)
    set(PROFILER_IN_DEBUG_AND_RELEASE OFF CACHE BOOL "" FORCE)
    set(ENABLE_OBJECT_STREAM OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        JoltPhysics
        GIT_REPOSITORY https://github.com/jrouwe/JoltPhysics.git
        GIT_TAG        v5.6.0
        SOURCE_SUBDIR  Build
    )
    FetchContent_MakeAvailable(JoltPhysics)
endif()

# Dear ImGui — opt-in debug windows (thistle::debug_ui_*). Off by default;
# -DTHISTLE_DEBUG_UI=ON, or `thistle enable debug_ui` in a CLI project. The
# pinned sokol's sokol_imgui.h needs ImGui 1.92+ (the texture API changed).
option(THISTLE_DEBUG_UI "Build the Dear ImGui debug UI" OFF)
if(THISTLE_DEBUG_UI)
    FetchContent_Declare(
        imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG        v1.92.9b
    )
    FetchContent_MakeAvailable(imgui)
endif()

# nlohmann/json (for Tiled .tmj tilemaps) is vendored as a single header in
# third_party/nlohmann/json.hpp — the full repo is huge, so we don't fetch it.
# We only use miniaudio's header (our audio_impl.c holds the implementation), but
# MakeAvailable is the non-deprecated path (FetchContent_Populate is a hard error
# under newer CMake, e.g. via emcmake); its extra targets just build unused.
FetchContent_MakeAvailable(sokol stb fontstash box2d miniaudio cgltf)

add_library(thistle STATIC
    src/thistle.cpp
    src/three_math.cpp
    src/three_camera.cpp
    src/three_mesh.cpp
    src/three_render.cpp
    src/three_model.cpp
    src/three_voxel.cpp
    src/three_collision.cpp
    src/three_particles.cpp
    src/three_noise.cpp
    src/three_terrain.cpp
    src/three_collider.cpp
    src/three_destruction.cpp
    src/three_audio.cpp
    src/cgltf_impl.c
    src/sokol_impl.c
    src/stb_impl.c
    src/audio_impl.c
    src/text_impl.c
)

target_include_directories(thistle
    # third_party (nlohmann/json) is PUBLIC because the networking API's
    # NetArgs type is nlohmann::json — consumers including thistle.hpp need
    # it on their include path too, not just the engine's own .cpp files.
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/third_party
    PRIVATE
        ${sokol_SOURCE_DIR}
        ${sokol_SOURCE_DIR}/util
        ${stb_SOURCE_DIR}
        ${miniaudio_SOURCE_DIR}
        ${fontstash_SOURCE_DIR}/src
        ${cgltf_SOURCE_DIR}
)

target_link_libraries(thistle PUBLIC box2d)

if(THISTLE_DEBUG_UI)
    target_sources(thistle PRIVATE
        src/debug_ui.cpp
        ${imgui_SOURCE_DIR}/imgui.cpp
        ${imgui_SOURCE_DIR}/imgui_draw.cpp
        ${imgui_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_SOURCE_DIR}/imgui_widgets.cpp
        ${imgui_SOURCE_DIR}/imgui_demo.cpp) # ImGui::ShowDemoWindow(): the best reference for what it can do
    # PUBLIC: games #include <imgui.h> themselves.
    target_include_directories(thistle PUBLIC ${imgui_SOURCE_DIR})
    target_compile_definitions(thistle PUBLIC THISTLE_DEBUG_UI=1)
else()
    target_sources(thistle PRIVATE src/debug_ui_off.cpp)
endif()

if(THISTLE_PHYSICS3D)
    target_sources(thistle PRIVATE src/three_physics.cpp)
    target_link_libraries(thistle PRIVATE Jolt)
    # PUBLIC so games can #if THISTLE_PHYSICS3D around physics-only code.
    target_compile_definitions(thistle PUBLIC THISTLE_PHYSICS3D=1)
else()
    target_sources(thistle PRIVATE src/three_physics_off.cpp)
endif()

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
            "-framework StoreKit"
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
            "-framework StoreKit"
            "-framework GameController"
        )
    endif()
elseif(WIN32)
    # sokol's #pragma comment(lib, ...) auto-linking is an MSVC-ism that MinGW's
    # linker doesn't honor, so d3d11/dxgi need to be listed explicitly here (MSVC
    # builds just get them twice, which is harmless).
    target_sources(thistle PRIVATE src/win32_support.cpp)
    target_link_libraries(thistle PRIVATE winhttp d3d11 dxgi ws2_32 dbghelp user32 xinput)
elseif(EMSCRIPTEN)
    # sokol uses WebGL2 (SOKOL_GLES3); the executable sets the emcc link flags.
    # Nothing to link into the engine library itself here.
elseif(ANDROID)
    target_link_libraries(thistle PRIVATE
        GLESv3 EGL android log OpenSLES
    )
elseif(UNIX)
    target_sources(thistle PRIVATE src/linux_gamepad.cpp)
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

    # The thistle::three renderer, visually: lit shapes, custom meshes,
    # transparency, 2D before/after 3D, a second viewport. Opens a window
    # and never exits on its own, same as thistle_smoketest above.
    add_executable(thistle_three_smoketest examples/three_smoketest.cpp)
    target_link_libraries(thistle_three_smoketest PRIVATE thistle)
    if(WIN32)
        set_target_properties(thistle_three_smoketest PROPERTIES WIN32_EXECUTABLE ON)
        if(MSVC)
            target_link_options(thistle_three_smoketest PRIVATE "/ENTRY:mainCRTStartup")
        endif()
    endif()

    # Point it at .obj/.gltf/.glb files to look at them (and at the loader).
    add_executable(thistle_model_viewer examples/model_viewer.cpp)
    target_link_libraries(thistle_model_viewer PRIVATE thistle)
    if(WIN32)
        set_target_properties(thistle_model_viewer PROPERTIES WIN32_EXECUTABLE ON)
        if(MSVC)
            target_link_options(thistle_model_viewer PRIVATE "/ENTRY:mainCRTStartup")
        endif()
    endif()

    # Point/spot lights, fog, sun disc, skybox orientation (see its header).
    add_executable(thistle_lights_smoketest examples/lights_smoketest.cpp)
    target_link_libraries(thistle_lights_smoketest PRIVATE thistle)
    if(WIN32)
        set_target_properties(thistle_lights_smoketest PROPERTIES WIN32_EXECUTABLE ON)
        if(MSVC)
            target_link_options(thistle_lights_smoketest PRIVATE "/ENTRY:mainCRTStartup")
        endif()
    endif()

    # A block world from a generated atlas plus a flat-colored statue.
    add_executable(thistle_voxel_demo examples/voxel_demo.cpp)
    target_link_libraries(thistle_voxel_demo PRIVATE thistle)
    if(WIN32)
        set_target_properties(thistle_voxel_demo PROPERTIES WIN32_EXECUTABLE ON)
        if(MSVC)
            target_link_options(thistle_voxel_demo PRIVATE "/ENTRY:mainCRTStartup")
        endif()
    endif()

    # World::draw_many(): 10k instanced trees + rocks.
    add_executable(thistle_forest_demo examples/forest_demo.cpp)
    target_link_libraries(thistle_forest_demo PRIVATE thistle)
    if(WIN32)
        set_target_properties(thistle_forest_demo PROPERTIES WIN32_EXECUTABLE ON)
        if(MSVC)
            target_link_options(thistle_forest_demo PRIVATE "/ENTRY:mainCRTStartup")
        endif()
    endif()

    # Terrain + noise: a low-poly island you can walk around.
    add_executable(thistle_terrain_demo examples/terrain_demo.cpp)
    target_link_libraries(thistle_terrain_demo PRIVATE thistle)
    if(WIN32)
        set_target_properties(thistle_terrain_demo PROPERTIES WIN32_EXECUTABLE ON)
        if(MSVC)
            target_link_options(thistle_terrain_demo PRIVATE "/ENTRY:mainCRTStartup")
        endif()
    endif()

    # Physics3D: crates, barrels and balls to knock over (Jolt builds only —
    # without it there'd be nothing to see).
    if(THISTLE_PHYSICS3D)
        add_executable(thistle_physics_demo examples/physics_demo.cpp)
        target_link_libraries(thistle_physics_demo PRIVATE thistle)
        if(WIN32)
            set_target_properties(thistle_physics_demo PROPERTIES WIN32_EXECUTABLE ON)
            if(MSVC)
                target_link_options(thistle_physics_demo PRIVATE "/ENTRY:mainCRTStartup")
            endif()
        endif()
    endif()

    # VoxelDestruction: a voxel town to blow apart (Jolt builds only).
    if(THISTLE_PHYSICS3D)
        add_executable(thistle_destruction_demo examples/destruction_demo.cpp)
        target_link_libraries(thistle_destruction_demo PRIVATE thistle)
        if(WIN32)
            set_target_properties(thistle_destruction_demo PROPERTIES WIN32_EXECUTABLE ON)
            if(MSVC)
                target_link_options(thistle_destruction_demo PRIVATE "/ENTRY:mainCRTStartup")
            endif()
        endif()
    endif()

    # VoxelWorld streaming: an endless generated block world.
    add_executable(thistle_endless_demo examples/endless_demo.cpp)
    target_link_libraries(thistle_endless_demo PRIVATE thistle)
    if(WIN32)
        set_target_properties(thistle_endless_demo PROPERTIES WIN32_EXECUTABLE ON)
        if(MSVC)
            target_link_options(thistle_endless_demo PRIVATE "/ENTRY:mainCRTStartup")
        endif()
    endif()

    # A plain console program (no window, no App/Frame) that spins up a real
    # NetServer + NetClient over localhost and checks the results — proof the
    # networking layer actually works, not just compiles. No WIN32_EXECUTABLE
    # fixup needed here since this one never touches sokol_app.
    add_executable(thistle_net_smoketest examples/net_smoketest.cpp)
    target_link_libraries(thistle_net_smoketest PRIVATE thistle)

    # Deliberately crashes one of three ways (segv/abort/throw) to verify the
    # crash handler for real — does a report file actually get written, with
    # the right content. Not registered as a ctest: it's supposed to crash,
    # and a passing "test" here means checking a file afterward, not the
    # process's exit code — see docs/crash-handler.md for how to run it.
    add_executable(thistle_crash_smoketest examples/crash_smoketest.cpp)
    target_link_libraries(thistle_crash_smoketest PRIVATE thistle)

    # Round-trips a Node tree through save_scene()/load_scene() and checks
    # every field survives. Headless like net_smoketest (constructs App for
    # load_texture()'s sake but never calls run()) — no window, no GPU, no
    # sprite assets needed since it never sets a sprite_path.
    add_executable(thistle_scene_smoketest examples/scene_smoketest.cpp)
    target_link_libraries(thistle_scene_smoketest PRIVATE thistle)

    # Checks the 3D collision overlap/intersection math against hand-computed
    # expected results. Even more headless than net_smoketest — pure geometry
    # functions, no App/window/GPU touched at all.
    add_executable(thistle_collision_smoketest examples/collision_smoketest.cpp)
    target_link_libraries(thistle_collision_smoketest PRIVATE thistle)

    # thistle::three's quat/mat4/Transform against hand-computed results.
    add_executable(thistle_math3d_smoketest examples/math3d_smoketest.cpp)
    target_link_libraries(thistle_math3d_smoketest PRIVATE thistle)

    # Winding/normals/bounds of every built-in 3D shape plus the MeshData
    # helpers — CPU-side geometry only, no GPU.
    add_executable(thistle_mesh3d_smoketest examples/mesh3d_smoketest.cpp)
    target_link_libraries(thistle_mesh3d_smoketest PRIVATE thistle)

    # three::load_model_data() against OBJ/MTL, .gltf and .glb files it
    # writes itself. Headless like scene_smoketest (App constructed, never run).
    add_executable(thistle_model_smoketest examples/model_smoketest.cpp)
    target_link_libraries(thistle_model_smoketest PRIVATE thistle)

    # three::VoxelWorld storage + mesher, CPU only.
    add_executable(thistle_voxel_smoketest examples/voxel_smoketest.cpp)
    target_link_libraries(thistle_voxel_smoketest PRIVATE thistle)

    # Voxel raycast, CollisionWorld, CharacterController simulated at 60 Hz.
    add_executable(thistle_character_smoketest examples/character_smoketest.cpp)
    target_link_libraries(thistle_character_smoketest PRIVATE thistle)

    add_executable(thistle_particles_smoketest examples/particles_smoketest.cpp)
    target_link_libraries(thistle_particles_smoketest PRIVATE thistle)

    add_executable(thistle_terrain_smoketest examples/terrain_smoketest.cpp)
    target_link_libraries(thistle_terrain_smoketest PRIVATE thistle)

    # three::Physics3D simulated headlessly. Built either way: with
    # THISTLE_PHYSICS3D off it checks that the stand-in does nothing, safely.
    add_executable(thistle_physics3d_smoketest examples/physics3d_smoketest.cpp)
    target_link_libraries(thistle_physics3d_smoketest PRIVATE thistle)

    # VoxelDestruction + voxel colliders; like physics3d_smoketest, built
    # either way (without Jolt it checks that carving works and nothing falls).
    add_executable(thistle_destruction_smoketest examples/destruction_smoketest.cpp)
    target_link_libraries(thistle_destruction_smoketest PRIVATE thistle)

    add_executable(thistle_audio3d_smoketest examples/audio3d_smoketest.cpp)
    target_link_libraries(thistle_audio3d_smoketest PRIVATE thistle)

    if(ANDROID)
        # NativeActivity only ever loads a shared library, never runs a plain
        # executable's main() (there isn't one) — see THISTLE_MAIN in
        # thistle.hpp. Named "main" to match the android.app.lib_name meta-data
        # convention (libmain.so) used by examples/android/AndroidManifest.xml.
        add_library(thistle_android_smoketest SHARED examples/android_smoketest.cpp)
        target_link_libraries(thistle_android_smoketest PRIVATE thistle)
        set_target_properties(thistle_android_smoketest PROPERTIES OUTPUT_NAME "main")
    endif()

    # net_smoketest and scene_smoketest are both headless and actually exit,
    # so both run as real ctests. thistle_smoketest opens a real window and
    # never quits on its own, so running it via ctest would just hang
    # forever; CI still builds it (catches link/compile regressions like the
    # two Windows ones already found this way) but never executes it.
    enable_testing()
    add_test(NAME net_smoketest COMMAND thistle_net_smoketest)
    set_tests_properties(net_smoketest PROPERTIES TIMEOUT 30)
    add_test(NAME scene_smoketest COMMAND thistle_scene_smoketest)
    set_tests_properties(scene_smoketest PROPERTIES TIMEOUT 30)
    add_test(NAME collision_smoketest COMMAND thistle_collision_smoketest)
    set_tests_properties(collision_smoketest PROPERTIES TIMEOUT 30)
    add_test(NAME math3d_smoketest COMMAND thistle_math3d_smoketest)
    set_tests_properties(math3d_smoketest PROPERTIES TIMEOUT 30)
    add_test(NAME mesh3d_smoketest COMMAND thistle_mesh3d_smoketest)
    set_tests_properties(mesh3d_smoketest PROPERTIES TIMEOUT 30)
    add_test(NAME model_smoketest COMMAND thistle_model_smoketest)
    set_tests_properties(model_smoketest PROPERTIES TIMEOUT 30)
    add_test(NAME voxel_smoketest COMMAND thistle_voxel_smoketest)
    set_tests_properties(voxel_smoketest PROPERTIES TIMEOUT 30)
    add_test(NAME character_smoketest COMMAND thistle_character_smoketest)
    set_tests_properties(character_smoketest PROPERTIES TIMEOUT 30)
    add_test(NAME particles_smoketest COMMAND thistle_particles_smoketest)
    set_tests_properties(particles_smoketest PROPERTIES TIMEOUT 30)
    add_test(NAME terrain_smoketest COMMAND thistle_terrain_smoketest)
    set_tests_properties(terrain_smoketest PROPERTIES TIMEOUT 60)
    add_test(NAME physics3d_smoketest COMMAND thistle_physics3d_smoketest)
    set_tests_properties(physics3d_smoketest PROPERTIES TIMEOUT 60)
    add_test(NAME destruction_smoketest COMMAND thistle_destruction_smoketest)
    set_tests_properties(destruction_smoketest PROPERTIES TIMEOUT 60)
    add_test(NAME audio3d_smoketest COMMAND thistle_audio3d_smoketest)
    set_tests_properties(audio3d_smoketest PROPERTIES TIMEOUT 30)
endif()
