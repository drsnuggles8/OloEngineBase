// The scene access every ReSTIR GI draw needs, in the ONE order that compiles:
// the GPU Scene tables, the parameter block, the shared ray-hit machinery, the
// shared G-Buffer surface, the GI reservoir and the shared estimator.
// Issue #1169.
//
// WHY IT IS A FILE AND NOT FOUR COPIES. The order is load-bearing and not
// obvious: include/RayTracedSurfaceHit.glsl reads the slot counts, the TLAS
// address and the material-texture table out of the parameter block, so the
// block must precede it; it then pulls in LightSampling.glsl and
// RayTracingAlphaTest.glsl itself, in the only order those two accept; and
// ReSTIRGICommon.glsl needs all of it plus the reservoir. Four shaders each
// re-deriving that order is four chances to get it subtly wrong — and "subtly
// wrong" here means one draw's rays alpha-test masked geometry and another's do
// not, which is a light leak on masked geometry that only shows up in a scene
// with foliage. Same arrangement, and the same reason, as
// include/ReSTIRDISceneAccess.glsl.
//
// The caller must have declared its own attachments and included
// include/BindlessHeap.glsl and include/PBRCommon.glsl first, and must have
// emitted the ray-query / buffer-reference / descriptor-heap #extension
// directives before any other token.
#ifndef OLO_RESTIR_GI_SCENE_ACCESS_GLSL
#define OLO_RESTIR_GI_SCENE_ACCESS_GLSL

#include "GPUScene.glsl"
#include "GPUSceneInstances.glsl"
#include "GPUSceneGeometries.glsl"
#include "GPUSceneMaterials.glsl"
#include "GPUSceneLights.glsl"
#include "PathTracerSampler.glsl"
#include "DescriptorHeapTextures.glsl"
#include "ReservoirGI.glsl"
#include "ReSTIRGIParams.glsl"

// What the scene looks like along a ray — the SAME code the reference path
// tracer uses, not a second copy of it (see the file's own header for why that
// is a requirement rather than tidiness).
#define OLO_RT_HIT_TLAS_ADDRESS u_TlasAddressAndFrame.xy
#define OLO_RT_HIT_INSTANCE_MASK u_TlasAddressAndFrame.z
#define OLO_RT_HIT_SLOT_COUNTS u_SlotCounts
#define OLO_RT_HIT_MATERIAL_TABLE u_MaterialTable.xy
#define OLO_RT_HIT_MATERIAL_COUNT u_MaterialTable.z
#define OLO_RT_HIT_SAMPLER_HEAP u_MaterialTable.w
#define OLO_RT_HIT_TEXTURES_REQUESTED ((u_EmissiveTable.w & OLO_RESTIR_GI_FLAG_TEXTURES) != 0u)
#include "RayTracedSurfaceHit.glsl"

#include "GBufferRaySurface.glsl"
#include "ReSTIRGICommon.glsl"

#endif // OLO_RESTIR_GI_SCENE_ACCESS_GLSL
