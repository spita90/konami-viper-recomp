/*
 * GPU renderer, the executing side (host main thread): runs the command lists the Voodoo device
 * records (runtime/voodoo/voodoo_gpu.h) with OpenGL 3.3 core, and shows the front buffer and the
 * enhanced-mode overlay in the window.
 *
 * The fragment shader is the Voodoo's pixel pipeline (MAME's voodoo_render.cpp: iterators, texture
 * fetch and combine, colour combine, chroma key, alpha test, fog, dithering, depth), evaluated from
 * the same start values and gradients with the same integer arithmetic. What differs from the
 * software rasterizer: alpha blending is the GPU's (the dithered source mixed with the destination,
 * not dithered again after the mix), and coverage follows the GPU's rasterization rules.
 */
#include <SDL.h>
#include <SDL_opengl.h>

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "voodoo/voodoo_gpu.h"

extern "C" void rt_log(const char *fmt, ...);

namespace {

// ------------------------------------------------------------------ GL entry points
#define GL_FUNCS \
	X(const GLubyte *, GetString, (GLenum)) \
	X(GLenum, GetError, (void)) \
	X(void, GetIntegerv, (GLenum, GLint *)) \
	X(void, Viewport, (GLint, GLint, GLsizei, GLsizei)) \
	X(void, Clear, (GLbitfield)) \
	X(void, ClearColor, (GLfloat, GLfloat, GLfloat, GLfloat)) \
	X(void, ClearDepth, (GLdouble)) \
	X(void, Enable, (GLenum)) \
	X(void, Disable, (GLenum)) \
	X(void, BlendFuncSeparate, (GLenum, GLenum, GLenum, GLenum)) \
	X(void, DepthFunc, (GLenum)) \
	X(void, DepthMask, (GLboolean)) \
	X(void, ColorMask, (GLboolean, GLboolean, GLboolean, GLboolean)) \
	X(void, DrawArrays, (GLenum, GLint, GLsizei)) \
	X(void, PixelStorei, (GLenum, GLint)) \
	X(void, Finish, (void)) \
	X(void, ReadPixels, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *)) \
	X(void, GenTextures, (GLsizei, GLuint *)) \
	X(void, DeleteTextures, (GLsizei, const GLuint *)) \
	X(void, BindTexture, (GLenum, GLuint)) \
	X(void, TexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *)) \
	X(void, TexSubImage2D, (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *)) \
	X(void, TexParameteri, (GLenum, GLenum, GLint)) \
	X(void, CopyTexSubImage2D, (GLenum, GLint, GLint, GLint, GLint, GLint, GLsizei, GLsizei)) \
	X(void, ActiveTexture, (GLenum)) \
	X(void, GenFramebuffers, (GLsizei, GLuint *)) \
	X(void, DeleteFramebuffers, (GLsizei, const GLuint *)) \
	X(void, BindFramebuffer, (GLenum, GLuint)) \
	X(void, FramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint)) \
	X(GLenum, CheckFramebufferStatus, (GLenum)) \
	X(void, GenBuffers, (GLsizei, GLuint *)) \
	X(void, BindBuffer, (GLenum, GLuint)) \
	X(void, BufferData, (GLenum, GLsizeiptr, const void *, GLenum)) \
	X(void, GenVertexArrays, (GLsizei, GLuint *)) \
	X(void, BindVertexArray, (GLuint)) \
	X(void, EnableVertexAttribArray, (GLuint)) \
	X(void, VertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void *)) \
	X(void, VertexAttribIPointer, (GLuint, GLint, GLenum, GLsizei, const void *)) \
	X(GLuint, CreateShader, (GLenum)) \
	X(void, ShaderSource, (GLuint, GLsizei, const GLchar *const *, const GLint *)) \
	X(void, CompileShader, (GLuint)) \
	X(void, GetShaderiv, (GLuint, GLenum, GLint *)) \
	X(void, GetShaderInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *)) \
	X(void, DeleteShader, (GLuint)) \
	X(GLuint, CreateProgram, (void)) \
	X(void, AttachShader, (GLuint, GLuint)) \
	X(void, LinkProgram, (GLuint)) \
	X(void, GetProgramiv, (GLuint, GLenum, GLint *)) \
	X(void, GetProgramInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *)) \
	X(void, UseProgram, (GLuint)) \
	X(GLint, GetUniformLocation, (GLuint, const GLchar *)) \
	X(void, Uniform1i, (GLint, GLint)) \
	X(void, Uniform1ui, (GLint, GLuint)) \
	X(void, Uniform1uiv, (GLint, GLsizei, const GLuint *)) \
	X(void, Uniform1f, (GLint, GLfloat)) \
	X(void, Uniform2f, (GLint, GLfloat, GLfloat)) \
	X(void, Uniform4f, (GLint, GLfloat, GLfloat, GLfloat, GLfloat))

struct gl_api
{
#define X(ret, name, args) ret (APIENTRY *name) args;
	GL_FUNCS
#undef X
} G;

