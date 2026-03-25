#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/ComputeShader.h"
#include "OloEngine/Renderer/Texture.h"

namespace OloEngine
{
    // @brief Generates a Hierarchical Z-Buffer (HZB) depth pyramid via compute.
    //
    // Creates a power-of-2 R32F texture with a full mip chain from the scene
    // depth buffer.  Each mip stores the max depth of a 2x2 quad from the
    // parent level, enabling fast conservative occlusion queries.
    //
    // Used by GTAO for adaptive mip-level sampling and future SSR for
    // hierarchical ray-marching.
    //
    // Dispatch pattern: 4 mips per dispatch via shared memory reduction,
    // iterating until all mips are generated.
    class HZBGenerator
    {
      public:
        HZBGenerator() = default;
        ~HZBGenerator() = default;

        void Initialize();
        void Shutdown();
        void Reload();

        // Resize the HZB texture to match the viewport.
        // Internally computes the next power-of-2 dimensions.
        void Resize(u32 viewportWidth, u32 viewportHeight);

        // Generate the HZB from a scene depth texture.
        void Generate(u32 sceneDepthTextureID);

        [[nodiscard]] bool IsValid() const;

        // Access the HZB texture (for binding by GTAO / SSR).
        [[nodiscard]] u32 GetHZBTextureID() const;
        [[nodiscard]] u32 GetMipCount() const;

        // UV factor to map viewport coordinates to HZB coordinates:
        // hzbUV = screenUV * UVFactor
        [[nodiscard]] glm::vec2 GetUVFactor() const { return m_UVFactor; }

      private:
        [[nodiscard]] static u32 NextPowerOfTwo(u32 v);
        void DispatchMipBatch(u32 startMip, u32 sceneDepthTextureID);

        Ref<ComputeShader> m_HZBShader;
        Ref<Texture2D> m_HZBTexture;

        u32 m_HZBWidth = 0;
        u32 m_HZBHeight = 0;
        u32 m_MipCount = 0;
        glm::vec2 m_UVFactor{ 1.0f };
        u32 m_ViewportWidth = 0;
        u32 m_ViewportHeight = 0;
    };
} // namespace OloEngine
