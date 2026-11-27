// SPDX-License-Identifier: MIT-0
// The camera's preview (docs/ios27/CAMERA.md, "The display side"): what a two-plane YCbCr texture over an IOSurface reads on
// this process's GPU. One thread for each point of an 8 x 8 grid (the middle of each cell), each writing three colors: the
// texel as read() gives it, then samples at the texel's middle through a linear and a nearest sampler. Built by tools/ios27/metal/mk-bundle27.sh
// into YCbCrProbe.inc; used by YCbCrProbe.h.
#include <metal_stdlib>
using namespace metal;

kernel void sweet_ycc_probe(texture2d<float, access::sample> picture [[texture(0)]], sampler filter [[sampler(0)]],
                            sampler point [[sampler(1)]], device float4 *out [[buffer(0)]], uint2 cell [[thread_position_in_grid]]) {
	if (cell.x >= 8 || cell.y >= 8)
		return;
	const uint w = picture.get_width(), h = picture.get_height();
	const uint2 at = uint2(w * (2 * cell.x + 1) / 16, h * (2 * cell.y + 1) / 16);
	out[cell.y * 8 + cell.x] = picture.read(at);
	out[64 + cell.y * 8 + cell.x] = picture.sample(filter, (float2(at) + 0.5f) / float2(w, h));
	out[128 + cell.y * 8 + cell.x] = picture.sample(point, (float2(at) + 0.5f) / float2(w, h));
}

// The same texture sampled where the window server samples it: a fragment function, in half precision, from slot 3, drawing a
// 16 x 16 target (one triangle over it; each pixel samples the texture at the matching place).
vertex float4 sweet_ycc_probe_vert(uint v [[vertex_id]]) {
	const float2 p = float2((v << 1) & 2, v & 2);
	return float4(p * 2.0f - 1.0f, 0.0f, 1.0f);
}

fragment half4 sweet_ycc_probe_frag(float4 at [[position]], texture2d<half> picture [[texture(3)]], sampler filter [[sampler(0)]]) {
	return picture.sample(filter, at.xy / 16.0f);
}

// The flattening (YCbCrProbe.h, SWEET_MTL2_YCCFLAT=1): every texel of a converted two-plane texture as read() gives it, written into
// a texture of 8-bit colors of the same size (alpha 1).
kernel void sweet_ycc_flatten(texture2d<float, access::read> picture [[texture(0)]], texture2d<float, access::write> flat [[texture(1)]],
                              uint2 at [[thread_position_in_grid]]) {
	if (at.x >= picture.get_width() || at.y >= picture.get_height())
		return;
	flat.write(float4(picture.read(at).rgb, 1.0f), at);
}
