// Loads a .scene.json with three::Scene3D and prints what it got, one line per
// entity: "name|kind|parent|x y z|model". Used by the Unity importer's test to
// prove the engine itself (not just a JSON reader) accepts what was written.
#include <thistle.hpp>
#include <cstdio>
using namespace thistle;
using namespace thistle::three;

int main(int argc, char** argv) {
    if (argc < 2) return 2;
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
