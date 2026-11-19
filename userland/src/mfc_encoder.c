/*
 * H.264 and HEVC encoding for mediaserverd on the Exynos 9810's MFC (kernel: bsd/dev/hakios.exynos_mfc.c, /dev/hakios-mfcenc), part of
 * libhakios.media.dylib: a VideoToolbox video encoder, registered for 'avc1' and 'hvc1' (hakios_mfc_encoder_register, called by
 * inject_media.c after the software HEVC encoder's registration).
 *
 * Why: this iPhone's VideoToolbox has no H.264 encoder and only a software HEVC one (VCPHEVC.videocodec). A video recording through that
 * one took four to seven cores, fell behind, and at the stop the movie was never finished (15 November 2026, 1080p and 720p alike).
 *
 * Why not by pointing the callers' pointers here (steps 77 to 80): in the shared cache, CMCapture's and MediaToolbox's calls into
 * VideoToolbox go through the cache's stub islands (adrp, add, br: the target written in the code), not through their __got, which no
 * call reads. Nothing patched there was ever called.
 *
 * The private ABI, from VCPHEVC.videocodec of 20F66 (HEVCVideoEncoder_CreateInstance, its vtable at 0x1e246ed40), read from its code:
 *   VTRegisterVideoEncoderWithInfo(codec, info, create): create(codec, allocator, &object);
 *   CMDerivedObjectCreate(allocator, &vtable, VTVideoEncoderGetClassID(), &object); vtable { 0, &base, &encoder };
 *   base: { 1, storage size, equal, invalidate, finalize, copyDebugDescription, copyProperty, setProperty, ... }
 *   encoder (version 3): [1] startSession(encoder, session, CMVideoDimensions (width low, height high), CFDictionaryRef),
 *     [2] encodeFrame(encoder, frameToken, imageBuffer, const CMTime *pts, const CMTime *duration, CFDictionaryRef frameProperties),
 *     [3] completeFrames(encoder, const CMTime *until), [4] copySupportedPropertyDictionary(encoder, CFDictionaryRef *); the rest NULL here.
 *   VTEncoderSessionEmitEncodedFrame(session, frameToken, status, infoFlags, sampleBuffer); VTEncoderSessionSetPixelBufferAttributes(session,
 *   attributes).
 *
 * An HEVC session that is not a video's size (the photographs' HEIC, made by CMPhoto) is handed, whole, to a VCPHEVC encoder made inside
 * this one: every call passed on. H.264 has nothing else, so it is always the block's. Pictures are encoded synchronously: encodeFrame
 * emits the picture's sample buffer before it returns. The block gives Annex B; the sample buffers carry length-prefixed NAL units
 * (4 bytes) and a format made from its parameter sets.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <VideoToolbox/VideoToolbox.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "hakios.mfc.h"

#define HEADER_ROOM     4096
#define STREAM_ROOM     (4u << 20)
#define ERR_VALUE_NOT_AVAILABLE (-12783)        /* kCMBaseObjectError_ValueNotAvailable */
#define ERR_NOT_AVAILABLE_NOW   (-12902)        /* kVTVideoEncoderNotAvailableNowErr */
#define ERR_COULD_NOT_FIND      (-12908)        /* kVTCouldNotFindVideoEncoderErr */
#define INFO_FRAME_DROPPED      2               /* kVTEncodeInfo_FrameDropped */

/* VideoToolbox's and CoreMedia's private functions, looked up (hakios_mfc_encoder_register) */
static CFTypeID (*VTVideoEncoderGetClassID_)(void);
static void *(*VTVideoEncoderGetCMBaseObject_)(void *encoder);
static OSStatus (*VTEncoderSessionEmitEncodedFrame_)(void *session, void *token, OSStatus status, uint32_t infoFlags, CMSampleBufferRef sample);
static OSStatus (*VTEncoderSessionSetPixelBufferAttributes_)(void *session, CFDictionaryRef attributes);
static OSStatus (*VTRegisterVideoEncoderWithInfo_)(uint32_t codec, CFDictionaryRef info, OSStatus (*create)(uint32_t, CFAllocatorRef, void **));
static OSStatus (*CMDerivedObjectCreate_)(CFAllocatorRef allocator, const void *vtable, CFTypeID classID, void **out);
static void *(*CMBaseObjectGetDerivedStorage_)(void *object);
static const void *(*CMBaseObjectGetVTable_)(void *object);
static OSStatus (*vcp_create)(uint32_t codec, CFAllocatorRef allocator, void **out);    /* VCPHEVC's HEVCVideoEncoder_CreateInstance */

