// What the headless server library (THISTLE_SERVER) has in place of the
// windowed engine's thistle.cpp: plain file reading, and textures. A server
// draws nothing, so a texture is an empty handle and its file isn't even
// read. Models still load as geometry (for CollisionWorld and raycasts):
// their materials' textures come out empty the same way.
#include "thistle_core.h"

#include "stb_image.h"

#include <fstream>
#include <iterator>

namespace thistle {

Texture load_texture(const std::string&) { return {}; }
Texture load_texture_from_memory(const void*, size_t) { return {}; }
Texture make_texture(int, int, const unsigned char*) { return {}; }
void unload_texture(Texture& tex) { tex = {}; }
void reload_texture(Texture) {}

namespace detail {

bool read_file_bytes(const std::string& path, std::vector<unsigned char>& out) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    out.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    return true;
}

bool read_file_text(const std::string& path, std::string& out) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    out.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    return true;
}

bool load_image_rgba(const std::string& path, std::vector<unsigned char>& out, int& w, int& h) {
    int channels = 0;
    unsigned char* px = stbi_load(path.c_str(), &w, &h, &channels, 4);
    if (!px) return false;
    out.assign(px, px + static_cast<size_t>(w) * h * 4);
    stbi_image_free(px);
    return true;
}

} // namespace detail
} // namespace thistle
