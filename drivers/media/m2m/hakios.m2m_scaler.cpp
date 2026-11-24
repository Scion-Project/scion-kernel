/* SPDX-License-Identifier: MIT-0 */
/*
 * AppleM2ScalerCSCDriver on the Galaxy S9: the memory-to-memory scaler and colour-space converter that iOS 27's
 * IOSurfaceAccelerator.framework opens, done by the CPU (hakios.m2m_core.h) in the caller's thread. docs/ios27/
 * M2M-SCALER.md has the interface (read from iOS 27.2.1's files: names and numbers only) and why it is the CPU for now.
 *
 * Who asks: VideoToolbox's hardware rotation chain (VTPixelRotationSession, the camera preview's stitcher among its
 * users) and CMCapture's FigM2MController, whose +maxUpscale is this service's IOSurfaceAcceleratorCapabilitiesDict.
 * With no such service the stitcher's up-scale limit is 0 and the viewfinder stays black (CAMERA.md).
 *
 * The service: an IOService of that class name on IOResources (the fork's gIOKernelConfigTables), its registry property
 * IOSurfaceAcceleratorCapabilitiesDict made from m2m_caps (what the core does, nothing more), and a user client for
 * IOServiceOpen type 0:
 *   sel 1   transform: struct in 0x1b0 bytes (the source and destination surface ids, a completion callback, option bits,
 *           a source rectangle in 16.16, a destination rectangle): done synchronously, kIOReturnSuccess when the
 *           destination holds the picture; kIOReturnUnsupported for anything the core does not do (a format, a filter
 *           of the caller's own, border fill, HDR, shared events, a completion callback ...), so the caller falls back
 *   sel 2/3 abort transfers / captures: nothing is ever queued: success
 *   sel 5   an id for the client (scalar out)
 *   sel 10  a property (struct in 0x18): accepted and kept
 *   other   kIOReturnUnsupported
 * Boot argument m2m=0: the service is not published (iOS then has no M2M scaler, as before).
 */
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#include <pexpert/pexpert.h>
#include <kern/clock.h>
#include <sys/proc.h>

#include "arm/SweetSurface.h"

#if defined(SWEET)
#include "hakios.m2m_core.h"

#define M2M_LOG "hakios: m2m: "

enum {
	kM2MSelTransform        = 1,
	kM2MSelAbortTransfers   = 2,
	kM2MSelAbortCaptures    = 3,
	kM2MSelGetID            = 5,
	kM2MSelSetProperty      = 10,
};

#define M2M_TRANSFORM_SIZE      0x1b0

/* The option bits (+0x20 of the transform) the core does, and the ones whose meaning changes the picture and that it does
 * not do (refused). Everything else is a hint (speed, dither of an 8-bit output that has nothing to dither, chroma
 * location of the source, which the core takes as 4:2:0 video's own) and is let pass. */
#define M2M_BIT_TRANSFORM       0x1fULL                 /* 'Transform': bits 0-2 known, 3-4 not */
#define M2M_BIT_CUSTOM_FILTER   (1ULL << 6)
#define M2M_BIT_NEAREST         (1ULL << 7)
#define M2M_BIT_BORDER_FILL     (1ULL << 28)
#define M2M_BIT_ZERO_FILL       (1ULL << 30)
#define M2M_REFUSED_BITS        (M2M_BIT_CUSTOM_FILTER | M2M_BIT_BORDER_FILL | (7ULL << 36) /* colour remap */ | \
	                         (1ULL << 40) /* HDR */ | (1ULL << 41) /* enhance */ | (1ULL << 42) /* noise estimate */ | \
	                         (1ULL << 43) /* read only */ | (1ULL << 49) /* write-only mode */ | \
	                         (1ULL << 50) /* film grain */ | (1ULL << 56) /* odd-size border fill */ | \
	                         (1ULL << 58) /* border fill of compressed formats */)

static uint32_t m2m_transfers, m2m_refusals, m2m_clients;

