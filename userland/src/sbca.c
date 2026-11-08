/*
 * Project Starbite: the library the kernel inserts into backboardd
 * (boot-arg sweetinject=/usr/lib/sbca.dylib,backboardd;
 * xnu patch 0316), where CoreAnimation's window server draws the screen.
 * It makes CoreAnimation render with OpenGL ES on the Mali-G72 (Mesa
 * Panfrost) instead of its CPU renderer.
 *
 * iPadOS 18's AccelServer::renderer() only tries Metal; with no Metal device
 * it returns NULL and the server draws with Server::sw_renderer(). The
 * OpenGL ES renderer (CA::OGL::GLESContext) is still in QuartzCore, and
 * -[CARenderer _initWithEAGLContext:options:] still builds one. So the
 * renderer() slot of the AccelServer vtable is replaced: when the server has
 * no renderer yet, ours builds what renderer() builds for Metal, with a
 * GLESContext over an EAGLContext of API 3 in place of the Metal context,
 * and the original then runs only its per-call tail. The EAGLContext is
 * HakiOS's (tools/ios27/ca-gpu/hakios/eagl.c, MIT), loaded from SBGL: it puts Mesa
 * behind EAGLContext.
 *
 * Boot-arg sbgl=0: load nothing, CoreAnimation keeps its CPU renderer.
 * Log: /private/var/tmp/sbca.log, also backboardd's stderr from here on, where
 * eagl.c writes (the kernel prints its tail before a sweetreboot reset with
 * sweetdumpfile=).
 *   tools/ios27/ca-gpu/build-sbca.sh -> out/ios/libsbca.dylib, out/ios/libsbgl.dylib
 */
#include <IOSurface/IOSurfaceRef.h>
#include <dispatch/dispatch.h>
#include <signal.h>
#include <sys/ucontext.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach-o/dyld.h>
#include <malloc/malloc.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <unistd.h>

#define SBGL "/usr/local/starbite/lib/libsbgl.dylib"

#ifndef SBCA_IOS27
#define SBCA_IOS27 1
#endif
#if SBCA_IOS27
#undef SBCA_IOS26
#define SBCA_IOS26 1    /* iOS 27 behaves as 26 (same flow); only the addresses and offsets differ */
#endif
#ifndef SBCA_IOS26
#define SBCA_IOS26 1
#endif
#if SBCA_IOS27
/*
 * QuartzCore of iOS 27.0.1 (iPhone 11, 24A446), unslid shared cache addresses (numbers only, read from the user's own
 * copy of the restore image). renderer(): ctx server+0x628, renderer
 * server+0x630, MTLDevice display+0x140, renderer display+0x150, context call vtbl+0xf0.
 */
#define QC_ACCEL_SERVER_VTABLE   0x1e8fd77d8UL
#define QC_RENDERER_SLOT         0x1e8fd79b8UL  /* +0x1d0 from the address point */
#define QC_ACCEL_RENDERER        0x184415838UL
#define QC_GLES_CONTEXT_CTOR     0x18466da0cUL
#define QC_MALLOC_ZONE           0x1e709e650UL
#define QC_MALLOC_ZONE_ONCE      0x1e709cf68UL
#define QC_MALLOC_ZONE_INIT      0x1844b62b8UL
#define QC_GLES_CONTEXT_VTABLE   0x1e8fd8250UL
#define QC_FUNCTION_SUPPORTED_AT 0x480
#define QC_FUNCTION_SUPPORTED    0x18471afecUL
#define QC_TEX_FRAGMENT_SHADERS  0x1e05feb80UL
#define IOS_GET_BULK_RETURN      0x0UL          /* iPadOS 18 only */
#define IOS_IVAR_IMPL            0x0UL
#define QC_METAL_WAIT_ONCE       0x1e4062540UL
#define QC_PUSH_SURFACE_FOUND_BR 0x184464104UL
#define PUSH_SURFACE_CBNZ        0xb50001e8u
#define PUSH_SURFACE_B           0x1400000fu
#define QC_FRAMEBUFFER_PIXEL_FORMAT 0x184461838UL
static const uint32_t framebuffer_format_was[3] = { 0xd503237fu, 0x6dbc23e9u, 0xa90157f6u };
static const uint32_t framebuffer_format_bgra[3] = { 0x528a4820u, 0x72a848e0u, 0xd65f03c0u };
#define GLES_CONTEXT_SIZE        0xba0
#define RENDERER_SIZE            0x68
#define RENDERER_BUFFER_SIZE     0x40000
#define SERVER_CONTEXT(s)        (*(void **)((char *)(s) + 0x628))
#define SERVER_RENDERER(s)       (*(void **)((char *)(s) + 0x630))
#define SERVER_DISPLAY(s)        (*(char **)((char *)(s) + 0x60))
#define DISPLAY_RENDERER(d)      (*(void **)((d) + 0x150))
#define CONTEXT_SET_COMPRESSION  0xf0
/*
 * CA::EDRClient::client(): asks backboardd's own libEDR server (com.apple.libEDR.Notification) for a
 * client with EDRCreateClientForDisplayWithTimeout; on the S9 the server never answers and every
 * attempt times out (EDR - transceive_data: mach_msg timed out, every ~5 s), and frames waited for it
 * (one swap per ~5.2 s). "return 0" = no EDR client, as when libEDR is absent.
 */
#define QC_EDR_CLIENT            0x184787f88UL
#elif SBCA_IOS26
/*
 * QuartzCore of iOS 26.6.2 (iPhone 11, 23G90), unslid shared cache addresses; mapped from the
 * iPadOS 18 ones in build/ca-gpu/sbca-ios26-offsets.md (with the evidence for each).
 */
