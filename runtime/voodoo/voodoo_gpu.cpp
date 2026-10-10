// recomp: GPU renderer, the recording side (guest thread). See voodoo_gpu.h.

#include "emu.h"
#include "voodoo.h"
#include "voodoo_gpu.h"

#include <atomic>
#include <cstdio>
#include <string>
#include <condition_variable>
#include <mutex>
#include <unordered_map>

extern "C" void rt_log(const char *fmt, ...);
extern unsigned long long g_voodoo_swaps;
extern "C" int enh_turbo(void);

using namespace voodoo;

namespace {

// lists recorded and not executed yet, and executed ones whose buffers can be reused; never
// destroyed, so the frontend can still take them while the process exits
std::mutex &s_lock = *new std::mutex;
std::condition_variable &s_cv = *new std::condition_variable;
std::vector<vgpu::frame_list> &s_pending = *new std::vector<vgpu::frame_list>, &s_free = *new std::vector<vgpu::frame_list>;
size_t s_pending_bytes;
std::atomic<int> s_want{0}, s_active{0};
// the guest waits for the frontend beyond 2 lists, so a GPU slower than the game slows the game
// down evenly (and the fps counter shows it) instead of piling up frames to show in bursts; a
// fast boot (many vblanks per window frame) may run ahead up to this much
constexpr size_t MAX_PENDING_LISTS = 2;
constexpr size_t MAX_PENDING_BYTES = size_t(128) << 20;

// as voodoo_render.cpp
s32 fast_log2(double value, int fracbits)
{
	if (value < 0)
		return 0;
	union { double d; u64 i; } temp;
	temp.d = value;
	u32 ival = temp.i >> 45;
	s32 exp = (ival >> 7) - 1023 - fracbits;
	static u8 const s_log2_table[128] =
	{
		  0,   2,   5,   8,  11,  14,  16,  19,  22,  25,  27,  30,  33,  35,  38,  40,
		 43,  46,  48,  51,  53,  56,  58,  61,  63,  65,  68,  70,  73,  75,  77,  80,
		 82,  84,  87,  89,  91,  93,  96,  98, 100, 102, 104, 106, 109, 111, 113, 115,
		117, 119, 121, 123, 125, 127, 129, 132, 134, 136, 138, 140, 141, 143, 145, 147,
		149, 151, 153, 155, 157, 159, 161, 162, 164, 166, 168, 170, 172, 173, 175, 177,
		179, 181, 182, 184, 186, 188, 189, 191, 193, 194, 196, 198, 200, 201, 203, 205,
		206, 208, 209, 211, 213, 214, 216, 218, 219, 221, 222, 224, 225, 227, 229, 230,
		232, 233, 235, 236, 238, 239, 241, 242, 244, 245, 247, 248, 250, 251, 253, 254
	};
	return (exp << 8) | s_log2_table[ival & 127];
}

s32 compute_lodbase(s64 dsdx, s64 dsdy, s64 dtdx, s64 dtdy)
{
	double fdsdx = double(dsdx), fdsdy = double(dsdy), fdtdx = double(dtdx), fdtdy = double(dtdy);
	double texdx = fdsdx * fdsdx + fdtdx * fdtdx;
	double texdy = fdsdy * fdsdy + fdtdy * fdtdy;
	return fast_log2(std::max(texdx, texdy), 64) / 2;
}

// poly.h round_coordinate
s32 round_coordinate(float value)
{
	float const ipart = std::floor(value);
	return s32(ipart) + ((value - ipart > 0.5f) ? 1 : 0);
}

// the pixel count the software rasterizer returns for a triangle (poly.h render_triangle, which
// the Voodoo runs without clipping): the emulated timing depends on it
s32 count_pixels(voodoo_renderer::vertex_t const &a, voodoo_renderer::vertex_t const &b, voodoo_renderer::vertex_t const &c)
{
	voodoo_renderer::vertex_t const *v1 = &a, *v2 = &b, *v3 = &c;
	if (v2->y < v1->y)
		std::swap(v1, v2);
	if (v3->y < v2->y)
	{
		std::swap(v2, v3);
		if (v2->y < v1->y)
			std::swap(v1, v2);
	}
	s32 const v1y = round_coordinate(v1->y), v3y = round_coordinate(v3->y);
	float const dxdy_v1v2 = (v2->y == v1->y) ? 0.0f : (v2->x - v1->x) / (v2->y - v1->y);
	float const dxdy_v1v3 = (v3->y == v1->y) ? 0.0f : (v3->x - v1->x) / (v3->y - v1->y);
	float const dxdy_v2v3 = (v3->y == v2->y) ? 0.0f : (v3->x - v2->x) / (v3->y - v2->y);
	s32 pixels = 0;
	for (s32 y = v1y; y < v3y; y++)
	{
		float const fully = float(y) + 0.5f;
		float const startx = v1->x + (fully - v1->y) * dxdy_v1v3;
		float const stopx = (fully < v2->y) ? v1->x + (fully - v1->y) * dxdy_v1v2 : v2->x + (fully - v2->y) * dxdy_v2v3;
		s32 istartx = round_coordinate(startx), istopx = round_coordinate(stopx);
		if (istartx > istopx)
			std::swap(istartx, istopx);
		pixels += istopx - istartx;
	}
	return pixels;
}

// RT_GPU_CAPTURE=file:first:count (debug): the device records next to the software rasterizer,
// and the lists of frames first to first + count - 1 are written to the file, after the VRAM and
// the lookup table rows as they were before the first one; tools/gpu_replay.cpp draws them
struct capture
{
	FILE *f = nullptr;
	u32 first = 0, count = 0, frame = 0;
	bool on = false;
} s_cap;

void capture_init()
{
	static bool done;
	if (done)
		return;
	done = true;
	char const *e = getenv("RT_GPU_CAPTURE");
	if (!e)
		return;
	std::string spec(e);
	size_t const c2 = spec.rfind(':'), c1 = c2 == std::string::npos ? c2 : spec.rfind(':', c2 - 1);
	if (c1 == std::string::npos || c1 == 0)
	{
		rt_log("RT_GPU_CAPTURE=file:first:count\n");
		return;
	}
	s_cap.first = u32(atoi(spec.c_str() + c1 + 1));
	s_cap.count = u32(atoi(spec.c_str() + c2 + 1));
	s_cap.f = fopen(spec.substr(0, c1).c_str(), "wb");
	s_cap.on = s_cap.f != nullptr && s_cap.count > 0 && s_cap.first > 0;
	if (s_cap.on)
	{
		s_want = 1;
		rt_log("voodoo: capturing GPU frames %u-%u to %s\n", s_cap.first, s_cap.first + s_cap.count - 1, spec.substr(0, c1).c_str());
	}
}

void write_words(void const *p, size_t n) { fwrite(p, 4, n, s_cap.f); }

u64 hash_words(u32 const *w, int n)
{
	u64 h = 1469598103934665603ull;
	for (int i = 0; i < n; i++)
		h = (h ^ w[i]) * 1099511628211ull;
	return h;
}

} // anonymous namespace


