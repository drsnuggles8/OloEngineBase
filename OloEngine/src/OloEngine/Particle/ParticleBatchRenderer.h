#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/Camera/Camera.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Renderer/Mesh.h"

#include <glm/glm.hpp>
#include <span>

namespace OloEngine
{
    class GPUParticleSystem;
    // Per-instance data for particle instanced rendering.
    // NOTE: PascalCase fields are intentional — this struct maps directly to GPU vertex/instance attributes.
    struct ParticleInstance
    {
        glm::vec4 PositionSize;     // xyz = world position, w = size
        glm::vec4 Color;            // rgba
        glm::vec4 UVRect;           // minU, minV, maxU, maxV
        glm::vec4 VelocityRotation; // xyz = velocity, w = rotation (radians)
        f32 StretchFactor;          // 0 = billboard, >0 = stretched (speed * lengthScale)
        int EntityID;               // editor picking
    };

    // Per-instance data for mesh particle rendering (std140 UBO layout).
    // NOTE: PascalCase fields are intentional — this struct maps directly to GPU uniform data.
    struct MeshParticleInstance
    {
        glm::mat4 Model; // 64 bytes
        glm::vec4 Color; // 16 bytes
        glm::ivec4 IDs;  // 16 bytes (x = EntityID, yzw = padding)
    };

    // Per-vertex data for trail quad rendering.
    // NOTE: PascalCase fields are intentional — this struct maps directly to GPU vertex attributes.
    struct TrailVertex
    {
        glm::vec3 Position;
        glm::vec4 Color;
        glm::vec2 TexCoord;
        int EntityID;
    };

    // Soft particle parameters for depth fade
    struct SoftParticleParams
    {
        bool Enabled = false;
        f32 Distance = 1.0f;
        u32 DepthTextureID = 0;
        f32 NearClip = 0.1f;
        f32 FarClip = 1000.0f;
        glm::vec2 ViewportSize = { 1280.0f, 720.0f };
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

        // Set soft particle parameters (call after BeginBatch, before Submit)
        static void SetSoftParticleParams(const SoftParticleParams& params);

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

        // Render mesh particles (standalone call, not part of billboard batching)
        static void RenderMeshParticles(const Ref<Mesh>& mesh,
                                        std::span<const MeshParticleInstance> instances,
                                        const Ref<Texture2D>& texture);

        // Submit a trail quad (4 vertices with positions, colors, UVs)
        static void SubmitTrailQuad(std::span<const glm::vec3, 4> positions,
                                    std::span<const glm::vec4, 4> colors,
                                    std::span<const glm::vec2, 4> texCoords,
                                    int entityID);

        // Set texture for trail rendering (call before SubmitTrailQuad)
        static void SetTrailTexture(const Ref<Texture2D>& texture);

        // Flush pending trail quads
        static void FlushTrails();

        // Render GPU particles using indirect draw (SSBO-based, no CPU instance data)
        static void RenderGPUBillboards(GPUParticleSystem& gpuSystem,
                                        const Ref<Texture2D>& texture,
                                        int entityID = -1);

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
} // namespace OloEngine
