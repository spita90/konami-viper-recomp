// Minimal stand-in for the parts of MAME's emu.h used by the Voodoo core
// (src/devices/video/voodoo*.cpp, poly.h, rgbutil.h), bound to the recomp runtime.
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cassert>
#include <climits>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>
#include <list>

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using s8 = int8_t;
using s16 = int16_t;
using s32 = int32_t;
using s64 = int64_t;
using offs_t = u32;
using osd_ticks_t = s64;
using attoseconds_t = s64;

#define ATTR_COLD
#define ATTR_HOT
#ifdef __clang__
#define ATTR_FORCE_INLINE inline __attribute__((always_inline))
#else                               /* GCC rejects a second inline; MAME's own definition */
#define ATTR_FORCE_INLINE __attribute__((always_inline))
#endif
#define ATTR_PRINTF(x, y) __attribute__((format(printf, x, y)))
#define FUNC(x) &x, #x
#define NAME(x) x, #x

constexpr attoseconds_t ATTOSECONDS_PER_SECOND = 1000000000000000000LL;

// ------------------------------------------------------------------ runtime hooks (C side)
extern "C" {
uint64_t rt_now(void);
void rt_log(const char *fmt, ...);
typedef void (*SchedCb)(void *arg);
void rt_sched_at(uint64_t cycle, SchedCb cb, void *arg);
void rt_sched_cancel(SchedCb cb, void *arg);
void rt_eat_cycles(uint32_t n);
}
constexpr double RT_CPU_HZ = 203212800.0;

// ------------------------------------------------------------------ bit helpers
template <typename T> constexpr T BIT(T x, int n) noexcept { return (x >> n) & T(1); }
template <typename T, typename U> constexpr T BIT(T x, U n, U w) noexcept
{
	return (x >> n) & ((w >= int(sizeof(T) * 8)) ? ~T(0) : ((T(1) << w) - 1));
}
template <typename T> constexpr T BIT(T x, int n, int w) noexcept
{
	return (x >> n) & ((w >= int(sizeof(T) * 8)) ? ~T(0) : ((T(1) << w) - 1));
}

namespace util {
template <typename T, typename U> constexpr std::make_signed_t<T> sext(T value, U width) noexcept
{
	using S = std::make_signed_t<T>;
	const int shift = int(sizeof(T) * 8) - int(width);
	return S(value << shift) >> shift;
}
}

#define COMBINE_DATA(varptr) (*(varptr) = (*(varptr) & ~mem_mask) | (data & mem_mask))
#define ACCESSING_BITS_0_7 ((mem_mask & 0x000000ffU) != 0)
#define ACCESSING_BITS_8_15 ((mem_mask & 0x0000ff00U) != 0)
#define ACCESSING_BITS_16_23 ((mem_mask & 0x00ff0000U) != 0)
#define ACCESSING_BITS_24_31 ((mem_mask & 0xff000000U) != 0)
#define ACCESSING_BITS_0_15 ((mem_mask & 0x0000ffffU) != 0)
#define ACCESSING_BITS_16_31 ((mem_mask & 0xffff0000U) != 0)

constexpr u32 swapendian_int32(u32 v) { return __builtin_bswap32(v); }
constexpr u16 swapendian_int16(u16 v) { return __builtin_bswap16(v); }
constexpr u32 little_endianize_int32(u32 v) { return v; }   // hosts are little-endian
constexpr u16 little_endianize_int16(u16 v) { return v; }

constexpr u8 pal5bit(u8 bits) { bits &= 0x1f; return (bits << 3) | (bits >> 2); }
constexpr u8 pal6bit(u8 bits) { bits &= 0x3f; return (bits << 2) | (bits >> 4); }
constexpr u8 pal4bit(u8 bits) { bits &= 0xf; return (bits << 4) | bits; }

template <int N> constexpr u8 palexpand(u8 bits)
{
	if (N == 1) return (bits & 1) ? 0xff : 0x00;
	if (N == 2) { bits &= 3; return (bits << 6) | (bits << 4) | (bits << 2) | bits; }
	if (N == 3) { bits &= 7; return (bits << 5) | (bits << 2) | (bits >> 1); }
	if (N == 4) { bits &= 0xf; return (bits << 4) | bits; }
	if (N == 5) { bits &= 0x1f; return (bits << 3) | (bits >> 2); }
	if (N == 6) { bits &= 0x3f; return (bits << 2) | (bits >> 4); }
	if (N == 7) { bits &= 0x7f; return (bits << 1) | (bits >> 6); }
	return bits;
}

