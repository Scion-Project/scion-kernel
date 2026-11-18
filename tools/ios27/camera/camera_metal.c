/*
 * SPDX-License-Identifier: MIT-0
 *
 * cameracaptured's Metal device: the trace of it (cam_trace=1) and, with cam_metal=1, cameracaptured made a client of the
 * Metal plug-in.
 *
 * Why (18 November 2026, iOS 27.2.1 24B156's CMCapture, its logic only): -[FigCapturePreviewSinkPipeline
 * _buildVideoPreviewSinkPipeline:...] asks +[FigMetalContext metalDevice] for the device before it builds any node, and
 * makes a command queue on it, and the two Metal dispatch queues. When any of the four is nil it makes a debug assertion
 * (FigDebugAssert3, which says nothing on a release build) and RETURNS 0, success, with no node built and no connection
 * asked for. That is what phone 1 showed with dylibs -21 to -27: the preview sink pipeline "made", its build a success, the
 * source node's Preview(PrimaryScaler) output connected to nothing, a black viewfinder. iOS 16.5's mediaserverd built its
 * preview without Metal (userland/src/inject_media.c even takes the device out of that process); iOS 27's does not.
 *
 * cameracaptured has no Metal device on this root: StarbiteMetal's HotMetal2.mm gives one only to the processes named in
 * /var/local/mtl2-clients, which the root step 80-metal-ca.sh writes as "backboardd". Phone 1's consoles have
 * "MTL2[pid cameracaptured]: plugin loaded" at cameracaptured's start (Metal was asked) and no device after it.
 *
 * The trace (cam_trace=1): +[FigMetalContext metalDevice]'s answer, once and whenever it changes from nil to a device, and
 * the first 80 nodes added to the capture pipelines (-[FigCapturePipeline addNode:error:]: the pipeline's class, the node's
 * class and name, the result), so that a phone boot shows whether the preview pipeline adds its nodes. Also, because phone 1
 * with cam_metal=1 (dylib -28, 18 November) saw cameracaptured abort on an Objective-C exception from an unrecognised
 * selector right after the preview pipeline's "Assistive Overlay" node, with no reason on the console: every unrecognised
 * selector (-/+[NSObject doesNotRecognizeSelector:]: the receiver's class and the selector, and the callers) and the
 * reason of an uncaught Objective-C exception (objc_setUncaughtExceptionHandler, chained to the one before) are said.
 *
 * cam_metal=1 on the boot line (off by default; without it this file changes nothing outside the trace): when the Metal
 * plug-in (/System/Library/Extensions/StarbiteMetal.bundle/StarbiteMetal) is loaded into cameracaptured, its import of
 * fopen() is pointed at a wrapper that, for /var/local/mtl2-clients only, gives the file's text with "cameracaptured" added
 * (a "-cameracaptured" in the file still wins). The file on the phone is left as it is, and no other process is touched.
 * A Metal client wires about 57 MiB of GPU memory when its device is made (docs/ios27/LIQUID-GLASS.md). The same can be
 * had without this switch by the root step: METAL_CA_CLIENTS="backboardd cameracaptured" for 80-metal-ca.sh.
 *
 * NOT RUN ON A PHONE.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#include <mach/mach.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/sysctl.h>

void hakios_camera_say(const char *format, ...) __attribute__((format(printf, 1, 2)));    /* camera_say.c */
void hakios_camera_trace_callers(const char *what, const char *camera);                  /* camera_say.c */
#define say hakios_camera_say

#define PLUGIN          "/StarbiteMetal.bundle/StarbiteMetal"
#define CLIENTS         "/var/local/mtl2-clients"
#define ME              "cameracaptured"

/* ---- cam_metal=1: the plug-in's fopen() of the client list ---- */

static bool
boot_arg(const char *word)
{
	char args[1024] = "";
	size_t n = sizeof(args) - 1;
	size_t len = strlen(word);

	if (sysctlbyname("kern.bootargs", args, &n, NULL, 0) != 0) {
		return false;
	}
	args[sizeof(args) - 1] = 0;
	for (char *w = args; (w = strstr(w, word)) != NULL; w += len) {
		if ((w == args || w[-1] == ' ') && (w[len] == 0 || w[len] == ' ')) {
			return true;
		}
	}
	return false;
}

