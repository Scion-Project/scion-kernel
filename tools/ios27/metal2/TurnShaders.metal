// SPDX-License-Identifier: MIT-0
// The copy behind a turned render target (NativeBridgeMetalFrontend.mm, "turned textures"): the window server draws the
// interface in its own orientation into a texture of that shape; this pass writes it, turned by quarter turns, into the
// display's surface, whose rows run along the panel. One triangle over the whole surface; the fragment reads the texel
// that belongs at its place. Built by the bundle's script (xcrun metal, metallib) into TurnShaders.inc.
#include <metal_stdlib>
using namespace metal;

struct TurnOut {
    float4 position [[position]];
    float2 at;                      // the surface's own coordinate, 0..1, origin top left
};

vertex TurnOut sweet_turn_vert(uint vid [[vertex_id]])
{
    float2 p = float2(float((vid << 1) & 2), float(vid & 2));      // (0,0) (2,0) (0,2)
    TurnOut o;
    o.position = float4(p.x * 2.0 - 1.0, 1.0 - p.y * 2.0, 0.0, 1.0);
    o.at = p;
    return o;
}

// MTLTextureRotation 1, 2, 3 = one, two, three quarter turns: where in the unturned picture the surface's point reads.
fragment float4 sweet_turn_frag_1(TurnOut in [[stage_in]], texture2d<float> picture [[texture(0)]], sampler nearest [[sampler(0)]])
{
    return picture.sample(nearest, float2(1.0 - in.at.y, in.at.x));
}

fragment float4 sweet_turn_frag_2(TurnOut in [[stage_in]], texture2d<float> picture [[texture(0)]], sampler nearest [[sampler(0)]])
{
    return picture.sample(nearest, float2(1.0 - in.at.x, 1.0 - in.at.y));
}

fragment float4 sweet_turn_frag_3(TurnOut in [[stage_in]], texture2d<float> picture [[texture(0)]], sampler nearest [[sampler(0)]])
{
    return picture.sample(nearest, float2(in.at.y, 1.0 - in.at.x));
}