struct voodoo_1_device::gpu_state
{
	vgpu::frame_list list;                  // being recorded
	std::vector<u64> dirty;                 // VRAM pages written since uploaded (1 bit each)
	u32 npages = 0;
	u32 dirty_gen = 1;                      // bumped at every write
	// what this list has set so far
	u32 color = ~0u, aux = ~0u, rowpixels = 0;
	s32 scale = -1, margin = -1, dispw = -1;
	u32 state[vgpu::STATE_WORDS];
	bool have_state = false;
	// texture setup of each TMU, as last exported
	struct tmu_cache
	{
		rasterizer_texture const *tex = nullptr;
		u32 gen = 0, format = ~0u, checked_gen = 0;
		u32 lo = 0, hi = 0;                 // VRAM byte range it reads
		u32 words[vgpu::TX_WORDS];
	} tmu[2];
	// 256-entry lookup tables on the GPU (texel lookups, fog table, CLUT), shared by all lists
	struct lut_row { u64 hash = 0; u32 data[256]; bool used = false; };
	std::vector<lut_row> luts = std::vector<lut_row>(vgpu::LUT_ROWS);
	std::unordered_map<u64, u32> lut_by_hash;
	u32 lut_next = 0;
	bool warned_depth_source = false, warned_lfb = false;
	// colour buffers the GPU drew into: what it drew (native) since the VRAM copy last got it
	struct target_info { u32 rowpixels = 0; s32 x0 = 0, y0 = 0, x1 = 0, y1 = 0; bool dirty = false, unsnapped = false; };
	std::unordered_map<u32, target_info> targets;
	u32 targets_dirty = 0;
	size_t last_copy = ~size_t(0);          // cmds index of the last CMD_COPY (to merge row blits)
	// widescreen motion blur, as blur_quad: the textures the blits filled from a render target
	struct blur_tex { std::vector<s16> srcrow; s32 coloffs = 0; unsigned long long filled = 0, done = ~0ull; };
	std::unordered_map<u32, blur_tex> blur;
};


//-------------------------------------------------
//  switching
//-------------------------------------------------

extern "C" void voodoo_set_gpu(int on)
{
	s_want = on;
	if (!on)
	{
		std::lock_guard<std::mutex> lock(s_lock);
		s_pending.clear();
		s_pending_bytes = 0;
		s_cv.notify_all();
	}
}

extern "C" int voodoo_gpu_active(void)
{
	return s_active.load();
}

bool voodoo_gpu_take(std::vector<vgpu::frame_list> &out)
{
	std::lock_guard<std::mutex> lock(s_lock);
	if (s_pending.empty())
		return false;
	for (auto &l : s_pending)
		out.push_back(std::move(l));
	s_pending.clear();
	s_pending_bytes = 0;
	s_cv.notify_all();
	return true;
}

void voodoo_gpu_recycle(std::vector<vgpu::frame_list> &lists)
{
	std::lock_guard<std::mutex> lock(s_lock);
	for (auto &l : lists)
		if (s_free.size() < 4)
		{
			l.clear();
			s_free.push_back(std::move(l));
		}
	lists.clear();
}

bool voodoo_1_device::gpu_wanted()
{
	capture_init();
	return s_want.load() != 0;
}

bool voodoo_1_device::gpu_shadow()
{
	return s_cap.on;
}

void voodoo_1_device::gpu_enable(bool on)
{
	if (on == (m_gpu != nullptr))
		return;
	m_renderer->wait("gpu_enable");
	if (on)
	{
		m_gpu = new gpu_state;
		m_gpu->npages = (m_fbmask + 1) >> vgpu::PAGE_SHIFT;
		m_gpu->dirty.assign((m_gpu->npages + 63) / 64, ~u64(0));   // all of the VRAM once
		rt_log("voodoo: GPU renderer on\n");
	}
	else
	{
		delete m_gpu;
		m_gpu = nullptr;
		rt_log("voodoo: GPU renderer off, back to the software rasterizer\n");
	}
	s_active = on;
}


//-------------------------------------------------
//  VRAM pages written by the CPU
//-------------------------------------------------

void voodoo_1_device::gpu_mark_pages(u32 addr, u32 bytes)
{
	auto &g = *m_gpu;
	u32 const first = (addr & m_fbmask) >> vgpu::PAGE_SHIFT, last = ((addr + bytes - 1) & m_fbmask) >> vgpu::PAGE_SHIFT;
	for (u32 p = first; ; p = (p + 1) % g.npages)
	{
		u64 &word = g.dirty[p >> 6];
		u64 const bit = u64(1) << (p & 63);
		if (!(word & bit))
		{
			word |= bit;
			g.dirty_gen++;              // only a newly dirty page needs the textures checked again
		}
		if (p == last)
			break;
	}
}