bool load_gl()
{
#define X(ret, name, args) \
	G.name = reinterpret_cast<ret (APIENTRY *) args>(SDL_GL_GetProcAddress("gl" #name)); \
	if (!G.name) { rt_log("gpu: missing gl%s\n", #name); return false; }
	GL_FUNCS
#undef X
	return true;
}

// ------------------------------------------------------------------ shaders
const char *const k_draw_vs = R"(#version 330 core
layout(location = 0) in vec2 a_pos;
layout(location = 1) in uint a_tri;
uniform vec2 u_size;
flat out uint v_tri;
void main()
{
	v_tri = a_tri;
	gl_Position = vec4(a_pos / u_size * 2.0 - 1.0, 0.0, 1.0);
}
)";

// The Voodoo pixel pipeline (MAME voodoo_render.cpp); the comments name the functions it follows.
const char *const k_draw_fs = R"(#version 330 core
uniform usampler2D u_vram;      // the VRAM, 1024 words per row
uniform usampler2D u_lut;       // 256-entry tables
uniform usampler2D u_tri;       // triangle records
uniform usampler2D u_state;     // state records
uniform sampler2D u_blur;       // widescreen motion blur: the last snapshot (TF_BLUR)
uniform int u_scale, u_margin, u_dispw;
uniform uint u_vram_mask;
flat in uint v_tri;
layout(location = 0, index = 0) out vec4 o_color;
layout(location = 0, index = 1) out vec4 o_prefog;

// record layout (voodoo_gpu.h)
const int T_AX = 0, T_AY = 1, T_START = 2, T_DX = 7, T_DY = 12, T_STARTW = 17, T_DWDX = 19, T_DWDY = 21,
	T_STATE = 23, T_FLAGS = 24, T_LODBASE0 = 25, T_LODBASE1 = 26, T_TEX0 = 27, T_TEX1 = 36;
const uint TF_STRETCH = 1u, TF_FILL = 2u, TF_BLUR = 4u;
const int S_FBZCP = 0, S_FBZMODE = 1, S_ALPHAMODE = 2, S_FOGMODE = 3, S_TEXMODE0 = 4, S_TEXMODE1 = 5,
	S_COLOR0 = 6, S_COLOR1 = 7, S_CHROMAKEY = 8, S_FOGCOLOR = 9, S_ZACOLOR = 10, S_STIPPLE = 11, S_ALPHAREF = 12,
	S_CHROMARANGE = 13, S_CLIP_X = 14, S_CLIP_Y = 15, S_YORIGIN = 16, S_FOG = 17, S_TMUCONFIG = 18, S_BILINEAR = 19,
	S_TEX0 = 20, S_TEX1 = 35, S_GENERIC = 50;
const uint GENERIC_TEX0_IDENTITY = 4u, GENERIC_TEX1_IDENTITY = 8u;   // the combine is skipped
const int TX_LUT = 0, TX_MASKS = 1, TX_LOD = 2, TX_LODBIAS = 3, TX_DETAILMAX = 4, TX_DETAILBIAS = 5, TX_OFFSET = 6;

uvec4 T[12];
uvec4 S[16];
uint tw(int i) { return T[i >> 2][i & 3]; }
int ti(int i) { return int(T[i >> 2][i & 3]); }
float tf(int i) { return uintBitsToFloat(T[i >> 2][i & 3]); }
uvec2 t64(int i) { return uvec2(tw(i), tw(i + 1)); }
uint sw(int i) { return S[i >> 2][i & 3]; }
uint bf(uint v, int pos, int n) { return (v >> uint(pos)) & ((1u << uint(n)) - 1u); }
bool bit(uint v, int pos) { return ((v >> uint(pos)) & 1u) != 0u; }
int sext16(uint v) { return int(v << 16) >> 16; }
ivec4 unpack(uint argb) { return ivec4(int((argb >> 16) & 255u), int((argb >> 8) & 255u), int(argb & 255u), int(argb >> 24)); }

const int k_dither4[16] = int[16](0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5);
const int k_dither2[16] = int[16](8, 10, 8, 10, 11, 9, 11, 9, 8, 10, 8, 10, 11, 9, 11, 9);
const int k_log2[128] = int[128](
	  0,   2,   5,   8,  11,  14,  16,  19,  22,  25,  27,  30,  33,  35,  38,  40,
	 43,  46,  48,  51,  53,  56,  58,  61,  63,  65,  68,  70,  73,  75,  77,  80,
	 82,  84,  87,  89,  91,  93,  96,  98, 100, 102, 104, 106, 109, 111, 113, 115,
	117, 119, 121, 123, 125, 127, 129, 132, 134, 136, 138, 140, 141, 143, 145, 147,
	149, 151, 153, 155, 157, 159, 161, 162, 164, 166, 168, 170, 172, 173, 175, 177,
	179, 181, 182, 184, 186, 188, 189, 191, 193, 194, 196, 198, 200, 201, 203, 205,
	206, 208, 209, 211, 213, 214, 216, 218, 219, 221, 222, 224, 225, 227, 229, 230,
	232, 233, 235, 236, 238, 239, 241, 242, 244, 245, 247, 248, 250, 251, 253, 254);

// ---- 64-bit integers as (lo, hi)
uvec2 add64(uvec2 a, uvec2 b) { uint lo = a.x + b.x; return uvec2(lo, a.y + b.y + (lo < a.x ? 1u : 0u)); }
uvec2 neg64(uvec2 a) { return add64(uvec2(~a.x, ~a.y), uvec2(1u, 0u)); }
uvec2 mul64(uvec2 a, int k)     // |k| < 2^15
{
	uint m = uint(abs(k));
	uint l0 = (a.x & 0xffffu) * m, l1 = (a.x >> 16) * m;
	uint lo = l0 + (l1 << 16);
	uvec2 r = uvec2(lo, a.y * m + (l1 >> 16) + (lo < l0 ? 1u : 0u));
	return k < 0 ? neg64(r) : r;
}
uvec2 from_float64(float f)
{
	float hi = floor(f / 4294967296.0);
	return uvec2(uint(f - hi * 4294967296.0), uint(int(hi)));
}
float to_float64(uvec2 a) { return float(int(a.y)) * 4294967296.0 + float(a.x); }
int clz32(uint v)
{
	if (v == 0u) return 32;
	int n = 0;
	if ((v & 0xffff0000u) == 0u) { n += 16; v <<= 16; }
	if ((v & 0xff000000u) == 0u) { n += 8; v <<= 8; }
	if ((v & 0xf0000000u) == 0u) { n += 4; v <<= 4; }
	if ((v & 0xc0000000u) == 0u) { n += 2; v <<= 2; }
	if ((v & 0x80000000u) == 0u) n += 1;
	return n;
}

// ---- dithering to 5-6-5 (dither_helper), expanded back to 8 bits as the display reads it
int dither_rb(int v, int d) { return ((v << 1) - (v >> 4) + (v >> 7) + d) >> 4; }
int dither_g(int v, int d) { return ((v << 2) - (v >> 4) + (v >> 6) + d) >> 4; }
vec3 to565(ivec3 c, uint fbz, ivec2 p)
{
	int r, g, b;
	if (!bit(fbz, 8)) { r = c.r >> 3; g = c.g >> 2; b = c.b >> 3; }
	else
	{
		int i = (p.y & 3) * 4 + (p.x & 3);
		int d = bit(fbz, 11) ? k_dither2[i] : k_dither4[i];
		r = dither_rb(c.r, d); g = dither_g(c.g, d); b = dither_rb(c.b, d);
	}
	return vec3(float((r << 3) | (r >> 2)), float((g << 2) | (g >> 4)), float((b << 3) | (b >> 2))) / 255.0;
}

// ---- iterators (compute_wfloat, clamped_argb, clamped_z, clamped_w, compute_depthval)
int wfloat(uvec2 w)
{
	int e = (w.y != 0u ? clz32(w.y) : 32 + clz32(w.x)) - 16;
	if (e < 0) return 0;
	if (e >= 16) return 0xffff;
	int sh = 35 - e;
	uint v = sh >= 32 ? (w.y >> uint(sh - 32)) : ((w.x >> uint(sh)) | (w.y << uint(32 - sh)));
	return ((e << 12) | int((v ^ 0x1fffu) & 0xfffu)) + 1;
}
int clamp_argb(int iter, bool clampit)
{
	int r = int(uint(iter) >> 20);
	if (clampit) return min(r, 255);
	if (r == 0xfff) return 0;
	if (r == 0x100) return 0xff;
	return r & 0xff;
}
int clamped_z(int iterz, uint cp)
{
	if (bit(cp, 28)) return clamp(iterz >> 12, 0, 0xffff);
	uint r = uint(iterz) >> 12;
	if (r == 0xfffffu) return 0;
	if (r == 0x10000u) return 0xffff;
	return int(r & 0xffffu);
}
int clamped_w(uvec2 w, uint cp)
{
	if (bit(cp, 28)) return clamp(int(w.y) >> 16, 0, 0xff);
	uint r = w.y >> 16;
	if (r == 0xffffu) return 0;
	if (r == 0x100u) return 0xff;
	return int(r & 0xffu);
}
int depth_value(uint fbz, uint cp, int wf, int iterz)
{
	int r;
	if (bit(fbz, 3))
	{
		if (!bit(fbz, 21)) r = wf;
		else if ((uint(iterz) & 0xf0000000u) != 0u) r = 0;
		else if ((uint(iterz) & 0x0ffff000u) == 0u) r = 0xffff;
		else
		{
			int e = clz32(uint(iterz)) - 4;
			return ((e << 12) | ((iterz >> (15 - e)) ^ 0x1fff)) + 1;
		}
	}
	else
		r = clamped_z(iterz, cp);
	if (bit(fbz, 16)) r = clamp(r + sext16(sw(S_ZACOLOR)), 0, 0xffff);
	return r;
}

// ---- textures (lookup_single_texel, fetch_texel, combine_texture)
uint vram_word(uint addr)
{
	addr &= u_vram_mask;
	return texelFetch(u_vram, ivec2(int((addr >> 2) & 1023u), int(addr >> 12)), 0).r;
}
uint lut(uint row, uint index) { return texelFetch(u_lut, ivec2(int(index), int(row)), 0).r; }
uint p5(uint v) { return (v << 3) | (v >> 2); }
uint p6(uint v) { return (v << 2) | (v >> 4); }
uint p4(uint v) { return (v << 4) | v; }
uint texel_lookup(uint format, uint row, uint base, int s, int t)
{
	if (format < 8u)
	{
		uint a = base + uint(t + s);
		return lut(row, (vram_word(a) >> ((a & 3u) * 8u)) & 0xffu);
	}
	uint a = base + 2u * uint(t + s);
	uint v = (vram_word(a) >> ((a & 2u) * 8u)) & 0xffffu;
	if (format == 10u)
		return 0xff000000u | (p5(v >> 11) << 16) | (p6((v >> 5) & 63u) << 8) | p5(v & 31u);
	if (format == 11u)
		return ((v & 0x8000u) != 0u ? 0xff000000u : 0u) | (p5((v >> 10) & 31u) << 16) | (p5((v >> 5) & 31u) << 8) | p5(v & 31u);
	if (format == 12u)
		return (p4(v >> 12) << 24) | (p4((v >> 8) & 15u) << 16) | (p4((v >> 4) & 15u) << 8) | p4(v & 15u);
	return (lut(row, v & 0xffu) & 0xffffffu) | ((v & 0xff00u) << 16);
}
int fast_log2(float v, int fracbits)
{
	if (v < 0.0) return 0;
	uint b = floatBitsToUint(v);
	int e = int((b >> 23) & 255u) - 127 - fracbits;
	return (e << 8) | k_log2[int((b >> 16) & 127u)];
}
ivec4 fetch_texel(int sb, uint tm, vec3 it, inout int lod, ivec2 p)
{
	int s, t;
	if (bit(tm, 0))
	{
		float recip = 256.0 / it.z;
		s = int(it.x * recip);
		t = int(it.y * recip);
		lod -= fast_log2(it.z, 32);
	}
	else
	{
		s = int(it.x * (1.0 / 16777216.0));
		t = int(it.y * (1.0 / 16777216.0));
	}
	if (bit(tm, 3) && it.z < 0.0) { s = 0; t = 0; }
	uint lodw = sw(sb + TX_LOD);
	int lodmin = sext16(lodw & 0xffffu), lodmax = int(lodw) >> 16;
	lod += int(sw(sb + TX_LODBIAS));
	if (bit(tm, 4)) lod += k_dither4[(p.y & 3) * 4 + (p.x & 3)] << 4;
	lod = clamp(lod, lodmin, lodmax);
	uint masks = sw(sb + TX_MASKS);
	int wmask = int(masks & 255u), hmask = int((masks >> 8) & 255u);
	uint lodmask = (masks >> 16) & 0x1ffu;
	int ilod = lod >> 8;
	ilod = min(ilod + int((~lodmask >> uint(ilod)) & 1u), 8);
	uint base = sw(sb + TX_OFFSET + ilod);
	int smax = wmask >> ilod, tmax = hmask >> ilod;
	uint format = bf(tm, 8, 4), row = sw(sb + TX_LUT);
	if ((lod == lodmin && !bit(tm, 2)) || (lod != lodmin && !bit(tm, 1)))
	{
		s >>= ilod + 8;
		t >>= ilod + 8;
		if (bit(tm, 6)) s = clamp(s, 0, smax);
		if (bit(tm, 7)) t = clamp(t, 0, tmax);
		s &= smax;
		t &= tmax;
		return unpack(texel_lookup(format, row, base, s, t * (smax + 1)));
	}
	s >>= ilod;
	t >>= ilod;
	s -= 0x80;
	t -= 0x80;
	int bm = int(sw(S_BILINEAR));
	int sfrac = s & bm, tfrac = t & bm;
	s >>= 8;
	t >>= 8;
	int s1 = s + 1, t1 = t + 1;
	if (bit(tm, 6)) { if (s < 0) { s = 0; s1 = 0; } else if (s >= smax) { s = smax; s1 = smax; } }
	s &= smax; s1 &= smax;
	if (bit(tm, 7)) { if (t < 0) { t = 0; t1 = 0; } else if (t >= tmax) { t = tmax; t1 = tmax; } }
	t &= tmax; t1 &= tmax;
	t *= smax + 1; t1 *= smax + 1;
	ivec4 c00 = unpack(texel_lookup(format, row, base, s, t)), c01 = unpack(texel_lookup(format, row, base, s1, t));
	ivec4 c10 = unpack(texel_lookup(format, row, base, s, t1)), c11 = unpack(texel_lookup(format, row, base, s1, t1));
	ivec4 a = c00 * (256 - sfrac) + c01 * sfrac;
	ivec4 b = c10 * (256 - sfrac) + c11 * sfrac;
	return (a * (256 - tfrac) + b * tfrac) >> 16;
}
ivec4 combine_texture(int sb, uint tm, ivec4 cl, ivec4 co, int lod)
{
	ivec4 blend = ivec4(bit(tm, 12) ? ivec3(0) : co.rgb, bit(tm, 21) ? 0 : co.a);
	if (bit(tm, 13) || bit(tm, 22))
		blend -= ivec4(bit(tm, 13) ? cl.rgb : ivec3(0), bit(tm, 22) ? cl.a : 0);
	int detailbias = int(sw(sb + TX_DETAILBIAS)), detailmax = int(sw(sb + TX_DETAILMAX));
	int detail = 0;
	if (detailbias > lod)
		detail = min(((((detailbias - lod) << int(sw(sb + TX_MASKS) >> 28)) >> 8) & 255), detailmax);
	ivec4 f = ivec4(0);
	uint ms = bf(tm, 14, 3);
	if (ms == 1u) f.rgb = cl.rgb;
	else if (ms == 2u) f.rgb = ivec3(co.a);
	else if (ms == 3u) f.rgb = ivec3(cl.a);
	else if (ms == 4u) f.rgb = ivec3(detail);
	else if (ms == 5u) f.rgb = ivec3(lod & 255);
	ms = bf(tm, 23, 3);
	if (ms == 1u || ms == 3u) f.a = cl.a;
	else if (ms == 2u) f.a = co.a;
	else if (ms == 4u) f.a = detail;
	else if (ms == 5u) f.a = lod & 255;
	if (!bit(tm, 17)) f.rgb ^= ivec3(255);
	if (!bit(tm, 26)) f.a ^= 255;
	f += 1;
	ivec4 add = ivec4(0);
	uint ad = bf(tm, 18, 2);
	if (ad == 1u) add.rgb = cl.rgb;
	else if (ad == 2u) add.rgb = ivec3(cl.a);
	if (bf(tm, 27, 2) != 0u) add.a = cl.a;
	blend = clamp(((blend * f) >> 8) + add, 0, 255);
	if (bit(tm, 20)) blend.rgb ^= ivec3(255);
	if (bit(tm, 29)) blend.a ^= 255;
	return blend;
}

// ---- chroma key (chroma_key_test): true when the pixel is keyed out
bool chroma_keyed(ivec3 c)
{
	uint key = sw(S_CHROMAKEY), range = sw(S_CHROMARANGE);
	ivec3 k = unpack(key).rgb;
	if (!bit(range, 28))
		return c == k;
	int res = (c.b >= k.b && c.b <= int(range & 255u)) ? 1 : 0;
	res ^= int(bf(range, 24, 1));
	res <<= 1;
	res |= (c.g >= k.g && c.g <= int((range >> 8) & 255u)) ? 1 : 0;
	res ^= int(bf(range, 25, 1));
	res <<= 1;
	res |= (c.r >= k.r && c.r <= int((range >> 16) & 255u)) ? 1 : 0;
	res ^= int(bf(range, 26, 1));
	return bit(range, 27) ? res != 0 : res == 7;
}

int frac_term(vec2 f, int ddx, int ddy)
{
	return (f.x == 0.0 && f.y == 0.0) ? 0 : int(floor(f.x * float(ddx) + f.y * float(ddy) + 0.5));
}

void main()
{
	int base = int(v_tri) * 12;
	for (int i = 0; i < 12; i++)
		T[i] = texelFetch(u_tri, ivec2((base + i) & 1023, (base + i) >> 10), 0);
	int sbase = int(tw(T_STATE)) * 16;
	for (int i = 0; i < 16; i++)
		S[i] = texelFetch(u_state, ivec2((sbase + i) & 1023, (sbase + i) >> 10), 0);

	ivec2 p = ivec2(gl_FragCoord.xy);      // render target pixel: x, row
	int N = u_scale, M = u_margin;
	uint fbz = sw(S_FBZMODE), flags = tw(T_FLAGS);
	o_prefog = vec4(0.0);

	// fast fill (rasterizer_fastfill): colour1 dithered by the raster row, zaColor
	if ((flags & TF_FILL) != 0u)
	{
		int yr = bit(fbz, 17) ? ((int(sw(S_YORIGIN)) + 1) * N - 1) - p.y : p.y;
		uint za = sw(S_ZACOLOR);
		o_color = vec4(to565(unpack(sw(S_COLOR1)).rgb, fbz, ivec2(p.x, yr)), float(za & 255u) / 255.0);
		gl_FragDepth = float(za & 0xffffu) / 65536.0;
		return;
	}

	// widescreen motion blur (blur_quad): the snapshot at this pixel times the start colour, its
	// start alpha for the blending
	if ((flags & TF_BLUR) != 0u)
	{
		ivec3 s = ivec3(texelFetch(u_blur, p, 0).rgb * 255.0 + 0.5);
		ivec4 it = clamp(ivec4(ti(T_START), ti(T_START + 1), ti(T_START + 2), ti(T_START + 3)) >> 12, 0, 255);
		o_color = vec4(to565((s * (it.rgb + 1)) >> 8, fbz, p), float(it.a) / 255.0);
		gl_FragDepth = 1.0;
		return;
	}

	// clipping, on the scaled target as the software renderer's
	if (bit(fbz, 0))
	{
		uint cx = sw(S_CLIP_X), cy = sw(S_CLIP_Y);
		int cl = int(cx & 0xffffu), cr = int(cx >> 16);
		int x0 = (cl + M) * N, x1 = (cr + M) * N;
		if (M != 0 && cl == 0 && cr >= u_dispw) { x0 = 0; x1 = (u_dispw + 2 * M) * N; }
		if (p.x < x0 || p.x >= x1 || p.y < int(cy & 0xffffu) * N || p.y >= int(cy >> 16) * N)
			discard;
	}
	// stipple, pattern mode (the rotating mode depends on the pixel order)
	if (bit(fbz, 2) && bit(fbz, 12))
		if (((sw(S_STIPPLE) >> uint(((p.y & 3) << 3) | (~p.x & 7))) & 1u) == 0u)
			discard;

	// the native position this pixel stands for: N scaled pixels are centred on a native one
	float xn = (float(p.x) + 0.5) / float(N) - 0.5 - float(M);
	float rn = (float(p.y) + 0.5) / float(N) - 0.5;
	if ((flags & TF_STRETCH) != 0u)
	{
		float c = float(u_dispw) * 0.5, k = float(u_dispw + 2 * M) / float(u_dispw);
		xn = (xn - c) / k + c;
	}
	float yn = bit(fbz, 17) ? float(int(sw(S_YORIGIN))) - rn : rn;
	float xi = floor(xn), yi = floor(yn);
	vec2 fr = vec2(xn - xi, yn - yi);
	int dx = int(xi) - (ti(T_AX) >> 4), dy = int(yi) - (ti(T_AY) >> 4);

	// iterated R, G, B, A, Z (12.12, 20.12) and W (16.32), as the rasterizer has them at this pixel
	int it[5];
	for (int i = 0; i < 5; i++)
		it[i] = ti(T_START + i) + dy * ti(T_DY + i) + dx * ti(T_DX + i) + frac_term(fr, ti(T_DX + i), ti(T_DY + i));
	uvec2 w = add64(add64(t64(T_STARTW), mul64(t64(T_DWDY), dy)), mul64(t64(T_DWDX), dx));
	if (fr != vec2(0.0))
		w = add64(w, from_float64(fr.x * to_float64(t64(T_DWDX)) + fr.y * to_float64(t64(T_DWDY))));
	uvec2 iterw = uvec2(w.x << 16, (w.y << 16) | (w.x >> 16));
	int iterz = it[4];
	uint cp = sw(S_FBZCP);
	int wf = wfloat(iterw);
	int depthval = depth_value(fbz, cp, wf, iterz);

	// textures: TMU1 feeds TMU0
	ivec4 texel = ivec4(0);
	int lodscale = int(round(log2(float(N)) * 256.0));
	vec2 d = vec2(float(dx) + fr.x, float(dy) + fr.y);
	uint tm1 = sw(S_TEXMODE1), tm0 = sw(S_TEXMODE0);
	if (tm1 != 0xffffffffu)
	{
		vec3 st = vec3(tf(T_TEX1), tf(T_TEX1 + 1), tf(T_TEX1 + 2)) + d.x * vec3(tf(T_TEX1 + 3), tf(T_TEX1 + 4), tf(T_TEX1 + 5))
			+ d.y * vec3(tf(T_TEX1 + 6), tf(T_TEX1 + 7), tf(T_TEX1 + 8));
		int lod = ti(T_LODBASE1) - lodscale;
		ivec4 t1 = fetch_texel(S_TEX1, tm1, st, lod, p);
		texel = (sw(S_GENERIC) & GENERIC_TEX1_IDENTITY) != 0u ? t1 : combine_texture(S_TEX1, tm1, t1, texel, lod);
	}
	if (tm0 != 0xffffffffu)
	{
		if (bit(tm0, 31))
			texel = ivec4(0, int((sw(S_TMUCONFIG) >> 8) & 255u), int(sw(S_TMUCONFIG) & 255u), 0);
		else
		{
			vec3 st = vec3(tf(T_TEX0), tf(T_TEX0 + 1), tf(T_TEX0 + 2)) + d.x * vec3(tf(T_TEX0 + 3), tf(T_TEX0 + 4), tf(T_TEX0 + 5))
				+ d.y * vec3(tf(T_TEX0 + 6), tf(T_TEX0 + 7), tf(T_TEX0 + 8));
			int lod = ti(T_LODBASE0) - lodscale;
			ivec4 t0 = fetch_texel(S_TEX0, tm0, st, lod, p);
			texel = (sw(S_GENERIC) & GENERIC_TEX0_IDENTITY) != 0u ? t0 : combine_texture(S_TEX0, tm0, t0, texel, lod);
		}
	}

	// colour combine (clamped_argb, combine_color)
	bool clampit = bit(cp, 28);
	int itera = it[3] << 8;
	ivec4 color = ivec4(clamp_argb(it[0] << 8, clampit), clamp_argb(it[1] << 8, clampit), clamp_argb(it[2] << 8, clampit), clamp_argb(itera, clampit));
	ivec4 c0 = unpack(sw(S_COLOR0)), c1 = unpack(sw(S_COLOR1));
	ivec4 co = ivec4(0);
	uint sel = bf(cp, 0, 2);
	co.rgb = sel == 0u ? color.rgb : sel == 1u ? texel.rgb : sel == 2u ? c1.rgb : ivec3(0);
	if (bit(fbz, 1) && chroma_keyed(co.rgb))
		discard;
	sel = bf(cp, 2, 2);
	co.a = sel == 0u ? color.a : sel == 1u ? texel.a : sel == 2u ? c1.a : 0;
	if (bit(fbz, 13) && (co.a & 1) == 0)
		discard;
	ivec4 cl;
	bool local0 = bit(cp, 7) ? (texel.a & 0x80) != 0 : bit(cp, 4);
	cl.rgb = local0 ? c0.rgb : color.rgb;
	sel = bf(cp, 5, 2);
	cl.a = sel == 0u ? color.a : sel == 1u ? c0.a : sel == 2u ? ((clamped_z(iterz, cp) >> 8) & 255) : (clamped_w(iterw, cp) & 255);
	ivec4 blend = ivec4(bit(cp, 8) ? ivec3(0) : co.rgb, bit(cp, 17) ? 0 : co.a);
	if (bit(cp, 9) || bit(cp, 18))
		blend -= ivec4(bit(cp, 9) ? cl.rgb : ivec3(0), bit(cp, 18) ? cl.a : 0);
	ivec4 f = ivec4(0);
	sel = bf(cp, 10, 3);
	if (sel == 1u) f.rgb = cl.rgb;
	else if (sel == 2u) f.rgb = ivec3(co.a);
	else if (sel == 3u) f.rgb = ivec3(cl.a);
	else if (sel == 4u) f.rgb = ivec3(texel.a);
	else if (sel == 5u) f.rgb = texel.rgb;
	sel = bf(cp, 19, 3);
	if (sel == 1u || sel == 3u) f.a = cl.a;
	else if (sel == 2u) f.a = co.a;
	else if (sel == 4u) f.a = texel.a;
	if (!bit(cp, 13)) f.rgb ^= ivec3(255);
	if (!bit(cp, 22)) f.a ^= 255;
	ivec4 add = ivec4(0);
	sel = bf(cp, 14, 2);
	if (sel == 1u) add.rgb = cl.rgb;
	else if (sel == 2u) add.rgb = ivec3(cl.a);
	if (bf(cp, 23, 2) != 0u) add.a = cl.a;
	f += 1;
	color = clamp(((blend * f) >> 8) + add, 0, 255);
	if (bit(cp, 25)) color.a ^= 255;
	if (bit(cp, 16)) color.rgb ^= ivec3(255);

	// alpha test
	uint am = sw(S_ALPHAMODE);
	if (bit(am, 0))
	{
		int ref = int(sw(S_ALPHAREF));
		uint fn = bf(am, 1, 3);
		bool pass = fn == 1u ? color.a < ref : fn == 2u ? color.a == ref : fn == 3u ? color.a <= ref :
			fn == 4u ? color.a > ref : fn == 5u ? color.a != ref : fn == 6u ? color.a >= ref : fn == 7u;
		if (!pass)
			discard;
	}

	// fog (apply_fogging)
	ivec4 prefog = color;
	uint fm = sw(S_FOGMODE);
	if (bit(fm, 0))
	{
		ivec3 fc = unpack(sw(S_FOGCOLOR)).rgb;
		if (bit(fm, 5))
		{
			if (!bit(fm, 2)) fc = clamp(fc + color.rgb, 0, 255);
		}
		else
		{
			int fogblend = 0;
			if (bit(fm, 1)) fc = ivec3(0);
			if (!bit(fm, 2)) fc -= color.rgb;
			uint zsel = bf(fm, 3, 2);
			if (zsel == 0u)
			{
				int fd = wf;
				if (bit(fbz, 16)) fd = clamp(fd + sext16(sw(S_ZACOLOR)), 0, 0xffff);
				uint e = lut(sw(S_FOG) & 0xffffu, uint(fd >> 10));
				int delta = int((e >> 8) & 255u);
				int dv = (delta & int(sw(S_FOG) >> 16)) * ((fd >> 2) & 255);
				if (bit(fm, 7) && (delta & 2) != 0) dv = -dv;
				dv >>= 6;
				if (bit(fm, 6)) dv += k_dither4[(p.y & 3) * 4 + (p.x & 3)];
				dv >>= 4;
				fogblend = int(e & 255u) + dv;
			}
			else if (zsel == 1u) fogblend = itera;
			else if (zsel == 2u) fogblend = clamped_z(iterz, cp) >> 8;
			else fogblend = clamped_w(iterw, cp);
			fogblend = sext16(uint(fogblend + 1));
			fc = clamp((fc * fogblend) >> 8, 0, 255);
			if (!bit(fm, 2)) fc = clamp(fc + color.rgb, 0, 255);
		}
		color.rgb = fc;
	}

	// out: 5-6-5 as the Voodoo writes it (blending then mixes it with the destination: exact for
	// opaque pixels, which most blended draws are)
	o_color = vec4(to565(color.rgb, fbz, p), float(color.a) / 255.0);
	o_prefog = vec4(vec3(prefog.rgb) / 255.0, 1.0);
	gl_FragDepth = float(depthval) / 65536.0;
}
)";

// a picture over the viewport: v_uv from (0, 0) at the top left to (1, 1)
const char *const k_quad_vs = R"(#version 330 core
out vec2 v_uv;
void main()
{
	v_uv = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1));
	gl_Position = vec4(v_uv.x * 2.0 - 1.0, 1.0 - v_uv.y * 2.0, 0.0, 1.0);
}
)";

