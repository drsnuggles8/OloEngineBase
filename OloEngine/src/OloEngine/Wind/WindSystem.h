#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Core/Timestep.h"
#include "OloEngine/Renderer/PostProcessSettings.h"
#include <glm/glm.hpp>

namespace OloEngine
{
    class ComputeShader;
    class Texture3D;
    class UniformBuffer;

    /**
     * @brief Engine-wide wind simulation system.
     *
     * Generates a 3D vector-field (128³ RGBA16F texture) on the GPU via a
     * compute shader every frame.  The field encodes wind velocity at each
     * voxel and is consumed by:
     *   - GPU particle simulation  (trilinear sample in Particle_Simulate.comp)
     *   - Foliage animation        (sample at blade root world pos)
     *   - Snow coverage drift      (sample in SnowCommon.glsl)
     *
     * The grid is axis-aligned and centered on the camera position each frame.
     *
     * Usage:
     *   WindSystem::Init();
     *   ...
     *   WindSystem::Update(settings, cameraPosition, dt);
     *   WindSystem::BindWindTexture();   // before any consumer dispatch
     *   ...
     *   WindSystem::Shutdown();
     */
    class WindSystem
    {
    public:
        /// Initialize GPU resources (compute shader, 3D texture, UBO).
        static void Init();

        /// Release GPU resources.
        static void Shutdown();

        /// @return true after Init() succeeds.
        [[nodiscard]] static bool IsInitialized();

        /**
         * @brief Regenerate the wind field for this frame.
         *
         * Dispatches the Wind_Generate compute shader which writes wind
         * velocity into every voxel of the 3D texture, then uploads the
         * WindUBOData so that consumers can locate and sample the field.
         *
         * @param settings   Current scene-level wind parameters.
         * @param cameraPos  Camera world position (grid is re-centered here).
         * @param dt         Frame delta time (for gust phase accumulation).
         */
        static void Update(const WindSettings& settings, const glm::vec3& cameraPos, Timestep dt);

        /// Bind the 3D wind-field texture to TEX_WIND_FIELD (slot 29).
        static void BindWindTexture();

        /**
         * @brief CPU-side wind query (approximate).
         *
         * Returns the base directional wind + gust at the given point.
         * Does NOT read back from the GPU texture — uses the analytical
         * model only (direction × speed + gust sine). Suitable for
         * audio, gameplay, or scripting queries where precision is secondary.
         *
         * @param settings  Current wind parameters.
         * @param worldPos  Query position.
         * @param time      Current elapsed time.
         * @return Approximate wind velocity vector at worldPos.
         */
        [[nodiscard]] static glm::vec3 GetWindAtPoint(const WindSettings& settings,
                                                       const glm::vec3& worldPos,
                                                       f32 time);

    private:
        struct WindSystemData
        {
            Ref<ComputeShader> GenerateShader;
            Ref<Texture3D> WindField;        // 128³ RGBA16F
            Ref<UniformBuffer> WindUBO;       // binding 15
            WindUBOData GPUData;

            f32 AccumulatedTime = 0.0f;
            bool Initialized = false;
        };

        static WindSystemData s_Data;
    };
} // namespace OloEngine