static FILE *
w_fopen(const char *path, const char *mode)
{
	static char text[2048 + 64];
	static bool said;
	FILE *f;
	size_t n = 0;

	if (path == NULL || (strcmp(path, CLIENTS) != 0 && strcmp(path, "/private" CLIENTS) != 0)) {
		return fopen(path, mode);
	}
	if ((f = fopen(path, "r")) != NULL) {
		n = fread(text, 1, 2048, f);
		fclose(f);
	}
	n += (size_t)snprintf(text + n, sizeof(text) - n, "\n" ME "\n");
	if (!said) {
		said = true;
		say("metal: cam_metal=1: the Metal plug-in reads " CLIENTS " with " ME " added: cameracaptured gets a Metal device");
	}
	return fmemopen(text, n, "r");
}

/* the plug-in's non-lazy and lazy pointers to _fopen, set to w_fopen; returns how many */
static int
point_fopen(const struct mach_header_64 *h, intptr_t slide)
{
	const struct load_command *lc = (const void *)(h + 1);
	const struct segment_command_64 *linkedit = NULL;
	const struct symtab_command *st = NULL;
	const struct dysymtab_command *dst = NULL;
	int done = 0;

	for (uint32_t i = 0; i < h->ncmds; i++, lc = (const void *)((const char *)lc + lc->cmdsize)) {
		if (lc->cmd == LC_SEGMENT_64 && strcmp(((const struct segment_command_64 *)lc)->segname, SEG_LINKEDIT) == 0) {
			linkedit = (const void *)lc;
		} else if (lc->cmd == LC_SYMTAB) {
			st = (const void *)lc;
		} else if (lc->cmd == LC_DYSYMTAB) {
			dst = (const void *)lc;
		}
	}
	if (linkedit == NULL || st == NULL || dst == NULL || dst->nindirectsyms == 0) {
		return -1;
	}
	uintptr_t base = (uintptr_t)slide + linkedit->vmaddr - linkedit->fileoff;
	const struct nlist_64 *syms = (const void *)(base + st->symoff);
	const char *strs = (const char *)(base + st->stroff);
	const uint32_t *indirect = (const void *)(base + dst->indirectsymoff);

	lc = (const void *)(h + 1);
	for (uint32_t i = 0; i < h->ncmds; i++, lc = (const void *)((const char *)lc + lc->cmdsize)) {
		const struct segment_command_64 *seg = (const void *)lc;

		if (lc->cmd != LC_SEGMENT_64 || (strcmp(seg->segname, SEG_DATA) != 0 && strcmp(seg->segname, "__DATA_CONST") != 0)) {
			continue;
		}
		const struct section_64 *sect = (const void *)(seg + 1);
		for (uint32_t s = 0; s < seg->nsects; s++, sect++) {
			uint32_t type = sect->flags & SECTION_TYPE;

			if (type != S_NON_LAZY_SYMBOL_POINTERS && type != S_LAZY_SYMBOL_POINTERS) {
				continue;
			}
			void **slots = (void **)((uintptr_t)slide + sect->addr);
			for (uint64_t k = 0; k < sect->size / sizeof(void *); k++) {
				uint32_t sym = indirect[sect->reserved1 + k];

				if ((sym & (INDIRECT_SYMBOL_LOCAL | INDIRECT_SYMBOL_ABS)) != 0 || sym >= st->nsyms ||
				    strcmp(strs + syms[sym].n_un.n_strx, "_fopen") != 0) {
					continue;
				}
				uintptr_t page = (uintptr_t)&slots[k] & ~(uintptr_t)(PAGE_SIZE - 1);
				if (vm_protect(mach_task_self(), page, PAGE_SIZE, false, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY) !=
				    KERN_SUCCESS) {
					return -2;
				}
				slots[k] = (void *)w_fopen;
				if (strcmp(seg->segname, "__DATA_CONST") == 0) {
					vm_protect(mach_task_self(), page, PAGE_SIZE, false, VM_PROT_READ);
				}
				done++;
			}
		}
	}
	return done;
}

