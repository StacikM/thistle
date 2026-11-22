// Post-processing for World::post: bloom (a chain of shrinking images that
// are blurred and added back up) and the final tonemap. The 3D scene is drawn
// into a half-float image first; colors in it are stored the way the other
// shaders already write them, gamma-encoded (pow 1/2.2) but not clamped, so
// the sky, billboards and lines needed no changes and anything brighter than
// white survives. Every pass here decodes to linear light, works, and (for
// the composite) encodes again.

@vs post_vs
layout(binding=0) uniform post_vs_params {
    vec4 flags; // x = 1 where texture row 0 is the top (D3D11, Metal), 0 on OpenGL
};

out vec2 v_uv;

void main() {
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2)) * 2.0 - 1.0;
    // The place this pixel was drawn to is where it is read from, whichever
    // way up the backend keeps its render targets.
    v_uv = vec2(p.x * 0.5 + 0.5, flags.x > 0.5 ? 0.5 - p.y * 0.5 : p.y * 0.5 + 0.5);
    gl_Position = vec4(p, 0.0, 1.0);
}
@end

@fs bloom_prefilter_fs
layout(binding=1) uniform bloom_prefilter_params {
    vec4 texel;  // xy = size of one source texel in uv
    vec4 bloom;  // x = threshold, y = soft knee (0..1)
};
layout(binding=0) uniform texture2D src_tex;
layout(binding=0) uniform sampler src_smp;

in vec2 v_uv;
out vec4 frag_color;

vec3 decode(vec3 c) { return pow(max(c, vec3(0.0)), vec3(2.2)); }

vec3 fetch(vec2 uv) {
    return decode(texture(sampler2D(src_tex, src_smp), uv).rgb);
}

void main() {
    vec2 o = texel.xy;
    vec3 c = (fetch(v_uv + vec2(-o.x, -o.y)) + fetch(v_uv + vec2(o.x, -o.y)) +
              fetch(v_uv + vec2(-o.x, o.y)) + fetch(v_uv + vec2(o.x, o.y))) * 0.25;
    // Soft threshold: light starts to glow gradually as it passes the knee,
    // so a pixel creeping past white doesn't make the glow pop on.
    float brightness = max(c.r, max(c.g, c.b));
    float knee = max(bloom.x * bloom.y, 1e-4);
    float soft = clamp(brightness - bloom.x + knee, 0.0, 2.0 * knee);
    soft = soft * soft / (4.0 * knee);
    float contribution = max(soft, brightness - bloom.x) / max(brightness, 1e-4);
    // A single blown-out pixel (a sun glint) would otherwise smear across the whole screen.
    frag_color = vec4(min(c * contribution, vec3(64.0)), 1.0);
}
@end

@fs bloom_down_fs
layout(binding=1) uniform bloom_down_params {
    vec4 texel; // xy = size of one source texel in uv
};
layout(binding=0) uniform texture2D src_tex;
layout(binding=0) uniform sampler src_smp;

in vec2 v_uv;
out vec4 frag_color;

void main() {
    vec2 o = texel.xy;
    vec3 sum = texture(sampler2D(src_tex, src_smp), v_uv).rgb * 4.0;
    sum += texture(sampler2D(src_tex, src_smp), v_uv + vec2(-o.x, -o.y)).rgb;
    sum += texture(sampler2D(src_tex, src_smp), v_uv + vec2(o.x, -o.y)).rgb;
    sum += texture(sampler2D(src_tex, src_smp), v_uv + vec2(-o.x, o.y)).rgb;
    sum += texture(sampler2D(src_tex, src_smp), v_uv + vec2(o.x, o.y)).rgb;
    frag_color = vec4(sum * 0.125, 1.0);
}
@end

// Drawn with additive blending onto the next bigger image in the chain.
@fs bloom_up_fs
layout(binding=1) uniform bloom_up_params {
    vec4 texel; // xy = size of one source texel in uv, already scaled by the radius
};
layout(binding=0) uniform texture2D src_tex;
layout(binding=0) uniform sampler src_smp;

in vec2 v_uv;
out vec4 frag_color;

vec3 fetch(vec2 uv) { return texture(sampler2D(src_tex, src_smp), uv).rgb; }

void main() {
    vec2 o = texel.xy;
    vec3 sum = fetch(v_uv + vec2(-2.0 * o.x, 0.0)) + fetch(v_uv + vec2(2.0 * o.x, 0.0)) +
               fetch(v_uv + vec2(0.0, -2.0 * o.y)) + fetch(v_uv + vec2(0.0, 2.0 * o.y));
    sum += 2.0 * (fetch(v_uv + vec2(-o.x, -o.y)) + fetch(v_uv + vec2(o.x, -o.y)) +
                  fetch(v_uv + vec2(-o.x, o.y)) + fetch(v_uv + vec2(o.x, o.y)));
    frag_color = vec4(sum / 12.0, 1.0);
}
@end

@fs post_composite_fs
layout(binding=1) uniform post_composite_params {
    vec4 grade; // x = exposure, y = tonemap (0 none, 1 Reinhard, 2 ACES), z = bloom intensity, w = bloom on (0/1)
};
layout(binding=0) uniform texture2D scene_tex;
layout(binding=1) uniform texture2D bloom_tex;
layout(binding=0) uniform sampler post_smp;

in vec2 v_uv;
out vec4 frag_color;

vec3 aces(vec3 x) {
    return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
}

void main() {
    vec4 scene = texture(sampler2D(scene_tex, post_smp), v_uv);
    vec3 c = pow(max(scene.rgb, vec3(0.0)), vec3(2.2));
    if (grade.w > 0.5) {
        c += texture(sampler2D(bloom_tex, post_smp), v_uv).rgb * grade.z;
    }
    c *= grade.x;
    if (grade.y > 1.5) {
        c = aces(c);
    } else if (grade.y > 0.5) {
        c = c / (1.0 + c);
    } else {
        c = clamp(c, 0.0, 1.0);
    }
    frag_color = vec4(pow(c, vec3(1.0 / 2.2)), scene.a);
}
@end

@program bloom_prefilter post_vs bloom_prefilter_fs
@program bloom_down post_vs bloom_down_fs
@program bloom_up post_vs bloom_up_fs
@program post_composite post_vs post_composite_fs
