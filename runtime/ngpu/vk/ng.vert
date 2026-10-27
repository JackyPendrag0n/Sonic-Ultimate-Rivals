// Native GE renderer, Vulkan: the vertex shader (the same as ng_gl.cpp's gen_vs, for every key)
#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec2 a_uv;
layout(location = 2) in vec3 a_nrm;
layout(location = 3) in vec4 a_col;
layout(location = 0) out vec4 v_col0;
layout(location = 1) flat out vec4 v_col0f;
layout(location = 2) out vec3 v_col1;
layout(location = 3) flat out vec3 v_col1f;
layout(location = 4) out vec3 v_tc;
layout(location = 5) out float v_fog;

float pspPow(float x, float e) { return e <= 0.0 ? 1.0 : pow(max(x, 0.0), e); }
vec3 nz(vec3 v, vec3 d) { float l = dot(v, v); return l == 0.0 ? d : v * inversesqrt(l); }

// light i (bits: enable, type 2 bits, computation 2 bits)
void light(int i, uint bits, vec3 wp, vec3 wn, vec3 viewDir, vec4 ambC, vec3 difC, vec3 spcC, inout vec4 l0, inout vec3 l1) {
	if ((bits & 1u) == 0u) return;
	uint type = (bits >> 1u) & 3u, comp = (bits >> 3u) & 3u;
	float scale = 1.0;
	vec3 L;
	if (type == 0u) {
		L = u_lpos[i].xyz;   // directional: normalised on upload
	} else {
		L = u_lpos[i].xyz - wp;
		float d2 = dot(L, L), dist = sqrt(d2);
		L = d2 > 0.0 ? L / dist : L;
		scale = clamp(1.0 / dot(u_latt[i].xyz, vec3(1.0, dist, d2)), 0.0, 1.0);
		if (type >= 2u) { float a = dot(u_ldir[i].xyz, L); scale = a >= u_lspot[i].x ? scale * pspPow(a, u_lspot[i].y) : 0.0; }
	}
	float ldot = dot(L, wn);
	if (comp == 2u) ldot = pspPow(ldot, u_matspec.a);
	l0.rgb += (u_lamb[i].rgb * ambC.rgb + u_ldiff[i].rgb * difC * max(ldot, 0.0)) * scale;
	if (comp == 1u && ldot >= 0.0) { vec3 h = nz(L + viewDir, vec3(0.0)); l1 += u_lspec[i].rgb * spcC * pspPow(dot(h, wn), u_matspec.a) * scale; }
}

void out_colors(vec4 c0, vec3 c1) { v_col0 = c0; v_col0f = c0; v_col1 = c1; v_col1f = c1; }

void main() {
	gl_PointSize = 1.0;
	vec4 vc = COLOR ? a_col : u_matamb;
	// (Vulkan's depth range is [0, w], not [-w, w]; PSP row 0 is image row 0, as on the OpenGL side)
	if (THROUGH) {
		gl_Position = vec4(a_pos.x * 2.0 / u_fbsize.x - 1.0, a_pos.y * 2.0 / u_fbsize.y - 1.0, a_pos.z * (1.0 / 65535.0), 1.0);
		out_colors(vc, vec3(0.0));
		v_tc = vec3(a_uv * u_fbsize.zw, 1.0);
		v_fog = 1.0;
		return;
	}
	vec4 wp = u_world * vec4(a_pos, 1.0);
	vec3 n0 = REVNORMAL ? -a_nrm : a_nrm;
	if (!NORMAL) n0 = REVNORMAL ? vec3(0.0, 0.0, -1.0) : vec3(0.0, 0.0, 1.0);
	vec3 wn = nz(mat3(u_world) * n0, vec3(0.0, 0.0, 1.0));
	vec4 vp = u_view * wp;
	vec4 clip = u_proj * vp;
	// the GE viewport, linear in clip space so that clipping still works: screen = ndc * scale + centre
	vec3 sc = clip.xyz * u_vpscale.xyz + u_vpcenter.xyz * clip.w;
	gl_Position = vec4(sc.x * 2.0 / u_fbsize.x - clip.w, sc.y * 2.0 / u_fbsize.y - clip.w, sc.z * (1.0 / 65535.0), clip.w);
	if (LIGHTING) {
		vec4 ambC = (MATUPD & 1u) != 0u ? vc : u_matamb;
		vec3 difC = (MATUPD & 2u) != 0u ? vc.rgb : u_matdiff.rgb;
		vec3 spcC = (MATUPD & 4u) != 0u ? vc.rgb : u_matspec.rgb;
		vec4 l0 = u_ambient * ambC + vec4(u_matemis.rgb, 0.0);
		vec3 l1 = vec3(0.0);
		vec3 viewDir = nz(vec3(u_view[0].z, u_view[1].z, u_view[2].z), vec3(0.0, 0.0, 1.0));
		light(0, LIGHT0, wp.xyz, wn, viewDir, ambC, difC, spcC, l0, l1);
		light(1, LIGHT1, wp.xyz, wn, viewDir, ambC, difC, spcC, l0, l1);
		light(2, LIGHT2, wp.xyz, wn, viewDir, ambC, difC, spcC, l0, l1);
		light(3, LIGHT3, wp.xyz, wn, viewDir, ambC, difC, spcC, l0, l1);
		if (LMODE) out_colors(clamp(l0, 0.0, 1.0), clamp(l1, 0.0, 1.0));
		else out_colors(clamp(vec4(l0.rgb + l1, l0.a), 0.0, 1.0), vec3(0.0));
	} else {
		out_colors(vc, vec3(0.0));
	}
	if (TEX) {
		if (TEXMAP == 1u) {
			// (the texture scale and offset only apply to UV mapping, not to the texture matrix)
			vec4 src = PROJSRC == 0u ? vec4(a_pos, 1.0) : PROJSRC == 1u ? vec4(a_uv, 0.0, 1.0) : PROJSRC == 2u ? vec4(nz(n0, vec3(0.0)), 1.0) : vec4(n0, 1.0);
			v_tc = (u_tgen * src).xyz;
		} else {
			v_tc = vec3(a_uv * u_uvso.xy + u_uvso.zw, 1.0);
		}
	} else {
		v_tc = vec3(0.0, 0.0, 1.0);
	}
	v_fog = FOG ? (vp.z + u_fog.x) * u_fog.y : 1.0;
}