#define UNEXPECTED(x) __builtin_expect(!!(x), 0)
#define EXPECTED(x) __builtin_expect(!!(x), 1)
#define ALLOW_SAVE_TYPE(x)
#define BYTE_XOR_LE(a) (a)
#define BYTE4_XOR_LE(a) (a)
#define WORD_XOR_LE(a) (a)
constexpr int WORK_MAX_THREADS = 16;
struct emu_profiler_scope { void stop() {} };
struct emu_profiler { template <typename T> emu_profiler_scope start(T) { return {}; } };
inline emu_profiler g_profiler;
enum { PROFILER_USER1, PROFILER_USER2, PROFILER_USER3 };

struct XTAL
{
	double v;
	constexpr explicit XTAL(double x) : v(x) {}
	constexpr double dvalue() const { return v; }
	constexpr u32 value() const { return u32(v); }
	constexpr XTAL operator*(double k) const { return XTAL(v * k); }
	constexpr XTAL operator/(double k) const { return XTAL(v / k); }
};

inline u8 count_leading_zeros_32(u32 v) { return v ? u8(__builtin_clz(v)) : 32; }
inline s64 mul_32x32(s32 a, s32 b) { return s64(a) * s64(b); }
inline u64 mulu_32x32(u32 a, u32 b) { return u64(a) * u64(b); }

// ------------------------------------------------------------------ string formatting
namespace emu_shim {
template <typename T> inline T fmt_arg(T v) { return v; }
inline const char *fmt_arg(const std::string &s) { return s.c_str(); }
inline const char *fmt_arg(std::string &s) { return s.c_str(); }
extern bool g_log_enabled;
}

template <typename... A> inline std::string string_format(const char *fmt, A &&...a)
{
	char buf[4096];
	snprintf(buf, sizeof buf, fmt, emu_shim::fmt_arg(a)...);
	return buf;
}

template <typename... A> inline void osd_printf_info(const char *fmt, A &&...a)
{
	rt_log(fmt, emu_shim::fmt_arg(a)...);
}
template <typename... A> [[noreturn]] inline void fatalerror(const char *fmt, A &&...a)
{
	rt_log("voodoo fatal: ");
	rt_log(fmt, emu_shim::fmt_arg(a)...);
	std::abort();
}
template <typename... A> inline void popmessage(A &&...) {}

// ------------------------------------------------------------------ rgb_t
class rgb_t
{
public:
	constexpr rgb_t() : m_data(0) {}
	constexpr rgb_t(u32 data) : m_data(data) {}
	constexpr rgb_t(u8 r, u8 g, u8 b) : m_data((255u << 24) | (u32(r) << 16) | (u32(g) << 8) | b) {}
	constexpr rgb_t(u8 a, u8 r, u8 g, u8 b) : m_data((u32(a) << 24) | (u32(r) << 16) | (u32(g) << 8) | b) {}
	constexpr operator u32() const { return m_data; }
	constexpr u8 a() const { return m_data >> 24; }
	constexpr u8 r() const { return m_data >> 16; }
	constexpr u8 g() const { return m_data >> 8; }
	constexpr u8 b() const { return m_data; }
	rgb_t &set_a(u8 v) { m_data = (m_data & 0x00ffffff) | (u32(v) << 24); return *this; }
	rgb_t &set_r(u8 v) { m_data = (m_data & 0xff00ffff) | (u32(v) << 16); return *this; }
	rgb_t &set_g(u8 v) { m_data = (m_data & 0xffff00ff) | (u32(v) << 8); return *this; }
	rgb_t &set_b(u8 v) { m_data = (m_data & 0xffffff00) | v; return *this; }
	static constexpr rgb_t black() { return rgb_t(0, 0, 0); }
	static constexpr rgb_t white() { return rgb_t(255, 255, 255); }
private:
	u32 m_data;
};