void voodoo_1_device::gpu_mark_all()
{
	if (m_gpu == nullptr)
		return;
	std::fill(m_gpu->dirty.begin(), m_gpu->dirty.end(), ~u64(0));
	m_gpu->dirty_gen++;
}

// upload the written pages of [lo, hi) (bytes, may wrap around the VRAM)
void voodoo_1_device::gpu_upload(u32 lo, u32 hi)
{
	auto &g = *m_gpu;
	if (hi <= lo)
		return;
	u32 const first = lo >> vgpu::PAGE_SHIFT, count = std::min(((hi - 1) >> vgpu::PAGE_SHIFT) - first + 1, g.npages);
	u32 run = 0, run_first = 0;
	auto flush = [&]()
	{
		if (run == 0)
			return;
		u32 const offset = u32(g.list.blob.size());
		g.list.blob.resize(offset + run * vgpu::PAGE_WORDS);
		memcpy(&g.list.blob[offset], m_fbram + (size_t(run_first) << vgpu::PAGE_SHIFT), size_t(run) << vgpu::PAGE_SHIFT);
		g.list.cmds.push_back({ vgpu::CMD_VRAM, run_first, run, offset, 0, 0 });
		run = 0;
	};
	for (u32 i = 0; i < count; i++)
	{
		u32 const p = (first + i) % g.npages;
		u64 &word = g.dirty[p >> 6];
		u64 const bit = u64(1) << (p & 63);
		if (word & bit)
		{
			word &= ~bit;
			if (run != 0 && p != run_first + run)
				flush();
			if (run == 0)
				run_first = p;
			run++;
		}
		else
			flush();
	}
	flush();
}


//-------------------------------------------------
//  lookup tables
//-------------------------------------------------

// the GPU row holding these 256 words, uploading them if no row has them
u32 voodoo_1_device::gpu_lut(u32 const *data)
{
	auto &g = *m_gpu;
	u64 const hash = hash_words(data, 256);
	auto it = g.lut_by_hash.find(hash);
	if (it != g.lut_by_hash.end() && !memcmp(g.luts[it->second].data, data, 1024))
		return it->second;
	u32 const row = g.lut_next;
	g.lut_next = (g.lut_next + 1) % vgpu::LUT_ROWS;
	auto &r = g.luts[row];
	if (r.used)
	{
		auto old = g.lut_by_hash.find(r.hash);
		if (old != g.lut_by_hash.end() && old->second == row)
			g.lut_by_hash.erase(old);
	}
	r.hash = hash;
	r.used = true;
	memcpy(r.data, data, 1024);
	g.lut_by_hash[hash] = row;
	u32 const offset = u32(g.list.blob.size());
	g.list.blob.insert(g.list.blob.end(), data, data + 256);
	g.list.cmds.push_back({ vgpu::CMD_LUT, row, offset, 0, 0, 0 });
	return row;
}


//-------------------------------------------------
//  per-list state: scale, render target
//-------------------------------------------------

void voodoo_1_device::gpu_scale()
{
	auto &g = *m_gpu;
	if (g.scale != m_hires_scale || g.margin != m_wide || g.dispw != m_display_w)
	{
		g.scale = m_hires_scale;
		g.margin = m_wide;
		g.dispw = m_display_w;
		g.list.cmds.push_back({ vgpu::CMD_SCALE, u32(g.scale), u32(g.margin), u32(g.dispw), 0, 0 });
	}
}

void voodoo_1_device::gpu_target(u16 const *dest, u16 const *depth)
{
	auto &g = *m_gpu;
	gpu_scale();
	u32 const color = u32((u8 const *)dest - m_fbram);
	u32 const aux = depth ? u32((u8 const *)depth - m_fbram) : ~0u;
	u32 const rowpixels = m_renderer->rowpixels();
	g.targets[color].rowpixels = rowpixels;
	if (color != g.color || aux != g.aux || rowpixels != g.rowpixels)
	{
		g.color = color;
		g.aux = aux;
		g.rowpixels = rowpixels;
		g.list.cmds.push_back({ vgpu::CMD_TARGET, color, aux, rowpixels, 0, 0 });
	}
}

u32 voodoo_1_device::gpu_state_index(u32 const *words)
{
	auto &g = *m_gpu;
	if (!g.have_state || memcmp(g.state, words, sizeof(g.state)))
	{
		memcpy(g.state, words, sizeof(g.state));
		g.have_state = true;
		g.list.states.insert(g.list.states.end(), words, words + vgpu::STATE_WORDS);
	}
	return u32(g.list.states.size() / vgpu::STATE_WORDS - 1);
}

void voodoo_1_device::gpu_draw(u32 nverts, u32 key, u32 state)
{
	auto &cmds = m_gpu->list.cmds;
	u32 const first = u32(m_gpu->list.verts.size()) - nverts;
	if (!cmds.empty() && cmds.back().type == vgpu::CMD_DRAW && cmds.back().c == key && cmds.back().d == state &&
		cmds.back().a + cmds.back().b == first)
		cmds.back().b += nverts;
	else
		cmds.push_back({ vgpu::CMD_DRAW, first, nverts, key, state, 0 });
}


//-------------------------------------------------
//  render targets read back as textures, 2D blits
//-------------------------------------------------

// the current target was drawn in [x0, x1) x [y0, y1) (native, rows of the buffer)
void voodoo_1_device::gpu_drawn(s32 x0, s32 y0, s32 x1, s32 y1)
{
	auto &g = *m_gpu;
	auto &t = g.targets[g.color];
	x0 = std::max(x0, 0);
	y0 = std::max(y0, 0);
	x1 = std::min(x1, s32(t.rowpixels));
	y1 = std::min(y1, s32(vgpu::NATIVE_ROWS));
	if (x1 <= x0 || y1 <= y0)
		return;
	t.unsnapped = true;
	if (!t.dirty)
	{
		t.dirty = true;
		t.x0 = x0; t.y0 = y0; t.x1 = x1; t.y1 = y1;
		g.targets_dirty++;
	}
	else
	{
		t.x0 = std::min(t.x0, x0); t.y0 = std::min(t.y0, y0);
		t.x1 = std::max(t.x1, x1); t.y1 = std::max(t.y1, y1);
	}
}

