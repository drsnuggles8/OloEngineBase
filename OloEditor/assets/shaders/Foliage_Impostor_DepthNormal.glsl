// =============================================================================
// Foliage_Impostor_DepthNormal.glsl — the forward depth-normal prepass for the
// octahedral impostor card (issue #1474). The instance card's twin is
// Foliage_Instance_DepthNormal.glsl, which says why the forward prepass has to
// draw foliage at all.
//
// Same coverage as Foliage_Impostor.glsl, by construction:
//   * the vertex stage is the colour pass's own include, with
//     `invariant gl_Position`;
//   * SampleImpostorCard() owns the discard rule, and this program compiles it
//     with OLO_FOLIAGE_IMPOSTOR_ALPHA_BLENDED exactly as the forward colour
//     program does, so the density dither the DEFERRED card applies is skipped
//     here as it is there.
//
// The normal is the forward card's shading normal: the baked object-space
// normal rotated by the instance, turned to face the viewer by the one shared
// two-sided rule, and encoded to scene attachment 2 (include/ViewNormalOutput.glsl).
// =============================================================================

#type vertex
#version 460 core

#define OLO_INSTANCE_NO_FORWARD 1
#include "include/FoliageImpostorVertexStage.glsl"

#type fragment
#version 460 core

// The whole varying contract of the vertex stage (see its header).
layout(location = 0) in vec3 v_CardWorld;
layout(location = 1) in vec3 v_PivotWorld;
layout(location = 2) in float v_MeshCoverage;
layout(location = 3) in vec2 v_LodSeedFade;
layout(location = 4) in float v_AlphaCutoff;
layout(location = 5) in float v_Rotation;
layout(location = 6) in vec3 v_PrevCardWorld;
layout(location = 7) in float v_Radius;
layout(location = 8) in float v_WindDisplacement;

#include "include/CameraCommon.glsl"
#include "include/FoliageParams.glsl"
// The LOBE half only, for oloFoliageFaceNormal: an impostor has no leaf maps.
#include "include/FoliageSurface.glsl"
#include "include/ViewNormalOutput.glsl"

// Foliage_Impostor.glsl's discard rule — see the header.
#define OLO_FOLIAGE_IMPOSTOR_ALPHA_BLENDED 1
#include "include/FoliageImpostorSampling.glsl"

// Scene attachment 2 only (see Foliage_Instance_DepthNormal.glsl).
layout(location = 2) out vec2 o_ViewNormal;

void main()
{
    ImpostorSample card = SampleImpostorCard();

    vec3 geometricN = normalize(rotateY(card.LocalNormal, v_Rotation));
    vec3 V = normalize(u_CameraPosition - v_CardWorld);
    o_ViewNormal = oloForwardViewNormalOutput(u_View, oloFoliageFaceNormal(geometricN, V));
}