template <int R, int G, int B> inline rgb_t rgbexpand(u32 data, u8 rshift, u8 gshift, u8 bshift)
{
	return rgb_t(palexpand<R>(data >> rshift), palexpand<G>(data >> gshift), palexpand<B>(data >> bshift));
}
template <int A, int R, int G, int B> inline rgb_t argbexpand(u32 data, u8 ashift, u8 rshift, u8 gshift, u8 bshift)
{
	return rgb_t(palexpand<A>(data >> ashift), palexpand<R>(data >> rshift), palexpand<G>(data >> gshift), palexpand<B>(data >> bshift));
}

// ------------------------------------------------------------------ rectangle / bitmap
struct rectangle
{
	int min_x = 0, max_x = 0, min_y = 0, max_y = 0;
	constexpr rectangle() = default;
	constexpr rectangle(int minx, int maxx, int miny, int maxy) : min_x(minx), max_x(maxx), min_y(miny), max_y(maxy) {}
	constexpr int left() const { return min_x; }
	constexpr int right() const { return max_x; }
	constexpr int top() const { return min_y; }
	constexpr int bottom() const { return max_y; }
	constexpr int width() const { return max_x + 1 - min_x; }
	constexpr int height() const { return max_y + 1 - min_y; }
	void set(int minx, int maxx, int miny, int maxy) { min_x = minx; max_x = maxx; min_y = miny; max_y = maxy; }
	void setx(int a, int b) { min_x = a; max_x = b; }
	void sety(int a, int b) { min_y = a; max_y = b; }
	constexpr bool empty() const { return min_x > max_x || min_y > max_y; }
	constexpr bool contains(int x, int y) const { return x >= min_x && x <= max_x && y >= min_y && y <= max_y; }
	rectangle &operator&=(const rectangle &s)
	{
		min_x = std::max(min_x, s.min_x); max_x = std::min(max_x, s.max_x);
		min_y = std::max(min_y, s.min_y); max_y = std::min(max_y, s.max_y);
		return *this;
	}
	constexpr bool operator==(const rectangle &o) const
	{
		return min_x == o.min_x && max_x == o.max_x && min_y == o.min_y && max_y == o.max_y;
	}
	constexpr bool operator!=(const rectangle &o) const { return !(*this == o); }
};

class bitmap_rgb32
{
public:
	bitmap_rgb32() = default;
	bitmap_rgb32(int w, int h) { allocate(w, h); }
	void allocate(int w, int h) { m_w = w; m_h = h; m_data.assign(size_t(w) * h, 0); }
	int width() const { return m_w; }
	int height() const { return m_h; }
	int rowpixels() const { return m_w; }
	u32 &pix(int y, int x = 0) { return m_data[size_t(y) * m_w + x]; }
	const u32 &pix(int y, int x = 0) const { return m_data[size_t(y) * m_w + x]; }
	u32 *raw() { return m_data.data(); }
	rectangle cliprect() const { return rectangle(0, m_w - 1, 0, m_h - 1); }
	void fill(u32 c) { std::fill(m_data.begin(), m_data.end(), c); }
	void fill(u32 c, const rectangle &r)
	{
		for (int y = std::max(r.min_y, 0); y <= std::min(r.max_y, m_h - 1); y++)
			for (int x = std::max(r.min_x, 0); x <= std::min(r.max_x, m_w - 1); x++)
				pix(y, x) = c;
	}
private:
	int m_w = 0, m_h = 0;
	std::vector<u32> m_data;
};

