/*
 * Project Starbite: first iOS app on the Mali-G72. Opens StarbiteMali (the
 * Panfrost-style user client, xnu/src/StarbiteMali.cpp), reads the GPU
 * parameters, creates and maps buffers, and runs two job chains from
 * userland in the apps' address space:
 *   1. WRITE_VALUE "SBGPUAPP" into a buffer (JS1)
 *   2. a fragment clear of a 64x64 RGBA8 buffer to green (JS0, null tiler;
 *      the layouts of bsd/dev/starlte_gpu.c stage 3)
 * Every result goes to the kernel log ("STARLTE GPU APP: ...", LOG
 * selector), so the persistent log pulled from TWRP shows it.
 *   tools/ios27/gpu/build-sbgpu-test.sh -> out/ios/sbgpu-test
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <IOKit/IOKitLib.h>

enum {
	SBM_SUBMIT = 0, SBM_CREATE_BO = 2, SBM_MMAP_BO = 3, SBM_GET_PARAM = 4,
	SBM_GEM_CLOSE = 0x100, SBM_VERSION = 0x101, SBM_LOG = 0x102,
};
struct sbm_submit {
	uint64_t jc, in_syncs;
	uint32_t in_sync_count, out_sync;
	uint64_t bo_handles;
	uint32_t bo_handle_count, requirements, jm_ctx_handle, pad;
};
struct sbm_create_bo {
	uint32_t size, flags, handle, pad;
	uint64_t offset;
};
struct sbm_mmap_bo {
	uint32_t handle, flags;
	uint64_t offset;
};
struct sbm_get_param {
	uint32_t param, pad;
	uint64_t value;
};

static io_connect_t conn;

static kern_return_t
call(uint32_t sel, void *in, size_t in_size, void *out, size_t out_size)
{
	size_t got = out_size;
	return IOConnectCallStructMethod(conn, sel, in, in_size, out, out ? &got : NULL);
}

static void
klog(const char *fmt, ...)
{
	char text[240];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(text, sizeof(text), fmt, ap);
	va_end(ap);
	printf("%s\n", text);
	call(SBM_LOG, text, strlen(text), NULL, 0);
}

struct bo {
	uint32_t handle;
	uint64_t va;
	volatile uint8_t *cpu;
};

static int
bo_new(struct bo *b, uint32_t size)
{
	struct sbm_create_bo c = { .size = size };
	kern_return_t kr = call(SBM_CREATE_BO, &c, sizeof(c), &c, sizeof(c));
	if (kr != KERN_SUCCESS) {
		klog("CREATE_BO %u failed: %#x", size, kr);
		return -1;
	}
	struct sbm_mmap_bo m = { .handle = c.handle };
	kr = call(SBM_MMAP_BO, &m, sizeof(m), &m, sizeof(m));
	if (kr != KERN_SUCCESS) {
		klog("MMAP_BO %u failed: %#x", c.handle, kr);
		return -1;
	}
	b->handle = c.handle;
	b->va = c.offset;
	b->cpu = (volatile uint8_t *)(uintptr_t)m.offset;
	return 0;
}

static void
put32(struct bo *b, uint32_t off, uint32_t v)
{
	*(volatile uint32_t *)(b->cpu + off) = v;
}

static void
put64(struct bo *b, uint32_t off, uint64_t v)
{
	*(volatile uint64_t *)(b->cpu + off) = v;
}

static kern_return_t
submit(uint64_t jc, uint32_t req)
{
	struct sbm_submit s = { .jc = jc, .requirements = req };
	__asm__ volatile ("dsb sy" ::: "memory");
	return call(SBM_SUBMIT, &s, sizeof(s), NULL, 0);
}

int
main(void)
{
	/* Run from a launchd job at boot: the kernel publishes StarbiteMali
	 * only after its GPU stages (some seconds in), so wait for it. */
	io_service_t svc = IO_OBJECT_NULL;
	for (int i = 0; i < 120 && svc == IO_OBJECT_NULL; i++) {
		svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("StarbiteMali"));
		if (svc == IO_OBJECT_NULL) {
			sleep(1);
		}
	}
	if (svc == IO_OBJECT_NULL) {
		printf("sbgpu-test: no StarbiteMali service\n");
		return 1;
	}
	kern_return_t kr = IOServiceOpen(svc, mach_task_self(), 0, &conn);
	IOObjectRelease(svc);
	if (kr != KERN_SUCCESS) {
		printf("sbgpu-test: IOServiceOpen failed: %#x\n", kr);
		return 1;
	}
	uint32_t ver[3] = { 0 };
	call(SBM_VERSION, NULL, 0, ver, sizeof(ver));
	uint64_t p[3] = { 0 };
	const uint32_t which[3] = { 0, 2, 12 };         /* GPU_PROD_ID, SHADER_PRESENT, MMU_FEATURES */
	for (int i = 0; i < 3; i++) {
		struct sbm_get_param g = { .param = which[i] };
		if (call(SBM_GET_PARAM, &g, sizeof(g), &g, sizeof(g)) == KERN_SUCCESS) {
			p[i] = g.value;
		}
	}
	klog("hello from userland pid %d: interface %u.%u.%u, GPU %#llx, shaders %#llx, MMU %#llx",
	    getpid(), ver[0], ver[1], ver[2], p[0], p[1], p[2]);

	struct bo jobs, pix;
	if (bo_new(&jobs, 0x4000) || bo_new(&pix, 64 * 64 * 4)) {
		return 1;
	}
	klog("buffers: jobs handle %u va %#llx cpu %p, pixels handle %u va %#llx cpu %p",
	    jobs.handle, jobs.va, jobs.cpu, pix.handle, pix.va, pix.cpu);

	/* 1. WRITE_VALUE: Job Header (64-bit, type 2, index 1), then address,
	 *    type 7 = immediate 64, value. */
	put32(&jobs, 16, 1u | 2u << 1 | 1u << 16);
	put64(&jobs, 32, jobs.va + 0x100);
	put32(&jobs, 40, 7);
	put64(&jobs, 48, 0x5050414950474253ull);        /* "SBGPUAPP" */
	put64(&jobs, 0x100, 0x1111111111111111ull);
	kr = submit(jobs.va, 0);
	uint64_t got = *(volatile uint64_t *)(jobs.cpu + 0x100);
	klog("WRITE_VALUE from userland: submit %#x, exception %#x, output %#llx -> %s", kr,
	    *(volatile uint32_t *)jobs.cpu, got,
	    got == 0x5050414950474253ull ? "AN APP RAN A GPU JOB" : "failed");

	/* 2. Fragment clear of pix (64x64) to green: fragment job at +0x400,
	 *    Framebuffer at +0x1000, Render Target at +0x1080, sample
	 *    positions at +0x1200 (Mesa genxml v6; stage 3's layout). */
	for (uint32_t off = 0x400; off < 0x2000; off += 4) {
		put32(&jobs, off, 0);
	}
	for (int i = 0; i < 64 * 64; i++) {
		((volatile uint32_t *)pix.cpu)[i] = 0x12345678u;
	}
	put32(&jobs, 0x1200, 128u | 128u << 16);
	put32(&jobs, 0x1200 + 32 * 4, 128u | 128u << 16);
	put64(&jobs, 0x1000 + 16, jobs.va + 0x1200);
	put32(&jobs, 0x1000 + 32, 63u | 63u << 16);
	put32(&jobs, 0x1000 + 40, 63u | 63u << 16);
	put32(&jobs, 0x1000 + 44, 2u << 6 | 8u << 9 | 1u << 24);
	put32(&jobs, 0x1000 + 48, 1u << 16);
	put32(&jobs, 0x1080 + 0, 1u << 26);
	put32(&jobs, 0x1080 + 4, 1u | 19u << 3 | 2u << 10 | 1u << 15 | 0x688u << 16 | 1u << 31);
	put64(&jobs, 0x1080 + 32, pix.va);
	put32(&jobs, 0x1080 + 40, 64 * 4);
	put32(&jobs, 0x1080 + 48, 0xff00ff00u);
	put32(&jobs, 0x400 + 16, 1u | 9u << 1 | 1u << 16);
	put32(&jobs, 0x400 + 36, 3u | 3u << 16);
	put64(&jobs, 0x400 + 40, (jobs.va + 0x1000) | 1);
	kr = submit(jobs.va + 0x400, 1);
	unsigned green = 0;
	for (int i = 0; i < 64 * 64; i++) {
		green += ((volatile uint32_t *)pix.cpu)[i] == 0xff00ff00u;
	}
	klog("FRAGMENT clear from userland: submit %#x, %u of 4096 pixels green -> %s", kr, green,
	    green == 4096 ? "AN APP DREW WITH THE GPU" : "failed");

	uint32_t close[2] = { jobs.handle, 0 };
	call(SBM_GEM_CLOSE, close, sizeof(close), NULL, 0);
	close[0] = pix.handle;
	call(SBM_GEM_CLOSE, close, sizeof(close), NULL, 0);
	IOServiceClose(conn);
	return 0;
}