// the written pages of [lo, hi) are the GPU's now: never uploaded over it
void voodoo_1_device::gpu_clean(u32 lo, u32 hi)
{
	auto &g = *m_gpu;
	if (hi <= lo)
		return;
	for (u32 p = lo >> vgpu::PAGE_SHIFT; p <= ((hi - 1) >> vgpu::PAGE_SHIFT) && p < g.npages; p++)
		g.dirty[p >> 6] &= ~(u64(1) << (p & 63));
}

void voodoo_1_device::gpu_copy(u32 const *w)
{
	auto &g = *m_gpu;
	auto &cmds = g.list.cmds;
	// a blit one row below the last one, its source one row above or below: the same copy
	if (!cmds.empty() && g.last_copy == cmds.size() - 1 && w[vgpu::COPY_H] == 1)
	{
		u32 *p = &g.list.blob[cmds.back().a];
		bool same = true;
		for (u32 k : { vgpu::COPY_TBASE, vgpu::COPY_SRCBASE, vgpu::COPY_SRCSTRIDE, vgpu::COPY_SX, vgpu::COPY_DSTBASE,
				vgpu::COPY_DSTSTRIDE, vgpu::COPY_X0, vgpu::COPY_W })
			same = same && p[k] == w[k];
		s32 const step = s32(w[vgpu::COPY_SY]) - s32(p[vgpu::COPY_SY]);
		s32 const dir = p[vgpu::COPY_H] == 1 ? step : s32(p[vgpu::COPY_SDIR]);
		if (same && w[vgpu::COPY_Y0] == p[vgpu::COPY_Y0] + p[vgpu::COPY_H] && (dir == 1 || dir == -1) &&
			step == dir * s32(p[vgpu::COPY_H]))
		{
			p[vgpu::COPY_SDIR] = u32(dir);
			p[vgpu::COPY_H]++;
			return;
		}
	}
	u32 const offset = u32(g.list.blob.size());
	g.list.blob.insert(g.list.blob.end(), w, w + vgpu::COPY_WORDS);
	g.last_copy = cmds.size();
	cmds.push_back({ vgpu::CMD_COPY, offset, 0, 0, 0, 0 });
}

// before a texture reads [lo, hi): the targets drawn there go into the VRAM copy
void voodoo_1_device::gpu_resolve(u32 lo, u32 hi)
{
	auto &g = *m_gpu;
	if (g.targets_dirty == 0)
		return;
	for (auto &it : g.targets)
	{
		auto &t = it.second;
		u32 const stride = t.rowpixels * 2;
		if (!t.dirty || it.first == g.color)
			continue;
		u32 const a = it.first + u32(t.y0) * stride, b = it.first + u32(t.y1) * stride;
		if (b <= lo || a >= hi)
			continue;
		s32 const x0 = t.x0 & ~1, x1 = std::min((t.x1 + 1) & ~1, s32(t.rowpixels));   // whole words
		u32 const w[vgpu::COPY_WORDS] = { it.first, t.rowpixels, it.first, stride, u32(x0), u32(t.y0), 1,
			it.first, stride, u32(x0), u32(t.y0), u32(x1 - x0), u32(t.y1 - t.y0) };
		gpu_copy(w);
		gpu_clean(a, b);
		t.dirty = false;
		g.targets_dirty--;
	}
}

bool voodoo_1_device::gpu_blit(u32 srcbase, u32 srcstride, s32 sx, s32 sy, u32 dstbase, u32 dststride, s32 x0, s32 y0, s32 w, s32 h)
{
	auto &g = *m_gpu;
	if (w <= 0 || h <= 0)
		return true;
	u32 const src = srcbase + u32(sy) * srcstride + u32(sx) * 2;
	for (auto const &it : g.targets)
	{
		u32 const size = it.second.rowpixels * 2 * vgpu::NATIVE_ROWS;
		if (src < it.first || src >= it.first + size || it.second.rowpixels == 0)
			continue;
		// widescreen: the finished picture, margins included, for the blur quads; and which source
		// row each texture row holds
		if (m_wide)
		{
			auto &t = g.targets[it.first];
			if (t.unsnapped)
			{
				t.unsnapped = false;
				g.list.cmds.push_back({ vgpu::CMD_SNAPSHOT, it.first, 0, 0, 0, 0 });
			}
			auto &bt = g.blur[dstbase];
			bt.coloffs = sx - x0;
			for (s32 r = 0; r < h; r++)
				if (y0 + r >= 0 && y0 + r < 4096)
				{
					if (bt.srcrow.size() <= size_t(y0 + r))
						bt.srcrow.resize(y0 + r + 1, -1);
					bt.srcrow[y0 + r] = s16(sy + r);
				}
			bt.filled = g_voodoo_swaps;
		}
		u32 const words[vgpu::COPY_WORDS] = { it.first, it.second.rowpixels, srcbase, srcstride, u32(sx), u32(sy), 1,
			dstbase, dststride, u32(x0), u32(y0), u32(w), u32(h) };
		gpu_copy(words);
		gpu_clean(dstbase + u32(y0) * dststride, dstbase + u32(y0 + h) * dststride);
		return true;
	}
	return false;
}


static u32 gl_key(u32 fbzmode, u32 alphamode, bool fill);