#define QC_ACCEL_SERVER_VTABLE   0x1ee982228UL  /* vtable for CA::WindowServer::AccelServer */
#define QC_RENDERER_SLOT         0x1ee9823e8UL  /* its renderer() entry (+0x1c0) */
#define QC_ACCEL_RENDERER        0x183f37dc0UL  /* CA::WindowServer::AccelServer::renderer() */
#define QC_GLES_CONTEXT_CTOR     0x1840d1c28UL  /* CA::OGL::GLESContext::GLESContext(void *eagl) */
#define QC_MALLOC_ZONE           0x1ecd9a860UL  /* malloc_zone, QuartzCore's x_malloc zone */
#define QC_MALLOC_ZONE_ONCE      0x1ecd98eb8UL  /* x_malloc_get_zone()::once */
#define QC_MALLOC_ZONE_INIT      0x183f9a264UL  /* malloc_zone_init() */
#define QC_GLES_CONTEXT_VTABLE   0x1ee982ac0UL  /* vtable for CA::OGL::GLESContext, + 0x10 (what objects hold) */
#define QC_FUNCTION_SUPPORTED_AT 0x438          /* GLContext::function_supported() const in that vtable */
#define QC_FUNCTION_SUPPORTED    0x18415dd6cUL
/* function_uniform() is inlined into GLContext::update_state on 26: no hook, no c3s numbers. */
#define QC_TEX_FRAGMENT_SHADERS  0x1e61fbad8UL  /* CA::OGL::tex_fragment_shaders: GLSL expression per TextureFunction */
#define IOS_GET_BULK_RETURN      0x1b228f1dcUL  /* IOSurfaceClientGetBulkAttachments, after its memmove */
#define IOS_IVAR_IMPL            0x1eb0284e4UL  /* _OBJC_IVAR_$_IOSurface._impl (int32 offset) */
/*
 * CAMetalContextCreate's dispatch_once predicate: its block waits up to 30 s (300 x 100 ms) for an
 * "IOAcceleratorES" service, called from the AccelServer constructor's direct renderer() call
 * (which the vtable hook does not see). Marked done before that: there is no Metal device anyway.
 */
#define QC_METAL_WAIT_ONCE       0x1e9ccc3d0UL
/*
 * CA::OGL::Context::push_surface asserts that the surface is in the context's tracked list
 * ("found != slist->end ()"). Only the Metal context's surfaces get there (track_surface); the
 * GLES context's display surface (GLESContext::make_buffer_current) and the CPU renderer's
 * AsynchronousNode surfaces never do, so both renderers abort on 26. The not-found branch
 * (cbnz x8, found) becomes an unconditional branch to "found": nothing else reads the list.
 */
#define QC_PUSH_SURFACE_FOUND_BR 0x183f0f1e4UL
#define PUSH_SURFACE_CBNZ        0xb50001e8u    /* cbnz x8, +0x3c */
#define PUSH_SURFACE_B           0x1400000fu    /* b +0x3c */
/*
 * IOMFBDisplay::framebuffer_pixel_format() picks 'b3a8' (30-bit RGB + 8-bit alpha plane) for
 * the iPhone 11's wide-colour panel (with compression on, '&w4a'); eagl.c and the kernel's
 * display only take 'BGRA', so the GPU's frames went nowhere (black screen, GPU boots 1-4).
 * Its first three instructions become "return 'BGRA'".
 */
#define QC_FRAMEBUFFER_PIXEL_FORMAT 0x183ef977cUL
static const uint32_t framebuffer_format_was[3] = { 0xd503237fu, 0x6dbc23e9u, 0xa90157f6u };
static const uint32_t framebuffer_format_bgra[3] = { 0x528a4820u, 0x72a848e0u, 0xd65f03c0u };

/* What renderer() makes, as -[CARenderer _initWithEAGLContext:options:] makes it. */
#define GLES_CONTEXT_SIZE        0xb10
#define RENDERER_SIZE            0x68
#define RENDERER_BUFFER_SIZE     0x40000
#define SERVER_CONTEXT(s)        (*(void **)((char *)(s) + 0x5c8))
#define SERVER_RENDERER(s)       (*(void **)((char *)(s) + 0x5d0))
#define SERVER_DISPLAY(s)        (*(char **)((char *)(s) + 0x60))
#define DISPLAY_RENDERER(d)      (*(void **)((d) + 0x148))  /* +0x138 is now the Metal device: left NULL */
#define CONTEXT_SET_COMPRESSION  0xe0           /* warmup_shaders(bool) on 26, empty for GLES */
#else
/* QuartzCore of iPadOS 18.7.11 (22H384), unslid shared cache addresses. */
#define QC_ACCEL_SERVER_VTABLE   0x1e697c2b8UL  /* vtable for CA::WindowServer::AccelServer */
#define QC_RENDERER_SLOT         0x1e697c450UL  /* its renderer() entry (+0x198) */
#define QC_ACCEL_RENDERER        0x184395468UL  /* CA::WindowServer::AccelServer::renderer() */
#define QC_GLES_CONTEXT_CTOR     0x18450bc84UL  /* CA::OGL::GLESContext::GLESContext(void *eagl) */
#define QC_MALLOC_ZONE           0x1f2a299f0UL  /* malloc_zone, QuartzCore's x_malloc zone */
#define QC_MALLOC_ZONE_ONCE      0x1f2a28048UL  /* x_malloc_get_zone()::once */
#define QC_MALLOC_ZONE_INIT      0x1844186a4UL  /* malloc_zone_init() */
#define QC_GLES_CONTEXT_VTABLE   0x1e6984358UL  /* vtable for CA::OGL::GLESContext, + 0x10 (what objects hold) */
#define QC_FUNCTION_SUPPORTED_AT 0x3d8
#define QC_FUNCTION_SUPPORTED    0x184589794UL  /* CA::OGL::GLContext::function_supported() const, at +0x3d8 */
#define QC_FUNCTION_UNIFORM      0x184589800UL  /* CA::OGL::GLContext::function_uniform(unsigned, ul, ul, const float *), +0x3e8 */
#define QC_TEX_FRAGMENT_SHADERS  0x1e6997ca0UL  /* CA::OGL::tex_fragment_shaders: GLSL expression per TextureFunction */
#define IOS_GET_BULK_RETURN      0x1a6712230UL  /* IOSurfaceClientGetBulkAttachments, after its memmove */
#define IOS_IVAR_IMPL            0x1f4a9e780UL  /* _OBJC_IVAR_$_IOSurface._impl (int32 offset) */

/* What renderer() makes, as -[CARenderer _initWithEAGLContext:options:] makes it. */
#define GLES_CONTEXT_SIZE        0x8b8
#define RENDERER_SIZE            0x68
#define RENDERER_BUFFER_SIZE     0x40000
#define SERVER_CONTEXT(s)        (*(void **)((char *)(s) + 0x400))
#define SERVER_RENDERER(s)       (*(void **)((char *)(s) + 0x408))
#define SERVER_DISPLAY(s)        (*(char **)((char *)(s) + 0x60))
#define DISPLAY_RENDERER(d)      (*(void **)((d) + 0x138))
#define CONTEXT_SET_COMPRESSION  0xb0           /* Context vtable entry: framebuffer compression on/off */
#endif

static int logfd = -1;
static intptr_t slide;

static uint64_t now_ms(void);

