#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/Camera/Camera.h"
#include "OloEngine/Renderer/Texture.h"

#include <glm/glm.hpp>

namespace OloEngine
{
    // Per-instance data for particle instanced rendering
    struct ParticleInstance
    {
        glm::vec4 PositionSize;       // xyz = world position, w = size
        glm::vec4 Color;              // rgba
        glm::vec4 UVRect;             // minU, minV, maxU, maxV
        glm::vec4 VelocityRotation;   // xyz = velocity, w = rotation (radians)
        f32 StretchFactor;            // 0 = billboard, >0 = stretched (speed * lengthScale)
        int EntityID;                 // editor picking
    };

    // Instanced particle batch renderer with GPU-side billboarding
    class ParticleBatchRenderer
    {
    public:
        static void Init();
        static void Shutdown();

        // Begin a new batch with camera data for GPU billboarding
        static void BeginBatch(const EditorCamera& camera);
        static void BeginBatch(const Camera& camera, const glm::mat4& cameraTransform);

        // Submit a billboard particle
        static void Submit(const glm::vec3& position, f32 size, f32 rotation,
                           const glm::vec4& color, const glm::vec4& uvRect,
                           int entityID);

        // Submit a stretched billboard particle
        static void SubmitStretched(const glm::vec3& position, f32 size,
                                    const glm::vec3& velocity, f32 stretchFactor,
                                    const glm::vec4& color, const glm::vec4& uvRect,
                                    int entityID);

        // Set texture for upcoming submissions (flushes if texture changes)
        static void SetTexture(const Ref<Texture2D>& texture);

        // End batch, flush remaining instances
        static void EndBatch();

        struct Statistics
        {
            u32 DrawCalls = 0;
            u32 InstanceCount = 0;
        };
        static void ResetStats();
        [[nodiscard]] static Statistics GetStats();

        // Flush pending instances (draw call) without ending the batch.
        // Call before GL state changes (blend mode) that affect rendering.
        static void Flush();
    private:
        static void StartNewBatch();
    };
}