// widescreen: a quad of Thrill Drive 2's motion blur (one texel per pixel of a texture the blits
// filled, drawn back over the same place) is drawn from the snapshot over the whole picture, its
// 4:3 edges out to the margins (as blur_quad); -1 if this is not one
s32 voodoo_1_device::gpu_blur_quad(poly_data const &poly, voodoo_renderer::vertex_t const *vert)
{
	auto &g = *m_gpu;
	auto it = g.blur.find(m_tmu[0].regs().texture_baseaddr());
	if (it == g.blur.end() || it->second.filled + 1 < g_voodoo_swaps)
		return -1;
	auto &bt = it->second;
	auto const alphamode = m_reg.alpha_mode();
	auto const fbzmode = m_reg.fbz_mode();
	if (!alphamode.alphablend() || alphamode.srcrgbblend() != 1 || alphamode.dstrgbblend() != 5 || fbzmode.enable_depthbuf())
		return -1;
	s32 const qx0 = s32(std::lround(std::min({vert[0].x, vert[1].x, vert[2].x}))), qx1 = s32(std::lround(std::max({vert[0].x, vert[1].x, vert[2].x})));
	s32 const qy0 = s32(std::lround(std::min({vert[0].y, vert[1].y, vert[2].y}))), qy1 = s32(std::lround(std::max({vert[0].y, vert[1].y, vert[2].y})));
	s32 const yorigin = m_renderer->yorigin();
	auto bufrow = [&](s32 y) { return fbzmode.y_origin() ? yorigin - y : y; };
	if (bt.coloffs != qx0 || qy1 - qy0 > s32(bt.srcrow.size()) || qx1 <= qx0 || qy1 <= qy0)
		return -1;
	s64 const ax = m_reg.ax() >> 4, ay = m_reg.ay() >> 4;
	if (poly.ds0dx != poly.dt0dy || poly.dt0dx != 0 || poly.ds0dy != 0 || poly.dw0dx != 0 || poly.dw0dy != 0 ||
		2 * poly.starts0 != (2 * (ax - qx0) + 1) * poly.ds0dx || 2 * poly.startt0 != (2 * (ay - qy0) + 1) * poly.dt0dy)
		return -1;
	for (s32 v = 0; v < qy1 - qy0; v++)
		if (bt.srcrow[v] != bufrow(qy0 + v))
			return -1;
	if (bt.done == g_voodoo_swaps)
		return 256 * 128;                       // the other triangle of the quad: already drawn
	bt.done = g_voodoo_swaps;

	gpu_target(poly.destbase, poly.depthbase);
	u32 st[vgpu::STATE_WORDS];
	gpu_common_state(poly, st);
	auto &tris = g.list.tris;
	u32 const index = u32(tris.size() / vgpu::TRI_WORDS);
	tris.resize(tris.size() + vgpu::TRI_WORDS, 0);
	u32 *t = &tris[size_t(index) * vgpu::TRI_WORDS];
	t[vgpu::T_FLAGS] = vgpu::TF_BLUR;
	t[vgpu::T_START + 0] = m_reg.start_r();
	t[vgpu::T_START + 1] = m_reg.start_g();
	t[vgpu::T_START + 2] = m_reg.start_b();
	t[vgpu::T_START + 3] = m_reg.start_a();
	t[vgpu::T_STATE] = gpu_state_index(st);

	// the area in scaled pixels; rows clipped in Y as blur_quad does, then following the Y origin
	s32 const n = m_hires_scale, m = m_wide, wide = (m_display_w + 2 * m) * n;
	s32 x0 = qx0 <= 0 ? 0 : (qx0 + m) * n, x1 = qx1 >= m_display_w ? wide : (qx1 + m) * n;
	s32 y0 = qy0, y1 = qy1;
	if (fbzmode.enable_clipping())
	{
		s32 const cl = m_reg.clip_left(), cr = m_reg.clip_right();
		x0 = std::max(x0, cl <= 0 ? 0 : (cl + m) * n);
		x1 = std::min(x1, cr >= m_display_w ? wide : (cr + m) * n);
		y0 = std::max(y0, s32(m_reg.clip_top()));
		y1 = std::min(y1, s32(m_reg.clip_bottom()));
	}
	if (x1 <= x0 || y1 <= y0)
		return 256 * 128;
	if (fbzmode.y_origin())
		std::tie(y0, y1) = std::make_pair(yorigin + 1 - y1, yorigin + 1 - y0);
	float const fx0 = float(x0), fx1 = float(x1), fy0 = float(y0 * n), fy1 = float(y1 * n);
	vgpu::vertex const quad[6] = { { fx0, fy0, index }, { fx1, fy0, index }, { fx0, fy1, index },
	                               { fx1, fy0, index }, { fx1, fy1, index }, { fx0, fy1, index } };
	g.list.verts.insert(g.list.verts.end(), quad, quad + 6);
	gpu_draw(6, gl_key(poly.raster.fbzmode().raw(), poly.raster.alphamode().raw(), false), t[vgpu::T_STATE]);
	return 256 * 128;
}


//-------------------------------------------------
//  textures
//-------------------------------------------------

// the TX_* words of a TMU's texture, after uploading the pages it reads that the CPU wrote
void voodoo_1_device::gpu_texture(int which, rasterizer_texture const &tex, u32 *words)
{
	auto &g = *m_gpu;
	auto &c = g.tmu[which];
	u32 const format = m_tmu[which].regs().texture_mode().format();
	if (&tex != c.tex || tex.gen() != c.gen || format != c.format)
	{
		c.tex = &tex;
		c.gen = tex.gen();
		c.format = format;
		c.checked_gen = 0;
		tex.gpu_export(c.words);
		bool const direct = (format >= 10 && format <= 12) || tex.lookup() == nullptr;   // decoded by the shader
		c.words[vgpu::TX_LUT] = direct ? 0 : gpu_lut(reinterpret_cast<u32 const *>(tex.lookup()));

		// the VRAM bytes the LODs it can use span
		u32 const masks = c.words[vgpu::TX_MASKS];
		u32 const wmask = masks & 0xff, hmask = (masks >> 8) & 0xff, lodmask = (masks >> 16) & 0x1ff;
		s32 const lodmin = s16(c.words[vgpu::TX_LOD] & 0xffff) >> 8, lodmax = s16(c.words[vgpu::TX_LOD] >> 16) >> 8;
		u32 const bppscale = format >> 3;
		c.lo = ~0u;
		c.hi = 0;
		for (s32 lod = std::max(lodmin, 0); lod <= std::min(lodmax, 8); lod++)
		{
			s32 const ilod = std::min(lod + s32((~lodmask >> lod) & 1), 8);
			u32 const size = (((wmask >> ilod) + 1) * ((hmask >> ilod) + 1)) << bppscale;
			u32 const base = c.words[vgpu::TX_OFFSET + ilod];
			c.lo = std::min(c.lo, base);
			c.hi = std::max(c.hi, base + size);
		}
	}
	if (c.checked_gen != g.dirty_gen)
	{
		if (c.hi > m_fbmask + 1)
		{
			gpu_upload(c.lo, m_fbmask + 1);
			gpu_upload(0, c.hi - (m_fbmask + 1));
		}
		else
			gpu_upload(c.lo, c.hi);
		c.checked_gen = g.dirty_gen;
	}
	gpu_resolve(c.lo, std::min(c.hi, m_fbmask + 1));
	memcpy(words, c.words, sizeof(c.words));
}


