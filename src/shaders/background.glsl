// Fullscreen sky pass that also resets depth to the far plane. Every 3D
// render starts with it: sokol has no mid-pass depth clear, and a 3D scene
// is drawn in the middle of the 2D frame (between sokol_gl layers), so this
// is how a World gets a clean depth buffer without ending the pass. With
// the sky hidden, the pipeline masks color writes and only depth is reset.
@ctype mat4 thistle::three::mat4
@ctype vec4 thistle::three::vec4

@vs background_vs
layout(binding=0) uniform background_vs_params {
    mat4 inv_view_proj; // OpenGL-convention clip space, whatever the backend
};

out vec3 v_dir;

void main() {
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2)) * 2.0 - 1.0;
    vec4 far_pt = inv_view_proj * vec4(p, 1.0, 1.0);
    vec4 near_pt = inv_view_proj * vec4(p, -1.0, 1.0);
    v_dir = far_pt.xyz / far_pt.w - near_pt.xyz / near_pt.w;
    gl_Position = vec4(p, 1.0, 1.0); // z == w: exactly the far plane on every backend
}
@end

@fs background_fs
layout(binding=1) uniform background_fs_params {
    vec4 top;     // sRGB
    vec4 horizon; // sRGB
    vec4 ground;  // sRGB
};

in vec3 v_dir;
out vec4 frag_color;

void main() {
    float y = normalize(v_dir).y;
    vec3 c = y >= 0.0
        ? mix(horizon.rgb, top.rgb, pow(y, 0.6))
        : mix(horizon.rgb, ground.rgb, pow(-y, 0.4));
    frag_color = vec4(c, 1.0);
}
@end

@program background background_vs background_fs