/* one line of the log, after the seconds since boot */
static void
say(const char *fmt, ...)
{
	char line[320];
	uint64_t ms = now_ms();
	int n = snprintf(line, sizeof(line), "[%4llu.%03llu] ", (unsigned long long)(ms / 1000),
	    (unsigned long long)(ms % 1000));
	va_list ap;
	va_start(ap, fmt);
	n += vsnprintf(line + n, sizeof(line) - (size_t)n, fmt, ap);
	va_end(ap);
	if (n > (int)sizeof(line) - 1) {
		n = sizeof(line) - 1;
	}
	if (logfd >= 0) {
		write(logfd, line, (size_t)n);
	}
}

static uint64_t
now_ms(void)
{
	static mach_timebase_info_data_t base;
	if (base.denom == 0) {
		mach_timebase_info(&base);
	}
	return mach_absolute_time() * base.numer / base.denom / 1000000;
}

/* The shared cache's slide: QuartzCore's, as every cache image has the same. */
static intptr_t
cache_slide(void)
{
	for (uint32_t i = 0; i < _dyld_image_count(); i++) {
		const char *name = _dyld_get_image_name(i);
		if (name != NULL && strstr(name, "/QuartzCore.framework/QuartzCore") != NULL) {
			return _dyld_get_image_vmaddr_slide(i);
		}
	}
	return -1;
}

/* Replace one entry of a virtual table (unslid address `slot`) that must now hold `expected`. */
static void *
replace_virtual(const char *what, uintptr_t slot, uintptr_t expected, void *replacement)
{
	uintptr_t *entry = (uintptr_t *)(slot + slide);
	vm_address_t page = (vm_address_t)entry & ~(vm_address_t)(vm_page_size - 1);
	void *original;

	if (*entry != expected + slide) {
		say("sbca: %s: found %#lx, expected %#lx; left alone\n", what, *entry - slide, expected);
		return NULL;
	}
	/* the tables are in read-only memory shared by every process: a private writable copy of the page */
	kern_return_t kr = vm_protect(mach_task_self(), page, vm_page_size, FALSE,
	    VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY);
	if (kr != KERN_SUCCESS) {
		say("sbca: %s: cannot make the table writable (%d)\n", what, kr);
		return NULL;
	}
	original = (void *)*entry;
	*entry = (uintptr_t)replacement;
	vm_protect(mach_task_self(), page, vm_page_size, FALSE, VM_PROT_READ);
	return original;
}

/* Replace one instruction of QuartzCore (unslid address) that must now be `expected`. */
extern void sys_icache_invalidate(void *start, size_t len);

static int __attribute__((unused))
patch_instruction(const char *what, uintptr_t address, uint32_t expected, uint32_t replacement)
{
	uint32_t *at = (uint32_t *)(address + slide);
	vm_address_t page = (vm_address_t)at & ~(vm_address_t)(vm_page_size - 1);

	if (*at != expected) {
		say("sbca: %s: found %#x, expected %#x; left alone\n", what, *at, expected);
		return 0;
	}
	kern_return_t kr = vm_protect(mach_task_self(), page, vm_page_size, FALSE,
	    VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY);
	if (kr != KERN_SUCCESS) {
		say("sbca: %s: cannot make the code writable (%d)\n", what, kr);
		return 0;
	}
	*at = replacement;
	kr = vm_protect(mach_task_self(), page, vm_page_size, FALSE, VM_PROT_READ | VM_PROT_EXECUTE);
	sys_icache_invalidate(at, sizeof(*at));
	say("sbca: %s: patched (%#x -> %#x), back to r-x: %d\n", what, expected, replacement, kr);
	return kr == KERN_SUCCESS;
}

/* ---- AccelServer::renderer() ------------------------------------------------------------------ */

static void *(*orig_renderer)(void *server);
static void schedule_shots(void);
static void start_profiler(void);
static mach_port_t render_thread = MACH_PORT_NULL;
static int gles_failed;
static void (*gl_set_context)(void *);    /* eagl.c: CoreAnimation's GL context (iOS 26) */
static unsigned long renderer_calls;
static uint64_t first_call_ms;

/* QuartzCore's own zone, so that what it frees of ours goes back where it came from */
static malloc_zone_t *
ca_zone(void)
{
	long *once = (long *)(QC_MALLOC_ZONE_ONCE + slide);
	if (*once != -1) {
		dispatch_once_f(once, NULL, (dispatch_function_t)(QC_MALLOC_ZONE_INIT + slide));
	}
	return *(malloc_zone_t **)(QC_MALLOC_ZONE + slide);
}

static int
make_gles_renderer(void *server)
{
	Class class = objc_getClass("EAGLContext");
	id eagl;
	malloc_zone_t *zone = ca_zone();
	void *context, **renderer;
	uint64_t start = now_ms();

	if (class == Nil || zone == NULL) {
		say("sbca: no EAGLContext class (%p) or QuartzCore zone (%p)\n", (void *)class, (void *)zone);
		return 0;
	}
	eagl = ((id (*)(Class, SEL))objc_msgSend)(class, sel_registerName("alloc"));
	eagl = ((id (*)(id, SEL, unsigned long, id))objc_msgSend)(eagl, sel_registerName("initWithAPI:properties:"),
	    3, nil);
	if (eagl == nil) {
		say("sbca: no OpenGL ES 3 EAGLContext; the CPU renderer keeps drawing\n");
		return 0;
	}
	say("sbca: EAGLContext %p (API %lu) in %llu ms\n", (void *)eagl,
	    ((unsigned long (*)(id, SEL))objc_msgSend)(eagl, sel_registerName("API")),
	    (unsigned long long)(now_ms() - start));

	if ((context = malloc_zone_calloc(zone, 1, GLES_CONTEXT_SIZE)) == NULL) {
		return 0;
	}
	((void (*)(void *, id))(QC_GLES_CONTEXT_CTOR + slide))(context, eagl);
	say("sbca: CA::OGL::GLESContext %p built (vtable %#lx)\n", context, *(uintptr_t *)context - slide);

	if ((renderer = malloc_zone_malloc(zone, RENDERER_SIZE)) == NULL) {
		return 0;
	}
	memset(renderer, 0, RENDERER_SIZE);
	renderer[0] = context;
	renderer[6] = malloc(RENDERER_BUFFER_SIZE);     /* +0x30 */
	*(uint32_t *)((char *)renderer + 0x60) = 1;
	SERVER_CONTEXT(server) = context;
	SERVER_RENDERER(server) = renderer;

	/* no framebuffer compression: there is no Apple GPU to read it */
	void (**vtable)(void *, int) = *(void (***)(void *, int))context;
	vtable[CONTEXT_SET_COMPRESSION / 8](context, 0);
	DISPLAY_RENDERER(SERVER_DISPLAY(server)) = renderer;
	if (gl_set_context != NULL) {
		gl_set_context(context);
	}
	say("sbca: OpenGL ES renderer %p for the server %p, %llu ms in all\n", (void *)renderer, server,
	    (unsigned long long)(now_ms() - start));
	return 1;
}

