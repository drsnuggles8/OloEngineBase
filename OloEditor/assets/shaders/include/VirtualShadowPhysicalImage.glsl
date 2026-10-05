#ifndef OLO_VIRTUAL_SHADOW_PHYSICAL_IMAGE_GLSL
#define OLO_VIRTUAL_SHADOW_PHYSICAL_IMAGE_GLSL
#include "VirtualShadowResources.glsl"
#include "BindlessHeap.glsl"
#ifdef OLO_BINDLESS
// Heap image variables are function scoped. Both raster writers instantiate
// this after the consuming program has received its physical-pool offset.
#define OLO_VSM_PHYSICAL_IMAGE() OLO_HEAP_IMAGE(r32ui, coherent, uimage2D, u_VSMPhysicalPages, 0)
#else
layout(r32ui, binding = 0) uniform coherent uimage2D u_VSMPhysicalPages;
#define OLO_VSM_PHYSICAL_IMAGE()
#endif
#endif
