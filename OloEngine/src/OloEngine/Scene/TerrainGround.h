#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Containers/Array.h"

#include <glm/glm.hpp>

#include <optional>

namespace OloEngine
{
    class Scene;
    class TerrainData;

    // The world height of the terrain surface under (x, z), from the first
    // TerrainComponent whose loaded height field covers that point; nullopt when
    // none does (off every terrain, or no height field built yet). The field is
    // sampled in the terrain entity's own space, the mapping the nav-mesh bake
    // and the terrain chunks use, so a moved or Y-turned terrain answers
    // correctly. The height field is built by the rendered scene update, so a
    // headless scene has none. Issue #1259: ground-following animals and the
    // acceptance scene's camera-clearance contract both read it.
    //
    // The sampler gathers the scene's terrains and their inverse world matrices
    // once, so a caller sampling many points in one tick (a herd) pays one
    // inversion per terrain rather than one per animal per terrain. It holds
    // raw pointers into the terrains' height fields: use it within the tick
    // that built it.
    class TerrainGroundSampler
    {
      public:
        explicit TerrainGroundSampler(const Scene& scene);

        [[nodiscard]] std::optional<f32> HeightAt(f32 x, f32 z) const;
        [[nodiscard]] bool IsEmpty() const
        {
            return m_Terrains.IsEmpty();
        }

      private:
        struct Terrain
        {
            const TerrainData* Data = nullptr;
            glm::mat4 World{ 1.0f };
            glm::mat4 InverseWorld{ 1.0f };
            f32 SizeX = 0.0f;
            f32 SizeZ = 0.0f;
            f32 HeightScale = 1.0f;
        };
        TArray<Terrain> m_Terrains;
    };

    [[nodiscard]] std::optional<f32> SampleTerrainGroundHeight(const Scene& scene, f32 x, f32 z);
} // namespace OloEngine
