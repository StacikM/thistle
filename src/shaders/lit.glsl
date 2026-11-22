// The main 3D surface shader: hemisphere ambient (sky above, ground below),
// one directional sun and up to 16 point/spot lights, each with a
// Blinn-Phong highlight (or, for materials with `pbr` on, a GGX
// metallic-roughness BRDF, normal/AO maps and reflected ambient), then
// distance fog. All lighting math
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

// Same outputs as lit_vs, for World::draw_many(): the per-copy transform and
// tint come from a second, per-instance vertex buffer. `model` (the uniform)
// is the part's own placement inside the model, applied first.
@vs lit_instanced_vs
layout(binding=0) uniform lit_vs_params {
    mat4 model;
    mat4 view_proj;
    mat4 normal_matrix;
    vec4 uv_transform;
};

in vec3 position;
in vec3 normal;
in vec2 texcoord0;
in vec4 color0;
in vec4 inst_m0;
in vec4 inst_m1;
in vec4 inst_m2;
in vec4 inst_m3;
in vec4 inst_color;

out vec3 v_world_pos;
out vec3 v_normal;
out vec2 v_uv;
out vec4 v_color;

void main() {
    mat4 world = mat4(inst_m0, inst_m1, inst_m2, inst_m3) * model;
    vec4 world_pos = world * vec4(position, 1.0);
    v_world_pos = world_pos.xyz;
    // The cofactor matrix is the inverse-transpose times the determinant:
    // right for non-uniform scale, and far cheaper than inverse() per
    // vertex. Its sign flips for mirrored copies, so undo that.
    vec3 c0 = world[0].xyz, c1 = world[1].xyz, c2 = world[2].xyz;
    mat3 cof = mat3(cross(c1, c2), cross(c2, c0), cross(c0, c1));
    float det = dot(c0, cross(c1, c2));
    v_normal = (det < 0.0 ? -1.0 : 1.0) * (cof * normal);
    v_uv = texcoord0 * uv_transform.xy + uv_transform.zw;
    v_color = color0 * inst_color;
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
    vec4 pbr;        // x = physically based (0/1), y = metallic, z = roughness, w = normal scale
    vec4 pbr_maps;   // x = has normal map, y = has metallic-roughness map, z = has occlusion map, w = occlusion strength
};

layout(binding=0) uniform texture2D base_tex;
layout(binding=1) uniform texture2D emissive_tex;
layout(binding=0) uniform sampler base_smp;
layout(binding=2) uniform texture2D shadow_map;
layout(binding=1) uniform samplerShadow shadow_smp;
layout(binding=3) uniform texture2D normal_tex;
layout(binding=4) uniform texture2D metal_rough_tex;
layout(binding=5) uniform texture2D occlusion_tex;

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

const float PI = 3.14159265;

// Bends the surface normal by a tangent-space normal map. The tangent comes
// from how the texture coordinates change across the screen, so models need
// no stored tangents (glTF's own rule for files that have none).
vec3 perturb_normal(vec3 n, vec3 world_pos, vec2 uv) {
    vec3 uv_dx = vec3(dFdx(uv), 0.0);
    vec3 uv_dy = vec3(dFdy(uv), 0.0);
    float det = uv_dx.x * uv_dy.y - uv_dy.x * uv_dx.y;
    if (abs(det) < 1e-12) {
        return n;
    }
    vec3 t = (uv_dy.y * dFdx(world_pos) - uv_dx.y * dFdy(world_pos)) / det;
    t = normalize(t - n * dot(n, t));
    vec3 b = cross(n, t);
    vec3 m = texture(sampler2D(normal_tex, base_smp), uv).xyz * 2.0 - 1.0;
    m.xy *= pbr.w;
    return normalize(mat3(t, b, n) * m);
}

float distribution_ggx(float nh, float a2) {
    float d = nh * nh * (a2 - 1.0) + 1.0;
    return a2 / (PI * d * d);
}

// Smith visibility, height-correlated, with the 1 / (4 n.l n.v) already in it.
float visibility_smith(float nl, float nv, float a2) {
    float gv = nl * sqrt(nv * nv * (1.0 - a2) + a2);
    float gl = nv * sqrt(nl * nl * (1.0 - a2) + a2);
    return 0.5 / max(gv + gl, 1e-5);
}

vec3 fresnel_schlick(vec3 f0, float vh) {
    return f0 + (1.0 - f0) * pow(1.0 - vh, 5.0);
}

