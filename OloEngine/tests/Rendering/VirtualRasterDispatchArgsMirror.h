#pragma once

// =============================================================================
// CPU mirror of OloEditor/assets/shaders/include/VirtualRasterDispatchArgs.glsl
// — the virtual-geometry software rasterizer's record-count -> workgroup-grid
// flattening (issue #1048).
//
// Two consumers, so the mirror is a header rather than a local copy in either:
//   * VirtualRasterDispatchArgsTest.cpp — pins the rule itself against the
//     raster's own index recovery, with no GL context (so it runs in CI);
//   * ShaderUnitTests.cpp — dispatches the SHIPPED include through
//     tests/ShaderUnit_VirtualRasterArgs.comp and compares against this, which
//     is what catches the GLSL and the mirror drifting apart.
//
// Line-for-line with the GLSL. If you change one, change both.
// =============================================================================

#include "OloEngine/Core/Base.h"

namespace OloEngine::Tests::VirtualRasterDispatchArgs
{
    // Mirror of kOloVirtualRasterMaxGroupsX.
    inline constexpr u32 kMaxGroupsX = 4096u;

    struct DispatchArgs
    {
        u32 X{ 0 };
        u32 Y{ 0 };
        u32 Z{ 0 };
    };

    // Mirror of OloVirtualRasterDispatchArgs.
    inline DispatchArgs FromCount(u32 count)
    {
        u32 const groupsX = count < kMaxGroupsX ? count : kMaxGroupsX;
        u32 const divisor = groupsX > 1u ? groupsX : 1u;
        u32 const groupsY = (count + divisor - 1u) / divisor;
        return { groupsX, groupsY, 1u };
    }

    // Mirror of VirtualClusterRaster.comp's main():
    //     swRecordIndex = gl_WorkGroupID.x + gl_WorkGroupID.y * gl_NumWorkGroups.x
    // This is the INVERSE the flattening has to satisfy, and the reason the two
    // must be read together: the grid is only correct relative to how the raster
    // turns a workgroup back into a record.
    inline u32 RecordIndexOf(u32 workgroupX, u32 workgroupY, u32 numGroupsX)
    {
        return workgroupX + workgroupY * numGroupsX;
    }
} // namespace OloEngine::Tests::VirtualRasterDispatchArgs
