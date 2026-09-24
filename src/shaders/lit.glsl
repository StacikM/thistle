// The main 3D surface shader: hemisphere ambient (sky above, ground below),
// one directional sun and up to 16 point/spot lights, each with a
// Blinn-Phong highlight, then distance fog. All lighting math
// happens in linear space; colors come in as sRGB (what you'd pick in a
// color picker, same as the 2D API) and go back out as sRGB.
@ctype mat4 thistle::three::mat4
@ctype vec4 thistle::three::vec4

@vs lit_vs
layout(binding=0) uniform lit_vs_params {
    mat4 model;
    mat4 view_proj;
    mat4 normal_matrix; // inverse-transpose of model, so non-uniform scale keeps normals right
    vec4 uv_transform;  // xy = scale (tiling), zw = offset
};

in vec3 position;
in vec3 normal;
in vec2 texcoord0;
in vec4 color0;

out vec3 v_world_pos;
out vec3 v_normal;
out vec2 v_uv;
out vec4 v_color;

void main() {
    vec4 world_pos = model * vec4(position, 1.0);
    v_world_pos = world_pos.xyz;
    v_normal = (normal_matrix * vec4(normal, 0.0)).xyz;
    v_uv = texcoord0 * uv_transform.xy + uv_transform.zw;
    v_color = color0;
    gl_Position = view_proj * world_pos;
}
@end

@fs lit_fs
layout(binding=1) uniform lit_scene_params {
    vec4 camera_pos;     // xyz
    vec4 sun_dir;        // xyz = direction the light travels (normalized)
    vec4 sun_color;      // rgb = linear color * intensity
    vec4 sky_ambient;    // rgb = linear, already scaled by the ambient amount
    vec4 ground_ambient; // rgb = linear, already scaled by the ambient amount
    vec4 fog_color;      // rgb = linear
    vec4 fog_params;     // x = start, y = 1 / (end - start), z = enabled (0/1)
    vec4 light_count;    // x = number of lights in use
    vec4 light_pos[16];  // xyz = position, w = range
    vec4 light_color[16];// rgb = linear color * intensity, w = cos(outer cone angle), or -2 for a point light
    vec4 light_dir[16];  // xyz = spot direction, w = cos(inner cone angle)
    mat4 shadow_matrix;  // world -> (shadow map u, v, depth), already adjusted for the backend
    vec4 shadow_params;  // x = enabled, y = PCF step (texels -> uv), z = normal offset (world units), w = strength
    vec4 shadow_fade;    // x = distance where shadows start fading, y = 1 / fade length
};

layout(binding=2) uniform lit_material_params {
    vec4 base_color; // sRGB rgb, a = alpha
    vec4 emissive;   // sRGB rgb
    vec4 surface;    // x = specular, y = shininess, z = unlit (0/1), w = alpha cutoff (<0 = none)
};

layout(binding=0) uniform texture2D base_tex;
layout(binding=1) uniform texture2D emissive_tex;
layout(binding=0) uniform sampler base_smp;
layout(binding=2) uniform texture2D shadow_map;
layout(binding=1) uniform samplerShadow shadow_smp;

in vec3 v_world_pos;
in vec3 v_normal;
in vec2 v_uv;
in vec4 v_color;

out vec4 frag_color;

vec3 to_linear(vec3 c) { return pow(max(c, vec3(0.0)), vec3(2.2)); }
vec3 to_srgb(vec3 c) { return pow(max(c, vec3(0.0)), vec3(1.0 / 2.2)); }

// 1 = fully lit by the sun, 0 = fully in shadow. 3x3 taps, each one a
// hardware-filtered 2x2 comparison, so edges come out soft, not stair-stepped.
float sun_visibility(vec3 n, float view_dist) {
    if (shadow_params.x < 0.5) {
        return 1.0;
    }
    // Pushing the lookup point out along the normal keeps a surface from
    // shadowing itself ("shadow acne") without the peter-panning a big
    // depth bias causes.
    vec3 p = v_world_pos + n * shadow_params.z;
    vec4 s = shadow_matrix * vec4(p, 1.0);
    if (s.x < 0.0 || s.x > 1.0 || s.y < 0.0 || s.y > 1.0 || s.z > 1.0) {
        return 1.0;
    }
    float lit = 0.0;
    for (int y = -1; y <= 1; y++) {
        for (int x = -1; x <= 1; x++) {
            vec2 uv = s.xy + vec2(float(x), float(y)) * shadow_params.y;
            lit += texture(sampler2DShadow(shadow_map, shadow_smp), vec3(uv, s.z));
        }
    }
    lit /= 9.0;
    float fade = clamp((view_dist - shadow_fade.x) * shadow_fade.y, 0.0, 1.0);
    return mix(1.0, mix(1.0, lit, shadow_params.w), 1.0 - fade);
}

void main() {
    vec4 tex = texture(sampler2D(base_tex, base_smp), v_uv);
    vec4 srgb = base_color * v_color * tex;
    if (srgb.a < surface.w) {
        discard;
    }
    vec3 albedo = to_linear(srgb.rgb);
    vec3 color;
    if (surface.z > 0.5) {
        color = albedo;
    } else {
        vec3 n = normalize(v_normal);
        if (!gl_FrontFacing) {
            n = -n; // double-sided materials light their back face as its own front
        }
        vec3 ambient = mix(ground_ambient.rgb, sky_ambient.rgb, n.y * 0.5 + 0.5);
        vec3 l = -sun_dir.xyz;
        float ndotl = max(dot(n, l), 0.0);
        vec3 to_eye = camera_pos.xyz - v_world_pos;
        vec3 v = normalize(to_eye);
        float sun_vis = sun_visibility(n, length(to_eye));
        ndotl *= sun_vis;
        vec3 h = normalize(l + v);
        float spec = ndotl > 0.0 ? pow(max(dot(n, h), 0.0), surface.y) * surface.x : 0.0;
        vec3 diffuse = ambient + sun_color.rgb * ndotl;
        vec3 highlight = sun_color.rgb * spec;
        int count = int(light_count.x);
        for (int i = 0; i < count; i++) {
            vec3 to_light = light_pos[i].xyz - v_world_pos;
            float dist = length(to_light);
            vec3 ll = to_light / max(dist, 1e-4);
            // Reaches exactly zero at the range, so a light's influence ends
            // where the game says it does instead of trailing off forever.
            float fade = clamp(1.0 - dist / light_pos[i].w, 0.0, 1.0);
            float atten = fade * fade;
            if (light_color[i].w > -1.5) {
                float cos_angle = dot(-ll, light_dir[i].xyz);
                atten *= smoothstep(light_color[i].w, light_dir[i].w, cos_angle);
            }
            float nl = max(dot(n, ll), 0.0);
            diffuse += light_color[i].rgb * (nl * atten);
            vec3 hl = normalize(ll + v);
            highlight += nl > 0.0 ? light_color[i].rgb * (pow(max(dot(n, hl), 0.0), surface.y) * surface.x * atten) : vec3(0.0);
        }
        color = albedo * diffuse + highlight;
    }
    color += to_linear(emissive.rgb * texture(sampler2D(emissive_tex, base_smp), v_uv).rgb);
    if (fog_params.z > 0.5) {
        float d = length(camera_pos.xyz - v_world_pos);
        color = mix(color, fog_color.rgb, clamp((d - fog_params.x) * fog_params.y, 0.0, 1.0));
    }
    frag_color = vec4(to_srgb(color), srgb.a);
}
@end

@program lit lit_vs lit_fs
