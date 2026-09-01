#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/ComputeShader.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Renderer/UniformBuffer.h"
#include "OloEngine/Terrain/Editor/TerrainBrush.h"
#include "OloEngine/Terrain/Editor/TerrainBrushUtils.h"
#include "OloEngine/Terrain/Editor/TerrainPaintBrush.h"

#include <glm/glm.hpp>

namespace OloEngine
{
    class TerrainData;
    class TerrainMaterial;

    // GPU-resident terrain authoring brush (issue #716).
    //
    // Replaces the per-texel CPU loops in TerrainBrush / TerrainPaintBrush with a
    // compute dispatch straight into the heightmap and splatmap images. The point
    // is not raw throughput: it is that the MAIN-THREAD cost of a stroke frame
    // becomes one UBO refill plus one dispatch, constant in the brush radius,
    // where the CPU brushes were O(radius^2) on the thread that also has to
    // produce the frame.
    //
    // Neither entry point reads anything back. Both mark the CPU mirror stale
    // (TerrainData::MarkGPUModified) and leave the single readback to
    // TerrainData::SyncFromGPU, which the next CPU consumer triggers.
    class TerrainGPUBrush
    {
      public:
        TerrainGPUBrush();

        // Both kernels compiled and usable. False on a headless / GL-less session,
        // where callers are expected to fall back to the CPU brushes.
        [[nodiscard]] bool IsReady() const;
        [[nodiscard]] bool IsSculptReady() const;
        [[nodiscard]] bool IsPaintReady() const;

        // Sculpt the heightmap in place. `targetHeight` is the normalized height
        // the Flatten / Level tools converge on; the caller captures it ONCE when
        // the stroke starts. That is a deliberate behaviour change from the CPU
        // brush, which re-sampled the height under the cursor every frame: doing
        // that here would mean a GPU->CPU height query per stroke frame, which is
        // the exact readback this issue removes. Capturing on press is also what
        // "level to where I clicked" already meant.
        TerrainBrush::DirtyRegion ApplySculpt(TerrainData& terrainData,
                                              const TerrainBrushSettings& settings,
                                              const glm::vec3& worldPos,
                                              f32 worldSizeX, f32 worldSizeZ, f32 heightScale,
                                              f32 deltaTime, f32 targetHeight);

        // Paint into the splatmap pair in place.
        TerrainPaintBrush::DirtyRegion ApplyPaint(TerrainMaterial& material,
                                                  const TerrainPaintSettings& settings,
                                                  const glm::vec3& worldPos,
                                                  f32 worldSizeX, f32 worldSizeZ,
                                                  f32 deltaTime);

      private:
        // Grow the scratch heightmap to hold at least width x height texels.
        // Returns false if the texture could not be created.
        bool EnsureScratch(u32 width, u32 height);

        Ref<ComputeShader> m_SculptShader;
        Ref<ComputeShader> m_PaintShader;
        // Shared std140 params block at ShaderBindingLayout::UBO_TERRAIN_BRUSH,
        // refilled immediately before each dispatch (issue #691 pattern).
        Ref<UniformBuffer> m_ParamsUBO;

        // Pre-stroke copy of the sculpt rect, so the Smooth tool's neighbour reads
        // are order-independent. The CPU brush copied the WHOLE heightmap into a
        // std::vector for this on every apply; here it is a rect-sized GPU-to-GPU
        // copy, and only the sculpt path pays for it.
        Ref<Texture2D> m_HeightScratch;
        u32 m_ScratchSize = 0;
    };
} // namespace OloEngine