// the front buffer through the CLUT (as voodoo_banshee update())
const char *const k_display_fs = R"(#version 330 core
uniform sampler2D u_frame;
uniform usampler2D u_lut;
uniform int u_clut;
uniform vec4 u_src;             // picture rectangle in render target pixels
uniform vec2 u_size;            // render target size
in vec2 v_uv;
out vec4 o_color;
void main()
{
	vec2 p = u_src.xy + v_uv * u_src.zw;
	if (p.x < 0.0 || p.y < 0.0 || p.x >= u_size.x || p.y >= u_size.y) { o_color = vec4(0.0, 0.0, 0.0, 1.0); return; }
	ivec3 c = ivec3(texture(u_frame, p / u_size).rgb * 255.0 + 0.5);
	uint r = texelFetch(u_lut, ivec2(c.r, u_clut), 0).r, g = texelFetch(u_lut, ivec2(c.g, u_clut), 0).r;
	uint b = texelFetch(u_lut, ivec2(c.b, u_clut), 0).r;
	o_color = vec4(float((r >> 16) & 255u), float((g >> 8) & 255u), float(b & 255u), 255.0) / 255.0;
}
)";

// a copy into the VRAM copy (vgpu::CMD_COPY): each fragment is one 32-bit word of the VRAM, two
// 5-6-5 pixels of the destination surface, read from the render target holding the source
const char *const k_copy_vs = R"(#version 330 core
uniform vec2 u_rows;            // VRAM rows (4 KB pages) the destination spans
uniform float u_pages;
void main()
{
	vec2 c = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1));
	gl_Position = vec4(c.x * 2.0 - 1.0, mix(u_rows.x, u_rows.y, c.y) / u_pages * 2.0 - 1.0, 0.0, 1.0);
}
)";

