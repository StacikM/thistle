// Visual check for the Material PBR fields, World::post (HDR, tonemapping,
// bloom), set_model_lods() and set_model_occluder(). Opens a window.
//
//   thistle_post_demo [classic|pbr] [post] [bloom] [occlusion] [frames=N]
//
// With no arguments it shows everything on. Keys: 1 toggles PBR, 2 post,
// 3 bloom, 4 occlusion culling. A row of metal spheres (roughness 0 to 1)
// above a row of plastic ones, a bright lamp (bloom), five spheres that
// get simpler with distance, and a wall hiding two crates. frames=N quits
// after N frames, printing the render stats, so it can run unattended.
#include <thistle.hpp>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
using namespace thistle;
using namespace thistle::three;

int main(int argc, char** argv) {
    bool pbr = true, post = true, bloom = true, occlusion = true;
    int frames = 0;
    bool any = false;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "classic")) { pbr = false; any = true; }
        else if (!std::strncmp(argv[i], "frames=", 7)) frames = std::atoi(argv[i] + 7);
    }
    if (argc > 1) {
        post = bloom = occlusion = false;
        for (int i = 1; i < argc; ++i) {
            if (!std::strcmp(argv[i], "post")) post = true;
            if (!std::strcmp(argv[i], "bloom")) post = bloom = true;
            if (!std::strcmp(argv[i], "occlusion")) occlusion = true;
        }
    }
    (void)any;

    App app{{.title = "thistle post demo", .width = 1280, .height = 720}};
    World world;
    world.sky.top = rgb(0.25f, 0.45f, 0.85f);
    world.sky.horizon = rgb(0.75f, 0.85f, 0.95f);
    Camera camera;
    camera.position = {0.0f, 3.2f, 11.0f};
    camera.rotation = quat::euler(-0.2f, 0.0f, 0.0f);

    const MeshData sphere = sphere_mesh(0.6f, 32, 48);
    std::vector<Model> metals, plastics;
    for (int i = 0; i < 5; ++i) {
        const float r = static_cast<float>(i) / 4.0f;
        Material m{.color = rgb(0.95f, 0.78f, 0.35f)};
        m.pbr = true; m.metallic = 1.0f; m.roughness = std::max(r, 0.05f);
        metals.push_back(make_model(sphere, m));
        Material p{.color = rgb(0.8f, 0.15f, 0.15f)};
        p.pbr = true; p.metallic = 0.0f; p.roughness = std::max(r, 0.05f);
        plastics.push_back(make_model(sphere, p));
    }
    Material lamp_mat{.color = rgb(1.0f, 0.9f, 0.6f), .emissive = rgb(4.0f, 3.4f, 2.0f)};
    lamp_mat.pbr = true;
    const Model lamp = make_model(sphere_mesh(0.35f, 24, 32), lamp_mat);

    // LOD: the same ball at three detail levels.
    const Material ball_mat{.color = rgb(0.3f, 0.7f, 0.4f)};
    const Model ball_hi = make_model(sphere_mesh(0.5f, 32, 48), ball_mat);
    const Model ball_mid = make_model(sphere_mesh(0.5f, 10, 14), ball_mat);
    const Model ball_lo = make_model(sphere_mesh(0.5f, 4, 6), ball_mat);
    set_model_lods(ball_hi, {{ball_mid, 14.0f}, {ball_lo, 24.0f}});

    const Model wall = make_model(box_mesh({7.0f, 4.0f, 0.5f}), Material{.color = rgb(0.6f, 0.55f, 0.5f)});
    set_model_occluder(wall, true);
    const Model crate = make_model(box_mesh({1, 1, 1}), Material{.color = rgb(0.7f, 0.45f, 0.2f)});

    // A bumpy normal map, drawn here instead of loaded: a grid of round domes
    // (green = up, as glTF says). With the sun above, lit tops and dark bottoms
    // mean the map is read the right way round; dents would look the other way.
    constexpr int n = 128;
    std::vector<unsigned char> bumps(n * n * 4);
    auto height = [](float x, float y) { return std::sin(x * 6.2832f * 6.0f) * std::sin(y * 6.2832f * 3.0f); };
    for (int y = 0; y < n; ++y) {
        for (int x = 0; x < n; ++x) {
            const float u = static_cast<float>(x) / n, v = static_cast<float>(y) / n, e = 1.0f / n;
            const float dh_dcol = (height(u + e, v) - height(u - e, v)) / (2.0f * e) * 0.012f;
            const float dh_drow = (height(u, v + e) - height(u, v - e)) / (2.0f * e) * 0.012f;
            vec3 nrm{-dh_dcol, dh_drow, 1.0f};
            nrm = normalize(nrm);
            unsigned char* px = &bumps[(y * n + x) * 4];
            px[0] = static_cast<unsigned char>((nrm.x * 0.5f + 0.5f) * 255.0f);
            px[1] = static_cast<unsigned char>((nrm.y * 0.5f + 0.5f) * 255.0f);
            px[2] = static_cast<unsigned char>((nrm.z * 0.5f + 0.5f) * 255.0f);
            px[3] = 255;
        }
    }
    Material bumpy{.color = rgb(0.8f, 0.8f, 0.85f)};
    bumpy.pbr = true;
    bumpy.roughness = 0.45f;
    bumpy.normal_texture = make_texture(n, n, bumps.data());
    const Model bumpy_ball = make_model(sphere_mesh(1.0f, 48, 64), bumpy);

    int frame = 0;
    app.update([&](Frame f) {
        if (f.key_pressed(Key::Num1)) pbr = !pbr;
        if (f.key_pressed(Key::Num2)) post = !post;
        if (f.key_pressed(Key::Num3)) bloom = !bloom;
        if (f.key_pressed(Key::Num4)) occlusion = !occlusion;
        world.post.enabled = post;
        world.post.bloom = bloom;
        world.occlusion_culling = occlusion;
        for (int i = 0; i < 5; ++i) {
            Material m = Material{.color = rgb(0.95f, 0.78f, 0.35f)};
            m.pbr = pbr; m.metallic = 1.0f; m.roughness = std::max(i / 4.0f, 0.05f);
            if (!pbr) { m.specular = 0.6f; m.shininess = 8.0f + 60.0f * (1.0f - i / 4.0f); }
            set_model_material(metals[i], m);
            Material p = Material{.color = rgb(0.8f, 0.15f, 0.15f)};
            p.pbr = pbr; p.roughness = std::max(i / 4.0f, 0.05f);
            if (!pbr) { p.specular = 0.4f; p.shininess = 8.0f + 60.0f * (1.0f - i / 4.0f); }
            set_model_material(plastics[i], p);
            world.draw(metals[i], Transform(vec3{-4.0f + i * 2.0f, 2.2f, 0.0f}));
            world.draw(plastics[i], Transform(vec3{-4.0f + i * 2.0f, 0.8f, 0.0f}));
        }
        world.box({0, -0.25f, -4}, {40, 0.5f, 30}, rgb(0.55f, 0.58f, 0.55f));
        world.draw(bumpy_ball, Transform(vec3{-6.5f, 1.2f, 3.0f}));
        world.draw(lamp, Transform(vec3{0.0f, 4.4f, 1.0f}));
        world.light(PointLight{.position = {0.0f, 4.4f, 1.0f}, .color = rgb(1.0f, 0.85f, 0.5f), .range = 9.0f, .intensity = 2.0f});
        for (int i = 0; i < 5; ++i) {
            world.draw(ball_hi, Transform(vec3{-8.0f + i * 1.2f, 0.5f, 5.0f - i * 6.0f}));
        }
        world.draw(wall, Transform(vec3{6.5f, 2.0f, -2.0f}));
        world.draw(crate, Transform(vec3{6.5f, 0.5f, -6.0f}));
        world.draw(crate, Transform(vec3{7.5f, 0.5f, -9.0f}));
        world.render(f, camera);
        f.text("1 pbr  2 post  3 bloom  4 occlusion", {16, 16}, {.size = 20});
        if (++frame == frames) {
            const RenderStats st = render_stats();
            std::printf("stats: draws=%d tris=%d culled=%d occluded=%d lod_swaps=%d\n", st.draw_calls, st.triangles, st.culled,
                        st.occluded, st.lod_swaps);
            quit();
        }
    });
    app.run();
    return 0;
}
