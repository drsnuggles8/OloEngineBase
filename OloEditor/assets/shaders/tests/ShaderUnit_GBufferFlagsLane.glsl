// =============================================================================
// ShaderUnit_GBufferFlagsLane.glsl
//
// Round-trips the G-Buffer RT2 material-flags lane (issues #996, #1231) through
// the PRODUCTION encode/decode helpers in include/PBRCommon.glsl. It INCLUDES
// them rather than transcribing them, so this is a test of the layout and not a
// test of a copy of the layout — the only other caller of the decode side is
// ComputeDeferredLit in include/DeferredLightingShared.glsl.
//
// The lane physically lives in the ALPHA channel of an RGBA16F attachment, so
// the probe pushes each encoded value through packHalf2x16/unpackHalf2x16 —
// IEEE half, the same quantisation the real attachment applies — BEFORE
// decoding. That is what makes this a test of the transport rather than of the
// arithmetic, and it is where the model index's only real ceiling comes from
// (half is exact to 2048, so `model * 64 + slot * 8 + kind * 2` must stay
// <= 2047; PBRModel.h's kPBRModelGBufferLaneMax static_asserts against it).
//
// Parameterization, now TWO axes because the lane carries three fields:
//   x = PBR closure model index, so a W-wide probe sweeps models 0 .. W-1
//   y = the two smaller fields packed together: kind = y % 4, slot = y / 4,
//       so a 32-row probe sweeps every (kind, slot) pair that exists
// Every texel is therefore a distinct (model, kind, slot) triple, which is what
// lets the CPU side assert that no two triples collide onto one lane value —
// a collision being exactly how a skin pixel would silently shade as terrain.
//
// Output channel usage:
//   .r = the encoded lane value, after fp16 storage
//   .g = the decoded model index          (must equal the column index)
//   .b = the decoded material kind        (must equal y % 4)
//   .a = the decoded skin profile slot    (must equal y / 4)
//
// The unlit bit has no channel left, and does not need one: the encode never
// sets it, so the CPU asserts the lane value is EVEN — which is the same claim,
// made against the transported number rather than against a second decode.
// =============================================================================

#type vertex
#version 460 core

layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec2 a_TexCoord;

layout(location = 0) out vec2 v_TexCoord;

void main()
{
    v_TexCoord = a_TexCoord;
    gl_Position = vec4(a_Position, 1.0);
}

#type fragment
#version 460 core

layout(location = 0) out vec4 o_Color;

layout(location = 0) in vec2 v_TexCoord;

#include "include/PBRCommon.glsl"

void main()
{
    int model = int(gl_FragCoord.x);
    int row = int(gl_FragCoord.y);
    int kind = row % 4;
    int slot = row / 4;

    // Encode exactly as every G-Buffer writer does ...
    float lane = oloEncodeGBufferPbrFlagsEx(model, kind, slot);
    // ... quantise through fp16, which is what writing RGBA16F does ...
    lane = unpackHalf2x16(packHalf2x16(vec2(lane, 0.0))).x;
    // ... and decode exactly as ComputeDeferredLit does.
    int gbFlags = oloDecodeGBufferFlags(lane);

    o_Color = vec4(lane,
                   float(oloGBufferFlagsPbrModel(gbFlags)),
                   float(oloGBufferFlagsMaterialKind(gbFlags)),
                   float(oloGBufferFlagsSkinProfileSlot(gbFlags)));
}
