#include "OloEnginePCH.h"
#include "OloEngine/Renderer/VRCS/ShadingRateClassifier.h"

#include "OloEngine/Renderer/HeapBindingSeam.h"
#include "OloEngine/Renderer/MemoryBarrierFlags.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"

#include <glm/glm.hpp>

namespace OloEngine
{
    namespace
    {
        // Pass-local heap-offset table indices, exactly like GTAO.comp's 3/4/5.
        // These are NOT engine-wide TEX_* reservations: this dispatch refills
        // and flushes the shared table immediately before running, and nothing
        // else reads these indices while its program is in flight.
        constexpr u32 kSceneDepthSlot = 3u;
        constexpr u32 kViewNormalsSlot = 4u;
        constexpr u32 kPreviousColorSlot = 5u;
        constexpr u32 kRateImageUnit = 0u;
    } // namespace

    void ShadingRateClassifier::Initialize()
    {
        OLO_PROFILE_FUNCTION();

        m_Shader = ComputeShader::Create("assets/shaders/compute/VRCSClassify.comp");
        OLO_CORE_INFO("ShadingRateClassifier: initialized (tile {}x{})", kTileSize, kTileSize);
    }

    void ShadingRateClassifier::Shutdown()
    {
        m_Shader.Reset();
        m_RateTexture.Reset();
        m_ParamsUBO.Reset();
        m_ViewportWidth = 0;
        m_ViewportHeight = 0;
        m_TilesX = 0;
        m_TilesY = 0;
        InvalidateFrame();
    }

    void ShadingRateClassifier::Reload()
    {
        if (m_Shader)
        {
            m_Shader->Reload();
        }
        // A recompiled classifier may classify differently, so the memoised
        // frame stamp is no longer a promise about the image's contents.
        InvalidateFrame();
    }

    void ShadingRateClassifier::Resize(u32 viewportWidth, u32 viewportHeight)
    {
        OLO_PROFILE_FUNCTION();

        if (viewportWidth == 0u || viewportHeight == 0u)
        {
            return;
        }

        const u32 tilesX = TileCountFor(viewportWidth);
        const u32 tilesY = TileCountFor(viewportHeight);

        m_ViewportWidth = viewportWidth;
        m_ViewportHeight = viewportHeight;

        if (tilesX == m_TilesX && tilesY == m_TilesY && m_RateTexture)
        {
            // The tile grid is what sizes the image, and it is coarse: a
            // one-pixel viewport drag usually lands in the same bucket. The
            // rates themselves are still recomputed every frame, so unlike
            // HZBGenerator's UV factor there is nothing to refresh here.
            return;
        }

        m_TilesX = tilesX;
        m_TilesY = tilesY;

        TextureSpecification spec;
        spec.Width = tilesX;
        spec.Height = tilesY;
        spec.Format = ImageFormat::R8UI;
        spec.GenerateMips = false;
        m_RateTexture = Texture2D::Create(spec);

        // A fresh texture holds no rates. Consumers decode an unwritten texel
        // as full rate, so this is safe rather than merely tidy — but the stamp
        // must still be dropped or the next Classify() would skip the dispatch
        // that fills the new image.
        InvalidateFrame();

        OLO_CORE_INFO("ShadingRateClassifier: {}x{} tiles for viewport {}x{}", tilesX, tilesY, viewportWidth,
                      viewportHeight);
    }

    RHI::ResourceHandle ShadingRateClassifier::GetRateTexture() const
    {
        return m_RateTexture ? m_RateTexture->GetRHIHandle() : RHI::ResourceHandle{};
    }