//-------------------------------------------------
//  triangles and fast fills
//-------------------------------------------------

// GL state of a draw (vgpu::GK_*)
static u32 gl_key(u32 fbzmode, u32 alphamode, bool fill)
{
	reg_fbz_mode const fbz(fbzmode);
	reg_alpha_mode const am(alphamode);
	bool const planes = fbz.enable_alpha_planes();
	u32 key = 0;
	if (fbz.rgb_buffer_mask())
		key |= vgpu::GK_RGB_WRITE;
	if (fbz.aux_buffer_mask())
		key |= planes ? vgpu::GK_ALPHA_WRITE : vgpu::GK_DEPTH_WRITE;
	if (!fill && fbz.enable_depthbuf() && !planes)
		key |= vgpu::GK_DEPTH_TEST | (fbz.depth_function() << vgpu::GK_DEPTHFUNC_SHIFT);
	else if (key & vgpu::GK_DEPTH_WRITE)
		key |= vgpu::GK_DEPTH_TEST | (7 << vgpu::GK_DEPTHFUNC_SHIFT);   // always: write only
	if (!fill && am.alphablend())
	{
		u32 src = am.srcrgbblend(), dst = am.dstrgbblend();
		if (!planes)
		{
			// no alpha planes: the destination alpha reads as 0xff
			auto fix = [](u32 f) { return f == 3 ? 4u : f == 7 ? 0u : f; };
			src = fix(src);
			dst = fix(dst);
			if (src == 15)
				src = 0;                    // saturate: min(As, 1 - Ad) = 0
		}
		key |= vgpu::GK_BLEND | (src << vgpu::GK_SRC_SHIFT) | (dst << vgpu::GK_DST_SHIFT);
		if (am.srcalphablend() == 4)
			key |= vgpu::GK_SRCA_ONE;
		if (am.dstalphablend() == 4)
			key |= vgpu::GK_DSTA_ONE;
	}
	return key;
}

// the state words every draw shares
void voodoo_1_device::gpu_common_state(poly_data const &poly, u32 *st)
{
	memset(st, 0, vgpu::STATE_WORDS * 4);
	st[vgpu::S_FBZCP] = poly.raster.fbzcp().raw();
	st[vgpu::S_FBZMODE] = poly.raster.fbzmode().raw();
	st[vgpu::S_ALPHAMODE] = poly.raster.alphamode().raw();
	st[vgpu::S_FOGMODE] = poly.raster.fogmode().raw();
	st[vgpu::S_TEXMODE0] = st[vgpu::S_TEXMODE1] = reg_texture_mode::NONE;
	st[vgpu::S_COLOR0] = poly.color0;
	st[vgpu::S_COLOR1] = poly.color1;
	st[vgpu::S_CHROMAKEY] = poly.chromakey;
	st[vgpu::S_FOGCOLOR] = poly.fogcolor;
	st[vgpu::S_ZACOLOR] = poly.zacolor;
	st[vgpu::S_STIPPLE] = poly.stipple;
	st[vgpu::S_ALPHAREF] = poly.alpharef;
	st[vgpu::S_CHROMARANGE] = m_reg.read(voodoo_regs::reg_chromaRange);
	st[vgpu::S_CLIP_X] = poly.clipleft | (u32(poly.clipright) << 16);
	st[vgpu::S_CLIP_Y] = poly.cliptop | (u32(poly.clipbottom) << 16);
	st[vgpu::S_YORIGIN] = u32(m_renderer->yorigin());
	st[vgpu::S_TMUCONFIG] = m_renderer->tmu_config();
	st[vgpu::S_BILINEAR] = m_renderer->bilinear_mask();
	st[vgpu::S_GENERIC] = poly.raster.generic();
}

