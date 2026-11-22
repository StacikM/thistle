// Loads a .scene.json with three::Scene3D and prints what it got, one line per
// entity: "name|kind|parent|x y z|model". Used by the Unity importer's test to
// prove the engine itself (not just a JSON reader) accepts what was written.
#include <thistle.hpp>
#include <cstdio>
#include <string>
using namespace thistle;
using namespace thistle::three;

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    if (argc >= 3 && std::string(argv[1]) == "--model") {
        const ModelData data = load_model_data(argv[2]);
        size_t verts = 0, tris = 0;
        Bounds box;
        for (const ModelData::Part& part : data.parts) {
            verts += part.mesh.vertices.size();
            tris += part.mesh.indices.size() / 3;
            for (const Vertex& v : part.mesh.vertices) box.add(v.position);
        }
        std::printf("model parts=%zu verts=%zu tris=%zu bounds=%.3f %.3f %.3f / %.3f %.3f %.3f\n", data.parts.size(), verts, tris, box.min.x,
                    box.min.y, box.min.z, box.max.x, box.max.y, box.max.z);
        return verts > 0 ? 0 : 1;
    }
    App app{{.title = "scene_dump", .width = 64, .height = 64}};
    Scene3D scene;
    if (!scene.load(argv[1])) {
        std::printf("load failed\n");
        return 1;
    }
    const char* kinds[] = {"empty", "model", "box", "sphere", "cylinder", "cone", "plane", "point_light", "spot_light", "trigger", "spawn", "voxels"};
    for (size_t i = 0; i < scene.entities.size(); ++i) {
        const SceneEntity& e = scene.entities[i];
        const Transform t = scene.world_transform(static_cast<int>(i));
        std::printf("%s|%s|%d|%.3f %.3f %.3f|%s\n", e.name.c_str(), kinds[static_cast<int>(e.kind)], e.parent, t.position.x, t.position.y, t.position.z,
                    e.model.c_str());
    }
    std::printf("sun %.3f %.3f %.3f\n", scene.sun.direction.x, scene.sun.direction.y, scene.sun.direction.z);
    return 0;
}