struct base_class {
	uint64_t version;
	size_t storage;
	void *equal;
	OSStatus (*invalidate)(void *);
	void (*finalize)(void *);
	void *copy_description;
	OSStatus (*copy_property)(void *, CFStringRef, CFAllocatorRef, void *);
	OSStatus (*set_property)(void *, CFStringRef, CFTypeRef);
	void *barrier, *protocol;
};

struct encoder_class {
	uint64_t version;
	OSStatus (*start)(void *, void *, uint64_t, CFDictionaryRef);
	OSStatus (*encode)(void *, void *, CVImageBufferRef, const CMTime *, const CMTime *, CFDictionaryRef);
	OSStatus (*complete)(void *, const CMTime *);
	OSStatus (*copy_supported)(void *, CFDictionaryRef *);
	void *later[24];
};

struct vtable { void *zero; const struct base_class *base; const struct encoder_class *derived; };

struct enc_session {
	void *session;                          /* VideoToolbox's encoder session */
	bool hevc;
	void *inner;                            /* a VCPHEVC encoder this one's calls go to (an HEVC session not a video's), or NULL */
	const struct vtable *inner_vt;
	CFMutableDictionaryRef early;           /* properties given before startSession, for the inner encoder */
	int fd;
	int32_t width, height;
	uint32_t fps, bitrate, gop, profile;
	bool realtime, fps_given;
	bool opened, failed;
	CMVideoFormatDescriptionRef format;
	uint8_t *stream;
	uint64_t frames, bytes, us;
};

static CFDictionaryRef supported;               /* the properties said to be supported (VCPHEVC's, their value limits taken off) */

static void
say(const char *format, ...)
{
	char line[300];
	va_list ap;
	int n = snprintf(line, sizeof(line), "hakios: mfcenc: ");

	va_start(ap, format);
	n += vsnprintf(line + n, sizeof(line) - (size_t)n, format, ap);
	va_end(ap);
	if (n > (int)sizeof(line) - 2) {
		n = sizeof(line) - 2;
	}
	line[n++] = '\n';
	write(2, line, (size_t)n);
}

static struct enc_session *
storage_of_base(void *base)
{
	return CMBaseObjectGetDerivedStorage_(base);
}

static struct enc_session *
storage(void *encoder)
{
	return CMBaseObjectGetDerivedStorage_(VTVideoEncoderGetCMBaseObject_(encoder));
}

static uint32_t
number(CFTypeRef value)
{
	double d = 0;

	if (value != NULL && CFGetTypeID(value) == CFNumberGetTypeID()) {
		CFNumberGetValue(value, kCFNumberDoubleType, &d);
	}
	return d > 0 && d < 4e9 ? (uint32_t)d : 0;
}

/* what this encoder takes of a property; the rest is accepted and ignored */
static void
take_property(struct enc_session *e, CFStringRef key, CFTypeRef value)
{
	if (CFEqual(key, kVTCompressionPropertyKey_AverageBitRate)) {
		e->bitrate = number(value);
	} else if (CFEqual(key, kVTCompressionPropertyKey_ExpectedFrameRate)) {
		e->fps = number(value) ? number(value) : e->fps;
		e->fps_given = number(value) != 0;
	} else if (CFEqual(key, kVTCompressionPropertyKey_RealTime)) {
		e->realtime = value == kCFBooleanTrue;
	} else if (CFEqual(key, kVTCompressionPropertyKey_MaxKeyFrameInterval)) {
		e->gop = number(value);
	} else if (CFEqual(key, kVTCompressionPropertyKey_ProfileLevel) && value != NULL && CFGetTypeID(value) == CFStringGetTypeID()) {
		char name[64] = "";

		CFStringGetCString(value, name, sizeof(name), kCFStringEncodingUTF8);
		e->profile = strncmp(name, "H264_Baseline", 13) == 0 ? HAKIOS_MFC_ENC_BASELINE :
		    strncmp(name, "H264_Main", 9) == 0 ? HAKIOS_MFC_ENC_MAIN : HAKIOS_MFC_ENC_HIGH;
	}
}

/* a NAL unit's type: H.264's 5 bits; HEVC's 6, after the forbidden bit */
static uint8_t
nal_type(bool hevc, uint8_t first)
{
	return hevc ? (first >> 1) & 0x3F : first & 0x1F;
}

/* parameter sets: H.264's SPS 7, PPS 8, AUD 9; HEVC's VPS 32, SPS 33, PPS 34, AUD 35 */
static bool
is_parameter_set(bool hevc, uint8_t type)
{
	return hevc ? (type >= 32 && type <= 35) : (type >= 7 && type <= 9);
}

