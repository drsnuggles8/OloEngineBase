// =============================================================================
// Foliage_Instance_DepthNormal.glsl — the forward depth-normal prepass for the
// instanced foliage draw (issue #1474).
//
// Forward and Forward+ apply screen-space AO to a surface's ambient term only
// if the forward depth-normal prepass drew that surface (issue #1452), because
// at any other pixel the AO buffer holds the occlusion of whatever lies behind
// it. FoliagePrepassPass replays the foliage bucket through this program ahead
// of the AO passes, so the AO buffer sees the leaves; Foliage_Instance.glsl then
// draws the same leaves in colour at GL_LEQUAL and applies that AO.
//
// It must carve EXACTLY the colour pass's coverage, or the colour pass loses
// edge fragments to depth it did not write (CommandDispatch::DrawFoliageLayer's
// note). So:
//   * the vertex stage is the colour pass's own include, with
//     `invariant gl_Position` — same placement, same wind, same depth;
//   * the fragment discards are Foliage_Instance.glsl's three, in its order:
//     the mesh-to-card hand-over, the cutout, and the distance fade at 0.
//     (Forward foliage is opaque alpha-tested with no blend, so the fade only
//     ever discards at 0 there; Foliage_Instance_GBuffer.glsl's 0.3 cut and
//     density dither are the DEFERRED rule and do not apply here.)
//
// The normal is the one the G-Buffer twin stores: oloFoliageSampleSurface's
// viewer-facing, leaf-normal-mapped normal, encoded to scene attachment 2 the
// way every forward writer encodes it (include/ViewNormalOutput.glsl).
// =============================================================================

#type vertex
#version 460 core

// This program's fragment stage never reads v_InstanceIndex, like the colour
// program's (a written-but-unconsumed output is a Vulkan interface warning).
#define OLO_INSTANCE_NO_FORWARD 1
#include "include/FoliageInstanceVertexStage.glsl"

#type fragment
#version 460 core

// The whole varying contract of the vertex stage, read or not, so no output
// the stage writes goes unconsumed.
layout(location = 0) in vec3 v_WorldPos;
layout(location = 1) in vec3 v_Normal;
layout(location = 2) in vec2 v_TexCoord;
layout(location = 3) in vec3 v_Color;
layout(location = 4) in float v_AlphaCutoff;
layout(location = 5) in float v_Fade;
layout(location = 6) in vec3 v_PrevWorldPos;
layout(location = 7) in float v_MeshCoverage;
layout(location = 8) in float v_InstanceSeed;

#include "include/CameraCommon.glsl"
#include "include/FoliageParams.glsl"
#include "include/FoliageInstanceGeometry.glsl"

#include "include/BindlessHeap.glsl"
#ifdef OLO_BINDLESS
#define u_DiffuseTexture OLO_HEAP_TEX_2D(0)    // TEX_DIFFUSE
#define u_LeafNormalMap OLO_HEAP_TEX_2D(2)     // TEX_NORMAL
#define u_LeafRoughnessMap OLO_HEAP_TEX_2D(6)  // TEX_ROUGHNESS
#define u_LeafThicknessMap OLO_HEAP_TEX_2D(7)  // TEX_METALLIC (repurposed)
#else
layout(binding = 0) uniform sampler2D u_DiffuseTexture;
layout(binding = 2) uniform sampler2D u_LeafNormalMap;     // TEX_NORMAL
layout(binding = 6) uniform sampler2D u_LeafRoughnessMap;  // TEX_ROUGHNESS
layout(binding = 7) uniform sampler2D u_LeafThicknessMap;  // TEX_METALLIC (repurposed: thickness)
#endif

#include "include/PBRCommon.glsl"
#define OLO_FOLIAGE_SURFACE_SAMPLING 1
#include "include/FoliageSurface.glsl"
#include "include/ViewNormalOutput.glsl"

// Scene attachment 2 only. CommandDispatch masks every other attachment during
// the prepass and opens this one for the depth-normal programs.
layout(location = 2) out vec2 o_ViewNormal;

void main()
{
    // Foliage_Instance.glsl's discards, in its order.
    if (!foliageLodKeep(u_MeshParams.x > 0.5, v_MeshCoverage, gl_FragCoord.xy, v_InstanceSeed,
                        foliageStochasticCoverage(u_LodTransition0)))
        discard;

    vec4 texColor = texture(u_DiffuseTexture, v_TexCoord);
    if (texColor.a < v_AlphaCutoff)
        discard;

    float dist = distance(v_WorldPos, u_CameraPosition);
    float fadeFactor = 1.0 - smoothstep(u_FadeStart, u_ViewDistance, dist);
    if (fadeFactor <= 0.0)
        discard;

    vec3 V = normalize(u_CameraPosition - v_WorldPos);
    OloFoliageSurface leaf = oloFoliageSampleSurface(v_WorldPos, v_Normal, v_TexCoord, V, v_Color, texColor);
    o_ViewNormal = oloForwardViewNormalOutput(u_View, leaf.Normal);
}