static void *
my_renderer(void *server)
{
	if (SERVER_RENDERER(server) == NULL && !gles_failed) {
		if (!make_gles_renderer(server)) {
			gles_failed = 1;
		}
	}
	void *renderer = orig_renderer(server);

	/* a frame asks once: the count is a frame rate (calls 1, 10, 100, 1000, ...) */
	renderer_calls++;
	if (renderer_calls == 1) {
		first_call_ms = now_ms();
		render_thread = mach_thread_self();
		schedule_shots();
		start_profiler();
	}
	for (unsigned long n = 1; n <= renderer_calls; n *= 10) {
		if (n == renderer_calls) {
			uint64_t ms = now_ms() - first_call_ms;
			say("sbca: renderer() call %lu -> %p, %llu ms after the first (%.1f a second)\n", n, renderer,
			    (unsigned long long)ms, ms ? (double)(n - 1) * 1000.0 / (double)ms : 0.0);
			break;
		}
	}
	return renderer;
}

/* ---- CoreAnimation's shader functions -------------------------------------------------------- */

/*
 * CoreAnimation draws a layer with one of its ~106 "functions" (a fragment shader each) and asks the
 * renderer whether it has it; the OpenGL ES renderer lacks many (blurs, vibrancy, colour matrices).
 * eagl.c draws some of those without CoreAnimation's program, with programs of its own, for which
 * it must be handed the function's numbers (the matrix of a vibrant layer...) and told what was
 * refused: HakiOS's inject.c did both from these two entries of the context's vtable. The function
 * is a byte at *(context + 0x10) + 0x10, numbered as in iPadOS 18 (eagl.c follows).
 */
static void (*gl_function_values)(void *, unsigned, unsigned, size_t, size_t, const float *);
static void (*gl_function_refused)(unsigned);
static int (*orig_function_supported)(void *context);
static void (*orig_function_uniform)(void *context, unsigned index, size_t components, size_t count,
    const float *values);

static unsigned
current_function(void *context)
{
	return *(*(uint8_t **)((char *)context + 0x10) + 0x10);
}

/*
 * TextureFunction 20 ("c3s") has no GL source (its tex_fragment_shaders entry is NULL) and
 * function_supported() says no, so whatever needs it is not drawn: Control Center's platters and,
 * it seems, the icons of Settings' rows. Said yes once (boot58), CoreAnimation gave it twelve numbers,
 * a 3x3 colour matrix and a bias (vec3 x 4, the same values its 4x4 neighbour c4b gets for the same
 * filter), and generated "s0 = gl_LastFragData[0]": it works on what is behind. So its entry gets
 * the 3x3-and-bias expression of c3b over premultiplied colour, kept within the alpha, marked
 * "sbc3s" (a comment) so that eagl.c declares the uniform (CoreAnimation declares it only for the
 * functions it has sources for). Boot-arg sbc3s=0: left refused.
 */
#define FUNCTION_C3S 20
static int c3s_given;
static const char c3s_source[] =
    "vec4(min(s0.r * color_matrix[0] + s0.g * color_matrix[1] + s0.b * color_matrix[2] + s0.a * color_matrix[3], "
    "vec3(s0.a)), s0.a) /*sbc3s*/";

static void
give_c3s_a_source(void)
{
	char arguments[1024] = "";
	size_t size = sizeof(arguments) - 1;
	if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) == 0 && strstr(arguments, "sbc3s=0") != NULL) {
		say("sbca: sbc3s=0: function 20 stays refused\n");
		return;
	}
	uintptr_t *table = (uintptr_t *)(QC_TEX_FRAGMENT_SHADERS + slide);
	if (table[FUNCTION_C3S] != 0) {
		say("sbca: tex_fragment_shaders[20] = %#lx: not empty, left alone\n", (unsigned long)table[FUNCTION_C3S]);
		return;
	}
	vm_address_t page = (vm_address_t)&table[FUNCTION_C3S] & ~(vm_address_t)(vm_page_size - 1);
	if (vm_protect(mach_task_self(), page, vm_page_size, FALSE, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY) !=
	    KERN_SUCCESS) {
		say("sbca: tex_fragment_shaders: cannot make it writable\n");
		return;
	}
	table[FUNCTION_C3S] = (uintptr_t)c3s_source;
	vm_protect(mach_task_self(), page, vm_page_size, FALSE, VM_PROT_READ);
	c3s_given = 1;
	say("sbca: function 20 (c3s) gets a source of ours\n");
}

static int
my_function_supported(void *context)
{
	static uint8_t said[256];
	int supported = orig_function_supported(context);
	unsigned function = current_function(context);

	if (!supported && function == FUNCTION_C3S && c3s_given) {
		if (!said[function]) {
			said[function] = 1;
			say("sbca: function 20 (c3s) asked for: said yes\n");
		}
		return 1;
	}

	if (!supported) {
		if (gl_function_refused != NULL) {
			gl_function_refused(function);
		}
		if (!said[function]) {
			said[function] = 1;
			say("sbca: the OpenGL ES renderer has no function %u: what needs it is not drawn\n", function);
		}
	}
	return supported;
}

/*
 * GLContext::function_uniform() sets a function's numbers at a location it looked up for the
 * function's program; for c3s, which it thinks has no source, it looked up none, so they went
 * nowhere and the matrix stayed zero (boot60: Control Center's platters black). After it has bound
 * the program (update_shader), they are set here by name, through Mesa (whose context is current
 * on the render thread).
 */
static int (*mesa_get_uniform_location)(unsigned program, const char *name);
static void (*mesa_get_integerv)(unsigned name, int *value);
static void (*mesa_uniform3fv)(int location, int count, const float *values);
#define GL_CURRENT_PROGRAM_ 0x8b8d

static void
my_function_uniform(void *context, unsigned index, size_t components, size_t count, const float *values)
{
	unsigned function = current_function(context);

	if (gl_function_values != NULL) {
		gl_function_values(context, function, index, components, count, values);
	}
	orig_function_uniform(context, index, components, count, values);
	if (function == FUNCTION_C3S && c3s_given && index == 0 && components == 3 && count <= 4 &&
	    mesa_uniform3fv != NULL) {
		static unsigned said;
		int program = 0;
		mesa_get_integerv(GL_CURRENT_PROGRAM_, &program);
		int location = program != 0 ? mesa_get_uniform_location((unsigned)program, "color_matrix") : -1;
		if (location >= 0) {
			mesa_uniform3fv(location, (int)count, values);
		}
		if (said++ < 2) {
			say("sbca: function 20's numbers set by hand: program %d, color_matrix at %d\n", program, location);
		}
	}
}