/* the block's parameter sets, Annex B: H.264's SPS and PPS, or HEVC's VPS, SPS and PPS */
static CMVideoFormatDescriptionRef
format_from(bool hevc, const uint8_t *h, size_t n)
{
	const uint8_t *sets[3] = { NULL, NULL, NULL };
	size_t sizes[3] = { 0, 0, 0 }, i = 0;

	while (i + 3 < n) {
		if (h[i] == 0 && h[i + 1] == 0 && h[i + 2] == 1) {
			size_t start = i + 3, end = start;

			while (end + 2 < n && !(h[end] == 0 && h[end + 1] == 0 && (h[end + 2] == 1 || (h[end + 2] == 0 && end + 3 < n && h[end + 3] == 1)))) {
				end++;
			}
			if (end + 2 >= n) {
				end = n;
			}
			uint8_t type = nal_type(hevc, h[start]);
			int k = hevc ? (type == 32 ? 0 : type == 33 ? 1 : type == 34 ? 2 : -1) : (type == 7 ? 0 : type == 8 ? 1 : -1);

			if (k >= 0 && sets[k] == NULL) {
				sets[k] = h + start, sizes[k] = end - start;
			}
			i = end;
		} else {
			i++;
		}
	}
	if (sets[0] == NULL || sets[1] == NULL || (hevc && sets[2] == NULL)) {
		say("the %s header lacks a parameter set (%zu bytes)", hevc ? "HEVC" : "H.264", n);
		return NULL;
	}
	CMVideoFormatDescriptionRef format = NULL;
	OSStatus status = hevc ? CMVideoFormatDescriptionCreateFromHEVCParameterSets(NULL, 3, sets, sizes, 4, NULL, &format) :
	    CMVideoFormatDescriptionCreateFromH264ParameterSets(NULL, 2, sets, sizes, 4, &format);

	if (status != noErr) {
		say("the %s format from parameter sets of %zu, %zu, %zu bytes: %d", hevc ? "HEVC" : "H.264", sizes[0], sizes[1], sizes[2], (int)status);
		return NULL;
	}
	return format;
}

/* Annex B to AVCC, in place into `out`; parameter sets dropped (they are in the format). Answers the bytes written. */
static size_t
to_avcc(bool hevc, const uint8_t *in, size_t n, uint8_t *out)
{
	size_t i = 0, at = 0;

	while (i + 3 <= n) {
		size_t sc = 0;

		if (in[i] == 0 && in[i + 1] == 0 && in[i + 2] == 1) {
			sc = 3;
		} else if (i + 4 <= n && in[i] == 0 && in[i + 1] == 0 && in[i + 2] == 0 && in[i + 3] == 1) {
			sc = 4;
		}
		if (sc == 0) {
			i++;
			continue;
		}
		size_t start = i + sc, end = start;

		while (end + 3 <= n && !(in[end] == 0 && in[end + 1] == 0 && (in[end + 2] == 1 || (end + 4 <= n && in[end + 2] == 0 && in[end + 3] == 1)))) {
			end++;
		}
		if (end + 3 > n) {
			end = n;
		}
		size_t len = end - start;
		if (len > 0 && !is_parameter_set(hevc, nal_type(hevc, in[start]))) {
			out[at] = (uint8_t)(len >> 24);
			out[at + 1] = (uint8_t)(len >> 16);
			out[at + 2] = (uint8_t)(len >> 8);
			out[at + 3] = (uint8_t)len;
			memmove(out + at + 4, in + start, len);
			at += 4 + len;
		}
		i = end;
	}
	return at;
}

