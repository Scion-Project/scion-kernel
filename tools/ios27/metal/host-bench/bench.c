/* SPDX-License-Identifier: MIT-0 */
/* host-bench: the native Metal bridge's CPU cost per draw on the Mac (KosmicKrisp as the Vulkan driver), in the shape of
 * CoreAnimation's home-screen main pass on the phone: one render pass over an 828x1702 BGRA8 target (load+store) with RGBA16F and
 * R32F memoryless attachments (dont-care), N draws of 4-vertex strips, each with vertex bytes, a scissor and (every K-th) another of P
 * pipelines (blend states differ). Times the encode (encoder new .. end + commit) per frame, then waits for completion (CA's
 * one-frame-at-a-time shape).
 *   bench LOADER CACHE [frames] [draws] [pipelines] [switch_every]   (LOADER: libkkshim.dylib, see build.sh)
 * BENCH_TEX=1: the fixture's sampled_fragment and one of 8 textures bound a draw; BENCH_FBYTES=1: fragment bytes a draw too;
 * BENCH_NO_MRT=1: the BGRA8 target alone. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include "NativeMetalObjects.h"

static double now_us(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1e6 + t.tv_nsec / 1e3;
}
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %s: %s\n", #x, nmb_last_error()); exit(1); } } while (0)

int main(int argc, char **argv)
{
	const char *loader = argv[1], *cache = argv[2];
	int frames = argc > 3 ? atoi(argv[3]) : 200, draws = argc > 4 ? atoi(argv[4]) : 100;
	int npipes = argc > 5 ? atoi(argv[5]) : 16, every = argc > 6 ? atoi(argv[6]) : 1;
	const int sixteen = getenv("BENCH_NO_MRT") == NULL;
	mkdir(cache, 0777);
	NmbDevice *dev = nmb_device_open(loader, cache);
	CHECK(dev);
	char name[128];
	nmb_device_name(dev, name, sizeof name);
	size_t airlen;
	const void *air = nmb_fixture_air(&airlen);
	NmbLibrary *lib = nmb_library_new(dev, air, airlen, true);
	CHECK(lib);
	NmbFunction *vs = nmb_function_new(lib, "colored_vertex"), *fs = nmb_function_new(lib, getenv("BENCH_TEX") ? "sampled_fragment" : "colored_fragment");
	CHECK(vs && fs);
	NmbRenderPipeline *pipes[64];
	if (npipes > 64) npipes = 64;
	double t0 = now_us();
	for (int i = 0; i < npipes; i++) {
		struct NmbRenderPipelineDescriptor d;
		memset(&d, 0, sizeof d);
		d.vertex = vs;
		d.fragment = fs;
		/* blend factors differ per pipeline: each is its own VkPipeline */
		static const uint64_t factors[] = {1, 5, 4, 0, 2, 3, 6, 7};
		d.colors[0] = (struct NmbColorAttachment){80, 0xf, (uint64_t)(i != 0), factors[i % 8], factors[(i / 8 + 1) % 8], 0,
		                                          factors[i % 8], factors[(i / 8 + 1) % 8], 0};
		if (sixteen) {
			d.colors[1] = (struct NmbColorAttachment){115, 0, 0, 1, 0, 0, 1, 0, 0};
			d.colors[2] = (struct NmbColorAttachment){55, 0, 0, 1, 0, 0, 1, 0, 0};
		}
		d.samples = 1;
		d.rasterization = 1;
		pipes[i] = nmb_render_pipeline_new(dev, &d);
		CHECK(pipes[i]);
	}
	printf("device %s; %d pipelines in %.0f ms\n", name, npipes, (now_us() - t0) / 1000);
	struct NmbTextureDescriptor td = {2, 80, 828, 1702, 1, 1, 1, 1, 5 /* read | render target */, 0x20 /* private */};
	NmbTexture *target = nmb_texture_new(dev, &td);
	td.format = 115; td.usage = 4; td.options = 0x30; /* memoryless */
	NmbTexture *t16 = nmb_texture_new(dev, &td);
	td.format = 55;
	NmbTexture *t32 = nmb_texture_new(dev, &td);
	CHECK(target && t16 && t32);
	float pos[4][4] = {{-1, -1, 0, 1}, {1, -1, 0, 1}, {-1, 1, 0, 1}, {1, 1, 0, 1}}, col[4][4];
	for (int i = 0; i < 4; i++) col[i][0] = col[i][1] = col[i][2] = col[i][3] = 0.5f;
	NmbBuffer *colors = nmb_buffer_new(dev, sizeof col, 0, col);
	NmbTexture *texs[8];
	struct NmbTextureDescriptor sd = {2, 80, 64, 64, 1, 1, 1, 1, 1, 0};
	for (int i = 0; i < 8; i++) { texs[i] = nmb_texture_new(dev, &sd); CHECK(texs[i]); }
	const int tex = getenv("BENCH_TEX") != NULL;
	float fb[8] = {0};
	NmbQueue *q = nmb_queue_new(dev);
	double enc_sum = 0, enc_min = 1e18, wait_sum = 0, draw_sum = 0;
	int counted = 0;
	for (int f = 0; f < frames; f++) {
		double a = now_us();
		NmbCommandBuffer *cb = nmb_command_buffer_new(q);
		struct NmbRenderPassDescriptor pass;
		memset(&pass, 0, sizeof pass);
		pass.colors[0] = (struct NmbPassAttachment){target, 0, 0, 0, 1, 1, {0, 0, 0, 0}};
		if (sixteen) {
			pass.colors[1] = (struct NmbPassAttachment){t16, 0, 0, 0, 0, 0, {0, 0, 0, 0}};
			pass.colors[2] = (struct NmbPassAttachment){t32, 0, 0, 0, 0, 0, {0, 0, 0, 0}};
		}
		pass.width = 828; pass.height = 1702; pass.array_length = 1; pass.samples = 1;
		NmbEncoder *e = nmb_render_encoder_new(cb, &pass);
		CHECK(e);
		double b = now_us();
		for (int i = 0; i < draws; i++) {
			if (i % every == 0)
				CHECK(nmb_render_set_pipeline(e, pipes[(i / every + f) % npipes]));
			float p[4][4];
			memcpy(p, pos, sizeof p);
			p[0][0] = -1 + (i % 7) * 0.01f;
			CHECK(nmb_encoder_set_bytes(e, 1, p, sizeof p, 0));
			CHECK(nmb_encoder_set_buffer(e, 1, colors, 0, 1));
			uint64_t sc[4] = {(uint64_t)(i * 3 % 400), (uint64_t)(i * 7 % 900), 400, 800};
			CHECK(nmb_render_set_scissor(e, sc));
			if (tex) CHECK(nmb_encoder_set_texture(e, 2, texs[(i * 5 + f) % 8], 0));
			if (getenv("BENCH_FBYTES")) { fb[0] = (float)i; CHECK(nmb_encoder_set_bytes(e, 2, fb, sizeof fb, 0)); }
			CHECK(nmb_render_draw(e, 4 /* strip */, 0, 4, 1, 0));
		}
		double c = now_us();
		CHECK(nmb_encoder_end(e));
		CHECK(nmb_command_buffer_commit(cb));
		double d = now_us();
		nmb_command_buffer_wait(cb, 0);
		double w = now_us();
		nmb_command_buffer_release(cb);
		if (f >= frames / 4) {
			enc_sum += d - a; draw_sum += c - b; wait_sum += w - d; counted++;
			if (d - a < enc_min) enc_min = d - a;
		}
	}
	printf("frames %d draws %d pipelines %d (switch every %d)%s: encode avg %.0f us (min %.0f), draw loop %.1f us/draw, "
	       "rest of encode %.0f us, wait %.0f us\n", counted, draws, npipes, every, sixteen ? " +RGBA16F+R32F memoryless" : "",
	       enc_sum / counted, enc_min, draw_sum / counted / draws, (enc_sum - draw_sum) / counted, wait_sum / counted);
	char text[4096];
	if (nmb_device_work_counts_text(dev, text, sizeof text)) printf("work: %s\n", text);
	while (nmb_take_message(text, sizeof text)) printf("msg: %.300s\n", text);
	return 0;
}
