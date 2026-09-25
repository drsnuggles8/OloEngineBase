// =============================================================================
// ShaderUnit_DiffusionHandoffLane.glsl
//
// Round-trips the diffusion hand-off lane's alpha (scene attachment 4) through
// the PRODUCTION encode/decode helpers — skin's (include/SkinDiffusionCommon.glsl)
// and snow's (include/SnowDiffusionCommon.glsl, issue #1451). One lane, two
// disjoint ranges: a skin slot s is (s + 1) / 8, a snow weight w is -w, the
// cleared value is 0. Every encoded value is pushed through packHalf2x16 /
// unpackHalf2x16 — the RGBA16F attachment's quantisation — and then decoded by
// BOTH decoders, so the test pins that each reads its own range and reads the
// other's as "not mine", in both directions.
//
// Parameterization:
//   x = the snow weight x / 63 (a 64-wide probe), used on the snow row
//   y = 0            -> the snow row, encodes -w
//       1 .. 7       -> skin slot y - 1
//       8            -> the cleared lane, 0
//
// Output:
//   .r = the lane value after fp16 storage
//   .g = oloSkinDiffusionSlot(lane)       (7 == "names no profile")
//   .b = oloSnowDiffusionWeight(lane)     (0 == "not snow")
//   .a = 1 when the snow side counts it as snow (weight > OLO_SNOW_MIN_WEIGHT)
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

#include "include/SkinDiffusionCommon.glsl"
#include "include/SnowDiffusionCommon.glsl"

// The probe is 64 texels wide (DiffusionHandoffLaneTest).
const int kProbeWidth = 64;

void main()
{
    int column = int(gl_FragCoord.x);
    int row = int(gl_FragCoord.y);
    float weight = float(column) / float(kProbeWidth - 1);

    float lane = 0.0;
    if (row == 0)
        lane = oloSnowDiffusionEncodeWeight(weight);
    else if (row <= OLO_SKIN_DIFFUSE_SLOT_NONE)
        lane = oloSkinDiffusionEncodeSlot(row - 1);

    // The RGBA16F attachment's quantisation.
    lane = unpackHalf2x16(packHalf2x16(vec2(lane, 0.0))).x;

    float snowWeight = oloSnowDiffusionWeight(lane);
    o_Color = vec4(lane, float(oloSkinDiffusionSlot(lane)), snowWeight,
                   snowWeight > OLO_SNOW_MIN_WEIGHT ? 1.0 : 0.0);
}
