// Unlit colored lines for World::line()/wire_box()/grid() — debug drawing
// and editor gizmos. Colors pass straight through (sRGB in, sRGB out).
@ctype mat4 thistle::three::mat4

@vs lines_vs
layout(binding=0) uniform lines_vs_params {
    mat4 view_proj;
};

in vec3 position;
in vec4 color0;
out vec4 v_color;

void main() {
    v_color = color0;
    gl_Position = view_proj * vec4(position, 1.0);
}
@end

@fs lines_fs
in vec4 v_color;
out vec4 frag_color;

void main() {
    frag_color = v_color;
}
@end

@program lines lines_vs lines_fs