// ------------------------------------------------------------------ attotime (double seconds)
class attotime
{
public:
	constexpr attotime() : m_s(0) {}
	constexpr explicit attotime(double s) : m_s(s) {}
	constexpr attotime(s64 secs, attoseconds_t atto) : m_s(double(secs) + double(atto) * 1e-18) {}
	static const attotime zero;
	static const attotime never;
	static constexpr attotime from_double(double s) { return attotime(s); }
	static constexpr attotime from_hz(double hz) { return attotime(hz > 0 ? 1.0 / hz : 1e30); }
	static constexpr attotime from_ticks(u64 ticks, u32 freq) { return attotime(double(ticks) / double(freq)); }
	static constexpr attotime from_nsec(s64 ns) { return attotime(double(ns) * 1e-9); }
	static constexpr attotime from_usec(s64 us) { return attotime(double(us) * 1e-6); }
	static constexpr attotime from_msec(s64 ms) { return attotime(double(ms) * 1e-3); }
	constexpr double as_double() const { return m_s; }
	constexpr double as_hz() const { return m_s > 0 ? 1.0 / m_s : 0.0; }
	constexpr attoseconds_t as_attoseconds() const { return attoseconds_t(m_s * 1e18); }
	constexpr attoseconds_t attoseconds() const { return attoseconds_t((m_s - double(s64(m_s))) * 1e18); }
	constexpr s64 seconds() const { return s64(m_s); }
	constexpr bool is_zero() const { return m_s == 0; }
	constexpr bool is_never() const { return m_s >= 1e29; }
	std::string as_string(int precision = 9) const
	{
		char buf[64];
		snprintf(buf, sizeof buf, "%.*f", precision, m_s);
		return buf;
	}
	constexpr attotime operator+(const attotime &o) const { return attotime(m_s + o.m_s); }
	constexpr attotime operator-(const attotime &o) const { return attotime(m_s - o.m_s); }
	attotime &operator+=(const attotime &o) { m_s += o.m_s; return *this; }
	attotime &operator-=(const attotime &o) { m_s -= o.m_s; return *this; }
	constexpr attotime operator*(double k) const { return attotime(m_s * k); }
	constexpr attotime operator/(double k) const { return attotime(m_s / k); }
	constexpr bool operator<(const attotime &o) const { return m_s < o.m_s; }
	constexpr bool operator>(const attotime &o) const { return m_s > o.m_s; }
	constexpr bool operator<=(const attotime &o) const { return m_s <= o.m_s; }
	constexpr bool operator>=(const attotime &o) const { return m_s >= o.m_s; }
	constexpr bool operator==(const attotime &o) const { return m_s == o.m_s; }
	constexpr bool operator!=(const attotime &o) const { return m_s != o.m_s; }
private:
	double m_s;
};
inline constexpr attotime attotime::zero = attotime(0.0);
inline constexpr attotime attotime::never = attotime(1e30);

inline attotime emu_time_now() { return attotime(double(rt_now()) / RT_CPU_HZ); }

// ------------------------------------------------------------------ delegate
template <typename Sig> class delegate;
template <typename R, typename... A> class delegate<R(A...)>
{
public:
	delegate() = default;
	template <typename C> delegate(R (C::*fn)(A...), C *obj) : m_fn([obj, fn](A... a) -> R { return (obj->*fn)(a...); }) {}
	template <typename C> delegate(R (C::*fn)(A...) const, const C *obj) : m_fn([obj, fn](A... a) -> R { return (obj->*fn)(a...); }) {}
	template <typename C> delegate(R (C::*fn)(A...), const char *, C *obj) : delegate(fn, obj) {}
	template <typename F> explicit delegate(F f) : m_fn(std::move(f)) {}
	R operator()(A... a) const { return m_fn(a...); }
	bool isnull() const { return !m_fn; }
	explicit operator bool() const { return bool(m_fn); }
private:
	std::function<R(A...)> m_fn;
};
using save_prepost_delegate = delegate<void()>;

// ------------------------------------------------------------------ osd work queue (synchronous for now)
struct osd_work_queue;
struct osd_work_item;
typedef void *(*osd_work_callback)(void *param, int threadid);
constexpr int WORK_QUEUE_FLAG_IO = 0x0001;
constexpr int WORK_QUEUE_FLAG_MULTI = 0x0002;
constexpr int WORK_QUEUE_FLAG_HIGH_FREQ = 0x0004;
constexpr int WORK_ITEM_FLAG_AUTO_RELEASE = 0x0001;
osd_work_queue *osd_work_queue_alloc(int flags);
int osd_work_queue_items(osd_work_queue *queue);
bool osd_work_queue_wait(osd_work_queue *queue, osd_ticks_t timeout);
void osd_work_queue_free(osd_work_queue *queue);
osd_work_item *osd_work_item_queue_multiple(osd_work_queue *queue, osd_work_callback callback, s32 numitems,
                                            void *parambase, s32 paramstep, u32 flags);
