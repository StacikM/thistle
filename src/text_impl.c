// Instantiates fontstash + sokol_fontstash in one translation unit.
// fontstash bundles stb_truetype's implementation itself.
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#if defined(__APPLE__)
    #define SOKOL_METAL
#elif defined(__EMSCRIPTEN__)
    #define SOKOL_GLES3
#elif defined(_WIN32)
    #define SOKOL_D3D11
#else
    #define SOKOL_GLCORE
#endif

// fontstash's fons__fopen() uses MAX_PATH/MultiByteToWideChar/CP_UTF8 on
// Windows to open UTF-8 paths; those come from windows.h.
#if defined(_WIN32)
    #include <windows.h>
#endif

#include "sokol_gfx.h"
#include "sokol_gl.h"

#define FONTSTASH_IMPLEMENTATION
#include "fontstash.h"

#define SOKOL_FONTSTASH_IMPL
#include "sokol_fontstash.h"
