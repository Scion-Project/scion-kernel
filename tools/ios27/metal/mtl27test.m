/*
 * mtl27test: Metal smoke test for iOS 27 (24A446) on the Galaxy S9 .
 * Each step prints "MTL27TEST: <n> <name> PASS|FAIL ..." on stdout and /dev/console.
 *  1 kernel     StarbiteMali, IOAcceleratorES (StarbiteAccelerator, boot-arg sbmtl=1), IOCoreSurfaceRoot
 *  2 iosurface  IOSurfaceCreate + CPU write/read
 *  3 device     MTLCreateSystemDefaultDevice() through Metal's own discovery (IOAcceleratorES -> MetalPluginName
 *               -> /System/Library/Extensions/StarbiteMetal.bundle -> HotMTL2Device); --direct: dlopen the bundle
 *               and call -initWithAcceleratorPort: ourselves (tells discovery problems from device problems)
 *  4 clear      render-pass clear of a private texture + blit to a shared buffer, centre pixel checked
 *  5 surface    render-pass clear straight into an IOSurface-backed texture (what CoreAnimation does), CPU check
 *  6 shaders    QuartzCore's default.metallib: up to --shaders N (default 8) vertex/fragment pairs -> pipelines
 *   mtl27test [--delay S] [--direct] [--shaders N] [--lib PATH]
 * Exit 0 when all passed, else the first failed step.
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <IOSurface/IOSurfaceRef.h>
#include <IOKit/IOKitLib.h>
#include <objc/message.h>
#include <dlfcn.h>
#include <pthread.h>
#include <fcntl.h>
#include <stdarg.h>
#include <unistd.h>

#define BUNDLE "/System/Library/Extensions/StarbiteMetal.bundle/StarbiteMetal"
#define QC_LIB "/System/Library/Frameworks/QuartzCore.framework/default.metallib"

static int console = -1, firstFailure;
static volatile int current_step = 1;

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...)
{
	char buf[1200];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(buf, sizeof(buf) - 2, fmt, ap);
	va_end(ap);
	if (n > (int)sizeof(buf) - 2)
		n = sizeof(buf) - 2;
	char line[1300];
	int m = snprintf(line, sizeof(line), "MTL27TEST: %s\n", buf);
	fwrite(line, 1, m, stdout);
	fflush(stdout);
	if (console >= 0)
		write(console, line, m);
}

static void *watchdog(void *arg)
{
	(void)arg;
	int last = 0, seconds = 0;
	for (;;) {
		sleep(15);
		seconds = current_step == last ? seconds + 15 : 15;
		last = current_step;
		say("still in step %d after %d s", last, seconds);
	}
	return NULL;
}

static bool step(int number, const char *name, bool ok, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static bool step(int number, const char *name, bool ok, const char *fmt, ...)
{
	char buf[1000];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	say("%d %s %s %s", number, name, ok ? "PASS" : "FAIL", buf);
	if (!ok && !firstFailure)
		firstFailure = number;
	current_step = number + 1;
	return ok;
}

static io_service_t service(const char *cls)
{
	return IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching(cls));
}

static bool wait_ok(id<MTLCommandBuffer> cb, double *ms)
{
	double t = CFAbsoluteTimeGetCurrent();
	[cb commit];
	[cb waitUntilCompleted];
	*ms = (CFAbsoluteTimeGetCurrent() - t) * 1000;
	if (cb.status != MTLCommandBufferStatusCompleted)
		say("command buffer status %ld error %s", (long)cb.status, cb.error.localizedDescription.UTF8String ?: "-");
	return cb.status == MTLCommandBufferStatusCompleted;
}

static bool near(uint32_t got, uint32_t want)
{
	for (int s = 0; s < 32; s += 8)
		if (abs((int)((got >> s) & 0xff) - (int)((want >> s) & 0xff)) > 1)
			return false;
	return true;
}

int main(int argc, char **argv)
{
	@autoreleasepool {
		bool direct = false;
		int maxShaders = 8;
		const char *libPath = QC_LIB;
		for (int i = 1; i < argc; i++) {
			if (!strcmp(argv[i], "--direct"))
				direct = true;
			else if (!strcmp(argv[i], "--shaders") && i + 1 < argc)
				maxShaders = atoi(argv[++i]);
			else if (!strcmp(argv[i], "--lib") && i + 1 < argc)
				libPath = argv[++i];
			else if (!strcmp(argv[i], "--delay") && i + 1 < argc)
				sleep(atoi(argv[++i]));
		}
		console = open("/dev/console", O_WRONLY | O_NOCTTY | O_NONBLOCK);
		pthread_t wd;
		pthread_create(&wd, NULL, watchdog, NULL);
		pthread_detach(wd);
		NSOperatingSystemVersion v = NSProcessInfo.processInfo.operatingSystemVersion;
		say("start: iOS %ld.%ld.%ld, pid %d uid %d%s", (long)v.majorVersion, (long)v.minorVersion, (long)v.patchVersion,
		    getpid(), getuid(), direct ? ", --direct" : "");

		/* 1 kernel */
		io_service_t mali = service("StarbiteMali"), accel = service("IOAcceleratorES"),
			     surf = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceNameMatching("IOCoreSurfaceRoot"));
		char plugin[128] = "-";
		if (accel) {
			CFTypeRef n = IORegistryEntryCreateCFProperty(accel, CFSTR("MetalPluginName"), NULL, 0);
			if (n && CFGetTypeID(n) == CFStringGetTypeID())
				CFStringGetCString(n, plugin, sizeof(plugin), kCFStringEncodingUTF8);
			if (n)
				CFRelease(n);
		}
		step(1, "kernel", mali && accel && surf, "StarbiteMali %s, IOAcceleratorES %s (MetalPluginName %s), IOCoreSurfaceRoot %s%s",
		     mali ? "yes" : "NO", accel ? "yes" : "NO", plugin, surf ? "yes" : "NO",
		     accel ? "" : " -- boot-arg sbmtl=1 missing?");

		/* 2 iosurface */
		NSDictionary *props = @{ (id)kIOSurfaceWidth : @64, (id)kIOSurfaceHeight : @64, (id)kIOSurfaceBytesPerElement : @4,
					 (id)kIOSurfacePixelFormat : @((uint32_t)'BGRA') };
		IOSurfaceRef probe = IOSurfaceCreate((__bridge CFDictionaryRef)props);
		bool surfOK = false;
		if (probe) {
			IOSurfaceLock(probe, 0, NULL);
			uint32_t *p = (uint32_t *)IOSurfaceGetBaseAddress(probe);
			p[0] = 0x12345678;
			p[IOSurfaceGetBytesPerRow(probe) / 4 * 63] = 0x9abcdef0;
			IOSurfaceUnlock(probe, 0, NULL);
			IOSurfaceLock(probe, kIOSurfaceLockReadOnly, NULL);
			surfOK = p[0] == 0x12345678 && p[IOSurfaceGetBytesPerRow(probe) / 4 * 63] == 0x9abcdef0;
			IOSurfaceUnlock(probe, kIOSurfaceLockReadOnly, NULL);
		}
		step(2, "iosurface", surfOK, "64x64 BGRA %s, id %u, bytes per row %zu", probe ? "made" : "NULL",
		     probe ? IOSurfaceGetID(probe) : 0, probe ? IOSurfaceGetBytesPerRow(probe) : 0);

		/* 3 device */
		double t0 = CFAbsoluteTimeGetCurrent();
		id<MTLDevice> dev = nil;
		char why[300] = "";
		if (direct) {
			void *h = dlopen(BUNDLE, RTLD_NOW);
			Class cls = h ? NSClassFromString(@"HotMTL2Device") : nil;
			if (!h)
				snprintf(why, sizeof(why), "dlopen: %s", dlerror());
			else if (!cls)
				snprintf(why, sizeof(why), "no class HotMTL2Device");
			else
				dev = ((id (*)(id, SEL, mach_port_t))objc_msgSend)([cls alloc], sel_registerName("initWithAcceleratorPort:"),
										    accel);
		} else {
			dev = MTLCreateSystemDefaultDevice();
			if (!dev)
				snprintf(why, sizeof(why), "nil (Metal found no device: not listed in /var/local/mtl2-clients, plugin "
							   "not loadable (arm64 into arm64e?), or no IOAcceleratorES)");
		}
		NSArray *all = direct ? nil : MTLCopyAllDevices();
		if (!step(3, "device", dev != nil, "%s %s in %.0f ms, class %s, registry id %#llx, MTLCopyAllDevices %lu%s%s",
			  direct ? "direct" : "MTLCreateSystemDefaultDevice", dev ? dev.name.UTF8String : "none",
			  (CFAbsoluteTimeGetCurrent() - t0) * 1000, dev ? object_getClassName(dev) : "-",
			  (unsigned long long)dev.registryID, (unsigned long)all.count, why[0] ? ": " : "", why)) {
			say("RESULT FAIL at step %d", firstFailure);
			return firstFailure;
		}

		/* 4 clear */
		id<MTLCommandQueue> queue = [dev newCommandQueue];
		MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
											     width:256 height:256 mipmapped:NO];
		td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
		td.storageMode = MTLStorageModePrivate;
		id<MTLTexture> target = [dev newTextureWithDescriptor:td];
		id<MTLBuffer> back = [dev newBufferWithLength:256 * 256 * 4 options:MTLResourceStorageModeShared];
		bool clearOK = false;
		double ms = 0;
		uint32_t got = 0;
		if (queue && target && back) {
			MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
			pass.colorAttachments[0].texture = target;
			pass.colorAttachments[0].loadAction = MTLLoadActionClear;
			pass.colorAttachments[0].storeAction = MTLStoreActionStore;
			pass.colorAttachments[0].clearColor = MTLClearColorMake(0.25, 0.5, 0.75, 1.0);
			id<MTLCommandBuffer> cb = [queue commandBuffer];
			id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:pass];
			[enc endEncoding];
			id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
			[blit copyFromTexture:target sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
				   sourceSize:MTLSizeMake(256, 256, 1) toBuffer:back destinationOffset:0
			  destinationBytesPerRow:256 * 4 destinationBytesPerImage:256 * 256 * 4];
			[blit endEncoding];
			bool done = wait_ok(cb, &ms);
			got = ((uint32_t *)back.contents)[128 * 256 + 128];
			clearOK = done && near(got, 0xff4080bf);
		}
		step(4, "clear", clearOK, "queue %s texture %s buffer %s; 256x256 clear + readback %.1f ms: centre %#010x (want ~0xff4080bf)",
		     queue ? "ok" : "nil", target ? "ok" : "nil", back ? "ok" : "nil", ms, got);

		/* 5 surface */
		NSDictionary *sp = @{ (id)kIOSurfaceWidth : @828, (id)kIOSurfaceHeight : @1792, (id)kIOSurfaceBytesPerElement : @4,
				      (id)kIOSurfacePixelFormat : @((uint32_t)'BGRA') };
		IOSurfaceRef big = IOSurfaceCreate((__bridge CFDictionaryRef)sp);
		bool surfDraw = false;
		uint32_t s0 = 0, s1 = 0;
		if (big) {
			MTLTextureDescriptor *sd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
												     width:828 height:1792 mipmapped:NO];
			sd.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
			sd.storageMode = MTLStorageModeShared;
			id<MTLTexture> st = [dev newTextureWithDescriptor:sd iosurface:big plane:0];
			if (st) {
				MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
				pass.colorAttachments[0].texture = st;
				pass.colorAttachments[0].loadAction = MTLLoadActionClear;
				pass.colorAttachments[0].storeAction = MTLStoreActionStore;
				pass.colorAttachments[0].clearColor = MTLClearColorMake(1.0, 0.0, 0.5, 1.0);
				id<MTLCommandBuffer> cb = [queue commandBuffer];
				[[cb renderCommandEncoderWithDescriptor:pass] endEncoding];
				bool done = wait_ok(cb, &ms);
				IOSurfaceLock(big, kIOSurfaceLockReadOnly, NULL);
				uint32_t *p = (uint32_t *)IOSurfaceGetBaseAddress(big);
				size_t row = IOSurfaceGetBytesPerRow(big) / 4;
				s0 = p[0];
				s1 = p[row * 1791 + 827];
				IOSurfaceUnlock(big, kIOSurfaceLockReadOnly, NULL);
				surfDraw = done && near(s0, 0xffff0080) && near(s1, 0xffff0080);
			}
			step(5, "surface", surfDraw, "828x1792 IOSurface id %u texture %s, clear %.1f ms: first %#010x last %#010x (want ~0xffff0080)",
			     IOSurfaceGetID(big), st ? "ok" : "nil", ms, s0, s1);
		} else
			step(5, "surface", false, "IOSurfaceCreate 828x1792 failed");

		/* 6 shaders */
		NSError *error = nil;
		if (access(libPath, R_OK) != 0 && !strcmp(libPath, QC_LIB))
			libPath = "/System/Cryptexes/OS" QC_LIB;
		double tl = CFAbsoluteTimeGetCurrent();
		id<MTLLibrary> lib = maxShaders > 0 ? [dev newLibraryWithURL:[NSURL fileURLWithPath:@(libPath)] error:&error] : nil;
		double libMs = (CFAbsoluteTimeGetCurrent() - tl) * 1000;
		unsigned made = 0, refused = 0, pairs = 0;
		double pipeMs = 0;
		NSString *firstRefusal = nil;
		if (lib) {
			NSArray<NSString *> *names = lib.functionNames;
			NSMutableArray *verts = [NSMutableArray new], *frags = [NSMutableArray new];
			for (NSString *n in names) {
				id<MTLFunction> f = [lib newFunctionWithName:n];
				if (f.functionType == MTLFunctionTypeVertex)
					[verts addObject:f];
				else if (f.functionType == MTLFunctionTypeFragment)
					[frags addObject:f];
			}
			say("library %s: %lu functions, %lu vertex, %lu fragment", libPath, (unsigned long)names.count,
			    (unsigned long)verts.count, (unsigned long)frags.count);
			/* QuartzCore's vertex/fragment pairs need its function constants (vry_has_* outputs), so the test
			 * builds its compute kernels instead, each on its own: AIR -> SPIR-V -> Bifrost, no pairing */
			NSMutableArray *kernels = [NSMutableArray new];
			for (NSString *n in names) {
				id<MTLFunction> f = [lib newFunctionWithName:n];
				if (f.functionType == MTLFunctionTypeKernel)
					[kernels addObject:f];
			}
			say("library: %lu compute kernels", (unsigned long)kernels.count);
			for (id<MTLFunction> kf in kernels) {
				if ((int)pairs >= maxShaders)
					break;
				pairs++;
				double tp = CFAbsoluteTimeGetCurrent();
				NSError *pe = nil;
				id<MTLComputePipelineState> ps = [dev newComputePipelineStateWithFunction:kf error:&pe];
				pipeMs += (CFAbsoluteTimeGetCurrent() - tp) * 1000;
				if (ps) {
					made++;
					say("  kernel %s ok (%.0f ms)", kf.name.UTF8String, (CFAbsoluteTimeGetCurrent() - tp) * 1000);
				} else {
					refused++;
					if (!firstRefusal)
						firstRefusal = [NSString stringWithFormat:@"%@: %@", kf.name, pe.localizedDescription];
				}
			}
		}
		if (maxShaders > 0)
			step(6, "shaders", lib && made > 0, "%s (%.0f ms); %u kernels: %u compute pipelines made, %u refused, %.0f ms%s%s",
			     lib ? "loaded" : error.localizedDescription.UTF8String ?: "nil", libMs, pairs, made, refused, pipeMs,
			     firstRefusal ? "; first refusal " : "", firstRefusal ? firstRefusal.UTF8String : "");
		if (firstFailure)
			say("RESULT FAIL at step %d", firstFailure);
		else
			say("RESULT PASS");
		return firstFailure;
	}
}
