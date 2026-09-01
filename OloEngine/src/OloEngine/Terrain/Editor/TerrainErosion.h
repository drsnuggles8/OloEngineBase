#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"

namespace OloEngine
{
    class ComputeShader;
    class UniformBuffer;
    class TerrainData;

    struct ErosionSettings
    {
        u32 DropletCount = 70000;        // Number of water droplets per iteration
        u32 MaxDropletSteps = 64;        // Max simulation steps per droplet
        f32 Inertia = 0.05f;             // Direction inertia [0,1]
        f32 SedimentCapacity = 4.0f;     // Sediment capacity multiplier
        f32 MinSedimentCapacity = 0.01f; // Minimum capacity floor
        f32 DepositSpeed = 0.3f;         // Deposit rate [0,1]
        f32 ErodeSpeed = 0.3f;           // Erosion rate [0,1]
        f32 EvaporateSpeed = 0.01f;      // Water evaporation per step [0,1]
        f32 Gravity = 4.0f;              // Gravity constant
        f32 InitialWater = 1.0f;         // Starting water volume
        f32 InitialSpeed = 1.0f;         // Starting droplet speed
        u32 ErosionRadius = 3;           // Brush radius for erosion/deposition (texels)
    };

    // GPU-accelerated hydraulic erosion for terrain heightmaps.
    // Uses a compute shader where each thread simulates one water droplet.
    class TerrainErosion
    {
      public:
        TerrainErosion();

        // Run one iteration of hydraulic erosion on the given terrain.
        //
        // Dispatch only — NOTHING is read back (issue #716). The former
        // per-iteration full-heightmap GetData was a GPU->CPU sync in the middle
        // of an interactive operation, which is what made dragging the iteration
        // slider unusable: the cost was one whole-map stall per iteration, not one
        // per settle. The dispatch marks the CPU mirror stale instead
        // (TerrainData::MarkGPUModified) and the next CPU consumer triggers the
        // single readback in TerrainData::SyncFromGPU.
        void Apply(TerrainData& terrainData, const ErosionSettings& settings);

        // Run multiple iterations (convenience wrapper). Also readback-free, so
        // N iterations cost N dispatches and at most one later sync, rather than N
        // stalls — the difference between watching erosion converge and waiting
        // for it.
        void ApplyIterations(TerrainData& terrainData, const ErosionSettings& settings, u32 iterations);

        // Rebuild the heightmap's mip chain after a batch of iterations. Public so
        // a caller driving Apply() directly (one iteration per frame, as the
        // editor's continuous mode does) can refresh once when it stops rather than
        // once per dispatch.
        static void RegenerateHeightMips(TerrainData& terrainData);

        [[nodiscard]] bool IsReady() const;

      private:
        Ref<ComputeShader> m_ErosionShader;
        // Terrain_Erosion.comp's former bare uniforms (issue #691), at
        // UBO_TERRAIN_EROSION. C++ twin: UBOStructures::TerrainErosionUBO.
        Ref<UniformBuffer> m_ParamsUBO;
        u32 m_IterationSeed = 0;
    };
} // namespace OloEngine
