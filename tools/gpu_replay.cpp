// Draws a GPU renderer capture (RT_GPU_CAPTURE=file:first:count) with runtime/gpu_gl.cpp in a
// hidden window and writes each frame's displayed picture as gpu_NNNNNN.ppm, to compare with the
// software rasterizer's frames of the same run (--frames DIR --frame-every 1); see
// tools/gpu_compare.py.
//
//   c++ -std=c++20 -O1 $(sdl2-config --cflags) -Iruntime tools/gpu_replay.cpp runtime/gpu_gl.cpp \
//       -o build/gpu_replay $(sdl2-config --libs)
//   build/gpu_replay capture.bin first out_dir
#include <SDL.h>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "voodoo/voodoo_gpu.h"

extern "C" void rt_log(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vfprintf(stdout, fmt, ap);
	va_end(ap);
}
bool voodoo_gpu_take(std::vector<vgpu::frame_list> &) { return false; }
void voodoo_gpu_recycle(std::vector<vgpu::frame_list> &lists) { lists.clear(); }
extern "C" void voodoo_set_gpu(int) {}

extern "C" int gpu_gl_init(SDL_Window *win, uint32_t vram_size);
extern "C" void gpu_gl_debug_load(const uint32_t *vram, const uint32_t *luts, int rows);
extern "C" void gpu_gl_debug_execute(void *list);
extern "C" double gpu_gl_debug_execute_timed(void *list);
extern "C" int gpu_gl_debug_read(uint32_t *out, int max_pixels, int *w, int *h);

template<typename T> static bool read_vec(FILE *f, std::vector<T> &v, uint32_t words)
{
	v.resize(size_t(words) * 4 / sizeof(T));
	return fread(v.data(), 4, words, f) == words;
}

int main(int argc, char **argv)
{
	if (argc < 4)
	{
		fprintf(stderr, "usage: gpu_replay capture.bin first out_dir\n");
		return 2;
	}
	FILE *f = fopen(argv[1], "rb");
	if (!f)
	{
		perror(argv[1]);
		return 1;
	}
	uint32_t header[3];
	if (fread(header, 4, 3, f) != 3 || header[0] != 0x55504756)
	{
		fprintf(stderr, "not a capture\n");
		return 1;
	}
	std::vector<uint32_t> vram(header[1] / 4), luts(size_t(header[2]) * 256);
	fread(vram.data(), 4, vram.size(), f);
	fread(luts.data(), 4, luts.size(), f);

	SDL_Init(SDL_INIT_VIDEO);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
	SDL_Window *win = SDL_CreateWindow("gpu_replay", 0, 0, 64, 64, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
	if (!win || !gpu_gl_init(win, header[1]))
		return 1;
	gpu_gl_debug_load(vram.data(), luts.data(), int(header[2]));

	std::vector<uint32_t> pix(2048 * 2048);
	double total_ms = 0;
	int lists = 0;
	for (int frame = atoi(argv[2]); ; frame++)
	{
		uint32_t counts[5];
		if (fread(counts, 4, 5, f) != 5)
			break;
		vgpu::frame_list l;
		bool ok = read_vec(f, l.cmds, counts[0] * uint32_t(sizeof(vgpu::cmd) / 4)) && read_vec(f, l.tris, counts[1]) &&
			read_vec(f, l.states, counts[2]) && read_vec(f, l.verts, counts[3] * uint32_t(sizeof(vgpu::vertex) / 4)) &&
			read_vec(f, l.blob, counts[4]);
		if (!ok)
			break;
		{
			int n[6] = {};
			for (auto const &c : l.cmds)
				n[c.type < 6 ? c.type : 0]++;
			printf("frame %d: %u cmds (scale %d, target %d, draw %d, vram %d, lut %d, display %d)\n", frame, counts[0], n[0], n[1], n[2], n[3], n[4], n[5]);
			for (auto const &c : l.cmds)
				if (c.type == vgpu::CMD_TARGET || c.type == vgpu::CMD_DISPLAY || c.type == vgpu::CMD_SCALE)
					printf("  cmd %u: %08x %08x %08x %08x %08x\n", c.type, c.a, c.b, c.c, c.d, c.e);
		}
		double const ms = gpu_gl_debug_execute_timed(&l);
		total_ms += ms;
		lists++;
		int w, h;
		if (!gpu_gl_debug_read(pix.data(), int(pix.size()), &w, &h))
			continue;
		char path[1024];
		snprintf(path, sizeof path, "%s/gpu_%06d.ppm", argv[3], frame);
		FILE *o = fopen(path, "wb");
		fprintf(o, "P6\n%d %d\n255\n", w, h);
		for (int i = 0; i < w * h; i++)
		{
			unsigned char px[3] = { (unsigned char)(pix[i] >> 16), (unsigned char)(pix[i] >> 8), (unsigned char)pix[i] };
			fwrite(px, 1, 3, o);
		}
		fclose(o);
		printf("frame %d: %zu triangles, %zu states\n", frame, l.tris.size() / vgpu::TRI_WORDS, l.states.size() / vgpu::STATE_WORDS);
	}
	if (lists)
		printf("GPU time: %.3f ms per list on average (%d lists)\n", total_ms / lists, lists);
	SDL_Quit();
	return 0;
}