static bool
open_encoder(struct enc_session *e)
{
	/*
	 * The bit rate as asked (CMCapture: 15.3 Mb/s for 1080p30), up to 20 Mb/s, 40 for 4K. (Step 84 capped it at 8 Mb/s on the
	 * guess that the storage could not keep up; step 85 measured the storage busy 1% while recording: the guess was wrong.)
	 */
	uint32_t cap = e->width * e->height > 1920 * 1088 ? 40000000u : 20000000u;

	if (e->bitrate == 0) {
		e->bitrate = e->width * e->height > 1280 * 720 ? 15000000u : 8000000u;
	} else if (e->bitrate > cap) {
		e->bitrate = cap;
	}
	uint8_t header[HEADER_ROOM];
	struct hakios_mfc_enc_open o = {
		.width = (uint32_t)e->width & ~1u, .height = (uint32_t)e->height & ~1u, .fps = e->fps, .bitrate = e->bitrate,
		.gop = e->gop, .profile = e->hevc ? 0 : e->profile,
		.level = e->hevc ? ((e->width * e->height > 1280 * 720) ? 41 : 31) : ((e->width * e->height > 1280 * 720) ? 40 : 31),
		.header_room = sizeof(header), .header = (uint64_t)(uintptr_t)header,
		.codec = e->hevc ? HAKIOS_MFC_HEVC : HAKIOS_MFC_H264,
	};

	if (ioctl(e->fd, HAKIOS_MFC_ENC_OPEN, &o) != 0) {
		say("session %p: the encoder refused %ux%u (%d)", (void *)e->session, o.width, o.height, errno);
		return false;
	}
	e->format = format_from(e->hevc, header, o.header_bytes);
	e->stream = malloc(STREAM_ROOM);
	if (e->format == NULL || e->stream == NULL) {
		return false;
	}
	say("session %p: %s %ux%u, %u fps, %u b/s, key every %u, profile %u: header %u bytes", (void *)e->session, e->hevc ? "HEVC" : "H.264",
	    o.width, o.height, o.fps, o.bitrate, o.gop, o.profile, o.header_bytes);
	return true;
}

/* one picture through the block; the sample buffer for it, or NULL with *status set */
static CMSampleBufferRef
encode(struct enc_session *e, CVImageBufferRef image, CMTime pts, CMTime duration, CFDictionaryRef options, OSStatus *status)
{
	OSType pf = CVPixelBufferGetPixelFormatType(image);
	struct hakios_mfc_enc_frame f;
	CMSampleBufferRef sample = NULL;

	*status = kVTVideoEncoderMalfunctionErr;
	if (pf != kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange && pf != kCVPixelFormatType_420YpCbCr8BiPlanarFullRange) {
		static int told;

		if (told++ < 3) {
			say("session %p: a picture of '%c%c%c%c', not NV12: refused", (void *)e->session, (char)(pf >> 24), (char)(pf >> 16),
			    (char)(pf >> 8), (char)pf);
		}
		*status = kVTPixelTransferNotSupportedErr;
		return NULL;
	}
	if (!e->opened) {
		/* (the picture's own size wins over the session's, should they differ) */
		e->width = (int32_t)CVPixelBufferGetWidth(image);
		e->height = (int32_t)CVPixelBufferGetHeight(image);
		e->opened = true;
		e->failed = !open_encoder(e);
	}
	if (e->failed || e->fd < 0) {
		return NULL;
	}
	if (CVPixelBufferLockBaseAddress(image, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess) {
		return NULL;
	}
	memset(&f, 0, sizeof(f));
	f.y = (uint64_t)(uintptr_t)CVPixelBufferGetBaseAddressOfPlane(image, 0);
	f.uv = (uint64_t)(uintptr_t)CVPixelBufferGetBaseAddressOfPlane(image, 1);
	f.y_stride = (uint32_t)CVPixelBufferGetBytesPerRowOfPlane(image, 0);
	f.uv_stride = (uint32_t)CVPixelBufferGetBytesPerRowOfPlane(image, 1);
	f.key = e->frames == 0 || (options != NULL && CFDictionaryGetValue(options, kVTEncodeFrameOptionKey_ForceKeyFrame) == kCFBooleanTrue);
	f.stream = (uint64_t)(uintptr_t)e->stream;
	f.stream_room = STREAM_ROOM;

	/* what the picture holds, sampled (64 points of its luma, 64 of its chroma): to tell a real picture from a blank one in the log */
	unsigned y_mean = 0, uv_mean = 0;
	{
		size_t w = CVPixelBufferGetWidthOfPlane(image, 0), h = CVPixelBufferGetHeightOfPlane(image, 0);
		const uint8_t *yp = (const uint8_t *)(uintptr_t)f.y, *cp = (const uint8_t *)(uintptr_t)f.uv;

		for (unsigned k = 0; k < 64 && yp != NULL && cp != NULL; k++) {
			size_t x = (w * (2 * (k % 8) + 1)) / 16, yy = (h * (2 * (k / 8) + 1)) / 16;

			y_mean += yp[yy * f.y_stride + x];
			uv_mean += cp[(yy / 2) * f.uv_stride + (x & ~(size_t)1)];
		}
		y_mean /= 64;
		uv_mean /= 64;
	}
	int r = ioctl(e->fd, HAKIOS_MFC_ENC_FRAME, &f);
	int error = errno;

	CVPixelBufferUnlockBaseAddress(image, kCVPixelBufferLock_ReadOnly);
	if (r != 0) {
		static int told;

		if (told++ < 5) {
			say("session %p: picture %llu: the block refused it (%d)", (void *)e->session, (unsigned long long)e->frames, error);
		}
		return NULL;
	}
	size_t n = to_avcc(e->hevc, e->stream, f.bytes, e->stream);
	if (n == 0) {
		*status = noErr;                /* (a picture the block skipped: nothing to emit) */
		return NULL;
	}
	CMBlockBufferRef block = NULL;
	void *data = malloc(n);

	if (data == NULL) {
		return NULL;
	}
	memcpy(data, e->stream, n);
	if (CMBlockBufferCreateWithMemoryBlock(NULL, data, n, kCFAllocatorMalloc, NULL, 0, n, 0, &block) != kCMBlockBufferNoErr) {
		free(data);
		return NULL;
	}
	CMSampleTimingInfo timing = { .duration = duration, .presentationTimeStamp = pts, .decodeTimeStamp = pts };
	size_t size = n;
	OSStatus s = CMSampleBufferCreateReady(NULL, block, e->format, 1, 1, &timing, 1, &size, &sample);

	CFRelease(block);
	if (s != noErr || sample == NULL) {
		say("session %p: the sample buffer: %d", (void *)e->session, (int)s);
		return NULL;
	}
	CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sample, true);

	if (attachments != NULL && CFArrayGetCount(attachments) > 0) {
		CFMutableDictionaryRef a = (CFMutableDictionaryRef)CFArrayGetValueAtIndex(attachments, 0);

		CFDictionarySetValue(a, kCMSampleAttachmentKey_NotSync, f.key ? kCFBooleanFalse : kCFBooleanTrue);
		CFDictionarySetValue(a, kCMSampleAttachmentKey_DependsOnOthers, f.key ? kCFBooleanFalse : kCFBooleanTrue);
	}
	e->frames++;
	e->bytes += n;
	e->us += f.us;
	if (e->frames <= 3 || e->frames % 30 == 0) {
		say("session %p: picture %llu: %zu bytes%s, %u us in the block; pts %.3f s; luma mean %u, Cb mean %u (sampled)", (void *)e->session,
		    (unsigned long long)e->frames, n, f.key ? " (key)" : "", f.us, CMTimeGetSeconds(pts), y_mean, uv_mean);
	}
	*status = noErr;
	return sample;
}