static uint32_t
m2m_get32(const uint8_t *p, unsigned off)
{
	uint32_t v;
	memcpy(&v, p + off, sizeof(v));
	return v;
}

static uint64_t
m2m_get64(const uint8_t *p, unsigned off)
{
	uint64_t v;
	memcpy(&v, p + off, sizeof(v));
	return v;
}

static void
m2m_fourcc(uint32_t f, char out[5])
{
	for (int i = 0; i < 4; i++) {
		char c = (char)(f >> (24 - 8 * i));
		out[i] = c >= 32 && c < 127 ? c : '?';
	}
	out[4] = 0;
}

/* The capabilities, as IOSurfaceAccelerator's readers look for them (FigM2MController needs every one of its keys, or it
 * stops at the first missing one and keeps zeros; VideoToolbox inverts the down-scale ratios it reads). */
static OSDictionary *
m2m_caps_dict(void)
{
	struct m2m_caps c;
	OSDictionary *d = OSDictionary::withCapacity(24);

	if (d == NULL) {
		return NULL;
	}
	m2m_caps_get(&c);
	const struct {
		const char *key;
		uint32_t value;
	} n[] = {
		{ "IOSurfaceAcceleratorFilterHorizontalTapsCount", c.taps },
		{ "IOSurfaceAcceleratorFilterVerticalTapsCount", c.taps },
		{ "IOSurfaceAcceleratorFilterHorizontalPhasesCount", c.phases },
		{ "IOSurfaceAcceleratorFilterVerticalPhasesCount", c.phases },
		{ "IOSurfaceAcceleratorFilterCoefficientsPrePointBits", c.coef_pre_bits },
		{ "IOSurfaceAcceleratorFilterCoefficientsPostPointBits", c.coef_post_bits },
		{ "IOSurfaceAcceleratorCapabilitiesMaxHUpscale", c.max_upscale },
		{ "IOSurfaceAcceleratorCapabilitiesMaxVUpscale", c.max_upscale },
		{ "IOSurfaceAcceleratorCapabilitiesMaxHDownscale", c.max_downscale },
		{ "IOSurfaceAcceleratorCapabilitiesMaxVDownscale", c.max_downscale },
		{ "IOSurfaceAcceleratorCapabilitiesSymmetricScaling", 0 },
		{ "IOSurfaceAcceleratorCapabilitiesMinSourceWidth", c.min_size },
		{ "IOSurfaceAcceleratorCapabilitiesMinSourceHeight", c.min_size },
		{ "IOSurfaceAcceleratorCapabilitiesMinDestWidth", c.min_size },
		{ "IOSurfaceAcceleratorCapabilitiesMinDestHeight", c.min_size },
		{ "IOSurfaceAcceleratorCapabilitiesMaxSourceWidth", c.max_size },
		{ "IOSurfaceAcceleratorCapabilitiesMaxSourceHeight", c.max_size },
		{ "IOSurfaceAcceleratorCapabilitiesMaxDestWidth", c.max_size },
		{ "IOSurfaceAcceleratorCapabilitiesMaxDestHeight", c.max_size },
		{ "IOSurfaceAcceleratorCapabilitiesFractionalDimensions", c.fractional },
		{ "IOSurfaceAcceleratorFormatIn2Planes42010", c.ten_bit },
		{ "IOSurfaceAcceleratorFormatOut2Planes42010", c.ten_bit },
	};
	for (size_t i = 0; i < sizeof(n) / sizeof(n[0]); i++) {
		OSNumber *v = OSNumber::withNumber(n[i].value, 32);
		if (v != NULL) {
			d->setObject(n[i].key, v);
			v->release();
		}
	}
	return d;
}

