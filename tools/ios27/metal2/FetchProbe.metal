// SPDX-License-Identifier: MIT-0
// Framebuffer fetch through a memoryless color attachment (docs/ios27/CAMERA.md, "The display side"): the shape of
// CoreAnimation's camera layer draw in the window server's main pass, on a small target. Built by
// tools/ios27/metal/mk-bundle27.sh into FetchProbe.inc; used by FetchProbe.h.
//   color 0: BGRA8Unorm (the framebuffer), color 1: RGBA16Float, color 2: R32Float (both memoryless in the window server).
//   sweet_fetch_write1: writes color 1 only (the pipeline masks color 0 and 2 off), as TimgA3S1Xhfu_Ixrg does.
//   sweet_fetch_read1:  reads color 1 by framebuffer fetch and writes color 0, as TimgBbcmA3Xhf_Isup does.
//   sweet_fetch_write0 / sweet_fetch_read0: the same through color 0 itself (a blend done in the shader).
#include <metal_stdlib>
using namespace metal;

vertex float4 sweet_fetch_vert(uint v [[vertex_id]]) {
	const float2 p = float2((v << 1) & 2, v & 2);
	return float4(p * 2.0f - 1.0f, 0.0f, 1.0f);
}

struct SweetFetchColor1 {
	half4 c1 [[color(1)]];
};
struct SweetFetchColor0 {
	half4 c0 [[color(0)]];
};

fragment SweetFetchColor1 sweet_fetch_write1() {
	return {half4(0.25h, 0.5h, 0.75h, 1.0h)};
}

fragment SweetFetchColor0 sweet_fetch_read1(half4 c1 [[color(1)]]) {
	return {c1};
}

fragment SweetFetchColor0 sweet_fetch_write0() {
	return {half4(0.25h, 0.5h, 0.75h, 1.0h)};
}

fragment SweetFetchColor0 sweet_fetch_read0(half4 c0 [[color(0)]]) {
	return {half4(c0.rgb * 0.5h, 1.0h)};
}

// The shadow probe (SWEET_MTL2_FBPROBE=2): an 8 x 8 grid of a texture read inside a rectangle (x, y, w, h) of it.
kernel void sweet_fetch_grid(texture2d<float, access::read> t [[texture(0)]], device float4 *out [[buffer(0)]],
                             constant uint4 &r [[buffer(1)]], uint2 c [[thread_position_in_grid]]) {
	if (c.x >= 8 || c.y >= 8)
		return;
	const uint2 at = uint2(r.x + r.z * (2 * c.x + 1) / 16, r.y + r.w * (2 * c.y + 1) / 16);
	out[c.y * 8 + c.x] = t.read(at);
}