/*
 * With a device, the preview's image queue sink is made, and its -_updateInputRequirements asks VideoToolbox's
 * FigGetAlignmentForIOSurfaceOutputToDisplayService (24B156) for the display's plane base alignment: it looks up the
 * AppleCLCD service's DisplayPipePlaneBaseAlignment and takes PlaneBaseAlignmentLinear OUT OF IT, a dictionary on Apple's
 * display. The kernel's SweetFramebuffer (the fork's iokit/Kernel/arm/SweetFramebuffer.cpp) publishes that property as the
 * number 64, CFDictionaryGetValue() on a number sends it -objectForKey:, and cameracaptured aborted on the unrecognised
 * selector (phone 1, dylib -29, cc-cam29-metal). Here, with cam_metal=1 and in cameracaptured only, NSNumber is given an
 * -objectForKey: that answers nil (a number has no keys): VideoToolbox then keeps its default of 64, which is the value the
 * kernel means. Said once, with its callers. The fix where the cause is would be the kernel publishing the property as
 * Apple's display does, {PlaneBaseAlignmentLinear = 64}; that is not done here (it would change what every process reads).
 */
static id
number_object_for_key(id self, SEL sel, id key)
{
	static bool said;

	if (!said) {
		said = true;
		hakios_camera_trace_callers("metal: cam_metal=1: a number asked -objectForKey: (the display's "
		    "DisplayPipePlaneBaseAlignment is a number here, a dictionary on Apple's display): answered nil", NULL);
		say("metal: cam_metal=1: a number was asked -objectForKey: and answered nil (VideoToolbox's alignment default, 64)");
	}
	return nil;
}

static void
number_keys(void)
{
	static bool done;
	Class c;
	SEL s = sel_registerName("objectForKey:");

	if (done || (c = objc_getClass("NSNumber")) == Nil) {
		return;
	}
	done = true;
	if (class_respondsToSelector(c, s)) {
		say("metal: cam_metal=1: NSNumber already answers -objectForKey:: left alone");
	} else if (!class_addMethod(c, s, (IMP)number_object_for_key, "@@:@")) {
		say("metal: cam_metal=1: NSNumber's -objectForKey: could not be added");
	}
}

static void
metal_image_added(const struct mach_header *header, intptr_t slide)
{
	Dl_info info;
	size_t n, m = strlen(PLUGIN);
	int r;

	number_keys();
	if (header->magic != MH_MAGIC_64 || dladdr(header, &info) == 0 || info.dli_fname == NULL ||
	    (n = strlen(info.dli_fname)) < m || strcmp(info.dli_fname + n - m, PLUGIN) != 0) {
		return;
	}
	r = point_fopen((const struct mach_header_64 *)header, slide);
	say("metal: cam_metal=1: the Metal plug-in is loaded: %s", r > 0 ? "its fopen() of the client list taken" :
	    r == 0 ? "it imports no fopen(): NOT taken, cameracaptured stays without a Metal device" :
	    "its symbol tables or pages could not be read: NOT taken, cameracaptured stays without a Metal device");
}

/* called from camera27.c's constructor in cameracaptured, before Metal is in the process */
void
scioncam_metal_client(void)
{
	if (!boot_arg("cam_metal=1")) {
		return;
	}
	say("metal: cam_metal=1: cameracaptured will be a client of the Metal plug-in (its preview pipeline needs a device)");
	_dyld_register_func_for_add_image(metal_image_added);
}

/* ---- the trace ---- */

static id
call0(id o, const char *sel)
{
	SEL s = sel_registerName(sel);

	return o == nil || !class_respondsToSelector(object_getClass(o), s) ? nil : ((id (*)(id, SEL))objc_msgSend)(o, s);
}

static void
text_of(id o, char *out, size_t cap)
{
	snprintf(out, cap, "-");
	if (o != nil && CFGetTypeID((CFTypeRef)o) == CFStringGetTypeID()) {
		CFStringGetCString((CFStringRef)o, out, (CFIndex)cap, kCFStringEncodingUTF8);
	}
}

static id (*o_metal_device)(id, SEL);
static int last_device = -1;

static id
w_metal_device(id self, SEL sel)
{
	id d = o_metal_device(self, sel);

	if ((d != nil) != last_device) {
		char name[96];

		last_device = d != nil;
		text_of(call0(d, "name"), name, sizeof(name));
		say("trace: metal: +[FigMetalContext metalDevice] -> %s%s", d != nil ? name : "nil",
		    d != nil ? "" : " (the video preview pipeline is then built with no node: a black viewfinder; cam_metal=1)");
	}
	return d;
}

typedef BOOL (*add_node_fn)(id, SEL, id, id *);
static add_node_fn o_add_node;
static int nodes_said;

