// Bridge between the recomp runtime (C) and MAME's voodoo_3_device (C++).
//
// Bus conventions match MAME's viper.cpp: BAR0 (0x82000000) -> read/write,
// BAR1 (0x84000000) -> read_lfb/write_lfb, I/O (0xfe800000) -> read_io/write_io,
// offsets in 32-bit words, data as little-endian register values.
#include "emu.h"
#include "voodoo_banshee.h"

#include <chrono>
#include <mutex>

extern "C" {
void epic_raise(int irq);
void rt_log(const char *fmt, ...);
void rt_pace_vblank(void);
void rt_frame_published(uint64_t count, const uint32_t *pix, int w, int h);
}

namespace emu_shim { bool g_log_enabled = false; }
voodoo_fbstats g_fbstats;

// ------------------------------------------------------------------ shim singletons
static running_machine s_machine;
static screen_device s_screen;
static cpu_device s_cpu;
running_machine &emu_machine() { return s_machine; }
screen_device &emu_screen() { return s_screen; }
cpu_device &emu_cpu() { return s_cpu; }
std::string running_machine::describe_context() const { return "voodoo"; }

// multithreaded work queue for poly.h (scanline bands of each primitive run in parallel);
// the submitting thread helps while waiting.  RT_RENDER_THREADS=N overrides the worker count.
#include <condition_variable>
#include <deque>
#include <thread>

struct osd_work_queue
{
	struct item { osd_work_callback cb; void *param; };
	std::mutex lock;
	std::condition_variable have_work, done;
	std::deque<item> items;
	int pending = 0;
	bool stop = false;
	std::vector<std::thread> workers;

	void run_one(std::unique_lock<std::mutex> &lk, int threadid)
	{
		item it = items.front();
		items.pop_front();
		lk.unlock();
		it.cb(it.param, threadid);
		lk.lock();
		if (--pending == 0) done.notify_all();
	}
};