class AppleM2ScalerCSCDriver : public IOService {
	OSDeclareDefaultStructors(AppleM2ScalerCSCDriver);

public:
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(AppleM2ScalerCSCDriver, IOService);

bool
AppleM2ScalerCSCDriver::start(IOService *provider)
{
	uint32_t on = 1;

	(void)PE_parse_boot_argn("m2m", &on, sizeof(on));
	if (on == 0) {
		static bool said;
		if (!said) {
			said = true;
			IOLog(M2M_LOG "m2m=0: no memory-to-memory scaler published\n");
		}
		return false;
	}
	if (!IOService::start(provider)) {
		return false;
	}
	OSDictionary *caps = m2m_caps_dict();
	if (caps == NULL) {
		return false;
	}
	setProperty("IOSurfaceAcceleratorCapabilitiesDict", caps);
	caps->release();
	setProperty(kIOUserClientClassKey, "HakiosM2MScalerUserClient");
	registerService();
	IOLog(M2M_LOG "AppleM2ScalerCSCDriver registered: the CPU's scaler, '420v' '420f' 'x420' 'xf20' in and out, "
	    "up %ux down %ux, rotation and flips, 256-phase bilinear, box when down\n", M2M_MAX_UPSCALE, M2M_MAX_DOWNSCALE);
	return true;
}

class HakiosM2MScalerUserClient : public IOUserClient {
	OSDeclareDefaultStructors(HakiosM2MScalerUserClient);

	uint32_t fID;
	uint32_t fTransfers;            /* this client's, for the log */
	uint8_t  fProperty[0x18];

