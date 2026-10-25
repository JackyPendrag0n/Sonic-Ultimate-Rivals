// The draw's shader key (ng_common.h: K_*), as specialization constants: each pipeline is this shader
// specialized for its key, so the branches below on key bits are resolved when the pipeline is created.
layout(constant_id = 0) const uint KLO = 0u;
layout(constant_id = 1) const uint KHI = 0u;
const bool THROUGH = (KLO & 1u) != 0u;
const bool CLEAR = (KLO & 2u) != 0u;
const bool COLOR = (KLO & 4u) != 0u;
const bool NORMAL = (KLO & 8u) != 0u;
const bool LIGHTING = (KLO & 16u) != 0u;
const bool LMODE = (KLO & 32u) != 0u;
const bool FOG = (KLO & 64u) != 0u;
const bool TEX = (KLO & 128u) != 0u;
const bool TEXALPHA = (KLO & 256u) != 0u;
const bool DOUBLE = (KLO & 512u) != 0u;
const bool DEPAL = (KLO & 1024u) != 0u;
const bool FLAT = (KLO & 2048u) != 0u;
const bool REVNORMAL = (KLO & 4096u) != 0u;
const bool TEXPROJ = (KLO & 8192u) != 0u;
const uint MATUPD = (KLO >> 16u) & 7u;
const uint TEXFUNC = (KLO >> 19u) & 7u;
const uint ATEST = (KLO >> 22u) & 7u;
const uint TEXMAP = (KLO >> 25u) & 3u;
const uint PROJSRC = (KLO >> 27u) & 3u;
const uint LIGHT0 = KHI & 31u;
const uint LIGHT1 = (KHI >> 5u) & 31u;
const uint LIGHT2 = (KHI >> 10u) & 31u;
const uint LIGHT3 = (KHI >> 15u) & 31u;

layout(std140, set = 0, binding = 0) uniform U {
	mat4 u_world, u_view, u_proj, u_tgen;
	vec4 u_vpscale, u_vpcenter, u_fbsize, u_uvso, u_fbtex;
	vec4 u_matamb, u_matdiff, u_matspec, u_matemis, u_ambient;
	vec4 u_lpos[4], u_ldir[4], u_latt[4], u_lamb[4], u_ldiff[4], u_lspec[4], u_lspot[4];
	vec4 u_fog, u_fogcolor, u_texenv;
	ivec4 u_atest, u_depal;
};