osd_work_queue *osd_work_queue_alloc(int flags)
{
	auto *q = new osd_work_queue;
	// one worker per hardware thread, leaving one for the guest (which helps while waiting), and
	// at most 4: more mostly add lock contention (measured on Apple M-series and a 4-core i5)
	int n = std::clamp(int(std::thread::hardware_concurrency()) - 1, 1, 4);
	if (getenv("RT_RENDER_THREADS")) n = atoi(getenv("RT_RENDER_THREADS"));
	n = std::clamp(n, 0, WORK_MAX_THREADS - 1);
	for (int t = 0; t < n; t++)
		q->workers.emplace_back([q, t] {
			std::unique_lock<std::mutex> lk(q->lock);
			for (;;) {
				q->have_work.wait(lk, [q] { return q->stop || !q->items.empty(); });
				if (q->stop) return;
				q->run_one(lk, t + 1);
			}
		});
	rt_log("voodoo: %d render worker threads\n", n);
	return q;
}
int osd_work_queue_items(osd_work_queue *q) { std::lock_guard<std::mutex> lk(q->lock); return q->pending; }
bool osd_work_queue_wait(osd_work_queue *q, osd_ticks_t)
{
	std::unique_lock<std::mutex> lk(q->lock);
	while (q->pending > 0) {
		if (!q->items.empty()) q->run_one(lk, 0);
		else q->done.wait(lk);
	}
	return true;
}
void osd_work_queue_free(osd_work_queue *q)
{
	{ std::lock_guard<std::mutex> lk(q->lock); q->stop = true; }
	q->have_work.notify_all();
	for (auto &t : q->workers) t.join();
	delete q;
}
osd_work_item *osd_work_item_queue_multiple(osd_work_queue *q, osd_work_callback cb, s32 n, void *base, s32 step, u32)
{
	if (q->workers.empty()) {
		for (s32 i = 0; i < n; i++) cb((u8 *)base + size_t(i) * step, 0);
		return nullptr;
	}
	{
		std::lock_guard<std::mutex> lk(q->lock);
		for (s32 i = 0; i < n; i++) q->items.push_back({cb, (u8 *)base + size_t(i) * step});
		q->pending += n;
	}
	q->have_work.notify_all();
	return nullptr;
}
osd_ticks_t osd_ticks()
{
	return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
osd_ticks_t osd_ticks_per_second() { return 1000000000; }

// ------------------------------------------------------------------ device
namespace {

class viper_voodoo : public voodoo_3_device
{
public:
	explicit viper_voodoo(u32 clock) : voodoo_3_device(machine_config(), "voodoo", nullptr, clock) {}
};

std::unique_ptr<viper_voodoo> s_dev;
bitmap_rgb32 s_bitmap;
std::mutex s_frame_lock;
std::vector<u32> s_frame;
int s_frame_w, s_frame_h;
u64 s_frame_count;

void publish_frame()
{
	const rectangle &vis = s_screen.visible_area();
	int w = vis.width(), h = vis.height();
	static int logged;
	if (!logged++) rt_log("voodoo: first vblank, visible area %d,%d-%d,%d\n", vis.min_x, vis.min_y, vis.max_x, vis.max_y);
	if (w <= 0 || h <= 0 || w > 2048 || h > 2048)
		return;
	if (s_bitmap.width() < vis.max_x + 1 || s_bitmap.height() < vis.max_y + 1)
		s_bitmap.allocate(vis.max_x + 1, vis.max_y + 1);
	s_dev->update(s_bitmap, vis);
	std::lock_guard<std::mutex> lock(s_frame_lock);
	u32 const *hires;
	int hw, hh;
	if (s_dev->hires_frame(hires, hw, hh))
	{
		// the front buffer was rendered at a higher resolution: publish that picture
		s_frame.assign(hires, hires + size_t(hw) * hh);
		w = hw;
		h = hh;
	}
	else
	{
		s_frame.resize(size_t(w) * h);
		for (int y = 0; y < h; y++)
			memcpy(&s_frame[size_t(y) * w], &s_bitmap.pix(vis.min_y + y, vis.min_x), size_t(w) * 4);
	}
	s_frame_w = w;
	s_frame_h = h;
	s_frame_count++;
	{	// debug: RT_VOODOO_VRAMDUMP=path:frame writes the whole VRAM at that frame
		static const char *dump = getenv("RT_VOODOO_VRAMDUMP");
		if (dump)
		{
			const char *colon = strrchr(dump, ':');
			if (colon && s_frame_count == strtoull(colon + 1, nullptr, 10))
			{
				std::string path(dump, colon - dump);
				if (FILE *f = fopen(path.c_str(), "wb"))
				{
					fwrite(s_dev->debug_fbram(), 1, s_dev->debug_fbsize(), f);
					fclose(f);
					rt_log("voodoo: VRAM dumped to %s\n", path.c_str());
				}
			}
		}
	}
	rt_frame_published(s_frame_count, s_frame.data(), w, h);
}

} // anonymous namespace

extern "C" {

/* VRAM for the enhanced mode's checks (read only, no sync) and patches (the renderer idle) */
const uint8_t *voodoo_vram(uint32_t *size)
{
	if (!s_dev) return nullptr;
	*size = s_dev->debug_fbsize();
	return s_dev->debug_fbram();
}

uint8_t *voodoo_vram_for_write(void)
{
	return s_dev ? s_dev->fbram_for_write() : nullptr;
}

static int s_scale = 1;          /* kept for voodoo_init when set before the device exists */
static int s_wide = 0;
void voodoo_init(void)
{
	emu_shim::g_log_enabled = getenv("RT_VOODOO_LOG") != nullptr;
	s_screen.configure(1024, 768, rectangle(0, 1023, 0, 767), attotime::from_hz(60));
	u32 clk = voodoo_3_device::NOMINAL_CLOCK;
	if (getenv("RT_VOODOO_CLOCK_X")) clk = u32(clk * atof(getenv("RT_VOODOO_CLOCK_X")));
	s_dev = std::make_unique<viper_voodoo>(clk);
	s_machine.m_root = s_dev.get();
	s_dev->set_fbmem(8);              // as MAME viper.cpp (TODO there: should be 16)
	s_dev->set_render_scale(s_scale);
	s_dev->set_wide_margin(s_wide);
	s_dev->set_status_cycles(getenv("RT_VOODOO_STATUS_CYCLES") ? u32(atoi(getenv("RT_VOODOO_STATUS_CYCLES"))) : 1000);
	s_dev->vblank_callback().set([](int state) {
		if (state) {
			publish_frame();
			epic_raise(0);             // EPIC IRQ0
			rt_pace_vblank();          // real-time pacing (no-op when headless)
		}
	});
	s_dev->pciint_callback().set([](int state) { if (state) epic_raise(4); });
	s_dev->start();
	s_dev->reset();
}

static u64 s_wcount[64];          // per 512KB region of BAR0
static u64 s_regcount[3][256];    // cmd/2d/3d register write counts
static u64 s_lfb_writes, s_lfb_reads;

uint32_t voodoo_reg_read(uint32_t off) { return s_dev->read(off >> 2); }
void voodoo_reg_write(uint32_t off, uint32_t v, uint32_t mask)
{
	s_wcount[(off >> 19) & 63]++;
	if (off >= 0x80000 && off < 0x100000) s_regcount[0][(off >> 2) & 0xff]++;
	else if (off >= 0x100000 && off < 0x200000) s_regcount[1][(off >> 2) & 0xff]++;
	else if (off >= 0x200000 && off < 0x600000) s_regcount[2][(off >> 2) & 0xff]++;
	s_dev->write(off >> 2, v, mask);
}
/* recomp: render the displayed buffers at n times the resolution (1 = native), from the next frame */
void voodoo_set_scale(int n) { s_scale = n; if (s_dev) s_dev->set_render_scale(n); }
/* recomp: widescreen margin, native pixels on each side of the displayed buffers (0 = 4:3) */
void voodoo_set_wide(int m) { s_wide = m; if (s_dev) s_dev->set_wide_margin(m); }

uint32_t voodoo_lfb_read(uint32_t off) { s_lfb_reads++; g_fbstats.lfb_read_mb[(off >> 20) & 15]++; return s_dev->read_lfb(off >> 2); }
void voodoo_lfb_write(uint32_t off, uint32_t v, uint32_t mask) { s_lfb_writes++; g_fbstats.lfb_write_mb[(off >> 20) & 15]++; s_dev->write_lfb(off >> 2, v, mask); }
uint32_t voodoo_io_read(uint32_t off) { return s_dev->read_io(off >> 2); }
void voodoo_io_write(uint32_t off, uint32_t v, uint32_t mask) { s_dev->write_io(off >> 2, v, mask); }

// copy of the most recent frame (thread-safe); returns frame counter
uint64_t voodoo_get_frame(uint32_t *dst, int max_pixels, int *w, int *h)
{
	std::lock_guard<std::mutex> lock(s_frame_lock);
	*w = s_frame_w;
	*h = s_frame_h;
	if (dst && s_frame_w * s_frame_h <= max_pixels)
		memcpy(dst, s_frame.data(), s_frame.size() * 4);
	return s_frame_count;
}

void voodoo_stats(void)
{
	rt_log("  voodoo frames published: %llu (last %dx%d)\n", (unsigned long long)s_frame_count, s_frame_w, s_frame_h);
	rt_log("  voodoo lfb writes=%llu reads=%llu\n", (unsigned long long)s_lfb_writes, (unsigned long long)s_lfb_reads);
	if (getenv("RT_VOODOO_FBSTATS"))
	{
		auto &f = g_fbstats;
		rt_log("  fbstats cmdfifo packets: t0=%llu t1=%llu t2=%llu t3=%llu t4=%llu t5=%llu\n", f.pkt[0], f.pkt[1], f.pkt[2], f.pkt[3], f.pkt[4], f.pkt[5]);
		for (int k = 0; k < 4; k++)
			if (f.p5_space[k]) rt_log("  fbstats packet5 space %d: %llu, %08x-%08x\n", k, f.p5_space[k], f.p5_min[k], f.p5_max[k]);
		for (int k = 0; k < 16; k++)
			if (f.blit[k]) rt_log("  fbstats 2D blit cmd %d: %llu\n", k, f.blit[k]);
		for (int k = 0; k < 16; k++)
			if (f.lfb_read_mb[k] || f.lfb_write_mb[k]) rt_log("  fbstats LFB MB %x: reads %llu writes %llu\n", k, f.lfb_read_mb[k], f.lfb_write_mb[k]);
		for (unsigned a : f.colbuf) rt_log("  fbstats colBufferAddr %08x\n", a);
	}
	for (int i = 0; i < 64; i++)
		if (s_wcount[i]) rt_log("  voodoo BAR0 region %07x: %llu writes\n", i << 19, (unsigned long long)s_wcount[i]);
	static const char *const nm[3] = {"cmd", "2d", "3d"};
	for (int k = 0; k < 3; k++)
		for (int i = 0; i < 256; i++)
			if (s_regcount[k][i]) rt_log("  voodoo %s reg %03x: %llu\n", nm[k], i * 4, (unsigned long long)s_regcount[k][i]);
}

} // extern "C"