const char *const k_copy_fs = R"(#version 330 core
uniform sampler2D u_src;        // the render target
uniform uint u_w[13];           // vgpu::COPY_* words
uniform int u_scale, u_margin;
layout(location = 0) out uint o_word;
uint pixel(int dx, int dy)
{
	// source address of destination pixel (dx, dy) of the copy, then its place in the target
	int sx = int(u_w[4]) + dx, sy = int(u_w[5]) + int(u_w[6]) * dy;
	uint a = u_w[2] + uint(sy) * u_w[3] + uint(sx) * 2u - u_w[0];
	uint stride = u_w[1] * 2u;
	int col = int((a % stride) / 2u), row = int(a / stride);
	vec3 c = texelFetch(u_src, ivec2((col + u_margin) * u_scale + u_scale / 2, row * u_scale + u_scale / 2), 0).rgb;
	uvec3 v = uvec3(c * 255.0 + 0.5);
	return ((v.r >> 3) << 11) | ((v.g >> 2) << 5) | (v.b >> 3);
}
void main()
{
	uint addr = (uint(gl_FragCoord.y) * 1024u + uint(gl_FragCoord.x)) * 4u;
	if (addr < u_w[7]) discard;
	uint local = addr - u_w[7];
	int py = int(local / u_w[8]) - int(u_w[10]), px = int((local % u_w[8]) / 2u) - int(u_w[9]);
	if (py < 0 || py >= int(u_w[12]) || px < 0 || px + 1 >= int(u_w[11]))
		discard;                // whole words only: the copies start and end on even pixels
	o_word = pixel(px, py) | (pixel(px + 1, py) << 16);
}
)";

