// Reading the finished frame back from the GPU, for THISTLE_SCREENSHOT (see
// thistle.cpp's maybe_screenshot). sokol has no read-back, so each graphics
// backend gets its own: OpenGL (Linux) and Direct3D 11 (Windows). Metal and
// the rest say they aren't supported.
#include "thistle_internal.h"

#include <cstring>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include "sokol_app.h"
#elif defined(__linux__) && !defined(__ANDROID__)
#include <GL/gl.h>
#endif

namespace thistle::detail {

bool framebuffer_readback_supported() {
#if defined(_WIN32) || (defined(__linux__) && !defined(__ANDROID__))
    return true;
#else
    return false;
#endif
}

bool read_framebuffer(int w, int h, std::vector<unsigned char>& rgba, std::string& why) {
    if (w <= 0 || h <= 0) {
        why = "the window has no size";
        return false;
    }
#if defined(_WIN32)
    IDXGISwapChain* swap = static_cast<IDXGISwapChain*>(const_cast<void*>(sapp_d3d11_get_swap_chain()));
    if (!swap) {
        why = "no Direct3D 11 swap chain";
        return false;
    }
    ID3D11Texture2D* back = nullptr;
    if (FAILED(swap->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back))) || !back) {
        why = "couldn't get the back buffer";
        return false;
    }
    D3D11_TEXTURE2D_DESC desc = {};
    back->GetDesc(&desc);
    ID3D11Device* device = nullptr;
    back->GetDevice(&device);
    ID3D11DeviceContext* context = nullptr;
    if (device) device->GetImmediateContext(&context);
    ID3D11Texture2D* staging = nullptr;
    bool ok = false;
    const bool bgra = desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM || desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    const bool rgba8 = desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM || desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    if (!device || !context) {
        why = "no Direct3D 11 device";
    } else if (desc.SampleDesc.Count != 1) {
        why = "the back buffer is multisampled";
    } else if (!bgra && !rgba8) {
        why = "the back buffer's pixel format isn't 8-bit RGBA or BGRA";
    } else {
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        desc.MiscFlags = 0;
        if (FAILED(device->CreateTexture2D(&desc, nullptr, &staging)) || !staging) {
            why = "couldn't make a staging texture";
        } else {
            context->CopyResource(staging, back);
            D3D11_MAPPED_SUBRESOURCE mapped = {};
            if (FAILED(context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) {
                why = "couldn't read the staging texture";
            } else {
                const int cw = static_cast<int>(desc.Width) < w ? static_cast<int>(desc.Width) : w;
                const int ch = static_cast<int>(desc.Height) < h ? static_cast<int>(desc.Height) : h;
                rgba.assign(static_cast<size_t>(w) * h * 4, 0);
                for (int y = 0; y < ch; ++y) {
                    const unsigned char* src = static_cast<const unsigned char*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch;
                    unsigned char* dst = &rgba[static_cast<size_t>(y) * w * 4];
                    for (int x = 0; x < cw; ++x) {
                        dst[x * 4 + 0] = src[x * 4 + (bgra ? 2 : 0)];
                        dst[x * 4 + 1] = src[x * 4 + 1];
                        dst[x * 4 + 2] = src[x * 4 + (bgra ? 0 : 2)];
                        dst[x * 4 + 3] = src[x * 4 + 3];
                    }
                }
                context->Unmap(staging, 0);
                ok = true;
            }
        }
    }
    if (staging) staging->Release();
    if (context) context->Release();
    if (device) device->Release();
    back->Release();
    return ok;
#elif defined(__linux__) && !defined(__ANDROID__)
    rgba.assign(static_cast<size_t>(w) * h * 4, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    if (const GLenum err = glGetError(); err != GL_NO_ERROR) {
        why = "glReadPixels failed (GL error " + std::to_string(err) + ")";
        return false;
    }
    // OpenGL's rows run bottom to top; an image's run top to bottom.
    std::vector<unsigned char> row(static_cast<size_t>(w) * 4);
    for (int y = 0; y < h / 2; ++y) {
        unsigned char* a = &rgba[static_cast<size_t>(y) * w * 4];
        unsigned char* b = &rgba[static_cast<size_t>(h - 1 - y) * w * 4];
        std::memcpy(row.data(), a, row.size());
        std::memcpy(a, b, row.size());
        std::memcpy(b, row.data(), row.size());
    }
    return true;
#else
    why = "reading the screen back isn't implemented on this platform yet (it works on Linux with OpenGL and on Windows with Direct3D 11)";
    return false;
#endif
}

} // namespace thistle::detail