	IOReturn transform(IOExternalMethodArguments *args);

public:
	bool initWithTask(task_t owningTask, void *securityID, UInt32 type) APPLE_KEXT_OVERRIDE;
	IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	IOReturn registerNotificationPort(mach_port_t port, UInt32 type, io_user_reference_t refCon) APPLE_KEXT_OVERRIDE;
	IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args, IOExternalMethodDispatch *dispatch,
	    OSObject *target, void *reference) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(HakiosM2MScalerUserClient, IOUserClient);

bool
HakiosM2MScalerUserClient::initWithTask(task_t owningTask, void *securityID, UInt32 type)
{
	if (type != 0 || !IOUserClient::initWithTask(owningTask, securityID, type)) {
		return false;
	}
	fID = OSIncrementAtomic((volatile SInt32 *)&m2m_clients) + 1;
	bzero(fProperty, sizeof(fProperty));
	fTransfers = 0;
	if (fID <= 32) {
		IOLog(M2M_LOG "client %u opened (pid %d)\n", fID, proc_selfpid());
	}
	return true;
}

IOReturn
HakiosM2MScalerUserClient::clientClose(void)
{
	terminate();
	return kIOReturnSuccess;
}

IOReturn
HakiosM2MScalerUserClient::registerNotificationPort(mach_port_t, UInt32, io_user_reference_t)
{
	/* the library asks for a run loop source; nothing is ever completed through it (every transfer is synchronous) */
	return kIOReturnSuccess;
}

/* A surface's two planes as the core's picture; false if the surface is not one the core can read or write. */
static bool
m2m_image_of(SweetSurface *s, struct m2m_image *im, uintptr_t *base)
{
	unsigned bits, full;

	if (s == NULL || s->memory == NULL || s->snapSize != 0 || s->planeCount != 2 ||
	    !m2m_format_info(s->pixelFormat, &bits, &full)) {
		return false;
	}
	*base = s->kernelAddress();
	if (*base == 0) {
		return false;
	}
	uint64_t length = s->memory->getLength();
	bzero(im, sizeof(*im));
	im->format = s->pixelFormat;
	im->width = s->width;
	im->height = s->height;
	for (unsigned i = 0; i < 2; i++) {
		const struct sweet_surface_plane *p = &s->planes[i];
		if ((uint64_t)p->offset + p->size > length) {
			return false;
		}
		im->plane[i].base = (uint8_t *)*base + p->offset;
		im->plane[i].size = p->size;
		im->plane[i].bytes_per_row = p->bytesPerRow;
		im->plane[i].width = p->width;
		im->plane[i].height = p->height;
	}
	return true;
}

static IOReturn
m2m_refuse(uint32_t n, const char *why, uint64_t value)
{
	uint32_t r = OSIncrementAtomic((volatile SInt32 *)&m2m_refusals) + 1;

	if (r <= 20 || (r % 1000) == 0) {
		IOLog(M2M_LOG "transfer %u refused: %s (%#llx); %u refused so far\n", n, why, value, r);
	}
	return kIOReturnUnsupported;
}

IOReturn
HakiosM2MScalerUserClient::transform(IOExternalMethodArguments *args)
{
	const uint8_t *t = (const uint8_t *)args->structureInput;
	uint32_t n = OSIncrementAtomic((volatile SInt32 *)&m2m_transfers) + 1;

	if (t == NULL || args->structureInputSize != M2M_TRANSFORM_SIZE) {
		return m2m_refuse(n, "struct size", args->structureInputSize);
	}
	uint64_t bits = m2m_get64(t, 0x20);
	if (m2m_get64(t, 0x08) != 0) {
		return m2m_refuse(n, "a completion callback (asynchronous)", m2m_get64(t, 0x08));
	}
	if (bits & M2M_REFUSED_BITS) {
		return m2m_refuse(n, "an option it does not do", bits & M2M_REFUSED_BITS);
	}
	if ((bits & M2M_BIT_TRANSFORM) & ~(uint64_t)M2M_XF_ALL) {
		return m2m_refuse(n, "a Transform it does not know", bits & M2M_BIT_TRANSFORM);
	}
	if (m2m_get32(t, 0x78) != 0 || m2m_get32(t, 0x90) != 0) {
		return m2m_refuse(n, "shared events", ((uint64_t)m2m_get32(t, 0x78) << 32) | m2m_get32(t, 0x90));
	}
	for (unsigned i = 0; i < 4; i++) {
		if (m2m_get32(t, 0x110 + 0x28 * i) != 0) {
			return m2m_refuse(n, "a colour-processing block", m2m_get32(t, 0x110 + 0x28 * i));
		}
	}

	uint32_t src_id = m2m_get32(t, 0x00), dst_id = m2m_get32(t, 0x04);
	SweetSurface *src = sweet_surface_copy(src_id), *dst = sweet_surface_copy(dst_id);
	struct m2m_job j;
	uintptr_t sbase = 0, dbase = 0;
	IOReturn ret = kIOReturnSuccess;

	bzero(&j, sizeof(j));
	if (src == NULL || dst == NULL) {
		ret = m2m_refuse(n, "no such surface", ((uint64_t)src_id << 32) | dst_id);
		goto out;
	}
	if (!m2m_image_of(src, &j.src, &sbase) || !m2m_image_of(dst, &j.dst, &dbase)) {
		ret = m2m_refuse(n, "a surface it cannot use (format, planes or memory)",
		    ((uint64_t)src->pixelFormat << 32) | dst->pixelFormat);
		goto out;
	}
	j.sx = m2m_get64(t, 0x28);
	j.sy = m2m_get64(t, 0x30);
	j.sw = m2m_get64(t, 0x38);
	j.sh = m2m_get64(t, 0x40);
	j.dx = m2m_get32(t, 0x60);
	j.dy = m2m_get32(t, 0x64);
	j.dw = m2m_get32(t, 0x68);
	j.dh = m2m_get32(t, 0x6c);
	j.transform = (uint32_t)(bits & M2M_XF_ALL);
	j.options = ((bits & M2M_BIT_ZERO_FILL) ? M2M_OPT_ZERO_FILL : 0) | ((bits & M2M_BIT_NEAREST) ? M2M_OPT_NEAREST : 0);
	{
		/* rectangles past 2^31 are nonsense; the core's arithmetic is checked for what m2m_check lets through */
		if ((j.sx | j.sy | j.sw | j.sh) >> 46) {
			ret = m2m_refuse(n, "a source rectangle out of range", j.sx | j.sy | j.sw | j.sh);
			goto out;
		}
		int e = m2m_check(&j);
		if (e != M2M_OK) {
			ret = m2m_refuse(n, e == M2M_ESCALE ? "a scale past the published limits" : e == M2M_ERECT ?
			    "a rectangle outside its surface, too small, or an odd destination origin" : e == M2M_EPLANE ?
			    "a surface's planes too small for it" : e == M2M_EFORMAT ? "a format" : "a Transform", (uint64_t)e);
			goto out;
		}
	}
	{
		uint64_t t0 = mach_absolute_time(), t1, t2, ns = 0, ns_run = 0;
		uint32_t mine = ++fTransfers;
		/* the source as memory has it (another writer's cache lines out), the destination there for its readers */
		(void)src->memory->performOperation(kIOMemoryIncoherentIOFlush, 0, src->memory->getLength());
		t1 = mach_absolute_time();
		m2m_run(&j);
		t2 = mach_absolute_time();
		(void)dst->memory->performOperation(kIOMemoryIncoherentIOStore, 0, dst->memory->getLength());
		absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
		absolutetime_to_nanoseconds(t2 - t1, &ns_run);
		if (mine <= 2 || mine == 30 || mine == 300 || (mine % 3000) == 0 || n == 30 || n == 300) {
			char sf[5], df[5];
			m2m_fourcc(j.src.format, sf);
			m2m_fourcc(j.dst.format, df);
			IOLog(M2M_LOG "transfer %u (client %u's %u, pid %d): surface %u %ux%u '%s' rect %llu.%04llu,%llu.%04llu "
			    "%llu.%04llu x %llu.%04llu -> surface %u %ux%u '%s' rect %u,%u %ux%u, transform %u, options %#llx: done in %llu us "
			    "(pixels %llu, caches %llu)\n", n, fID, mine, proc_selfpid(),
			    src_id, j.src.width, j.src.height, sf, j.sx >> 16, ((j.sx & 0xffff) * 10000) >> 16, j.sy >> 16,
			    ((j.sy & 0xffff) * 10000) >> 16, j.sw >> 16, ((j.sw & 0xffff) * 10000) >> 16, j.sh >> 16,
			    ((j.sh & 0xffff) * 10000) >> 16, dst_id, j.dst.width, j.dst.height, df, j.dx, j.dy, j.dw, j.dh, j.transform,
			    bits, ns / 1000, ns_run / 1000, (ns - ns_run) / 1000);
		}
	}
out:
	OSSafeReleaseNULL(src);
	OSSafeReleaseNULL(dst);
	return ret;
}

IOReturn
HakiosM2MScalerUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args,
    __unused IOExternalMethodDispatch *dispatch, __unused OSObject *target, __unused void *reference)
{
	switch (selector) {
	case kM2MSelTransform:
		return transform(args);
	case kM2MSelAbortTransfers:
	case kM2MSelAbortCaptures:
		return kIOReturnSuccess;
	case kM2MSelGetID:
		if (args->scalarOutputCount < 1) {
			return kIOReturnBadArgument;
		}
		args->scalarOutput[0] = fID;
		args->scalarOutputCount = 1;
		return kIOReturnSuccess;
	case kM2MSelSetProperty:
		if (args->structureInput == NULL || args->structureInputSize != sizeof(fProperty)) {
			return kIOReturnBadArgument;
		}
		memcpy(fProperty, args->structureInput, sizeof(fProperty));
		return kIOReturnSuccess;
	default: {
		static uint32_t logged;
		if (selector < 32 && !(OSBitOrAtomic(1u << selector, &logged) & (1u << selector))) {
			IOLog(M2M_LOG "selector %u not done (unsupported)\n", selector);
		}
		return kIOReturnUnsupported;
	}
	}
}

#endif /* SWEET */