const char *const k_overlay_fs = R"(#version 330 core
uniform sampler2D u_overlay;    // premultiplied alpha
in vec2 v_uv;
out vec4 o_color;
void main() { o_color = texture(u_overlay, v_uv); }
)";

GLuint compile(GLenum type, const char *src)
{
	GLuint s = G.CreateShader(type);
	G.ShaderSource(s, 1, &src, nullptr);
	G.CompileShader(s);
	GLint ok = 0;
	G.GetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok)
	{
		char log[4096];
		G.GetShaderInfoLog(s, sizeof log, nullptr, log);
		rt_log("gpu: shader compile failed:\n%s\n", log);
		G.DeleteShader(s);
		return 0;
	}
	return s;
}

GLuint link(const char *vs, const char *fs)
{
	GLuint v = compile(GL_VERTEX_SHADER, vs), f = compile(GL_FRAGMENT_SHADER, fs);
	if (!v || !f)
		return 0;
	GLuint p = G.CreateProgram();
	G.AttachShader(p, v);
	G.AttachShader(p, f);
	G.LinkProgram(p);
	G.DeleteShader(v);
	G.DeleteShader(f);
	GLint ok = 0;
	G.GetProgramiv(p, GL_LINK_STATUS, &ok);
	if (!ok)
	{
		char log[4096];
		G.GetProgramInfoLog(p, sizeof log, nullptr, log);
		rt_log("gpu: shader link failed:\n%s\n", log);
		return 0;
	}
	return p;
}

// ------------------------------------------------------------------ state
struct target
{
	GLuint fbo = 0, color = 0;
	uint32_t aux = ~0u;            // depth texture attached (aux buffer offset)
	int w = 0, h = 0;
};

struct executor
{
	SDL_GLContext ctx = nullptr;
	GLuint draw_prog = 0, display_prog = 0, overlay_prog = 0, copy_prog = 0, vram_fbo = 0;
	GLint c_rows = -1, c_pages = -1, c_w = -1, c_scale = -1, c_margin = -1;
	GLint u_size = -1, u_scale = -1, u_margin = -1, u_dispw = -1;
	GLint d_clut = -1, d_src = -1, d_size = -1;
	GLuint vao = 0, vbo = 0;
	GLuint vram = 0, lut = 0, tri = 0, state = 0, overlay = 0, blur = 0;
	int blur_w = 0, blur_h = 0;
	int tri_rows = 0, state_rows = 0, overlay_w = 0, overlay_h = 0;
	uint32_t vram_pages = 0;
	std::unordered_map<uint32_t, target> targets;       // by colour buffer offset
	std::unordered_map<uint32_t, GLuint> depths;        // by aux buffer offset
	int scale = 1, margin = 0, dispw = 512;
	target *cur = nullptr;
	uint32_t key = ~0u;
	// the displayed picture
	bool have_display = false;
	uint32_t disp_color = 0, disp_clut = 0;
	int disp_x0 = 0, disp_y0 = 0, disp_w = 0, disp_h = 0;
	std::vector<vgpu::frame_list> lists;
} X;

