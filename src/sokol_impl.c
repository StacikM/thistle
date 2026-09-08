// Single translation unit that instantiates the sokol implementations.
// On Apple this file is compiled as Objective-C (see CMakeLists.txt).

#define SOKOL_IMPL

#if defined(__APPLE__)
    #define SOKOL_METAL
#elif defined(__ANDROID__)
    #define SOKOL_GLES3
#elif defined(__EMSCRIPTEN__)
    #define SOKOL_GLES3
#elif defined(_WIN32)
    #define SOKOL_D3D11
#else
    #define SOKOL_GLCORE
#endif

// sokol_app.h hard-errors if SOKOL_NO_ENTRY is defined on Android: there is no
// process main() there, so sokol_app.h must own ANativeActivity_onCreate and
// call our sokol_main() (defined in thistle.cpp) itself. Everywhere else we
// still provide our own main() via App::run().
#if !defined(__ANDROID__)
    #define SOKOL_NO_ENTRY
#endif

#include "sokol_app.h"
#include "sokol_gfx.h"
#include "sokol_glue.h"
#include "sokol_log.h"
#include "sokol_gl.h"
