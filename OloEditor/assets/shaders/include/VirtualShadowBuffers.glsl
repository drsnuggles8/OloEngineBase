#ifndef OLO_VIRTUAL_SHADOW_BUFFERS_GLSL
#define OLO_VIRTUAL_SHADOW_BUFFERS_GLSL

// =============================================================================
// VirtualShadowBuffers.glsl — the working set shared by the VSM compute kernels
// (issue #702). Declared in ONE place because the kernels form a pipeline: the
// marker's request records are the allocator's input, the allocator's dirty bits
// are the HPB's input, and a layout that drifted between two of them would
// corrupt page ownership rather than fail to compile.
//
// The lit pass and the depth raster deliberately do NOT include this — they need
// the page table and the physical pool only, which come from
// VirtualShadowResources.glsl.
// =============================================================================

#include "VirtualShadowResources.glsl"

// Physical page -> its owning virtual page (or unallocated). One entry per
// physical page; see the VSM_META_* encoding in VirtualShadowCommon.glsl.
layout(std430, binding = 59) coherent buffer VSMMetaTable { uint b_MetaTable[]; };

// Hierarchical Page Buffer: VSM_HPB_MIP_COUNT mips of the DIRTY flag per clip
// level, packed by vsmHPBIndex().
layout(std430, binding = 60) coherent buffer VSMHierarchicalPageBuffer { uint b_HPB[]; };

// Allocation requests appended by the marker, consumed by the allocator.
// x = clip level, y = wrapped page X, z = wrapped page Y, w = unused.
layout(std430, binding = 61) coherent buffer VSMRequests
{
    uint b_RequestCount;
    uint b_RequestConsumed;
    uint _vsmReqPad0;
    uint _vsmReqPad1;
    uvec4 b_Requests[];
};

// Two free lists in one buffer, because only the LAST member of an SSBO may be
// unsized. Entries [0, physicalPageCount) are genuinely free pages; entries
// [physicalPageCount, 2 * physicalPageCount) are resident pages whose owner was
// NOT referenced this frame and may therefore be evicted. The allocator drains
// the first list before touching the second, so eviction only ever happens under
// real pressure.
layout(std430, binding = 62) coherent buffer VSMFreePages
{
    uint b_FreeCount;
    uint b_NotVisitedCount;
    uint b_FreeConsumed;
    uint _vsmFreePad0;
    uint b_FreeList[];
};

// Dynamic-caster bounds submitted this frame; pairs of (min.xyz, max.xyz).
layout(std430, binding = 64) readonly buffer VSMInvalidations
{
    uint b_InvalidationCount;
    uint _vsmInvPad0;
    uint _vsmInvPad1;
    uint _vsmInvPad2;
    vec4 b_InvalidationBounds[];
};

// Cull input — C++ twin VSM::CullInstance.
struct VSMCullInstance
{
    mat4 Transform;
    vec4 BoundsMin;
    vec4 BoundsMax;
    uvec4 Batch; // x = batch index, y = run base, z = run capacity, w = unbounded flag
};
layout(std430, binding = 65) readonly buffer VSMCullInstances { VSMCullInstance b_CullInstances[]; };

// Cull output. Declared in its own header because the DEPTH RASTER reads the
// very same buffer from a vertex stage — sharing the declaration is what stops
// the producer and the consumer from disagreeing about the record layout.
#include "VirtualShadowDrawList.glsl"

// GL DrawElementsIndirectCommand, one per caster batch. Only InstanceCount is
// GPU-written.
struct VSMDrawCommand
{
    uint IndexCount;
    uint InstanceCount;
    uint FirstIndex;
    uint BaseVertex;
    uint BaseInstance;
};
layout(std430, binding = 67) coherent buffer VSMDrawCommands { VSMDrawCommand b_DrawCommands[]; };

// C++ twin VSM::Statistics. Zeroed at the top of each frame's page update and
// read back one frame late, so nothing here ever stalls the GPU.
layout(std430, binding = 68) coherent buffer VSMStatistics
{
    uint b_PagesRequested;
    uint b_PagesAllocated;
    uint b_PagesFailed;
    uint b_PagesDrawn;
    uint b_PagesResident;
    uint b_PagesFreed;
    uint b_StatDrawInstances; // NOT b_DrawInstances — that name is the cull's output buffer
    uint b_CullOverflows;
};

int vsmPhysicalPageCount()
{
    return VSM_PHYSICAL_PAGE_TABLE_RES * VSM_PHYSICAL_PAGE_TABLE_RES;
}

#endif // OLO_VIRTUAL_SHADOW_BUFFERS_GLSL
