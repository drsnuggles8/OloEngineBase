#pragma once

// Pure selection behind olo_perf_frame_history's 'worst' list (#1533): the
// slowest frames of the profiler's ring, each with its ring position and its
// time split. A hitch is a few frames in a thousand; a percentile says how slow
// they are, and this says what they spent the time on and how often they come.
// The Vulkan editor's 44 ms frames read off as present waits fourteen frames
// apart -- a display-paced swapchain, not rendering.
//
// The handler in McpToolsPerf.cpp copies RendererProfiler's ring into the plain
// struct below inside a MarshalRead and hands it here. The test binary compiles
// this header and NOT McpToolsPerf.cpp: the sibling pattern of McpCpuScopes.h
// and McpPassTimings.h.

#include "OloEngine/Core/Base.h"

#include <algorithm>
#include <cstddef>
#include <vector>

namespace OloEngine::MCP::FrameHistory
{
    // One frame of the ring, engine-free (mirrors RendererProfilerFrameData).
    struct FrameSample
    {
        f64 FrameTimeMs = 0.0;
        f64 CpuMs = 0.0;
        f64 GpuMs = 0.0;
        f64 FenceWaitMs = 0.0;
        f64 PresentWaitMs = 0.0;
        f64 GpuWaitMs = 0.0;
        u32 DrawCalls = 0;
    };

    // The ring positions of the `count` slowest frames, OLDEST FIRST, so the
    // gaps between them are the period. A tie keeps the older frame; a frame
    // with no time yet (a ring still filling) is never one of them.
    [[nodiscard]] inline std::vector<std::size_t> SlowestFrames(const std::vector<FrameSample>& frames,
                                                                std::size_t count)
    {
        std::vector<std::size_t> order;
        order.reserve(frames.size());
        for (std::size_t i = 0; i < frames.size(); ++i)
        {
            if (frames[i].FrameTimeMs > 0.0)
            {
                order.push_back(i);
            }
        }
        const std::size_t k = std::min(count, order.size());
        std::partial_sort(order.begin(), order.begin() + static_cast<std::ptrdiff_t>(k), order.end(),
                          [&frames](std::size_t a, std::size_t b)
                          {
                              if (frames[a].FrameTimeMs > frames[b].FrameTimeMs)
                              {
                                  return true;
                              }
                              if (frames[b].FrameTimeMs > frames[a].FrameTimeMs)
                              {
                                  return false;
                              }
                              return a < b;
                          });
        order.resize(k);
        std::sort(order.begin(), order.end());
        return order;
    }
} // namespace OloEngine::MCP::FrameHistory