/* ---- backtraces (bring-up aid) ------------------------------------------------------------------ */

extern const void *_dyld_get_shared_cache_range(size_t *length);

/* "cache 0x18xxxxxxx" (unslid, for ipsw dsc a2s) or "image+0xoffset (symbol+0xoffset)" */
static void
describe(uintptr_t address, char *out, size_t size)
{
	size_t length = 0;
	uintptr_t cache = (uintptr_t)_dyld_get_shared_cache_range(&length);
	Dl_info info;

	if (cache != 0 && address >= cache && address < cache + length) {
		snprintf(out, size, "cache %#lx", (unsigned long)(address - (uintptr_t)slide));
	} else if (dladdr((const void *)address, &info) != 0 && info.dli_fname != NULL) {
		const char *base = strrchr(info.dli_fname, '/');
		snprintf(out, size, "%s+%#lx (%s+%#lx)", base != NULL ? base + 1 : info.dli_fname,
		    (unsigned long)(address - (uintptr_t)info.dli_fbase), info.dli_sname != NULL ? info.dli_sname : "?",
		    (unsigned long)(address - (uintptr_t)info.dli_saddr));
	} else {
		snprintf(out, size, "%#lx", (unsigned long)address);
	}
}

/* pc, lr and up to 14 return addresses by the frame pointers, read without faulting */
static void
say_backtrace(const char *what, uintptr_t pc, uintptr_t lr, uintptr_t fp)
{
	char place[160];

	describe(pc, place, sizeof(place));
	say("sbca: %s: pc %s\n", what, place);
	describe(lr, place, sizeof(place));
	say("sbca: %s:   lr %s\n", what, place);
	for (unsigned i = 0; i < 14 && fp != 0 && (fp & 7) == 0; i++) {
		uintptr_t frame[2];
		vm_size_t got = 0;
		if (vm_read_overwrite(mach_task_self(), fp, sizeof(frame), (vm_address_t)frame, &got) !=
		    KERN_SUCCESS || got != sizeof(frame) || frame[1] == 0) {
			break;
		}
		describe(frame[1], place, sizeof(place));
		say("sbca: %s:   %2u %s\n", what, i, place);
		fp = frame[0] > fp ? frame[0] : 0;      /* (the chain only goes up the stack) */
	}
}

/*
 * Boot 56: the render loop stopped after five frames, with nothing in any log. Every second this
 * checks that renderer() is still being called; after 3 s without, each thread of backboardd is
 * written down (at most three times a run, and again only after the loop has moved).
 */
static void
watch_render_loop(void *context)
{
	static unsigned long seen;
	static unsigned still, dumps;
	(void)context;

	if (renderer_calls == 0 || renderer_calls != seen) {
		seen = renderer_calls;
		still = 0;
		return;
	}
	if (++still != 3 || dumps >= 3) {
		return;
	}
	dumps++;
	thread_act_array_t threads;
	mach_msg_type_number_t count = 0;
	if (task_threads(mach_task_self(), &threads, &count) != KERN_SUCCESS) {
		return;
	}
	say("sbca: no renderer() call for 3 s (after %lu): %u threads\n", renderer_calls, count);
	for (mach_msg_type_number_t i = 0; i < count; i++) {
		arm_thread_state64_t state;
		mach_msg_type_number_t n = ARM_THREAD_STATE64_COUNT;
		char what[48];
		if (threads[i] != mach_thread_self() &&
		    thread_get_state(threads[i], ARM_THREAD_STATE64, (thread_state_t)&state, &n) == KERN_SUCCESS) {
			snprintf(what, sizeof(what), "thread %u", i);
			say_backtrace(what, (uintptr_t)arm_thread_state64_get_pc(state),
			    (uintptr_t)arm_thread_state64_get_lr(state), (uintptr_t)arm_thread_state64_get_fp(state));
		}
		mach_port_deallocate(mach_task_self(), threads[i]);
	}
	vm_deallocate(mach_task_self(), (vm_address_t)threads, count * sizeof(threads[0]));
}

static void
start_watchdog(void)
{
	dispatch_source_t timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0,
	    dispatch_queue_create("sbca.watchdog", DISPATCH_QUEUE_SERIAL));
	dispatch_source_set_timer(timer, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), NSEC_PER_SEC,
	    NSEC_PER_SEC / 10);
	dispatch_set_context(timer, NULL);
	dispatch_source_set_event_handler_f(timer, watch_render_loop);
	dispatch_resume(timer);
}

/* ---- where the render thread's time goes (bring-up aid) ----------------------------------------- */

/*
 * The user's sessions: ~30 ms of a frame's 30 ms on the CPU (2.4 ms waiting for the GPU), ~0.3 ms
 * a draw. Every millisecond the render thread's pc is read and counted by the function it is in
 * (dladdr: the image and the nearest exported symbol; for shared cache code also the unslid
 * address, for ipsw dsc a2s). Each 5000 samples, the 25 places most often seen are logged, with
 * how many samples were spent waiting in the kernel (a Mach trap: SUBMIT, IOSurface, messages).
 * Boot-arg sbprof=0: off.
 */
#define PROF_SLOTS 2048
static struct prof_slot {
	uintptr_t function;
	uint32_t count;
} prof[PROF_SLOTS];
static unsigned prof_samples, prof_reports, prof_in_kernel;