osd_ticks_t osd_ticks();
osd_ticks_t osd_ticks_per_second();
inline osd_ticks_t get_profile_ticks() { return osd_ticks(); }

// ------------------------------------------------------------------ devices, machine, screen
struct machine_config {};
using device_type = const char *;
#define DECLARE_DEVICE_TYPE(Type, Class) extern const device_type Type;
#define DEFINE_DEVICE_TYPE(Type, Class, Short, Full) const device_type Type = Short;

class device_t;
class emu_timer;

class running_machine
{
public:
	struct save_mgr { template <typename... A> void register_presave(A &&...) {} template <typename... A> void register_postload(A &&...) {} };
	struct sched_mgr { void trigger(int) {} };
	struct input_mgr { template <typename T> bool code_pressed(T) const { return false; } };
	attotime time() const { return emu_time_now(); }
	std::string describe_context() const;
	save_mgr &save() { return m_save; }
	sched_mgr &scheduler() { return m_sched; }
	input_mgr &input() { return m_input; }
	device_t &root_device() { return *m_root; }
	device_t *m_root = nullptr;
private:
	save_mgr m_save;
	sched_mgr m_sched;
	input_mgr m_input;
};
enum { KEYCODE_L, KEYCODE_ENTER, KEYCODE_BACKSLASH };
running_machine &emu_machine();

class emu_timer
{
public:
	explicit emu_timer(std::function<void(s32)> cb) : m_cb(std::move(cb)) {}
	~emu_timer() { reset(); }
	void adjust(attotime delay, s32 param = 0, attotime period = attotime::never)
	{
		reset();
		m_param = param;
		m_period = period;
		double d = delay.as_double();
		if (d < 0) d = 0;
		m_expire = rt_now() + u64(d * RT_CPU_HZ);
		m_armed = true;
		rt_sched_at(m_expire, &emu_timer::fire, this);
	}
	void reset(attotime = attotime::never) { if (m_armed) rt_sched_cancel(&emu_timer::fire, this); m_armed = false; }
	void enable(bool en = true) { if (!en) reset(); }
	bool enabled() const { return m_armed; }
	s32 param() const { return m_param; }
	attotime remaining() const { return m_armed ? attotime(double(m_expire - rt_now()) / RT_CPU_HZ) : attotime::never; }
	attotime expire() const { return attotime(double(m_expire) / RT_CPU_HZ); }
private:
	static void fire(void *arg)
	{
		auto *t = static_cast<emu_timer *>(arg);
		t->m_armed = false;
		if (!t->m_period.is_never() && t->m_period.as_double() > 0)
			t->adjust(t->m_period, t->m_param, t->m_period);
		t->m_cb(t->m_param);
	}
	std::function<void(s32)> m_cb;
	s32 m_param = 0;
	attotime m_period = attotime::never;
	u64 m_expire = 0;
	bool m_armed = false;
};

class device_t
{
public:
	device_t(const machine_config &, device_type type, const char *tag, device_t *, u32 clock)
		: m_type(type), m_tag(tag), m_clock(clock) {}
	virtual ~device_t() = default;
	const char *tag() const { return m_tag; }
	device_type type() const { return m_type; }
	u32 clock() const { return m_clock; }
	running_machine &machine() const { return emu_machine(); }
	attotime clocks_to_attotime(u64 clocks) const { return attotime::from_ticks(clocks, m_clock); }
	u64 attotime_to_clocks(const attotime &t) const { return u64(t.as_double() * m_clock); }
	template <typename... A> void logerror(const char *fmt, A &&...a) const
	{
		if (emu_shim::g_log_enabled) rt_log(fmt, emu_shim::fmt_arg(a)...);
	}
	template <typename... A> void save_item(A &&...) {}
	template <typename... A> void save_pointer(A &&...) {}
	template <typename C> emu_timer *timer_alloc(void (C::*fn)(s32), const char *, C *obj)
	{
		m_timers.push_back(std::make_unique<emu_timer>([obj, fn](s32 p) { (obj->*fn)(p); }));
		return m_timers.back().get();
	}
	// lifecycle, driven by the bridge
	void start() { device_start(); }
	void reset() { device_reset(); }
protected:
	virtual void device_start() {}
	virtual void device_stop() {}
	virtual void device_reset() {}
	virtual void device_post_load() {}
private:
	device_type m_type;
	const char *m_tag;
	u32 m_clock;
	std::vector<std::unique_ptr<emu_timer>> m_timers;
};