GLuint new_texture(GLenum internal, int w, int h, GLenum format, GLenum type, GLint filter)
{
	GLuint t;
	G.GenTextures(1, &t);
	G.BindTexture(GL_TEXTURE_2D, t);
	G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
	G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
	G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	G.TexImage2D(GL_TEXTURE_2D, 0, internal, w, h, 0, format, type, nullptr);
	return t;
}

void drop_targets()
{
	for (auto &t : X.targets)
	{
		G.DeleteFramebuffers(1, &t.second.fbo);
		G.DeleteTextures(1, &t.second.color);
	}
	for (auto &d : X.depths)
		G.DeleteTextures(1, &d.second);
	X.targets.clear();
	X.depths.clear();
	X.cur = nullptr;
}

// bind the render target of a colour buffer (and its aux buffer as depth)
void bind_target(uint32_t color, uint32_t aux, uint32_t rowpixels)
{
	int const w = (int(rowpixels) + 2 * X.margin) * X.scale, h = int(vgpu::NATIVE_ROWS) * X.scale;
	target &t = X.targets[color];
	bool const fresh = t.fbo == 0 || t.w != w || t.h != h;
	if (fresh)
	{
		if (t.fbo)
		{
			G.DeleteFramebuffers(1, &t.fbo);
			G.DeleteTextures(1, &t.color);
		}
		G.ActiveTexture(GL_TEXTURE4);
		t.color = new_texture(GL_RGBA8, w, h, GL_RGBA, GL_UNSIGNED_BYTE, GL_LINEAR);
		G.GenFramebuffers(1, &t.fbo);
		G.BindFramebuffer(GL_FRAMEBUFFER, t.fbo);
		G.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t.color, 0);
		t.w = w;
		t.h = h;
		t.aux = ~0u;
	}
	G.BindFramebuffer(GL_FRAMEBUFFER, t.fbo);
	if (aux != t.aux)
	{
		GLuint depth = 0;
		if (aux != ~0u)
		{
			GLuint &d = X.depths[aux];
			if (d == 0)
			{
				G.ActiveTexture(GL_TEXTURE4);
				d = new_texture(GL_DEPTH_COMPONENT32F, w, h, GL_DEPTH_COMPONENT, GL_FLOAT, GL_NEAREST);
				G.FramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, d, 0);
				G.DepthMask(GL_TRUE);
				G.ClearDepth(1.0);
				G.Clear(GL_DEPTH_BUFFER_BIT);
				X.key = ~0u;
			}
			depth = d;
		}
		G.FramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, depth, 0);
		t.aux = aux;
	}
	if (fresh)
	{
		if (G.CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
			rt_log("gpu: render target %06x incomplete\n", color);
		G.ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
		G.ClearColor(0, 0, 0, 0);
		G.Clear(GL_COLOR_BUFFER_BIT);
		X.key = ~0u;
	}
	G.Viewport(0, 0, w, h);
	G.Uniform2f(X.u_size, float(w), float(h));
	X.cur = &t;
}

void apply_key(uint32_t key)
{
	if (key == X.key)
		return;
	X.key = key;
	static const GLenum k_src[16] = { GL_ZERO, GL_SRC_ALPHA, GL_DST_COLOR, GL_DST_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA,
		GL_ONE_MINUS_DST_COLOR, GL_ONE_MINUS_DST_ALPHA, GL_ZERO, GL_ZERO, GL_ZERO, GL_ZERO, GL_ZERO, GL_ZERO, GL_ZERO,
		GL_SRC_ALPHA_SATURATE };
	static const GLenum k_dst[16] = { GL_ZERO, GL_SRC_ALPHA, GL_SRC_COLOR, GL_DST_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA,
		GL_ONE_MINUS_SRC_COLOR, GL_ONE_MINUS_DST_ALPHA, GL_ZERO, GL_ZERO, GL_ZERO, GL_ZERO, GL_ZERO, GL_ZERO, GL_ZERO,
		GL_SRC1_COLOR };
	static const GLenum k_depth[8] = { GL_NEVER, GL_LESS, GL_EQUAL, GL_LEQUAL, GL_GREATER, GL_NOTEQUAL, GL_GEQUAL, GL_ALWAYS };
	if (key & vgpu::GK_BLEND)
	{
		G.Enable(GL_BLEND);
		G.BlendFuncSeparate(k_src[(key >> vgpu::GK_SRC_SHIFT) & 15], k_dst[(key >> vgpu::GK_DST_SHIFT) & 15],
			(key & vgpu::GK_SRCA_ONE) ? GL_ONE : GL_ZERO, (key & vgpu::GK_DSTA_ONE) ? GL_ONE : GL_ZERO);
	}
	else
		G.Disable(GL_BLEND);
	if (key & vgpu::GK_DEPTH_TEST)
	{
		G.Enable(GL_DEPTH_TEST);
		G.DepthFunc(k_depth[(key >> vgpu::GK_DEPTHFUNC_SHIFT) & 7]);
	}
	else
		G.Disable(GL_DEPTH_TEST);
	G.DepthMask((key & vgpu::GK_DEPTH_WRITE) ? GL_TRUE : GL_FALSE);
	GLboolean const rgb = (key & vgpu::GK_RGB_WRITE) ? GL_TRUE : GL_FALSE;
	G.ColorMask(rgb, rgb, rgb, (key & vgpu::GK_ALPHA_WRITE) ? GL_TRUE : GL_FALSE);
}

// upload a list's records (uvec4 rows of 1024) into a data texture, growing it
void upload_records(GLuint &tex, int &rows, std::vector<uint32_t> &words, GLenum unit)
{
	if (words.empty())
		return;
	int const need = int((words.size() / 4 + 1023) / 1024);
	words.resize(size_t(need) * 1024 * 4, 0);
	G.ActiveTexture(unit);
	if (need > rows)
	{
		int n = 16;
		while (n < need)
			n *= 2;
		if (tex)
			G.DeleteTextures(1, &tex);
		tex = new_texture(GL_RGBA32UI, 1024, n, GL_RGBA_INTEGER, GL_UNSIGNED_INT, GL_NEAREST);
		rows = n;
	}
	G.BindTexture(GL_TEXTURE_2D, tex);
	G.TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1024, need, GL_RGBA_INTEGER, GL_UNSIGNED_INT, words.data());
}

// vgpu::CMD_COPY: draw the words of the destination rows into the VRAM copy
void copy(uint32_t const *w)
{
	auto it = X.targets.find(w[vgpu::COPY_TBASE]);
	if (it == X.targets.end() || w[vgpu::COPY_W] < 2 || w[vgpu::COPY_H] == 0)
		return;
	uint32_t const first = w[vgpu::COPY_DSTBASE] + w[vgpu::COPY_Y0] * w[vgpu::COPY_DSTSTRIDE] + w[vgpu::COPY_X0] * 2;
	uint32_t const last = first + (w[vgpu::COPY_H] - 1) * w[vgpu::COPY_DSTSTRIDE] + w[vgpu::COPY_W] * 2 - 1;
	G.BindFramebuffer(GL_FRAMEBUFFER, X.vram_fbo);
	G.Viewport(0, 0, 1024, GLsizei(X.vram_pages));
	G.Disable(GL_BLEND);
	G.Disable(GL_DEPTH_TEST);
	G.ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	X.key = ~0u;
	G.UseProgram(X.copy_prog);
	G.Uniform2f(X.c_rows, float(first >> vgpu::PAGE_SHIFT), float((last >> vgpu::PAGE_SHIFT) + 1));
	G.Uniform1uiv(X.c_w, vgpu::COPY_WORDS, w);
	G.Uniform1i(X.c_scale, X.scale);
	G.Uniform1i(X.c_margin, X.margin);
	G.ActiveTexture(GL_TEXTURE6);
	G.BindTexture(GL_TEXTURE_2D, it->second.color);
	G.DrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	// back to drawing
	G.UseProgram(X.draw_prog);
	if (X.cur != nullptr)
	{
		G.BindFramebuffer(GL_FRAMEBUFFER, X.cur->fbo);
		G.Viewport(0, 0, X.cur->w, X.cur->h);
	}
}