    bool ShadingRateClassifier::Classify(u64 frameIndex, const Inputs& inputs, const Thresholds& thresholds)
    {
        OLO_PROFILE_FUNCTION();

        if (!IsValid())
        {
            return false;
        }

        // Depth and normals are the two signals that make classification
        // conservative around silhouettes and creases. Without either, a
        // "classification" would be a guess, and a wrong coarse tile is a
        // visible artefact while a missing one only costs time — so decline.
        if (!inputs.SceneDepth.IsValid() || !inputs.ViewNormals.IsValid())
        {
            return false;
        }

        if (m_HasClassified && m_ClassifiedFrame == frameIndex)
        {
            // Already classified this frame by another consumer. Reuse it —
            // that reuse is the whole reason this is one shared utility rather
            // than a classification pass per consumer.
            return true;
        }

        if (!m_ParamsUBO)
        {
            m_ParamsUBO = UniformBuffer::Create(UBOStructures::ShadingRateUBO::GetSize(),
                                                ShaderBindingLayout::UBO_USER_0);
        }

        const bool hasPreviousColor = inputs.PreviousColor.IsValid();

        UBOStructures::ShadingRateUBO params{};
        params.ScreenAndTiles = glm::ivec4(static_cast<i32>(m_ViewportWidth), static_cast<i32>(m_ViewportHeight),
                                           static_cast<i32>(m_TilesX), static_cast<i32>(m_TilesY));
        params.Thresholds = glm::vec4(thresholds.Depth, thresholds.Normal, thresholds.Luma,
                                      thresholds.Coarse4x4Scale);
        params.ClassifyControl = glm::vec4(inputs.DepthLinearizeA, inputs.DepthLinearizeB,
                                           hasPreviousColor ? 1.0f : 0.0f, thresholds.Allow4x4 ? 1.0f : 0.0f);

        // BIND THE SHADER BEFORE THE IMAGE. The seam asks
        // Shader::IsBoundProgramBindless() to decide between writing an offset
        // and issuing a real bind, and that flag describes the program in
        // flight — an image bound first silently takes the fallback path even
        // with the heap on (glsl-shaders.md §5b).
        m_Shader->Bind();

        m_ParamsUBO->SetData(&params, UBOStructures::ShadingRateUBO::GetSize());
        m_ParamsUBO->Bind();

        HeapBinding::BindImageOrOffset(kRateImageUnit, GetRateTexture(), 0, false, 0, RHI::Access::StorageWrite,
                                       RHI::Format::R8UInt, GetRateLifetime());

        HeapBinding::BindTextureOrOffset(kSceneDepthSlot, inputs.SceneDepth, inputs.SceneDepthLifetime);
        HeapBinding::BindTextureOrOffset(kViewNormalsSlot, inputs.ViewNormals, inputs.ViewNormalsLifetime);

        // The previous-colour slot is bound UNCONDITIONALLY, with scene depth
        // standing in when no history exists. Leaving it unbound is not the
        // same as binding nothing: the heap-offset table is shared and
        // persistent across dispatches, so an index this dispatch never writes
        // keeps whatever the last flush left there — a stale descriptor for an
        // unrelated resource. The shader never samples it (ClassifyControl.z is
        // 0), so the substitute's contents are irrelevant; only its being a
        // live, type-correct sampler2D matters.
        HeapBinding::BindTextureOrOffset(kPreviousColorSlot,
                                         hasPreviousColor ? inputs.PreviousColor : inputs.SceneDepth,
                                         hasPreviousColor ? inputs.PreviousColorLifetime
                                                          : inputs.SceneDepthLifetime);

        HeapBinding::FlushOffsets();

        // One workgroup per tile, one invocation per pixel of that tile — the
        // shader's local_size is kTileSize x kTileSize, so the dispatch is the
        // tile grid itself and NOT a ceil-divide of it.
        RenderCommand::DispatchCompute(m_TilesX, m_TilesY, 1u);

        // Consumers read the rates with texelFetch, so the image writes have to
        // be visible as texture fetches, not merely as image accesses.
        RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderImageAccess | MemoryBarrierFlags::TextureFetch);

        m_Shader->Unbind();

        m_ClassifiedFrame = frameIndex;
        m_HasClassified = true;
        return true;
    }
} // namespace OloEngine