struct device_enumerator
{
	struct iterator
	{
		device_t *const *p;
		device_t &operator*() const { return **p; }
		iterator &operator++() { ++p; return *this; }
		bool operator!=(const iterator &o) const { return p != o.p; }
	};
	explicit device_enumerator(device_t &root) : m_root(&root) {}
	iterator begin() const { return {&m_root}; }
	iterator end() const { return {&m_root + 1}; }
	device_t *m_root;
};

class screen_device
{
public:
	void configure(int htotal, int vtotal, const rectangle &visarea, attotime period)
	{
		m_htotal = htotal; m_vtotal = vtotal; m_visarea = visarea; m_period = period;
		m_frame_base = emu_time_now();
		m_configured = true;
	}
	const rectangle &visible_area() const { return m_visarea; }
	int width() const { return m_htotal; }
	int height() const { return m_vtotal; }
	attotime frame_period() const { return m_period; }
	double frame_pos() const  // 0..1 within the current frame
	{
		double p = m_period.as_double();
		double t = (emu_time_now() - m_frame_base).as_double();
		double f = t / p;
		return f - std::floor(f);
	}
	int vpos() const { return int(frame_pos() * m_vtotal) % std::max(m_vtotal, 1); }
	int hpos() const
	{
		double lines = frame_pos() * m_vtotal;
		return int((lines - std::floor(lines)) * m_htotal) % std::max(m_htotal, 1);
	}
	attotime time_until_pos(int vpos, int hpos = 0) const
	{
		double p = m_period.as_double();
		double target = (double(vpos) * m_htotal + hpos) / (double(m_vtotal) * m_htotal);
		double cur = frame_pos();
		double d = target - cur;
		if (d <= 0) d += 1.0;
		return attotime(d * p);
	}
	u64 frame_number() const { return u64((emu_time_now() - m_frame_base).as_double() / m_period.as_double()); }
	void update_partial(int) {}
	void update_now() {}
private:
	int m_htotal = 1024, m_vtotal = 768;
	rectangle m_visarea = rectangle(0, 1023, 0, 767);
	attotime m_period = attotime::from_hz(60);
	attotime m_frame_base;
	bool m_configured = false;
};
screen_device &emu_screen();

class device_video_interface
{
public:
	device_video_interface(const machine_config &, device_t &) {}
	screen_device &screen() const { return emu_screen(); }
};

class devcb_write_line
{
public:
	explicit devcb_write_line(device_t &) {}
	struct binder
	{
		devcb_write_line *target;
		template <typename F> void set(F f) { target->set(std::move(f)); }
	};
	binder bind() { return binder{this}; }
	template <typename F> void set(F f) { m_fn = std::function<void(int)>(std::move(f)); }
	void operator()(int state) { if (m_fn) m_fn(state); }
	void resolve_safe(int = 0) {}
	bool isunset() const { return !m_fn; }
private:
	std::function<void(int)> m_fn;
};

struct finder_base { static constexpr const char *DUMMY_TAG = "dummy"; };
class cpu_device
{
public:
	void eat_cycles(int n) { rt_eat_cycles(u32(n)); }
	void spin_until_trigger(int) {}
	void trigger(int) {}
	offs_t pc() const { return 0; }
};
cpu_device &emu_cpu();
template <typename T> class required_device
{
public:
	required_device(device_t &, const char *) {}
	template <typename U> void set_tag(U &&) {}
	T *operator->() const { return &emu_cpu(); }
	T &operator*() const { return emu_cpu(); }
	bool found() const { return true; }
};

struct address_map_entry
{
	template <typename... A> address_map_entry &rw(A &&...) { return *this; }
	template <typename... A> address_map_entry &r(A &&...) { return *this; }
	template <typename... A> address_map_entry &w(A &&...) { return *this; }
	template <typename... A> address_map_entry &m(A &&...) { return *this; }
	address_map_entry &mirror(offs_t) { return *this; }
};
struct address_map
{
	address_map_entry operator()(offs_t, offs_t) { return {}; }
};
