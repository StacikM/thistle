// three::Physics3D when the engine is built without Jolt (THISTLE_PHYSICS3D
// off, the default). Everything compiles and links so a game can ship
// with physics as an optional feature, but nothing simulates: bodies are
// never added (add() returns an invalid handle), and the first use logs
// how to turn it on.
#include "thistle_internal.h"

namespace thistle::three {

struct Physics3DImpl {
    std::vector<Contact> contacts;
    std::vector<TriggerEvent> triggers;
};

namespace {
void explain_once() {
    static bool said = false;
    if (said) return;
    said = true;
    log_warn("Physics3D is off in this build, so bodies won't be added or simulated. Turn it on with "
             "`thistle enable physics3d` (CLI projects) or -DTHISTLE_PHYSICS3D=ON.");
}
} // namespace

bool physics3d_available() { return false; }

Physics3D::Physics3D(int) : impl_(std::make_unique<Physics3DImpl>()) {}
Physics3D::~Physics3D() = default;
Physics3D::Physics3D(Physics3D&&) noexcept = default;
Physics3D& Physics3D::operator=(Physics3D&&) noexcept = default;

RigidBody Physics3D::add(const BodySettings&) { explain_once(); return {}; }
RigidBody Physics3D::add_box(vec3, vec3, BodyType) { explain_once(); return {}; }
RigidBody Physics3D::add_sphere(vec3, float, BodyType) { explain_once(); return {}; }
RigidBody Physics3D::add_static(Model, const Transform&) { explain_once(); return {}; }
RigidBody Physics3D::add_static(const Terrain&) { explain_once(); return {}; }
void Physics3D::add_static(const VoxelWorld&) { explain_once(); }
void Physics3D::remove_static(const VoxelWorld&) {}
void Physics3D::remove(RigidBody) {}
void Physics3D::clear() {}
bool Physics3D::contains(RigidBody) const { return false; }
int Physics3D::body_count() const { return 0; }
void Physics3D::step(float) {}

Transform Physics3D::transform(RigidBody, vec3 scale) const { return Transform{vec3{}, quat{}, scale}; }
vec3 Physics3D::position(RigidBody) const { return {}; }
quat Physics3D::rotation(RigidBody) const { return {}; }
void Physics3D::set_position(RigidBody, vec3) {}
void Physics3D::set_rotation(RigidBody, quat) {}
void Physics3D::move_kinematic(RigidBody, vec3, quat, float) {}
vec3 Physics3D::velocity(RigidBody) const { return {}; }
void Physics3D::set_velocity(RigidBody, vec3) {}
vec3 Physics3D::angular_velocity(RigidBody) const { return {}; }
void Physics3D::set_angular_velocity(RigidBody, vec3) {}
void Physics3D::apply_force(RigidBody, vec3) {}
void Physics3D::apply_torque(RigidBody, vec3) {}
void Physics3D::apply_impulse(RigidBody, vec3) {}
void Physics3D::apply_impulse(RigidBody, vec3, vec3) {}
float Physics3D::mass(RigidBody) const { return 0.0f; }
BodyType Physics3D::type(RigidBody) const { return BodyType::Static; }
void Physics3D::set_type(RigidBody, BodyType) {}
bool Physics3D::sleeping(RigidBody) const { return false; }
void Physics3D::wake(RigidBody) {}
Bounds Physics3D::bounds(RigidBody) const { return {}; }
uint64_t Physics3D::user(RigidBody) const { return 0; }
void Physics3D::set_collider(RigidBody, const Collider&) {}

PhysicsHit Physics3D::raycast(const Ray&, float, RigidBody) const { return {}; }
std::vector<RigidBody> Physics3D::overlap_sphere(vec3, float) const { return {}; }
std::vector<RigidBody> Physics3D::overlap_box(const Bounds&) const { return {}; }
int Physics3D::explode(vec3, float, float) { return 0; }

const std::vector<Contact>& Physics3D::contacts() const { return impl_->contacts; }
const std::vector<TriggerEvent>& Physics3D::trigger_events() const { return impl_->triggers; }
void Physics3D::on_contact(std::function<void(const Contact&)>) {}
void Physics3D::on_trigger(std::function<void(const TriggerEvent&)>) {}
void Physics3D::set_layers_collide(int, int, bool) {}
bool Physics3D::layers_collide(int, int) const { return true; }
void Physics3D::draw_debug(World&, bool) const {}

} // namespace thistle::three
