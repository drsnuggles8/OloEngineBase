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
    // depth buffer.  Each mip stores either the max (farthest) or the min
    // (nearest) depth of a 2x2 quad from the parent level, selected by
    // SetReduceMode():
    //   * Max — conservative occlusion queries (GTAO horizon integration).
    //   * Min — front-to-back hierarchical-Z SSR traversal (#284), where each
    //     coarse cell's nearest surface tells the ray whether the whole cell can
    //     be skipped.
    //
    // Dispatch pattern: 4 mips per dispatch via shared memory reduction,
    // iterating until all mips are generated.
    class HZBGenerator
    {
      public:
        // Per-quad reduction operator. Must match u_ReduceOp in HZB.comp.
        enum class ReduceMode : i32
        {
            Max = 0, // Farthest surface — GTAO occlusion.
            Min = 1, // Nearest surface — HiZ SSR traversal.
        };

        // Power-of-2 HZB texture dimensions + full-mip-chain count + the
        // viewport→HZB UV scale derived from a viewport size. Single source of
        // truth shared by Resize() and any consumer (e.g. the SSR UBO upload)
        // that must agree on the same numbers without owning the generator.
        struct Dimensions
        {
            u32 Width = 0;
            u32 Height = 0;
            u32 MipCount = 0;
            glm::vec2 UVFactor{ 1.0f };
        };
        [[nodiscard]] static Dimensions ComputeDimensions(u32 viewportWidth, u32 viewportHeight);

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

        // Bind a transient/external HZB texture for the current frame.
        // When non-zero, Generate() writes into this texture instead of m_HZBTexture.
        void SetExternalHZBTexture(u32 textureID, u32 mipCount = 0);
        void ClearExternalHZBTexture();

        // Select the per-quad reduction operator. Defaults to Max (GTAO). SSR
        // sets Min so each cell stores its nearest surface. Cheap; safe to call
        // once after Initialize().
        void SetReduceMode(ReduceMode mode)
        {
            m_ReduceMode = mode;
        }
        [[nodiscard]] ReduceMode GetReduceMode() const
        {
            return m_ReduceMode;
        }

        [[nodiscard]] bool IsValid() const;

        // Access the HZB texture (for binding by GTAO / SSR).
        [[nodiscard]] u32 GetHZBTextureID() const;
        [[nodiscard]] u32 GetMipCount() const;
        [[nodiscard]] u32 GetHZBWidth() const
        {
            return m_HZBWidth;
        }
        [[nodiscard]] u32 GetHZBHeight() const
        {
            return m_HZBHeight;
        }

        // UV factor to map viewport coordinates to HZB coordinates:
        // hzbUV = screenUV * UVFactor
        [[nodiscard]] glm::vec2 GetUVFactor() const
        {
            return m_UVFactor;
        }

      private:
        [[nodiscard]] static u32 NextPowerOfTwo(u32 v);
        void DispatchMipBatch(u32 startMip, u32 mipCount, u32 sceneDepthTextureID);

        Ref<ComputeShader> m_HZBShader;
        Ref<Texture2D> m_HZBTexture;
        ReduceMode m_ReduceMode = ReduceMode::Max;
        u32 m_ExternalHZBTextureID = 0;
        u32 m_ExternalMipCount = 0;

        u32 m_HZBWidth = 0;
        u32 m_HZBHeight = 0;
        u32 m_MipCount = 0;
        glm::vec2 m_UVFactor{ 1.0f };
        u32 m_ViewportWidth = 0;
        u32 m_ViewportHeight = 0;
    };
} // namespace OloEngine
