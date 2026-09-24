// Camera-facing quads for World::billboard() and particle systems. No
// vertex buffer: each instance is 6 vertices whose corner comes from
// gl_VertexIndex, and everything else from one per-instance buffer. Unlit,
// fogged, sRGB in and out like the 2D side.
@ctype mat4 thistle::three::mat4
@ctype vec4 thistle::three::vec4

@vs billboard_vs
layout(binding=0) uniform billboard_vs_params {
    mat4 view_proj;
    vec4 cam_right; // xyz
    vec4 cam_up;    // xyz
    vec4 cam_pos;   // xyz
};

in vec4 inst_pos;    // xyz = center, w = width
in vec4 inst_params; // x = height, y = rotation (radians), z = upright (0/1)
in vec4 inst_color;
in vec4 inst_uv;     // u0, v0, u1, v1

out vec2 v_uv;
out vec4 v_color;
out vec3 v_world_pos;

void main() {
    int corner = gl_VertexIndex % 6;
    // Two triangles, counter-clockwise: (0,0) (1,0) (1,1) / (0,0) (1,1) (0,1).
    vec2 q = vec2(corner == 1 || corner == 2 || corner == 4 ? 1.0 : 0.0,
                  corner == 2 || corner == 4 || corner == 5 ? 1.0 : 0.0);
    vec2 c = q - 0.5;
    float s = sin(inst_params.y), k = cos(inst_params.y);
    c = vec2(c.x * k - c.y * s, c.x * s + c.y * k);
    vec3 right = cam_right.xyz;
    vec3 up = cam_up.xyz;
    if (inst_params.z > 0.5) {
        // Upright: turn to face the camera around the vertical axis only,
        // like a tree or a Doom enemy, instead of tipping back when seen from above.
        vec3 to_cam = cam_pos.xyz - inst_pos.xyz;
        vec3 flat_dir = normalize(vec3(to_cam.x, 0.0, to_cam.z) + vec3(1e-5, 0.0, 0.0));
        right = vec3(flat_dir.z, 0.0, -flat_dir.x);
        up = vec3(0.0, 1.0, 0.0);
    }
    vec3 world = inst_pos.xyz + right * (c.x * inst_pos.w) + up * (c.y * inst_params.x);
    v_world_pos = world;
    v_uv = vec2(mix(inst_uv.x, inst_uv.z, q.x), mix(inst_uv.w, inst_uv.y, q.y));
    v_color = inst_color;
    gl_Position = view_proj * vec4(world, 1.0);
}
@end

@fs billboard_fs
layout(binding=1) uniform billboard_fs_params {
    vec4 fog_color;  // rgb = linear
    vec4 fog_params; // x = start, y = 1 / (end - start), z = enabled, w = additive (fog fades it out instead)
    vec4 fs_cam_pos; // xyz
};

layout(binding=0) uniform texture2D sprite_tex;
layout(binding=0) uniform sampler sprite_smp;

in vec2 v_uv;
in vec4 v_color;
in vec3 v_world_pos;
out vec4 frag_color;

void main() {
    vec4 c = texture(sampler2D(sprite_tex, sprite_smp), v_uv) * v_color;
    if (c.a < 0.004) {
        discard;
    }
    if (fog_params.z > 0.5) {
        float f = clamp((length(fs_cam_pos.xyz - v_world_pos) - fog_params.x) * fog_params.y, 0.0, 1.0);
        if (fog_params.w > 0.5) {
            c.rgb *= 1.0 - f; // additive glow just fades away in fog
        } else {
            c.rgb = pow(mix(pow(c.rgb, vec3(2.2)), fog_color.rgb, f), vec3(1.0 / 2.2));
        }
    }
    frag_color = c;
}
@end

@program billboard billboard_vs billboard_fs