// One light's contribution. `radiance` is the light's color already scaled by
// its distance falloff, in this engine's units (a white surface facing a
// light of 1 comes out 1, the same as the classic path). The specular lobe is
// multiplied by pi to match: Lambert's 1/pi is folded into those units.
vec3 pbr_light(vec3 n, vec3 v, vec3 l, vec3 radiance, vec3 diffuse_color, vec3 f0, float a2) {
    float nl = max(dot(n, l), 0.0);
    if (nl <= 0.0) {
        return vec3(0.0);
    }
    vec3 h = normalize(l + v);
    float nv = max(dot(n, v), 1e-4);
    float nh = max(dot(n, h), 0.0);
    float vh = max(dot(v, h), 0.0);
    vec3 f = fresnel_schlick(f0, vh);
    vec3 spec = f * (distribution_ggx(nh, a2) * visibility_smith(nl, nv, a2));
    return ((1.0 - f) * diffuse_color + spec * PI) * radiance * nl;
}

// Karis' analytic fit to the split-sum environment BRDF: how much of the
// reflected ambient light a surface of this roughness actually returns.
vec3 env_brdf(vec3 f0, float roughness, float nv) {
    const vec4 c0 = vec4(-1.0, -0.0275, -0.572, 0.022);
    const vec4 c1 = vec4(1.0, 0.0425, 1.04, -0.04);
    vec4 r = roughness * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * nv)) * r.x + r.y;
    vec2 ab = vec2(-1.04, 1.04) * a004 + r.zw;
    return f0 * ab.x + ab.y;
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
        if (pbr.x > 0.5 && pbr_maps.x > 0.5) {
            n = perturb_normal(n, v_world_pos, v_uv);
        }
        vec3 ambient = mix(ground_ambient.rgb, sky_ambient.rgb, n.y * 0.5 + 0.5);
        vec3 l = -sun_dir.xyz;
        float ndotl = max(dot(n, l), 0.0);
        vec3 to_eye = camera_pos.xyz - v_world_pos;
        vec3 v = normalize(to_eye);
        float sun_vis = sun_visibility(n, length(to_eye));
        ndotl *= sun_vis;
        vec3 h = normalize(l + v);
        if (pbr.x > 0.5) {
            float metallic = clamp(pbr.y, 0.0, 1.0);
            float roughness = clamp(pbr.z, 0.04, 1.0);
            float occlusion = 1.0;
            if (pbr_maps.y > 0.5) {
                vec3 mr = texture(sampler2D(metal_rough_tex, base_smp), v_uv).rgb;
                roughness = clamp(roughness * mr.g, 0.04, 1.0);
                metallic = clamp(metallic * mr.b, 0.0, 1.0);
            }
            if (pbr_maps.z > 0.5) {
                occlusion = mix(1.0, texture(sampler2D(occlusion_tex, base_smp), v_uv).r, pbr_maps.w);
            }
            float a = roughness * roughness;
            float a2 = max(a * a, 0.002);
            vec3 diffuse_color = albedo * (1.0 - metallic);
            vec3 f0 = mix(vec3(0.04), albedo, metallic);
            float nv = max(dot(n, v), 1e-4);

            vec3 lit = pbr_light(n, v, l, sun_color.rgb * sun_vis, diffuse_color, f0, a2);
            int pcount = int(light_count.x);
            for (int i = 0; i < pcount; i++) {
                vec3 to_light = light_pos[i].xyz - v_world_pos;
                float dist = length(to_light);
                vec3 ll = to_light / max(dist, 1e-4);
                float fade = clamp(1.0 - dist / light_pos[i].w, 0.0, 1.0);
                float atten = fade * fade;
                if (light_color[i].w > -1.5) {
                    float cos_angle = dot(-ll, light_dir[i].xyz);
                    atten *= smoothstep(light_color[i].w, light_dir[i].w, cos_angle);
                }
                lit += pbr_light(n, v, ll, light_color[i].rgb * atten, diffuse_color, f0, a2);
            }
            // Ambient: the sky/ground hemisphere lights the diffuse part, and
            // is mirrored (blurrier as the surface gets rougher) in the specular part.
            vec3 refl = reflect(-v, n);
            vec3 env_sharp = mix(ground_ambient.rgb, sky_ambient.rgb, refl.y * 0.5 + 0.5);
            vec3 env = mix(env_sharp, (ground_ambient.rgb + sky_ambient.rgb) * 0.5, roughness * roughness);
            color = lit + (diffuse_color * ambient + env * env_brdf(f0, roughness, nv)) * occlusion;
        } else {
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
@program lit_instanced lit_instanced_vs lit_fs