// vgpu::CMD_SNAPSHOT: keep a render target's picture for the widescreen blur quads
void snapshot(uint32_t color)
{
	auto it = X.targets.find(color);
	if (it == X.targets.end())
		return;
	target const &t = it->second;
	G.ActiveTexture(GL_TEXTURE7);
	if (t.w != X.blur_w || t.h != X.blur_h)
	{
		if (X.blur)
			G.DeleteTextures(1, &X.blur);
		X.blur = new_texture(GL_RGBA8, t.w, t.h, GL_RGBA, GL_UNSIGNED_BYTE, GL_NEAREST);
		X.blur_w = t.w;
		X.blur_h = t.h;
	}
	G.BindTexture(GL_TEXTURE_2D, X.blur);
	G.BindFramebuffer(GL_FRAMEBUFFER, t.fbo);
	G.CopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, t.w, t.h);
	if (X.cur != nullptr)
		G.BindFramebuffer(GL_FRAMEBUFFER, X.cur->fbo);
}

void execute(vgpu::frame_list &l)
{
	upload_records(X.tri, X.tri_rows, l.tris, GL_TEXTURE2);
	upload_records(X.state, X.state_rows, l.states, GL_TEXTURE3);
	G.BindBuffer(GL_ARRAY_BUFFER, X.vbo);
	if (!l.verts.empty())
		G.BufferData(GL_ARRAY_BUFFER, GLsizeiptr(l.verts.size() * sizeof(vgpu::vertex)), l.verts.data(), GL_STREAM_DRAW);
	G.UseProgram(X.draw_prog);
	X.cur = nullptr;
	for (auto const &c : l.cmds)
		switch (c.type)
		{
		case vgpu::CMD_SCALE:
			if (int(c.a) != X.scale || int(c.b) != X.margin)
				drop_targets();
			X.scale = int(c.a);
			X.margin = int(c.b);
			X.dispw = int(c.c);
			G.Uniform1i(X.u_scale, X.scale);
			G.Uniform1i(X.u_margin, X.margin);
			G.Uniform1i(X.u_dispw, X.dispw);
			break;
		case vgpu::CMD_TARGET:
			bind_target(c.a, c.b, c.c);
			break;
		case vgpu::CMD_DRAW:
			if (X.cur == nullptr)
				break;
			apply_key(c.c);
			G.DrawArrays(GL_TRIANGLES, GLint(c.a), GLsizei(c.b));
			break;
		case vgpu::CMD_VRAM:
			G.ActiveTexture(GL_TEXTURE0);
			G.BindTexture(GL_TEXTURE_2D, X.vram);
			if (c.a + c.b <= X.vram_pages)
				G.TexSubImage2D(GL_TEXTURE_2D, 0, 0, GLint(c.a), 1024, GLsizei(c.b), GL_RED_INTEGER, GL_UNSIGNED_INT, &l.blob[c.c]);
			break;
		case vgpu::CMD_LUT:
			G.ActiveTexture(GL_TEXTURE1);
			G.BindTexture(GL_TEXTURE_2D, X.lut);
			G.TexSubImage2D(GL_TEXTURE_2D, 0, 0, GLint(c.a), 256, 1, GL_RED_INTEGER, GL_UNSIGNED_INT, &l.blob[c.b]);
			break;
		case vgpu::CMD_COPY:
			copy(&l.blob[c.a]);
			break;
		case vgpu::CMD_SNAPSHOT:
			snapshot(c.a);
			break;
		case vgpu::CMD_DISPLAY:
			X.have_display = true;
			X.disp_color = c.a;
			X.disp_x0 = int(c.b);
			X.disp_y0 = int(c.c);
			X.disp_w = int(c.d & 0xffff);
			X.disp_h = int(c.d >> 16);
			X.disp_clut = c.e;
			break;
		}
}

void draw_quad(GLuint tex, int unit)
{
	G.ActiveTexture(GLenum(GL_TEXTURE0 + unit));
	G.BindTexture(GL_TEXTURE_2D, tex);
	G.DrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

} // anonymous namespace


extern "C" {

// make the GL context and the GPU resources; 0 (and nothing left) if this host cannot
int gpu_gl_init(SDL_Window *win, uint32_t vram_size)
{
	X.ctx = SDL_GL_CreateContext(win);
	if (!X.ctx)
	{
		rt_log("gpu: no OpenGL 3.3 context: %s\n", SDL_GetError());
		return 0;
	}
	if (!load_gl())
	{
		SDL_GL_DeleteContext(X.ctx);
		X.ctx = nullptr;
		return 0;
	}
	rt_log("gpu: %s, %s, OpenGL %s\n", (const char *)G.GetString(GL_VENDOR), (const char *)G.GetString(GL_RENDERER),
		(const char *)G.GetString(GL_VERSION));
	X.draw_prog = link(k_draw_vs, k_draw_fs);
	X.display_prog = link(k_quad_vs, k_display_fs);
	X.overlay_prog = link(k_quad_vs, k_overlay_fs);
	X.copy_prog = link(k_copy_vs, k_copy_fs);
	if (!X.draw_prog || !X.display_prog || !X.overlay_prog || !X.copy_prog)
	{
		SDL_GL_DeleteContext(X.ctx);
		X.ctx = nullptr;
		return 0;
	}
	SDL_GL_SetSwapInterval(1);
	G.PixelStorei(GL_UNPACK_ALIGNMENT, 4);

	G.UseProgram(X.draw_prog);
	G.Uniform1i(G.GetUniformLocation(X.draw_prog, "u_vram"), 0);
	G.Uniform1i(G.GetUniformLocation(X.draw_prog, "u_lut"), 1);
	G.Uniform1i(G.GetUniformLocation(X.draw_prog, "u_tri"), 2);
	G.Uniform1i(G.GetUniformLocation(X.draw_prog, "u_state"), 3);
	G.Uniform1i(G.GetUniformLocation(X.draw_prog, "u_blur"), 7);
	G.Uniform1ui(G.GetUniformLocation(X.draw_prog, "u_vram_mask"), vram_size - 1);
	X.u_size = G.GetUniformLocation(X.draw_prog, "u_size");
	X.u_scale = G.GetUniformLocation(X.draw_prog, "u_scale");
	X.u_margin = G.GetUniformLocation(X.draw_prog, "u_margin");
	X.u_dispw = G.GetUniformLocation(X.draw_prog, "u_dispw");
	G.Uniform1i(X.u_scale, 1);
	G.Uniform1i(X.u_margin, 0);
	G.Uniform1i(X.u_dispw, 512);
	G.UseProgram(X.display_prog);
	G.Uniform1i(G.GetUniformLocation(X.display_prog, "u_frame"), 4);
	G.Uniform1i(G.GetUniformLocation(X.display_prog, "u_lut"), 1);
	X.d_clut = G.GetUniformLocation(X.display_prog, "u_clut");
	X.d_src = G.GetUniformLocation(X.display_prog, "u_src");
	X.d_size = G.GetUniformLocation(X.display_prog, "u_size");
	G.UseProgram(X.overlay_prog);
	G.Uniform1i(G.GetUniformLocation(X.overlay_prog, "u_overlay"), 5);
	G.UseProgram(X.copy_prog);
	G.Uniform1i(G.GetUniformLocation(X.copy_prog, "u_src"), 6);
	X.c_rows = G.GetUniformLocation(X.copy_prog, "u_rows");
	X.c_pages = G.GetUniformLocation(X.copy_prog, "u_pages");
	X.c_w = G.GetUniformLocation(X.copy_prog, "u_w");
	X.c_scale = G.GetUniformLocation(X.copy_prog, "u_scale");
	X.c_margin = G.GetUniformLocation(X.copy_prog, "u_margin");

	X.vram_pages = vram_size >> vgpu::PAGE_SHIFT;
	G.ActiveTexture(GL_TEXTURE0);
	X.vram = new_texture(GL_R32UI, 1024, int(X.vram_pages), GL_RED_INTEGER, GL_UNSIGNED_INT, GL_NEAREST);
	G.ActiveTexture(GL_TEXTURE1);
	X.lut = new_texture(GL_R32UI, 256, int(vgpu::LUT_ROWS), GL_RED_INTEGER, GL_UNSIGNED_INT, GL_NEAREST);
	G.ActiveTexture(GL_TEXTURE7);                   // the blur snapshot, empty until the first
	X.blur = new_texture(GL_RGBA8, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, GL_NEAREST);
	X.blur_w = X.blur_h = 1;
	G.GenFramebuffers(1, &X.vram_fbo);              // CMD_COPY draws into the VRAM copy
	G.BindFramebuffer(GL_FRAMEBUFFER, X.vram_fbo);
	G.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, X.vram, 0);
	if (G.CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
		rt_log("gpu: the VRAM copy cannot be drawn into\n");
	G.BindFramebuffer(GL_FRAMEBUFFER, 0);
	G.UseProgram(X.copy_prog);
	G.Uniform1f(X.c_pages, float(X.vram_pages));

	G.GenVertexArrays(1, &X.vao);
	G.BindVertexArray(X.vao);
	G.GenBuffers(1, &X.vbo);
	G.BindBuffer(GL_ARRAY_BUFFER, X.vbo);
	G.EnableVertexAttribArray(0);
	G.VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(vgpu::vertex), (const void *)0);
	G.EnableVertexAttribArray(1);
	G.VertexAttribIPointer(1, 1, GL_UNSIGNED_INT, sizeof(vgpu::vertex), (const void *)offsetof(vgpu::vertex, tri));
	GLenum const err = G.GetError();
	if (err != GL_NO_ERROR)
		rt_log("gpu: GL error %04x at init\n", err);
	return 1;
}

