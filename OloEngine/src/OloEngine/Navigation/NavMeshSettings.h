#pragma once

#include "OloEngine/Core/Base.h"

namespace OloEngine
{
    struct NavMeshSettings
    {
        f32 CellSize = 0.3f;
        f32 CellHeight = 0.2f;
        f32 AgentRadius = 0.5f;
        f32 AgentHeight = 2.0f;
        f32 AgentMaxClimb = 0.9f;
        f32 AgentMaxSlope = 45.0f;
        i32 RegionMinSize = 8;
        i32 RegionMergeSize = 20;
        f32 EdgeMaxLen = 12.0f;
        f32 EdgeMaxError = 1.3f;
        i32 VertsPerPoly = 6;
        f32 DetailSampleDist = 6.0f;
        f32 DetailSampleMaxError = 1.0f;
    };
} // namespace OloEngine