static void
prof_report(void)
{
	uint32_t total = prof_samples;
	say("sbca: profile %u of %s: %u samples of its %s thread (1 ms apart), %u in system calls (counted at their "
	    "caller), the top places:\n", ++prof_reports, getprogname(), total,
	    strcmp(getprogname(), "backboardd") == 0 ? "render" : "main", prof_in_kernel);
	prof_in_kernel = 0;
	for (unsigned k = 0; k < 25; k++) {
		unsigned best = PROF_SLOTS;
		for (unsigned i = 0; i < PROF_SLOTS; i++) {
			if (prof[i].count != 0 && (best == PROF_SLOTS || prof[i].count > prof[best].count)) {
				best = i;
			}
		}
		if (best == PROF_SLOTS) {
			break;
		}
		char place[160];
		Dl_info info;
		size_t length = 0;
		uintptr_t cache = (uintptr_t)_dyld_get_shared_cache_range(&length), f = prof[best].function;
		if (dladdr((const void *)f, &info) != 0 && info.dli_fname != NULL) {
			const char *base = strrchr(info.dli_fname, '/');
			snprintf(place, sizeof(place), "%s %s", base != NULL ? base + 1 : info.dli_fname,
			    info.dli_sname != NULL ? info.dli_sname : "?");
		} else {
			snprintf(place, sizeof(place), "%#lx", (unsigned long)f);
		}
		say("sbca:   %5.1f%% %s%s%#lx\n", 100.0 * prof[best].count / (total ? total : 1), place,
		    f >= cache && f < cache + length ? " cache " : " +",
		    (unsigned long)(f >= cache && f < cache + length ? f - (uintptr_t)slide :
		    (info.dli_fbase != NULL ? f - (uintptr_t)info.dli_fbase : f)));
		prof[best].count = 0;
	}
	bzero(prof, sizeof(prof));
	prof_samples = 0;
}

static void
prof_sample(void *context)
{
	arm_thread_state64_t state;
	mach_msg_type_number_t n = ARM_THREAD_STATE64_COUNT;
	(void)context;

	if (render_thread == MACH_PORT_NULL ||
	    thread_get_state(render_thread, ARM_THREAD_STATE64, (thread_state_t)&state, &n) != KERN_SUCCESS) {
		return;
	}
	uintptr_t pc = (uintptr_t)arm_thread_state64_get_pc(state);
	Dl_info info;
	/*
	 * In a system call (libsystem_kernel: Mach messages, IOKit calls, waits), the time is the
	 * caller's: counted at the first return address outside libsystem_kernel and IOKit, so that an
	 * idle CoreAnimation server and a GPU submit that waits in the kernel are told apart.
	 */
	if (dladdr((const void *)pc, &info) != 0 && info.dli_fname != NULL &&
	    strstr(info.dli_fname, "libsystem_kernel") != NULL) {
		uintptr_t fp = (uintptr_t)arm_thread_state64_get_fp(state), ret = (uintptr_t)arm_thread_state64_get_lr(state);
		for (unsigned i = 0; i < 12; i++) {
			Dl_info up;
			if (dladdr((const void *)ret, &up) == 0 || up.dli_fname == NULL ||
			    (strstr(up.dli_fname, "libsystem_kernel") == NULL && strstr(up.dli_fname, "/IOKit") == NULL)) {
				pc = ret;
				break;
			}
			uintptr_t frame[2];
			vm_size_t got = 0;
			if (fp == 0 || (fp & 7) != 0 || vm_read_overwrite(mach_task_self(), fp, sizeof(frame),
			    (vm_address_t)frame, &got) != KERN_SUCCESS || got != sizeof(frame)) {
				break;
			}
			ret = frame[1];
			fp = frame[0] > fp ? frame[0] : 0;
		}
		prof_in_kernel++;
	}
	/* by function: the nearest symbol dladdr knows (an exported one; else the address in 256-byte steps) */
	uintptr_t f = dladdr((const void *)pc, &info) != 0 && info.dli_saddr != NULL ? (uintptr_t)info.dli_saddr :
	    (pc & ~(uintptr_t)0xff);
	unsigned h = (unsigned)((f >> 2) * 2654435761u) % PROF_SLOTS;
	for (unsigned probe = 0; probe < 64; probe++, h = (h + 1) % PROF_SLOTS) {
		if (prof[h].function == f || prof[h].count == 0) {
			prof[h].function = f;
			prof[h].count++;
			break;
		}
	}
	if (++prof_samples >= 5000) {
		prof_report();
	}
}

static void
start_profiler(void)
{
	char arguments[1024] = "";
	size_t size = sizeof(arguments) - 1;
	if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) == 0 && strstr(arguments, "sbprof=0") != NULL) {
		return;
	}
	dispatch_source_t timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0,
	    dispatch_queue_create("sbca.profiler", DISPATCH_QUEUE_SERIAL));
	dispatch_source_set_timer(timer, dispatch_time(DISPATCH_TIME_NOW, 0), NSEC_PER_MSEC, NSEC_PER_MSEC / 10);
	dispatch_source_set_event_handler_f(timer, prof_sample);
	dispatch_resume(timer);
	say("sbca: profiling the render thread every 1 ms\n");
}

/* ---- IOSurface clients without a state page (bring-up aid) ----------------------------------- */

/*
 * Boot 46: the first GPU frame crashed in IOSurfaceClientGetBulkAttachments (memmove from
 * NULL + 0x30), CA::WindowServer::IOSurface::wrap_buffer -> CA::Render::iosurface_get_edr_factor:
 * an IOSurfaceClient whose shared state page pointer (+0x78) is NULL. Until it is known which
 * surfaces those are, such a fault is caught here: the client's record is logged, the client is
 * given a zeroed page of its own and the call returns as if the attachments were all zero (EDR
 * factor 1). wrap_buffer keeps the IOSurface in x19, which neither callee touches.
 */
static char zero_state[16384] __attribute__((aligned(16384)));
static unsigned state_faults;

static void
on_fault(int sig, siginfo_t *info, void *context)
{
	ucontext_t *uc = context;
	struct __darwin_arm_thread_state64 *ss = &uc->uc_mcontext->__ss;

	/* (iOS 26: the client's layout is not checked yet; such a fault is only reported below) */
	if (!SBCA_IOS26 && (uintptr_t)info->si_addr == 0x30 && ss->__lr == IOS_GET_BULK_RETURN + slide &&
	    ss->__x[19] != 0) {
		char *surface = (char *)ss->__x[19];
		char *client = *(char **)(surface + *(int32_t *)(IOS_IVAR_IMPL + slide));
		if (client != NULL && *(void **)(client + 0x78) == NULL) {
			uint64_t *q = (uint64_t *)(client + 0x70);
			if (state_faults++ < 16) {
				say("sbca: IOSurface %p client %p has no state page: id %u, %llu x %llu, fmt %#x, size %#llx, "
				    "bpr %#llx; base %#llx times %#llx; header %llx %llx %llx %llx\n",
				    surface, client, *(uint32_t *)(client + 0x88), (unsigned long long)q[5],
				    (unsigned long long)q[6], *(uint32_t *)(client + 0xb8), (unsigned long long)q[4],
				    (unsigned long long)q[7], (unsigned long long)q[0], (unsigned long long)q[2],
				    ((unsigned long long *)client)[0], ((unsigned long long *)client)[1],
				    ((unsigned long long *)client)[2], ((unsigned long long *)client)[13]);
			}
			*(void **)(client + 0x78) = zero_state;
			ss->__x[0] = 0;
			ss->__pc = ss->__lr;
			return;
		}
	}
	char what[64];
	snprintf(what, sizeof(what), "signal %d, address %p", sig, info->si_addr);
	say_backtrace(what, (uintptr_t)ss->__pc, (uintptr_t)ss->__lr, (uintptr_t)ss->__fp);
	signal(sig, SIG_DFL);
}