/* ---- the encoder's base object ---- */

static OSStatus
invalidate(void *base)
{
	struct enc_session *e = storage_of_base(base);

	if (e->inner != NULL && e->inner_vt->base->invalidate != NULL) {
		e->inner_vt->base->invalidate(VTVideoEncoderGetCMBaseObject_(e->inner));
	}
	if (e->fd >= 0) {
		say("session %p: invalidated after %llu pictures", e->session, (unsigned long long)e->frames);
		close(e->fd);
		e->fd = -1;
	}
	return noErr;
}

static void
finalize(void *base)
{
	struct enc_session *e = storage_of_base(base);

	if (e->frames != 0) {
		say("session %p: %llu pictures, %llu KB, %llu us a picture in the block on average", e->session,
		    (unsigned long long)e->frames, (unsigned long long)(e->bytes / 1024), (unsigned long long)(e->us / e->frames));
	}
	if (e->fd >= 0) {
		close(e->fd);
	}
	if (e->inner != NULL) {
		CFRelease(e->inner);
	}
	if (e->early != NULL) {
		CFRelease(e->early);
	}
	if (e->format != NULL) {
		CFRelease(e->format);
	}
	free(e->stream);
}

static OSStatus
copy_property(void *base, CFStringRef key, CFAllocatorRef allocator, void *value)
{
	struct enc_session *e = storage_of_base(base);

	if (e->inner != NULL) {
		return e->inner_vt->base->copy_property != NULL ?
		    e->inner_vt->base->copy_property(VTVideoEncoderGetCMBaseObject_(e->inner), key, allocator, value) : ERR_VALUE_NOT_AVAILABLE;
	}
	if (value != NULL && CFEqual(key, CFSTR("UsingHardwareAcceleratedVideoEncoder"))) {
		*(CFTypeRef *)value = kCFBooleanTrue;
		return noErr;
	}
	return ERR_VALUE_NOT_AVAILABLE;
}

static OSStatus
set_property(void *base, CFStringRef key, CFTypeRef value)
{
	struct enc_session *e = storage_of_base(base);

	if (e->inner != NULL) {
		return e->inner_vt->base->set_property != NULL ?
		    e->inner_vt->base->set_property(VTVideoEncoderGetCMBaseObject_(e->inner), key, value) : noErr;
	}
	if (key == NULL) {
		return noErr;
	}
	if (e->session == NULL && e->hevc && value != NULL) {
		if (e->early == NULL) {
			e->early = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
		}
		CFDictionarySetValue(e->early, key, value);
	}
	take_property(e, key, value);
	return noErr;
}