// run what the device recorded, then show the picture (letterboxed in the window, the overlay
// over it) and swap; vsync paces the caller
void gpu_gl_frame(SDL_Window *win, const uint32_t *overlay, int ow, int oh, int nearest)
{
	if (voodoo_gpu_take(X.lists))
	{
		for (auto &l : X.lists)
			execute(l);
		voodoo_gpu_recycle(X.lists);
	}

	G.BindFramebuffer(GL_FRAMEBUFFER, 0);
	X.cur = nullptr;
	X.key = ~0u;
	G.Disable(GL_DEPTH_TEST);
	G.Disable(GL_BLEND);
	G.DepthMask(GL_TRUE);
	G.ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	int dw, dh;
	SDL_GL_GetDrawableSize(win, &dw, &dh);
	G.Viewport(0, 0, dw, dh);
	G.ClearColor(0, 0, 0, 1);
	G.Clear(GL_COLOR_BUFFER_BIT);

	// the picture keeps its aspect ratio, centred (as SDL_RenderSetLogicalSize)
	int const pw = (X.disp_w + 2 * X.margin) * X.scale, ph = X.disp_h * X.scale;
	auto it = X.have_display ? X.targets.find(X.disp_color) : X.targets.end();
	if (pw > 0 && ph > 0)
	{
		float const k = std::min(float(dw) / float(pw), float(dh) / float(ph));
		int const vw = int(float(pw) * k + 0.5f), vh = int(float(ph) * k + 0.5f);
		G.Viewport((dw - vw) / 2, (dh - vh) / 2, vw, vh);
		if (it != X.targets.end())
		{
			target const &t = it->second;
			G.UseProgram(X.display_prog);
			G.Uniform1i(X.d_clut, GLint(X.disp_clut));
			G.Uniform4f(X.d_src, float(X.disp_x0 * X.scale), float(X.disp_y0 * X.scale), float(pw), float(ph));
			G.Uniform2f(X.d_size, float(t.w), float(t.h));
			G.ActiveTexture(GL_TEXTURE4);
			G.BindTexture(GL_TEXTURE_2D, t.color);
			GLint const filter = nearest ? GL_NEAREST : GL_LINEAR;
			G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
			G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
			G.DrawArrays(GL_TRIANGLE_STRIP, 0, 4);
		}
		if (overlay != nullptr)
		{
			G.ActiveTexture(GL_TEXTURE5);
			if (ow != X.overlay_w || oh != X.overlay_h)
			{
				if (X.overlay)
					G.DeleteTextures(1, &X.overlay);
				X.overlay = new_texture(GL_RGBA8, ow, oh, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, GL_LINEAR);
				X.overlay_w = ow;
				X.overlay_h = oh;
			}
			G.BindTexture(GL_TEXTURE_2D, X.overlay);
			G.TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, ow, oh, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, overlay);
			G.Enable(GL_BLEND);
			G.BlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
			G.UseProgram(X.overlay_prog);
			draw_quad(X.overlay, 5);
			G.Disable(GL_BLEND);
		}
	}
	static int errors;
	GLenum const err = G.GetError();
	if (err != GL_NO_ERROR && errors < 10)
	{
		errors++;
		rt_log("gpu: GL error %04x\n", err);
	}
	SDL_GL_SwapWindow(win);
}

// RT_GPU_BENCH=1 with --headless (benchmark): the main thread runs the lists in a hidden window,
// waiting for the GPU after each batch, and never returns (the run ends at --seconds)
void gpu_gl_run_headless(uint32_t vram_size)
{
	SDL_Init(SDL_INIT_VIDEO);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
	SDL_Window *win = SDL_CreateWindow("gpu", 0, 0, 64, 64, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
	if (!win || !gpu_gl_init(win, vram_size))
	{
		rt_log("gpu: not available\n");
		voodoo_set_gpu(0);
		for (;;)
			SDL_Delay(1000);
	}
	SDL_GL_SetSwapInterval(0);
	for (;;)
	{
		if (!voodoo_gpu_take(X.lists))
		{
			SDL_Delay(1);
			continue;
		}
		for (auto &l : X.lists)
			execute(l);
		voodoo_gpu_recycle(X.lists);
		G.Finish();
	}
}

// ------------------------------------------------------------------ debug (tools/gpu_replay.cpp)
void gpu_gl_debug_load(const uint32_t *vram, const uint32_t *luts, int rows)
{
	G.ActiveTexture(GL_TEXTURE0);
	G.BindTexture(GL_TEXTURE_2D, X.vram);
	G.TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1024, GLsizei(X.vram_pages), GL_RED_INTEGER, GL_UNSIGNED_INT, vram);
	G.ActiveTexture(GL_TEXTURE1);
	G.BindTexture(GL_TEXTURE_2D, X.lut);
	G.TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, rows, GL_RED_INTEGER, GL_UNSIGNED_INT, luts);
}

void gpu_gl_debug_execute(void *list)
{
	execute(*static_cast<vgpu::frame_list *>(list));
}

// the displayed picture at the render size, through the CLUT, top row first (0xAARRGGBB)
int gpu_gl_debug_read(uint32_t *out, int max_pixels, int *w, int *h)
{
	int const pw = (X.disp_w + 2 * X.margin) * X.scale, ph = X.disp_h * X.scale;
	auto it = X.have_display ? X.targets.find(X.disp_color) : X.targets.end();
	if (pw <= 0 || ph <= 0 || pw * ph > max_pixels || it == X.targets.end())
		return 0;
	G.ActiveTexture(GL_TEXTURE5);
	GLuint tex = new_texture(GL_RGBA8, pw, ph, GL_RGBA, GL_UNSIGNED_BYTE, GL_NEAREST), fbo;
	G.GenFramebuffers(1, &fbo);
	G.BindFramebuffer(GL_FRAMEBUFFER, fbo);
	G.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
	G.Disable(GL_DEPTH_TEST);
	G.Disable(GL_BLEND);
	G.ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	G.Viewport(0, 0, pw, ph);
	G.UseProgram(X.display_prog);
	G.Uniform1i(X.d_clut, GLint(X.disp_clut));
	G.Uniform4f(X.d_src, float(X.disp_x0 * X.scale), float(X.disp_y0 * X.scale), float(pw), float(ph));
	G.Uniform2f(X.d_size, float(it->second.w), float(it->second.h));
	G.ActiveTexture(GL_TEXTURE4);
	G.BindTexture(GL_TEXTURE_2D, it->second.color);
	G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	G.DrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	std::vector<uint32_t> rgba(size_t(pw) * ph);
	G.ReadPixels(0, 0, pw, ph, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
	for (int y = 0; y < ph; y++)
		for (int x = 0; x < pw; x++)
		{
			uint32_t const p = rgba[size_t(ph - 1 - y) * pw + x];     // bottom row first
			out[size_t(y) * pw + x] = 0xff000000u | ((p & 0xff) << 16) | (p & 0xff00) | ((p >> 16) & 0xff);
		}
	G.BindFramebuffer(GL_FRAMEBUFFER, 0);
	G.DeleteFramebuffers(1, &fbo);
	G.DeleteTextures(1, &tex);
	X.key = ~0u;
	*w = pw;
	*h = ph;
	return 1;
}

void gpu_gl_shutdown(void)
{
	if (!X.ctx)
		return;
	drop_targets();
	SDL_GL_DeleteContext(X.ctx);
	X.ctx = nullptr;
}

} // extern "C"
