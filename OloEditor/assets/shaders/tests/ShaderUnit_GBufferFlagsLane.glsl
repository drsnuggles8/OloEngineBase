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
// decoding the material kind, profile slot and unlit bit.
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
    int kind = int(gl_FragCoord.x);
    int slot = int(gl_FragCoord.y);
    // Encode exactly as every G-Buffer writer does ...
    float lane = oloEncodeGBufferPbrFlagsEx(kind, slot);
    // ... quantise through fp16, which is what writing RGBA16F does ...
    lane = unpackHalf2x16(packHalf2x16(vec2(lane, 0.0))).x;
    // ... and decode exactly as ComputeDeferredLit does.
    int gbFlags = oloDecodeGBufferFlags(lane);

    o_Color = vec4(lane,
                   float(oloGBufferFlagsAreUnlit(gbFlags)),
                   float(oloGBufferFlagsMaterialKind(gbFlags)),
                   float(oloGBufferFlagsSkinProfileSlot(gbFlags)));
}
