// Instantiates fontstash + sokol_fontstash in one translation unit.
// fontstash bundles stb_truetype's implementation itself.
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

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

// sokol_fontstash's own pipeline writes each glyph quad's alpha straight
// into the target, so the see-through corners of every glyph cut a hole in
// an opaque render target. This swaps it for one whose alpha only goes up,
// the same blend the engine's 2D and 3D pipelines use. It reaches into
// _sfons_t, which only this translation unit can see; sokol is pinned in
// CMakeLists.txt, so that struct can't change under it unnoticed.
void thistle_sfons_keep_alpha(FONScontext* ctx) {
    _sfons_t* sfons = (_sfons_t*)ctx->params.userPtr;
    sg_pipeline_desc pd;
    memset(&pd, 0, sizeof(pd));
    pd.shader = sfons->shd;
    pd.colors[0].write_mask = SG_COLORMASK_RGBA; // sokol_gl's default leaves alpha out
    pd.colors[0].blend.enabled = true;
    pd.colors[0].blend.src_factor_rgb = SG_BLENDFACTOR_SRC_ALPHA;
    pd.colors[0].blend.dst_factor_rgb = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    pd.colors[0].blend.src_factor_alpha = SG_BLENDFACTOR_ONE;
    pd.colors[0].blend.dst_factor_alpha = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    pd.label = "thistle-text-pipeline";
    sgl_destroy_pipeline(sfons->pip);
    sfons->pip = sgl_make_pipeline(&pd);
}