static BOOL
w_add_node(id self, SEL sel, id node, id *error)
{
	BOOL ok = o_add_node(self, sel, node, error);

	if (nodes_said < 80) {
		char name[96];

		nodes_said++;
		text_of(call0(node, "name"), name, sizeof(name));
		say("trace: metal: %s adds %s \"%s\": %s", object_getClassName(self), node != nil ? object_getClassName(node) : "nil",
		    name, ok ? "yes" : "NO");
	}
	return ok;
}

typedef void (*dnr_fn)(id, SEL, SEL);
static dnr_fn o_dnr_instance, o_dnr_class;
static int dnr_said;

static void
say_unrecognised(id self, SEL sel, bool class_side)
{
	char what[200];

	if (dnr_said++ >= 40) {
		return;
	}
	snprintf(what, sizeof(what), "exception: %c[%s %s] NOT RECOGNISED", class_side ? '+' : '-',
	    self != nil ? object_getClassName(self) : "nil", sel != NULL ? sel_getName(sel) : "?");
	hakios_camera_trace_callers(what, NULL);
}

static void
w_dnr_instance(id self, SEL cmd, SEL sel)
{
	say_unrecognised(self, sel, false);
	o_dnr_instance(self, cmd, sel);
}

static void
w_dnr_class(id self, SEL cmd, SEL sel)
{
	say_unrecognised(self, sel, true);
	o_dnr_class(self, cmd, sel);
}

/* libobjc's (objc/objc-exception.h, not in the iPhoneOS SDK's headers; exported by libobjc on iOS) */
typedef void (*uncaught_fn)(id);
extern uncaught_fn objc_setUncaughtExceptionHandler(uncaught_fn fn);
static uncaught_fn o_uncaught;

static void
w_uncaught(id exception)
{
	char name[96], reason[300];

	text_of(call0(exception, "name"), name, sizeof(name));
	text_of(call0(exception, "reason"), reason, sizeof(reason));
	say("trace: exception: uncaught %s: %s", name, reason);
	if (o_uncaught != NULL) {
		o_uncaught(exception);
	}
}

static void
trace_exceptions(void)
{
	Class ns = objc_getClass("NSObject");
	SEL s = sel_registerName("doesNotRecognizeSelector:");
	Method mi = ns != Nil ? class_getInstanceMethod(ns, s) : NULL, mc = ns != Nil ? class_getClassMethod(ns, s) : NULL;

	if (mi != NULL && mc != NULL) {
		o_dnr_instance = (dnr_fn)method_getImplementation(mi);
		o_dnr_class = (dnr_fn)method_getImplementation(mc);
		method_setImplementation(mi, (IMP)w_dnr_instance);
		method_setImplementation(mc, (IMP)w_dnr_class);
	}
	o_uncaught = objc_setUncaughtExceptionHandler(w_uncaught);
}

/* with the flow trace, once CMCapture is there (idempotent) */
void
scioncam_trace_metal(void)
{
	static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
	static bool done;
	Class mc = objc_getClass("FigMetalContext"), pc = objc_getClass("FigCapturePipeline");

	pthread_mutex_lock(&lock);
	if (!done && mc != Nil && pc != Nil) {
		Method m = class_getClassMethod(mc, sel_registerName("metalDevice"));
		Method a = class_getInstanceMethod(pc, sel_registerName("addNode:error:"));
		char t[8] = "";

		done = true;
		if (m != NULL && method_getNumberOfArguments(m) == 2 && (method_getReturnType(m, t, sizeof(t)), t[0] == '@')) {
			o_metal_device = (id (*)(id, SEL))method_getImplementation(m);
			method_setImplementation(m, (IMP)w_metal_device);
		} else {
			say("trace: metal: +[FigMetalContext metalDevice] is not as on iOS 27.2.1: not traced");
		}
		if (a != NULL && method_getNumberOfArguments(a) == 4) {
			o_add_node = (add_node_fn)method_getImplementation(a);
			method_setImplementation(a, (IMP)w_add_node);
		} else {
			say("trace: metal: -[FigCapturePipeline addNode:error:] is not as on iOS 27.2.1: not traced");
		}
		trace_exceptions();
		say("trace: metal: the Metal device, the pipelines' nodes, unrecognised selectors and uncaught exceptions traced");
	}
	pthread_mutex_unlock(&lock);
}
