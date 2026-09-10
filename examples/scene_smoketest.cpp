// Round-trips a Node tree through save_scene()/load_scene() and checks every
// field survives — headless, no window, no sprite assets needed (this tests
// the JSON plumbing itself, not sprite loading). Constructing an App and
// never calling run() is a safe, established pattern here — see
// thistle_net_smoketest and thistle_crash_smoketest.
#include <thistle.hpp>
#include <cmath>
#include <cstdio>
using namespace thistle;

namespace {
int g_failures = 0;
bool near_eq(float a, float b) { return std::fabs(a - b) < 0.001f; }
void check(bool cond, const char* msg) {
    if (cond) { std::printf("  ok  %s\n", msg); }
    else { std::printf("  FAIL %s\n", msg); ++g_failures; }
}
} // namespace

int main() {
    App app{{.title = "SceneSmoketest", .width = 640, .height = 480}};

    Node root;
    root.pos = {10, 20};
    root.rotation = 0.5f;
    root.alpha = 0.75f;
    root.visible = false;
    root.sprite_tint = rgb(0.2f, 0.4f, 0.6f);
    root.sprite_src = Rect{{1, 2}, {3, 4}};
    Node* child = root.add_child();
    child->pos = {5, 6};
    child->scale = {2, 2};
    Node* grandchild = child->add_child();
    grandchild->pos = {-1, -2};

    // No public "give me the writable directory" API — save::path() is the
    // save.dat file itself, so strip its filename to get the directory.
    const std::string save_file = save::path();
    const std::string dir = save_file.substr(0, save_file.find_last_of("/\\"));
    const std::string path = dir + "/scene_smoketest.json";

    check(save_scene(root, path), "save_scene wrote the file");

    std::unique_ptr<Node> loaded = load_scene(path);
    check(loaded != nullptr, "load_scene returned a root");
    if (!loaded) { std::printf("\n%d FAILURE(S)\n", g_failures + 1); return 1; }

    check(near_eq(loaded->pos.x, 10) && near_eq(loaded->pos.y, 20), "root pos round-tripped");
    check(near_eq(loaded->rotation, 0.5f), "root rotation round-tripped");
    check(near_eq(loaded->alpha, 0.75f), "root alpha round-tripped");
    check(loaded->visible == false, "root visible round-tripped");
    check(near_eq(loaded->sprite_tint.g, 0.4f), "root sprite_tint round-tripped");
    check(near_eq(loaded->sprite_src.size.x, 3) && near_eq(loaded->sprite_src.size.y, 4), "root sprite_src round-tripped");
    check(loaded->child_count() == 1, "root has one child");
    if (loaded->child_count() == 1) {
        Node* c = loaded->child(0);
        check(near_eq(c->pos.x, 5) && near_eq(c->pos.y, 6), "child pos round-tripped");
        check(near_eq(c->scale.x, 2), "child scale round-tripped");
        check(c->child_count() == 1, "child has one grandchild");
        if (c->child_count() == 1) {
            check(near_eq(c->child(0)->pos.x, -1), "grandchild pos round-tripped");
        }
    }

    if (g_failures == 0) { std::printf("\nALL PASS\n"); return 0; }
    std::printf("\n%d FAILURE(S)\n", g_failures);
    return 1;
}