s32 voodoo_1_device::gpu_triangle(poly_data &poly, voodoo_renderer::vertex_t const *vert)
{
	auto &g = *m_gpu;
	if (m_wide && poly.tex0 != nullptr && !g.blur.empty())
		if (s32 const pixels = gpu_blur_quad(poly, vert); pixels >= 0)
			return pixels;
	gpu_target(poly.destbase, poly.depthbase);

	u32 st[vgpu::STATE_WORDS];
	gpu_common_state(poly, st);
	reg_fbz_mode const fbzmode = poly.raster.fbzmode();
	if (fbzmode.depth_source_compare() && fbzmode.enable_depthbuf() && !g.warned_depth_source)
	{
		g.warned_depth_source = true;
		rt_log("voodoo: GPU renderer: depth source compare not supported\n");
	}

	// textures: the real format (the normalized mode keeps only its class)
	u32 tex[2][vgpu::TX_WORDS] = {};
	rasterizer_texture const *texture[2] = { poly.tex0, poly.tex1 };
	u32 const texmode[2] = { poly.raster.texmode0().raw(), poly.raster.texmode1().raw() };
	for (int i = 0; i < 2; i++)
		if (texture[i] != nullptr && texmode[i] != reg_texture_mode::NONE)
		{
			st[i ? vgpu::S_TEXMODE1 : vgpu::S_TEXMODE0] = (texmode[i] & ~(0xf << 8)) | (m_tmu[i].regs().texture_mode().format() << 8);
			gpu_texture(i, *texture[i], tex[i]);
			memcpy(&st[i ? vgpu::S_TEX1 : vgpu::S_TEX0], tex[i], sizeof(tex[i]));
		}

	// fog table
	reg_fog_mode const fogmode = poly.raster.fogmode();
	if (fogmode.enable_fog() && !fogmode.fog_constant() && fogmode.fog_zalpha() == 0)
	{
		u32 table[256] = {};
		for (int i = 0; i < 64; i++)
			table[i] = m_renderer->fogblend()[i] | (u32(m_renderer->fogdelta()[i]) << 8);
		st[vgpu::S_FOG] = gpu_lut(table) | (u32(m_renderer->fogdelta_mask()) << 16);
	}

	// the triangle: vertex A, the iterators' start values and gradients
	auto &tris = g.list.tris;
	u32 const index = u32(tris.size() / vgpu::TRI_WORDS);
	tris.resize(tris.size() + vgpu::TRI_WORDS);
	u32 *t = &tris[size_t(index) * vgpu::TRI_WORDS];
	memset(t, 0, vgpu::TRI_WORDS * 4);
	t[vgpu::T_AX] = poly.ax;
	t[vgpu::T_AY] = poly.ay;
	s32 const start[5] = { poly.startr, poly.startg, poly.startb, poly.starta, poly.startz };
	s32 const dx[5] = { poly.drdx, poly.dgdx, poly.dbdx, poly.dadx, poly.dzdx };
	s32 const dy[5] = { poly.drdy, poly.dgdy, poly.dbdy, poly.dady, poly.dzdy };
	for (int i = 0; i < 5; i++)
	{
		t[vgpu::T_START + i] = start[i];
		t[vgpu::T_DX + i] = dx[i];
		t[vgpu::T_DY + i] = dy[i];
	}
	auto put64 = [t](u32 at, s64 v) { t[at] = u32(v); t[at + 1] = u32(u64(v) >> 32); };
	put64(vgpu::T_STARTW, poly.startw);
	put64(vgpu::T_DWDX, poly.dwdx);
	put64(vgpu::T_DWDY, poly.dwdy);
	auto putf = [t](u32 at, s64 v) { float const f = float(double(v)); memcpy(&t[at], &f, 4); };
	if (texture[0] != nullptr && texmode[0] != reg_texture_mode::NONE)
	{
		s64 const v[9] = { poly.starts0, poly.startt0, poly.startw0, poly.ds0dx, poly.dt0dx, poly.dw0dx, poly.ds0dy, poly.dt0dy, poly.dw0dy };
		for (int i = 0; i < 9; i++)
			putf(vgpu::T_TEX0 + i, v[i]);
		t[vgpu::T_LODBASE0] = compute_lodbase(poly.ds0dx, poly.ds0dy, poly.dt0dx, poly.dt0dy);
	}
	if (texture[1] != nullptr && texmode[1] != reg_texture_mode::NONE)
	{
		s64 const v[9] = { poly.starts1, poly.startt1, poly.startw1, poly.ds1dx, poly.dt1dx, poly.dw1dx, poly.ds1dy, poly.dt1dy, poly.dw1dy };
		for (int i = 0; i < 9; i++)
			putf(vgpu::T_TEX1 + i, v[i]);
		t[vgpu::T_LODBASE1] = compute_lodbase(poly.ds1dx, poly.ds1dy, poly.dt1dx, poly.dt1dy);
	}

	// widescreen: an untextured triangle spanning exactly the 4:3 width (the fades to a colour)
	// is stretched about the centre to the whole picture (as hires_scale_poly)
	float xs[3] = { vert[0].x, vert[1].x, vert[2].x };
	s32 const n = m_hires_scale, m = m_wide;
	if (m && st[vgpu::S_TEXMODE0] == reg_texture_mode::NONE && st[vgpu::S_TEXMODE1] == reg_texture_mode::NONE)
	{
		float const x0 = std::min({xs[0], xs[1], xs[2]}), x1 = std::max({xs[0], xs[1], xs[2]});
		if (std::abs(x0) <= 0.5f && std::abs(x1 - float(m_display_w)) <= 0.5f)
		{
			float const c = float(m_display_w) / 2, k = float(m_display_w + 2 * m) / float(m_display_w);
			for (float &x : xs)
				x = (x - c) * k + c;
			t[vgpu::T_FLAGS] |= vgpu::TF_STRETCH;
		}
	}
	t[vgpu::T_STATE] = gpu_state_index(st);

	// vertices in render target pixels: native x moves right by the margin, rows follow the
	// Y origin (row = yorigin - y), everything N times larger
	s32 const yorigin = m_renderer->yorigin();
	float rmin = 1e9f, rmax = -1e9f;
	for (int i = 0; i < 3; i++)
	{
		float const row = fbzmode.y_origin() ? float(yorigin + 1) - vert[i].y : vert[i].y;
		g.list.verts.push_back({ (xs[i] + float(m)) * float(n), row * float(n), index });
		rmin = std::min(rmin, row);
		rmax = std::max(rmax, row);
	}
	{
		s32 x0 = s32(std::floor(std::min({xs[0], xs[1], xs[2]}))), x1 = s32(std::ceil(std::max({xs[0], xs[1], xs[2]}))) + 1;
		s32 y0 = s32(std::floor(rmin)), y1 = s32(std::ceil(rmax)) + 1;
		if (fbzmode.enable_clipping())
		{
			x0 = std::max(x0, s32(poly.clipleft)); x1 = std::min(x1, s32(poly.clipright));
			y0 = std::max(y0, s32(poly.cliptop)); y1 = std::min(y1, s32(poly.clipbottom));
		}
		gpu_drawn(x0, y0, x1, y1);
	}
	gpu_draw(3, gl_key(fbzmode.raw(), poly.raster.alphamode().raw(), false), t[vgpu::T_STATE]);
	return count_pixels(vert[0], vert[1], vert[2]);
}

