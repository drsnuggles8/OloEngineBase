#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"

namespace OloEngine
{
    class ComputeShader;
    class TerrainData;
    class TerrainChunkManager;

    struct ErosionSettings
    {
        u32 DropletCount = 70000;          // Number of water droplets per iteration
        u32 MaxDropletSteps = 64;          // Max simulation steps per droplet
        f32 Inertia = 0.05f;               // Direction inertia [0,1]
        f32 SedimentCapacity = 4.0f;       // Sediment capacity multiplier
        f32 MinSedimentCapacity = 0.01f;   // Minimum capacity floor
        f32 DepositSpeed = 0.3f;           // Deposit rate [0,1]
        f32 ErodeSpeed = 0.3f;             // Erosion rate [0,1]
        f32 EvaporateSpeed = 0.01f;        // Water evaporation per step [0,1]
        f32 Gravity = 4.0f;                // Gravity constant
        f32 InitialWater = 1.0f;           // Starting water volume
        f32 InitialSpeed = 1.0f;           // Starting droplet speed
        i32 ErosionRadius = 3;             // Brush radius for erosion/deposition (texels)
    };

    // GPU-accelerated hydraulic erosion for terrain heightmaps.
    // Uses a compute shader where each thread simulates one water droplet.
    class TerrainErosion
    {
      public:
        TerrainErosion();

        // Run one iteration of hydraulic erosion on the given terrain.
        // After dispatch, reads back the GPU heightmap to the CPU buffer.
        void Apply(TerrainData& terrainData, const ErosionSettings& settings);

        // Run multiple iterations (convenience wrapper)
        void ApplyIterations(TerrainData& terrainData, const ErosionSettings& settings, u32 iterations);

        [[nodiscard]] bool IsReady() const;

      private:
        Ref<ComputeShader> m_ErosionShader;
        u32 m_IterationSeed = 0;
    };
} // namespace OloEngine
