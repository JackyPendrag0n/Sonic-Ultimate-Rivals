// Native GE renderer, Vulkan: a textured rectangle (guest memory drawn into a framebuffer)
#version 450
layout(push_constant) uniform P { vec4 rect, uv; } p;
layout(location = 0) out vec2 v_uv;
void main() {
	vec2 c = vec2(gl_VertexIndex & 1, gl_VertexIndex >> 1);
	gl_Position = vec4(mix(p.rect.xy, p.rect.zw, c), 0.0, 1.0);
	v_uv = mix(p.uv.xy, p.uv.zw, c);
}