u32 voodoo_1_device::gpu_fastfill(poly_data &poly)
{
	auto &g = *m_gpu;
	reg_fbz_mode const fbzmode = poly.raster.fbzmode();
	if (!fbzmode.rgb_buffer_mask() && !fbzmode.aux_buffer_mask())
		return 0;
	gpu_target(poly.destbase, poly.depthbase);
	u32 st[vgpu::STATE_WORDS];
	gpu_common_state(poly, st);

	auto &tris = g.list.tris;
	u32 const index = u32(tris.size() / vgpu::TRI_WORDS);
	tris.resize(tris.size() + vgpu::TRI_WORDS, 0);
	u32 *t = &tris[size_t(index) * vgpu::TRI_WORDS];
	t[vgpu::T_FLAGS] = vgpu::TF_FILL;
	t[vgpu::T_STATE] = gpu_state_index(st);

	// the clip rectangle, its rows following the Y origin; at a margin, a rectangle spanning the
	// whole 4:3 picture reaches the margins too (as hires_scale_poly)
	s32 const n = m_hires_scale, m = m_wide;
	s32 x0 = poly.clipleft + m, x1 = poly.clipright + m;
	if (m && poly.clipleft == 0 && poly.clipright >= m_display_w)
	{
		x0 = 0;
		x1 = m_display_w + 2 * m;
	}
	s32 y0 = poly.cliptop, y1 = poly.clipbottom;
	if (fbzmode.y_origin())
	{
		s32 const yorigin = m_renderer->yorigin();
		std::tie(y0, y1) = std::make_pair(yorigin + 1 - poly.clipbottom, yorigin + 1 - poly.cliptop);
	}
	float const fx0 = float(x0 * n), fx1 = float(x1 * n), fy0 = float(y0 * n), fy1 = float(y1 * n);
	vgpu::vertex const quad[6] = { { fx0, fy0, index }, { fx1, fy0, index }, { fx0, fy1, index },
	                               { fx1, fy0, index }, { fx1, fy1, index }, { fx0, fy1, index } };
	g.list.verts.insert(g.list.verts.end(), quad, quad + 6);
	gpu_draw(6, gl_key(fbzmode.raw(), 0, true), t[vgpu::T_STATE]);
	if (fbzmode.rgb_buffer_mask())
		gpu_drawn(poly.clipleft, y0, poly.clipright, y1);
	return u32(std::max(poly.clipright - poly.clipleft, 0) * std::max(poly.clipbottom - poly.cliptop, 0));
}


//-------------------------------------------------
//  gpu_frame - at every vblank: the displayed buffer, then the list goes to the frontend
//-------------------------------------------------

void voodoo_1_device::gpu_frame(rectangle const &vis, u32 const *clut)
{
	auto &g = *m_gpu;
	m_display_w = vis.width();
	gpu_scale();
	u32 const front = u32((u8 const *)draw_buffer(m_frontbuf) - m_fbram);
	g.list.cmds.push_back({ vgpu::CMD_DISPLAY, front, u32(vis.min_x - m_xoffs), u32(vis.min_y - m_yoffs),
		u32(vis.width()) | (u32(vis.height()) << 16), gpu_lut(clut) });

	if (s_cap.on)
	{
		// capturing: the frames asked are written, nothing goes to a frontend
		s_cap.frame++;
		if (s_cap.frame >= s_cap.first && s_cap.frame < s_cap.first + s_cap.count)
		{
			auto &l = g.list;
			u32 const counts[5] = { u32(l.cmds.size()), u32(l.tris.size()), u32(l.states.size()), u32(l.verts.size()), u32(l.blob.size()) };
			write_words(counts, 5);
			write_words(l.cmds.data(), l.cmds.size() * sizeof(vgpu::cmd) / 4);
			write_words(l.tris.data(), l.tris.size());
			write_words(l.states.data(), l.states.size());
			write_words(l.verts.data(), l.verts.size() * sizeof(vgpu::vertex) / 4);
			write_words(l.blob.data(), l.blob.size());
			if (s_cap.frame == s_cap.first + s_cap.count - 1)
			{
				fclose(s_cap.f);
				s_cap.f = nullptr;
				rt_log("voodoo: GPU capture written\n");
			}
		}
		g.list.clear();
	}
	else
	// hand the list over (waiting while the frontend is far behind) and start the next
	{
		std::unique_lock<std::mutex> lock(s_lock);
		bool const turbo = enh_turbo() != 0;
		s_cv.wait(lock, [turbo] {
			return (s_pending_bytes < MAX_PENDING_BYTES && (turbo || s_pending.size() < MAX_PENDING_LISTS)) || !s_want.load();
		});
		if (s_want.load())
		{
			s_pending_bytes += g.list.bytes();
			s_pending.push_back(std::move(g.list));
		}
		g.list = vgpu::frame_list();
		if (!s_free.empty())
		{
			g.list = std::move(s_free.back());
			s_free.pop_back();
		}
	}
	g.color = g.aux = ~0u;
	g.rowpixels = 0;
	g.last_copy = ~size_t(0);
	g.scale = g.margin = g.dispw = -1;
	g.have_state = false;

	// capturing: the next list is the first asked, so the state it starts from goes first
	if (s_cap.on && s_cap.f && s_cap.frame + 1 == s_cap.first)
	{
		u32 const header[3] = { 0x55504756, m_fbmask + 1, vgpu::LUT_ROWS };    // "VGPU"
		write_words(header, 3);
		write_words(m_fbram, (m_fbmask + 1) / 4);
		for (auto const &r : g.luts)
			write_words(r.data, 256);
	}

	// nothing was queued to the software rasterizer: its pools are only reset here
	if (!s_cap.on)
		m_renderer->reset_arrays();
}
