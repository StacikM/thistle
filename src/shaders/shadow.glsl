// Depth-only pass from the sun's point of view: fills the shadow map that
// lit.glsl samples. Cutout materials still test their texture's alpha here,
// so a leaf card casts a leaf-shaped shadow instead of a rectangle.
@ctype mat4 thistle::three::mat4
@ctype vec4 thistle::three::vec4

@vs shadow_vs
layout(binding=0) uniform shadow_vs_params {
    mat4 light_mvp;
    vec4 uv_transform;
};

in vec3 position;
in vec2 texcoord0;
in vec4 color0;

out vec2 v_uv;
out float v_alpha;

void main() {
    v_uv = texcoord0 * uv_transform.xy + uv_transform.zw;
    v_alpha = color0.a;
    gl_Position = light_mvp * vec4(position, 1.0);
}
@end

@vs shadow_instanced_vs
layout(binding=0) uniform shadow_vs_params {
    mat4 light_mvp; // light view-projection * the part's placement in the model
    vec4 uv_transform;
};

in vec3 position;
in vec2 texcoord0;
in vec4 color0;
in vec4 inst_m0;
in vec4 inst_m1;
in vec4 inst_m2;
in vec4 inst_m3;
in vec4 inst_color;

out vec2 v_uv;
out float v_alpha;

void main() {
    v_uv = texcoord0 * uv_transform.xy + uv_transform.zw;
    v_alpha = color0.a * inst_color.a;
    // light_mvp is (light view-proj * part placement); the instance
    // transform goes between them.
    gl_Position = light_mvp * (mat4(inst_m0, inst_m1, inst_m2, inst_m3) * vec4(position, 1.0));
}
@end

@fs shadow_fs
layout(binding=1) uniform shadow_fs_params {
    vec4 cutout; // x = alpha cutoff (< 0: opaque, skip the texture read), y = material alpha
};

layout(binding=0) uniform texture2D base_tex;
layout(binding=0) uniform sampler base_smp;

in vec2 v_uv;
in float v_alpha;

void main() {
    if (cutout.x >= 0.0) {
        float a = texture(sampler2D(base_tex, base_smp), v_uv).a * v_alpha * cutout.y;
        if (a < cutout.x) {
            discard;
        }
    }
}
@end

@program shadow shadow_vs shadow_fs
@program shadow_instanced shadow_instanced_vs shadow_fs
