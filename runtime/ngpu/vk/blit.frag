#version 450
layout(set = 1, binding = 0) uniform sampler2D u_tex;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_col;
void main() { o_col = vec4(texture(u_tex, v_uv).rgb, 1.0); }
