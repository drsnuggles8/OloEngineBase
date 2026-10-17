#pragma once

// The ShadowPass GPU attribution vocabulary (#1590).
//
// ShadowPass is the largest GPU pass at every outdoor camera, and until #1590
// only its foliage was bracketed, so its cost sat outside every sub-bracket.
// Each VIEW FAMILY now opens a region bracket and each CASTER CLASS inside it
// a nested one, published by GPUPassTimerPool as
//
//     ShadowPass/<Family>             e.g. ShadowPass/Atlas
//     ShadowPass/<Family>/<Class>     e.g. ShadowPass/Atlas/MeshCasters
//
// A class bracket is opened once per VIEW (a cascade, an atlas face, a clip
// level), so a frame publishes the same name many times; a consumer sums the
// repeats of a name within one frame (scripts/perf/acceptance-benchmark.py).
//
// Class brackets are skipped inside a parallel recording item: the pool is
// not thread-safe. On Vulkan the CSM and atlas regions fork, so there only
// their region brackets (and the region-level foliage cull) are published;
// on GL every item records inline and the classes are complete. The VSM route
// records sequentially on both backends.

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Debug/GPUPassTimerPool.h"
#include "OloEngine/Renderer/RenderCommand.h"

#include <string>

namespace OloEngine::ShadowBrackets
{
    // View families: the region brackets.
    inline constexpr const char* kCsm = "Csm";
    inline constexpr const char* kAtlas = "Atlas";
    inline constexpr const char* kVsmPages = "VsmPages"; // invalidation + page allocation/clear
    inline constexpr const char* kVsmCull = "VsmCull";   // the GPU mesh cull, sun and local layers
    inline constexpr const char* kVsmSun = "VsmSun";     // the sixteen directional clip levels
    inline constexpr const char* kVsmLocal = "VsmLocal"; // the local-light layers

    // Caster classes: nested inside a family.
    inline constexpr const char* kMeshCasters = "MeshCasters";
    inline constexpr const char* kSkinnedCasters = "SkinnedCasters";
    inline constexpr const char* kVoxelCasters = "VoxelCasters";
    inline constexpr const char* kGroomCasters = "GroomCasters";
    inline constexpr const char* kTerrainCasters = "TerrainCasters";
    inline constexpr const char* kFoliageCull = "FoliageCull";
    inline constexpr const char* kFoliageCasters = "FoliageCasters";
    inline constexpr const char* kVirtualGeometry = "VirtualGeometry";

    // One GPU bracket for a scope, or none inside a parallel item or when
    // `active` is false (a class with nothing to draw publishes nothing).
    class Scope
    {
      public:
        explicit Scope(const char* name, bool active = true)
        {
            if (!active || RenderCommand::IsRecordingParallelItem())
                return;
            m_Pool = &GPUPassTimerPool::GetInstance();
            m_Pool->BeginSubPass(name);
        }
        ~Scope()
        {
            if (m_Pool != nullptr)
                m_Pool->EndSubPass();
        }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
        Scope(Scope&&) = delete;
        Scope& operator=(Scope&&) = delete;

      private:
        GPUPassTimerPool* m_Pool = nullptr;
    };
} // namespace OloEngine::ShadowBrackets