/* ---- the encoder ---- */

static void
pass_early(const void *key, const void *value, void *context)
{
	struct enc_session *e = context;

	e->inner_vt->base->set_property(VTVideoEncoderGetCMBaseObject_(e->inner), key, value);
}

/* an HEVC session not the block's: a VCPHEVC encoder made for it, which has every call from here on */
static OSStatus
hand_on(struct enc_session *e, void *session, uint64_t dimensions, CFDictionaryRef dictionary)
{
	OSStatus status;

	if (vcp_create == NULL) {
		return ERR_COULD_NOT_FIND;
	}
	status = vcp_create('hvc1', NULL, &e->inner);
	if (status != noErr || e->inner == NULL) {
		e->inner = NULL;
		return status != noErr ? status : ERR_COULD_NOT_FIND;
	}
	e->inner_vt = CMBaseObjectGetVTable_(VTVideoEncoderGetCMBaseObject_(e->inner));
	if (e->early != NULL && e->inner_vt->base->set_property != NULL) {
		CFDictionaryApplyFunction(e->early, pass_early, e);
	}
	return e->inner_vt->derived->start(e->inner, session, dimensions, dictionary);
}

static OSStatus
start_session(void *encoder, void *session, uint64_t dimensions, CFDictionaryRef dictionary)
{
	struct enc_session *e = storage(encoder);
	int32_t w = (int32_t)(uint32_t)dimensions, h = (int32_t)(uint32_t)(dimensions >> 32);
	int32_t big = w > h ? w : h, small = w > h ? h : w;
	bool video_size = (big == 1920 && small == 1080) || (big == 1280 && small == 720) || (big == 3840 && small == 2160);

	if (e->hevc && !video_size) {
		say("HEVC session %p, %dx%d: not a video's size: VCPHEVC's", session, w, h);
		return hand_on(e, session, dimensions, dictionary);
	}
	e->fd = open("/dev/hakios-mfcenc", O_RDWR);
	if (e->fd < 0) {
		int error = errno;

		if (e->hevc) {
			say("HEVC session %p, %dx%d: the block is not to be had (%d): VCPHEVC's", session, w, h, error);
			return hand_on(e, session, dimensions, dictionary);
		}
		say("H.264 session %p, %dx%d: the block is not to be had (%d)", session, w, h, error);
		return ERR_NOT_AVAILABLE_NOW;
	}
	e->session = session;
	e->width = w;
	e->height = h;

	int formats[2] = { kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, kCVPixelFormatType_420YpCbCr8BiPlanarFullRange };
	CFNumberRef f0 = CFNumberCreate(NULL, kCFNumberIntType, &formats[0]), f1 = CFNumberCreate(NULL, kCFNumberIntType, &formats[1]);
	CFNumberRef nw = CFNumberCreate(NULL, kCFNumberIntType, &w), nh = CFNumberCreate(NULL, kCFNumberIntType, &h);
	const void *fs[2] = { f0, f1 };
	CFArrayRef pixel_formats = CFArrayCreate(NULL, fs, 2, &kCFTypeArrayCallBacks);
	CFDictionaryRef surface = CFDictionaryCreate(NULL, NULL, NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	const void *keys[] = { kCVPixelBufferPixelFormatTypeKey, kCVPixelBufferWidthKey, kCVPixelBufferHeightKey, kCVPixelBufferIOSurfacePropertiesKey };
	const void *values[] = { pixel_formats, nw, nh, surface };
	CFDictionaryRef attributes = CFDictionaryCreate(NULL, keys, values, 4, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	OSStatus status = VTEncoderSessionSetPixelBufferAttributes_(session, attributes);

	CFRelease(attributes);
	CFRelease(surface);
	CFRelease(pixel_formats);
	CFRelease(nw);
	CFRelease(nh);
	CFRelease(f0);
	CFRelease(f1);
	say("%s session %p, %dx%d: the MFC's (pixel buffer attributes: %d)", e->hevc ? "HEVC" : "H.264", session, w, h, (int)status);
	return noErr;
}

static OSStatus
encode_frame(void *encoder, void *token, CVImageBufferRef image, const CMTime *pts, const CMTime *duration, CFDictionaryRef properties)
{
	struct enc_session *e = storage(encoder);
	OSStatus status;

	if (e->inner != NULL) {
		return e->inner_vt->derived->encode(e->inner, token, image, pts, duration, properties);
	}
	CMSampleBufferRef sample = encode(e, image, *pts, *duration, properties, &status);

	OSStatus emitted;

	if (sample != NULL) {
		emitted = VTEncoderSessionEmitEncodedFrame_(e->session, token, noErr, 0, sample);
		CFRelease(sample);
	} else {
		emitted = VTEncoderSessionEmitEncodedFrame_(e->session, token, status, status == noErr ? INFO_FRAME_DROPPED : 0, NULL);
		say("session %p: picture %llu: none emitted (%d)", e->session, (unsigned long long)e->frames, (int)status);
	}
	if (emitted != noErr || e->frames <= 1) {
		say("session %p: picture %llu given to VideoToolbox: %d", e->session, (unsigned long long)e->frames, (int)emitted);
	}
	return noErr;
}

static OSStatus
complete_frames(void *encoder, const CMTime *until)
{
	struct enc_session *e = storage(encoder);

	if (e->inner != NULL && e->inner_vt->derived->complete != NULL) {
		return e->inner_vt->derived->complete(e->inner, until);
	}
	say("session %p: complete the frames until %.3f s (%llu pictures so far; every one already emitted)", e->session,
	    until != NULL ? CMTimeGetSeconds(*until) : -1.0, (unsigned long long)e->frames);
	return noErr;           /* (every picture has been emitted already) */
}

static OSStatus
copy_supported(void *encoder, CFDictionaryRef *out)
{
	struct enc_session *e = encoder != NULL ? storage(encoder) : NULL;

	if (e != NULL && e->inner != NULL && e->inner_vt->derived->copy_supported != NULL) {
		return e->inner_vt->derived->copy_supported(e->inner, out);
	}
	if (supported == NULL) {
		return ERR_VALUE_NOT_AVAILABLE;
	}
	if (out != NULL) {
		*out = CFRetain(supported);
	}
	return noErr;
}

static const struct base_class base_class = { 1, sizeof(struct enc_session), NULL, invalidate, finalize, NULL, copy_property, set_property, NULL, NULL };
static const struct encoder_class encoder_class = { 3, start_session, encode_frame, complete_frames, copy_supported, { NULL } };
static const struct vtable vtable = { NULL, &base_class, &encoder_class };

static OSStatus
create(uint32_t codec, CFAllocatorRef allocator, void **out)
{
	void *object = NULL;
	OSStatus status;

	if (access("/dev/hakios-mfcenc", F_OK) != 0) {
		return ERR_COULD_NOT_FIND;
	}
	status = CMDerivedObjectCreate_(allocator, &vtable, VTVideoEncoderGetClassID_(), &object);
	if (status != noErr) {
		return status;
	}
	struct enc_session *e = CMBaseObjectGetDerivedStorage_(object);

	memset(e, 0, sizeof(*e));
	e->fd = -1;
	e->hevc = codec == 'hvc1';
	e->fps = 30;
	*out = object;
	return noErr;
}

/* VCPHEVC's supported properties, each one's value limits taken off (they are HEVC's: H.264's profiles would not be in them) */
static void
loosen(const void *key, const void *value, void *context)
{
	CFTypeRef v = value;

	if (v != NULL && CFGetTypeID(v) == CFDictionaryGetTypeID()) {
		CFMutableDictionaryRef d = CFDictionaryCreateMutableCopy(NULL, 0, v);

		CFDictionaryRemoveValue(d, kVTPropertySupportedValueListKey);
		CFDictionaryRemoveValue(d, kVTPropertySupportedValueMinimumKey);
		CFDictionaryRemoveValue(d, kVTPropertySupportedValueMaximumKey);
		CFDictionarySetValue(context, key, d);
		CFRelease(d);
	} else {
		CFDictionarySetValue(context, key, v);
	}
}

static void
make_supported(void)
{
	void *probe = NULL;
	CFDictionaryRef theirs = NULL;
	CFMutableDictionaryRef ours = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);

	if (vcp_create != NULL && vcp_create('hvc1', NULL, &probe) == noErr && probe != NULL) {
		const struct vtable *vt = CMBaseObjectGetVTable_(VTVideoEncoderGetCMBaseObject_(probe));

		if (vt->derived->copy_supported != NULL) {
			vt->derived->copy_supported(probe, &theirs);
		}
		CFRelease(probe);
	}
	if (theirs != NULL) {
		CFDictionaryApplyFunction(theirs, loosen, ours);
		CFRelease(theirs);
	}
	CFStringRef extra[] = { kVTCompressionPropertyKey_H264EntropyMode, kVTCompressionPropertyKey_ProfileLevel,
		kVTCompressionPropertyKey_AverageBitRate, kVTCompressionPropertyKey_DataRateLimits, kVTCompressionPropertyKey_ExpectedFrameRate,
		kVTCompressionPropertyKey_MaxKeyFrameInterval, kVTCompressionPropertyKey_MaxKeyFrameIntervalDuration,
		kVTCompressionPropertyKey_RealTime, kVTCompressionPropertyKey_AllowFrameReordering, kVTCompressionPropertyKey_Quality };
	CFDictionaryRef empty = CFDictionaryCreate(NULL, NULL, NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);

	for (size_t i = 0; i < sizeof(extra) / sizeof(extra[0]); i++) {
		if (!CFDictionaryContainsKey(ours, extra[i])) {
			CFDictionarySetValue(ours, extra[i], empty);
		}
	}
	CFRelease(empty);
	supported = ours;
	say("%ld properties said to be supported (%s)", (long)CFDictionaryGetCount(ours), theirs != NULL ? "VCPHEVC's, loosened" : "a short list of ours");
}

void
hakios_mfc_encoder_register(void)
{
	static const struct { uint32_t codec; CFStringRef id, name; } codecs[] = {
		{ 'avc1', CFSTR("com.apple.videotoolbox.videoencoder.avc.hakios.mfc"), CFSTR("H.264 (Exynos MFC)") },
		{ 'hvc1', CFSTR("com.apple.videotoolbox.videoencoder.hevc.hakios.mfc"), CFSTR("HEVC (Exynos MFC)") },
	};
	void *vt = dlopen("/System/Library/Frameworks/VideoToolbox.framework/VideoToolbox", RTLD_NOW | RTLD_GLOBAL);
	void *cm = dlopen("/System/Library/Frameworks/CoreMedia.framework/CoreMedia", RTLD_NOW | RTLD_GLOBAL);
	void *vcp = dlopen("/System/Library/VideoCodecs/VCPHEVC.videocodec", RTLD_NOW | RTLD_LOCAL);
	CFStringRef *id_key, *name_key, *hardware_key;

	if (access("/dev/hakios-mfcenc", F_OK) != 0 || vt == NULL || cm == NULL) {
		say("no /dev/hakios-mfcenc: the MFC's encoder is not registered");
		return;
	}
#define LOOK(from, name) (name##_ = dlsym(from, #name))
	id_key = dlsym(vt, "kVTVideoEncoderList_EncoderID");
	name_key = dlsym(vt, "kVTVideoEncoderList_EncoderName");
	hardware_key = dlsym(vt, "kVTVideoEncoderList_IsHardwareAccelerated");
	if (!LOOK(vt, VTVideoEncoderGetClassID) || !LOOK(vt, VTVideoEncoderGetCMBaseObject) || !LOOK(vt, VTEncoderSessionEmitEncodedFrame) ||
	    !LOOK(vt, VTEncoderSessionSetPixelBufferAttributes) || !LOOK(vt, VTRegisterVideoEncoderWithInfo) || !LOOK(cm, CMDerivedObjectCreate) ||
	    !LOOK(cm, CMBaseObjectGetDerivedStorage) || !LOOK(cm, CMBaseObjectGetVTable) || id_key == NULL || name_key == NULL || hardware_key == NULL) {
		say("something of VideoToolbox's is not where it was: the MFC's encoder is not registered");
		return;
	}
#undef LOOK
	vcp_create = vcp != NULL ? (OSStatus (*)(uint32_t, CFAllocatorRef, void **))dlsym(vcp, "HEVCVideoEncoder_CreateInstance") : NULL;
	make_supported();
	for (size_t i = 0; i < sizeof(codecs) / sizeof(codecs[0]); i++) {
		const void *keys[] = { *id_key, *name_key, *hardware_key };
		const void *values[] = { codecs[i].id, codecs[i].name, kCFBooleanTrue };
		CFDictionaryRef info = CFDictionaryCreate(NULL, keys, values, 3, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
		OSStatus status = VTRegisterVideoEncoderWithInfo_(codecs[i].codec, info, create);

		CFRelease(info);
		say("the MFC's encoder registered for '%c%c%c%c' (%d)%s", (char)(codecs[i].codec >> 24), (char)(codecs[i].codec >> 16),
		    (char)(codecs[i].codec >> 8), (char)codecs[i].codec, (int)status,
		    codecs[i].codec == 'hvc1' ? (vcp_create != NULL ? "; other sizes handed to VCPHEVC" : "; no VCPHEVC for other sizes") : "");
	}
}