/* ---- screenshots (bring-up aid) ---------------------------------------------------------------- */

/*
 * Nobody may be looking at the phone during a test boot: 17, 20, 25, 55, 68, 97 and 103 s after
 * the first frame on the panel (with kernel boot-arg sweetswipe=1, whose script counts from the same
 * moment: Control Center over the lock screen opening and open, the applications' page of
 * the home screen, Settings, Control Center over the home screen), every
 * panel-sized BGRA surface backboardd has (the display's buffers) is written to
 * /private/var/tmp/sbca-shot-<s>-<id>-<W>x<H>-<rowbytes>.bgra (raw rows), read off the root after
 * the run (tools/ios27/ca-gpu/fetch-shots.sh). Boot-arg sbshot=0: none.
 */
static void
take_shots(void *context)
{
	unsigned seconds = (unsigned)(uintptr_t)context;

	for (uint32_t id = 1; id < 400; id++) {
		IOSurfaceRef surface = IOSurfaceLookup(id);
		if (surface == NULL) {
			continue;
		}
		size_t width = IOSurfaceGetWidth(surface), height = IOSurfaceGetHeight(surface);
		size_t row = IOSurfaceGetBytesPerRow(surface);
		/* the display's buffers: the canvas (iOS 26: 828x1792), not the panel's own 1440x2960 surface */
		if (width >= 800 && height >= 1000 && height < 2500 && IOSurfaceGetPixelFormat(surface) == 'BGRA' &&
		    IOSurfaceLock(surface, kIOSurfaceLockReadOnly, NULL) == 0) {
			char path[96];
			snprintf(path, sizeof(path), "/private/var/tmp/sbca-shot-%03u-%u-%zux%zu-%zu.bgra", seconds, id, width,
			    height, row);
			int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
			if (fd >= 0) {
				const uint8_t *base = IOSurfaceGetBaseAddress(surface);
				ssize_t wrote = write(fd, base, row * height);
				fcntl(fd, F_FULLFSYNC);  /* (the timed reset does not wait for the buffer cache) */
				close(fd);
				say("sbca: shot %s (%zd bytes)\n", path, wrote);
			}
			IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, NULL);
		}
		CFRelease(surface);
	}
	sync();                         /* and the directory entries (boot52-53 lost the 120 s files) */
}

/*
 * The kernel's scripted input counts from the first frame on the panel (kern.sweet_first_present,
 * mach_absolute_time); backboardd's first renderer() call comes seconds earlier (boot59: the user
 * saw Control Center open twice, between the shots). So the shots count from the same moment: a
 * thread waits for the sysctl (up to 60 s; without it, from now).
 */
static void
wait_and_schedule(void *context)
{
	/* (iOS 26, no scripted input: every 30 s while the user looks around, until the 420 s reset) */
	static const unsigned at[] = { 60, 90, 120, 150, 180, 210, 240, 270, 300, 330, 360 };
	mach_timebase_info_data_t timebase;
	uint64_t first = 0;
	size_t size = sizeof(first);
	(void)context;

	mach_timebase_info(&timebase);
	for (unsigned i = 0; i < 240; i++) {
		if (sysctlbyname("kern.sweet_first_present", &first, &size, NULL, 0) != 0) {
			first = 0;
			break;                  /* (a kernel without it) */
		}
		if (first != 0) {
			break;
		}
		usleep(250000);
	}
	uint64_t now = mach_absolute_time();
	if (first == 0 || first > now) {
		first = now;
	}
	uint64_t since_ns = (now - first) * timebase.numer / timebase.denom;
	say("sbca: shots count from the first frame on the panel, %llu ms ago\n",
	    (unsigned long long)(since_ns / 1000000));
	for (unsigned i = 0; i < sizeof(at) / sizeof(at[0]); i++) {
		int64_t delay = (int64_t)at[i] * NSEC_PER_SEC - (int64_t)since_ns;
		dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, delay > 0 ? delay : 0),
		    dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), (void *)(uintptr_t)at[i], take_shots);
	}
}

static void
schedule_shots(void)
{
	char arguments[1024] = "";
	size_t size = sizeof(arguments) - 1;
	if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) == 0 && strstr(arguments, "sbshot=0") != NULL) {
		return;
	}
	dispatch_async_f(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), NULL, wait_and_schedule);
}

/* ---- loading ---------------------------------------------------------------------------------- */


static id
empty_interfaces(id self, SEL _cmd)
{
	(void)self; (void)_cmd;
	return ((id (*)(id, SEL))objc_msgSend)((id)objc_getClass("NSArray"), sel_registerName("array"));
}

static void
no_ethernet_watcher(void)
{
	Class c = objc_getClass("PSSystemConfigurationDynamicStoreEthernetWatcher");
	Method m = c != Nil ? class_getInstanceMethod(c, sel_registerName("ethernetNetworkInterfaces")) : NULL;
	if (m == NULL) {
		say("sbca: PSSystemConfigurationDynamicStoreEthernetWatcher not found; left alone\n");
		return;
	}
	method_setImplementation(m, (IMP)empty_interfaces);
	say("sbca: -[PSSystemConfigurationDynamicStoreEthernetWatcher ethernetNetworkInterfaces] -> empty\n");
}

static int
gl_wanted(void)
{
	char arguments[1024] = "";
	size_t size = sizeof(arguments) - 1;
	if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) == 0) {
		for (const char *a = arguments; (a = strstr(a, "sbgl=")) != NULL; a += 5) {
			if (a == arguments || a[-1] == ' ') {
				return a[5] != '0';
			}
		}
	}
	return 1;
}

