cmake_minimum_required(VERSION 3.20)
project(thistle_editor LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Thistle Editor is a real consumer of the engine, not an engine-internal
# smoketest — it links against `thistle` the same way a scaffolded game
# (`thistle new`) would, just pointed two directories up at this repo's own
# engine instead of a copied/vendored one.
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../.. ${CMAKE_BINARY_DIR}/thistle_engine)

if(APPLE)
    add_executable(thistle_editor MACOSX_BUNDLE main.cpp)
else()
    add_executable(thistle_editor main.cpp)
endif()

target_link_libraries(thistle_editor PRIVATE thistle)

# The editor's own UI font (Inter Regular, OFL — https://github.com/rsms/inter;
# same font Blender's own UI uses), copied next to the built executable as its
# own `editor_assets/` folder —
# deliberately NOT using the engine's thistle_bundle_assets() helper, since
# that always copies into a destination folder literally named `assets/`,
# which is where the user's own .obj props live for the palette to scan.
# Reusing that name here would mean "put your font next to your models or
# the UI breaks," which is backwards — this tool's own resources and the
# user's project assets should never collide.
add_custom_command(TARGET thistle_editor POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/editor_assets" "$<TARGET_FILE_DIR:thistle_editor>/editor_assets"
    COMMENT "Copying Thistle Editor's own UI assets next to the executable"
)
