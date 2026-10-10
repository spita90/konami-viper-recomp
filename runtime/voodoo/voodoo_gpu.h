// recomp: hardware-accelerated Voodoo rendering (enhanced mode, DISPLAY > RENDERER).
//
// The device does not rasterize: at every triangle and fast fill it records what to draw into a
// command list (guest thread). At every vblank the list goes to the frontend, which executes it
// with OpenGL 3.3 (host main thread, runtime/gpu_gl.cpp) and shows the front buffer.
//
// Each triangle carries the same values the software rasterizer starts from (vertex A, the start
// values and gradients of the iterators), and the fragment shader runs the Voodoo's pixel pipeline
// on them with the same integer arithmetic. Textures are read from a copy of the VRAM on the GPU:
// the pages the CPU writes are uploaded before the first triangle that reads them. Each colour
// buffer the game draws into is a GL render target, N times larger at render scale N.
#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

namespace vgpu {

constexpr uint32_t PAGE_SHIFT = 12;                 // VRAM upload granularity (4 KB)
constexpr uint32_t PAGE_WORDS = 1 << (PAGE_SHIFT - 2);
constexpr uint32_t TRI_WORDS = 48;                  // per triangle record (12 uvec4)
constexpr uint32_t STATE_WORDS = 64;                // per state record (16 uvec4)
constexpr uint32_t LUT_ROWS = 2048;                 // 256-entry lookup tables kept on the GPU
constexpr uint32_t NATIVE_ROWS = 512;               // rows of a render target (the display uses 384)

// triangle record (u32 words)
enum : uint32_t
{
	T_AX = 0, T_AY,
	T_START = 2,                    // R, G, B, A, Z (s32)
	T_DX = 7,                       // their gradients per X
	T_DY = 12,                      // and per Y
	T_STARTW = 17, T_DWDX = 19, T_DWDY = 21,    // s64 as lo, hi
	T_STATE = 23,                   // index of the state record
	T_FLAGS = 24,                   // TF_*
	T_LODBASE0 = 25, T_LODBASE1 = 26,
	T_TEX0 = 27, T_TEX1 = 36        // S, T, W start, d/dx, d/dy (floats)
};
enum : uint32_t
{
	TF_STRETCH = 1,                 // widescreen: a fade drawn over the 4:3 width, stretched to the margins
	TF_FILL = 2,                    // fast fill (the clip rectangle, colour1 and zaColor)
	TF_BLUR = 4                     // widescreen motion blur: the last snapshot, same place (blur_quad)
};

// state record (u32 words)
enum : uint32_t
{
	S_FBZCP = 0, S_FBZMODE, S_ALPHAMODE, S_FOGMODE, S_TEXMODE0, S_TEXMODE1,
	S_COLOR0, S_COLOR1, S_CHROMAKEY, S_FOGCOLOR, S_ZACOLOR, S_STIPPLE, S_ALPHAREF, S_CHROMARANGE,
	S_CLIP_X, S_CLIP_Y,             // left | right << 16, top | bottom << 16 (native)
	S_YORIGIN,
	S_FOG,                          // fog table LUT row | delta mask << 16
	S_TMUCONFIG, S_BILINEAR,
	S_TEX0 = 20, S_TEX1 = 35,       // TX_* words for each TMU
	S_GENERIC = 50                  // rasterizer_params::generic(): a TMU whose combine is identity
};
enum : uint32_t
{
	TX_LUT = 0,                     // LUT row of the 8-bit lookup (formats 0-9, 13, 14)
	TX_MASKS,                       // wmask | hmask << 8 | lodmask << 16 | detailscale << 28
	TX_LOD,                         // lodmin | lodmax << 16 (s16 each)
	TX_LODBIAS,                     // s32
	TX_DETAILMAX, TX_DETAILBIAS,
	TX_OFFSET = 6,                  // 9 LOD offsets in the VRAM
	TX_WORDS = 15
};

// GL state of a draw, from fbzMode and alphaMode (bits)
enum : uint32_t
{
	GK_SRC_SHIFT = 0,               // Voodoo source RGB factor (4 bits)
	GK_DST_SHIFT = 4,               // Voodoo destination RGB factor (4 bits)
	GK_SRCA_ONE = 1 << 8, GK_DSTA_ONE = 1 << 9, GK_BLEND = 1 << 10,
	GK_DEPTHFUNC_SHIFT = 11,        // 3 bits
	GK_DEPTH_TEST = 1 << 14, GK_DEPTH_WRITE = 1 << 15, GK_RGB_WRITE = 1 << 16, GK_ALPHA_WRITE = 1 << 17
};

enum cmd_type : uint32_t
{
	CMD_SCALE,                      // a = N, b = margin M, c = display width (native)
	CMD_TARGET,                     // a = colour buffer offset, b = aux offset (~0: none), c = row pixels
	CMD_DRAW,                       // a = first vertex, b = vertices, c = GL key, d = state record
	CMD_VRAM,                       // a = first page, b = pages, c = blob offset
	CMD_LUT,                        // a = row, b = blob offset (256 words)
	CMD_DISPLAY,                    // a = buffer offset, b = x0, c = y0 (native, may be < 0), d = w | h << 16, e = CLUT row
	CMD_COPY,                       // a = blob offset of the COPY_* words
	CMD_SNAPSHOT                    // a = colour buffer offset: its render target, kept for TF_BLUR
};

// a copy from a render target into the VRAM copy (a 2D blit, or a target the textures read)
enum : uint32_t
{
	COPY_TBASE = 0, COPY_TROW,      // the render target: colour buffer offset, row pixels
	COPY_SRCBASE, COPY_SRCSTRIDE,   // source surface (bytes)
	COPY_SX, COPY_SY, COPY_SDIR,    // its corner; the source row step per destination row (+1, -1)
	COPY_DSTBASE, COPY_DSTSTRIDE,   // destination surface (bytes)
	COPY_X0, COPY_Y0, COPY_W, COPY_H,
	COPY_WORDS
};

struct cmd
{
	uint32_t type, a, b, c, d, e;
};

struct vertex
{
	float x, y;                     // render target pixels
	uint32_t tri;                   // triangle index in the list
};

// everything the device recorded between two vblanks
struct frame_list
{
	std::vector<cmd> cmds;
	std::vector<uint32_t> tris;     // TRI_WORDS each
	std::vector<uint32_t> states;   // STATE_WORDS each
	std::vector<vertex> verts;      // 3 (or 6 for a fill) per draw
	std::vector<uint32_t> blob;     // uploaded data
	size_t bytes() const { return tris.size() * 4 + states.size() * 4 + verts.size() * sizeof(vertex) + blob.size() * 4 + cmds.size() * sizeof(cmd); }
	void clear() { cmds.clear(); tris.clear(); states.clear(); verts.clear(); blob.clear(); }
	bool empty() const { return cmds.empty(); }
};

} // namespace vgpu

// executor side (frontend): take the lists recorded since the last call, oldest first
extern "C" {
void voodoo_set_gpu(int on);        // before voodoo_init: record for the GPU; later 0: back to software
int voodoo_gpu_active(void);
}
bool voodoo_gpu_take(std::vector<vgpu::frame_list> &out);       // false: none pending
void voodoo_gpu_recycle(std::vector<vgpu::frame_list> &lists);  // give the buffers back
