// Native GE renderer, Vulkan: the fragment shader (the same as ng_gl.cpp's gen_fs, for every key)
#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

layout(set = 1, binding = 0) uniform sampler2D u_tex;
layout(set = 1, binding = 1) uniform sampler2D u_pal;
layout(location = 0) in vec4 v_col0;
layout(location = 1) flat in vec4 v_col0f;
layout(location = 2) in vec3 v_col1;
layout(location = 3) flat in vec3 v_col1f;
layout(location = 4) in vec3 v_tc;
layout(location = 5) in float v_fog;
layout(location = 0) out vec4 o_col;

void main() {
	vec4 c = FLAT ? v_col0f : v_col0;
	if (CLEAR) { o_col = c; return; }
	if (TEX) {
		vec2 uv = TEXPROJ ? (v_tc.z != 0.0 ? v_tc.xy / v_tc.z : vec2(0.0)) : v_tc.xy;
		uv = uv * u_fbtex.xy + u_fbtex.zw;
		vec4 t;
		if (DEPAL) {
			// a framebuffer read as a 32-bit indexed texture: the index is a byte of each pixel
			ivec2 ts = textureSize(u_tex, 0);
			uvec4 b = uvec4(texelFetch(u_tex, clamp(ivec2(uv * vec2(ts)), ivec2(0), ts - 1), 0) * 255.0 + 0.5);
			uint px = b.r | (b.g << 8) | (b.b << 16) | (b.a << 24);
			int idx = (int(px >> uint(u_depal.x)) & u_depal.y) | u_depal.z;
			t = texelFetch(u_pal, ivec2(idx & 255, 0), 0);
		} else {
			t = texture(u_tex, uv);
		}
		if (TEXFUNC == 1u) c = TEXALPHA ? vec4(mix(c.rgb, t.rgb, t.a), c.a) : vec4(t.rgb, c.a);                        // decal
		else if (TEXFUNC == 2u) c = vec4(mix(c.rgb, u_texenv.rgb, t.rgb), TEXALPHA ? c.a * t.a : c.a);               // blend
		else if (TEXFUNC == 3u) c = TEXALPHA ? t : vec4(t.rgb, c.a);                                                 // replace
		else if (TEXFUNC == 4u) c = vec4(c.rgb + t.rgb, TEXALPHA ? c.a * t.a : c.a);                                 // add
		else c = TEXALPHA ? c * t : vec4(c.rgb * t.rgb, c.a);                                                         // modulate
		if (DOUBLE) c.rgb *= 2.0;
	}
	c.rgb = clamp(c.rgb + (FLAT ? v_col1f : v_col1), 0.0, 1.0);
	if (ATEST == 0u) discard;
	if (ATEST > 1u) {
		int a = int(c.a * 255.0 + 0.5) & u_atest.y, r = u_atest.x & u_atest.y;
		bool pass = ATEST == 2u ? a == r : ATEST == 3u ? a != r : ATEST == 4u ? a < r : ATEST == 5u ? a <= r : ATEST == 6u ? a > r : a >= r;
		if (!pass) discard;
	}
	if (FOG) c.rgb = mix(u_fogcolor.rgb, c.rgb, clamp(v_fog, 0.0, 1.0));
	o_col = c;
}