__attribute__((constructor))
static void
sbca_init(void)
{
	/* one log a boot: a file older than the kernel's boot time is last boot's */
	struct timeval boottime = { 0 };
	size_t boottime_size = sizeof(boottime);
	struct stat old;
	char logname[96] = "/private/var/tmp/sbca.log";
	if (strcmp(getprogname(), "backboardd") != 0) {
		snprintf(logname, sizeof(logname), "/private/var/tmp/sbca-%s.log", getprogname());
	}
	int trunc = sysctlbyname("kern.boottime", &boottime, &boottime_size, NULL, 0) == 0 &&
	    stat(logname, &old) == 0 && old.st_mtime < boottime.tv_sec ? O_TRUNC : 0;
	logfd = open(logname, O_WRONLY | O_CREAT | O_APPEND | trunc, 0644);
	char name[64] = "";
	uint32_t size = sizeof(name);
	_NSGetExecutablePath(name, &size);
	say("sbca: loaded into %s pid %d\n", name, getpid());
	if ((slide = cache_slide()) == -1) {
		say("sbca: QuartzCore not loaded yet\n");
		return;
	}
	/*
	 * Settings (sweetinject=...,Preferences): on the S9 its main thread blocked at launch in
	 * -[PSSystemConfigurationDynamicStoreEthernetWatcher ethernetNetworkInterfaces] (a
	 * SystemConfiguration round trip to configd that does not come back) until FrontBoard's
	 * watchdog SIGKILLed it (k4 boot: "CRASH Preferences sig 9", bt in _waitForMachMessage).
	 * There is no Ethernet here: the method answers an empty list.
	 */
	if (strcmp(getprogname(), "Preferences") == 0) {
		no_ethernet_watcher();
	}
	/* in an app (sweetinject=...,SpringBoard,Preferences): only where its main thread's time goes */
	if (strcmp(getprogname(), "backboardd") != 0) {
		render_thread = mach_thread_self();
		start_profiler();
		return;
	}
#if SBCA_IOS27
	if (!getenv("SBCA_EDR")) {
		patch_instruction("EDRClient::client -> 0 (ret)", QC_EDR_CLIENT + 4, 0xd10143ffu, 0xd65f03c0u);
		patch_instruction("EDRClient::client -> 0 (mov w0, #0)", QC_EDR_CLIENT, 0xd503237fu, 0x52800000u);
	}
#endif
#if SBCA_IOS26
	/* both renderers need it, so before sbgl=0 is looked at */
	patch_instruction("Context::push_surface tracked-list assert", QC_PUSH_SURFACE_FOUND_BR, PUSH_SURFACE_CBNZ,
	    PUSH_SURFACE_B);
#endif
#if SBCA_IOS26
	/*
	 * Both renderers too: the panel takes only 'BGRA', and metal2vulkan has no 'b3a8' (metal1:
	 * "refused render pipeline: Unsupported(Format)" -> MetalContext::create_pipeline_state abort).
	 * (The last instruction first, so the function is never half patched.)
	 */
	for (int i = 2; i >= 0; i--) {
		patch_instruction("IOMFBDisplay::framebuffer_pixel_format -> 'BGRA'", QC_FRAMEBUFFER_PIXEL_FORMAT + 4 * (unsigned)i,
		    framebuffer_format_was[i], framebuffer_format_bgra[i]);
	}
#endif
	if (!gl_wanted()) {
		/* (with sbmtl=1 and backboardd in /var/local/mtl2-clients: its Metal renderer) */
		say("sbca: sbgl=0: CoreAnimation keeps its own renderer (Metal or CPU)\n");
		schedule_shots();
		return;
	}
	/* eagl.c says what it does on stderr */
	if (logfd >= 0) {
		dup2(logfd, 2);
	}
	/*
	 * EAGLContext must be there when SBGL's constructor replaces its methods. SBGL does not
	 * link OpenGLES itself: its gl* symbols are to bind to Mesa, not to Apple's.
	 */
	uint64_t start = now_ms();
	if (dlopen("/System/Library/Frameworks/OpenGLES.framework/OpenGLES", RTLD_NOW) == NULL) {
		say("sbca: OpenGLES: %s\n", dlerror());
	}
	void *sbgl = dlopen(SBGL, RTLD_NOW);
	if (sbgl == NULL) {
		say("sbca: %s: %s\n", SBGL, dlerror());
		return;
	}
	gl_function_refused = (void (*)(unsigned))dlsym(sbgl, "hakios_gl_function_refused");
#if SBCA_IOS26
	/* (no function_uniform to hook: eagl.c reads the numbers from the context at each draw) */
	gl_set_context = (void (*)(void *))dlsym(sbgl, "hakios_gl_set_context");
#else
	gl_function_values = (void (*)(void *, unsigned, unsigned, size_t, size_t, const float *))dlsym(sbgl,
	    "hakios_gl_function_values");
#endif
	void *mesa = dlopen("/usr/local/starbite/lib/libOSMesa.8.dylib", RTLD_NOW | RTLD_NOLOAD);
	if (mesa != NULL) {
		mesa_get_uniform_location = (int (*)(unsigned, const char *))dlsym(mesa, "glGetUniformLocation");
		mesa_get_integerv = (void (*)(unsigned, int *))dlsym(mesa, "glGetIntegerv");
		mesa_uniform3fv = (void (*)(int, int, const float *))dlsym(mesa, "glUniform3fv");
	}
	if (mesa_get_uniform_location == NULL || mesa_get_integerv == NULL) {
		mesa_uniform3fv = NULL;
	}
	say("sbca: %s loaded in %llu ms\n", SBGL, (unsigned long long)(now_ms() - start));
	struct sigaction sa = { 0 };
	sa.sa_sigaction = on_fault;
	sa.sa_flags = SA_SIGINFO;
	sigaction(SIGSEGV, &sa, NULL);
	sigaction(SIGBUS, &sa, NULL);
#if SBCA_IOS26
	long *metal_wait = (long *)(QC_METAL_WAIT_ONCE + slide);
	say("sbca: CAMetalContextCreate's 30 s wait: predicate %ld, marked done\n", *metal_wait);
	*metal_wait = -1;
#endif
	orig_renderer = replace_virtual("AccelServer::renderer", QC_RENDERER_SLOT, QC_ACCEL_RENDERER, my_renderer);
	orig_function_supported = replace_virtual("GLESContext::function_supported",
	    QC_GLES_CONTEXT_VTABLE + QC_FUNCTION_SUPPORTED_AT, QC_FUNCTION_SUPPORTED, my_function_supported);
#if !SBCA_IOS26
	orig_function_uniform = replace_virtual("GLESContext::function_uniform", QC_GLES_CONTEXT_VTABLE + 0x3e8,
	    QC_FUNCTION_UNIFORM, my_function_uniform);
#endif
	give_c3s_a_source();
	start_watchdog();
	say("sbca: renderer hook %s, function_supported hook %s\n", orig_renderer ? "installed" : "NOT installed",
	    orig_function_supported ? "installed" : "NOT installed");
}
