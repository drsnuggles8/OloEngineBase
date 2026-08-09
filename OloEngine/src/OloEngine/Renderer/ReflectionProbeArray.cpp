#include "OloEnginePCH.h"
#include "OloEngine/Renderer/ReflectionProbeArray.h"

#include "OloEngine/Core/Log.h"
#include "OloEngine/Debug/Instrumentor.h"
#include "OloEngine/Renderer/CameraRelative.h"
#include "OloEngine/Renderer/ComputeShader.h"
#include "OloEngine/Renderer/HeapBindingSeam.h"
#include "OloEngine/Renderer/LightCulling/ClusteredLighting.h"
#include "OloEngine/Renderer/MemoryBarrierFlags.h"
#include "OloEngine/Renderer/ReflectionProbeDistanceField.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/StorageBuffer.h"
#include "OloEngine/Renderer/UniformBuffer.h"

#include <algorithm>
#include <utility>

namespace OloEngine
{
    void ReflectionProbeArray::Init()
    {
        OLO_PROFILE_FUNCTION();

        m_ProbeUBO = UniformBuffer::Create(UBOStructures::ReflectionProbeUBO::GetSize(),
                                           ShaderBindingLayout::UBO_REFLECTION_PROBES);
        m_GridSSBO = StorageBuffer::Create(ClusteredLighting::kTotalClusters * sizeof(u32),
                                           ShaderBindingLayout::SSBO_REFLECTION_PROBE_GRID,
                                           StorageBufferUsage::DynamicCopy);
        m_CullShader = ComputeShader::Create("assets/shaders/compute/ReflectionProbeCull.comp");
        if (!m_CullShader || !m_CullShader->IsValid())
        {
            // Non-fatal: shading falls back to testing every probe per pixel
            // (the UBO's grid-valid flag stays 0).
            OLO_CORE_ERROR("ReflectionProbeArray: cull compute unavailable — probe cluster grid disabled");
        }

        // 1-layer stand-ins so the samplerCubeArray slots always carry a
        // legal texture even before the first probe uploads.
        CubemapArraySpecification placeholderSpec;
        placeholderSpec.Resolution = 1;
        placeholderSpec.Layers = 1;
        placeholderSpec.MipLevels = 1;
        placeholderSpec.Format = ImageFormat::RGBA8;
        m_PlaceholderRadiance = TextureCubemapArray::Create(placeholderSpec);
        placeholderSpec.Format = ImageFormat::R32F;
        m_PlaceholderDistance = TextureCubemapArray::Create(placeholderSpec);

        m_Initialized = m_ProbeUBO && m_GridSSBO && m_PlaceholderRadiance && m_PlaceholderDistance;
        if (!m_Initialized)
        {
            OLO_CORE_ERROR("ReflectionProbeArray: initialization failed");
            return;
        }
        OLO_CORE_INFO("ReflectionProbeArray: initialized (max {} probes, {} clusters)",
                      UBOStructures::ReflectionProbeUBO::MAX_PROBES, ClusteredLighting::kTotalClusters);
    }

    void ReflectionProbeArray::Shutdown()
    {
        m_Layers.clear();
        m_RadianceArray.Reset();
        m_DistanceArray.Reset();
        m_PlaceholderRadiance.Reset();
        m_PlaceholderDistance.Reset();
        m_ProbeUBO.Reset();
        m_GridSSBO.Reset();
        m_CullShader.Reset();
        m_Submitted.clear();
        m_UploadedCount = 0;
        m_GridValid = false;
        m_Initialized = false;
    }

    void ReflectionProbeArray::SetProbes(std::vector<ReflectionProbeRenderData>&& probes)
    {
        m_Submitted = std::move(probes);
    }

    bool ReflectionProbeArray::EnsureArrays(u32 requiredLayers, const Ref<TextureCubemap>& referencePrefilter)
    {
        // The radiance array's face size / format follow the probes'
        // prefilter maps (every bake uses the same IBLConfiguration, so all
        // probes agree; a mismatching probe is skipped in PrepareFrame).
        u32 const capacity = static_cast<u32>(m_Layers.size());
        if (m_RadianceArray && m_DistanceArray && requiredLayers <= capacity)
        {
            return true;
        }

        u32 newCapacity = std::max(4u, capacity);
        while (newCapacity < requiredLayers)
        {
            newCapacity *= 2u;
        }
        newCapacity = std::min(newCapacity, UBOStructures::ReflectionProbeUBO::MAX_PROBES);

        // Prefer an already-resident layer's spec (a growth must keep the
        // spec the occupied layers were uploaded with); fall back to the
        // caller-provided reference on first creation.
        Ref<TextureCubemap> reference = referencePrefilter;
        for (auto const& slot : m_Layers)
        {
            if (slot.Environment && slot.Environment->GetPrefilterMap())
            {
                reference = slot.Environment->GetPrefilterMap();
                break;
            }
        }
        if (!reference)
        {
            OLO_CORE_ERROR("ReflectionProbeArray::EnsureArrays: no reference prefilter map to size the arrays from");
            return false;
        }

        CubemapArraySpecification radianceSpec;
        radianceSpec.Resolution = reference->GetCubemapSpecification().Width;
        radianceSpec.Layers = newCapacity;
        radianceSpec.Format = reference->GetCubemapSpecification().Format;
        radianceSpec.MipLevels = reference->GetMipLevelCount();

        CubemapArraySpecification distanceSpec;
        distanceSpec.Resolution = kProbeDistanceResolution;
        distanceSpec.Layers = newCapacity;
        distanceSpec.Format = ImageFormat::R32F;
        distanceSpec.MipLevels = 0; // full chain — the CPU field builds every max-mip

        auto radiance = TextureCubemapArray::Create(radianceSpec);
        auto distance = TextureCubemapArray::Create(distanceSpec);
        if (!radiance || !radiance->IsLoaded() || !distance || !distance->IsLoaded())
        {
            OLO_CORE_ERROR("ReflectionProbeArray: failed to (re)create probe arrays ({} layers)", newCapacity);
            return false;
        }

        // Swap in the new arrays, then re-upload every occupied layer — the
        // old GPU contents die with the old textures.
        std::vector<LayerSlot> previous = std::move(m_Layers);
        m_Layers.assign(newCapacity, {});
        m_RadianceArray = radiance;
        m_DistanceArray = distance;

        for (sizet i = 0; i < previous.size(); ++i)
        {
            if (previous[i].Environment && UploadLayer(static_cast<u32>(i), *previous[i].Environment))
            {
                m_Layers[i].Environment = previous[i].Environment;
            }
        }

        OLO_CORE_INFO("ReflectionProbeArray: arrays sized to {} layers ({}^2 radiance, {}^2 distance)",
                      newCapacity, radianceSpec.Resolution, distanceSpec.Resolution);
        return true;
    }

    bool ReflectionProbeArray::UploadLayer(u32 layer, const EnvironmentMap& environment)
    {
        OLO_PROFILE_FUNCTION();

        auto const& prefilter = environment.GetPrefilterMap();
        auto const& field = environment.GetProbeDistanceField();
        if (!prefilter || !field)
        {
            return false;
        }
        if (!m_RadianceArray->CopyLayerFromCubemap(layer, *prefilter))
        {
            return false;
        }

        u32 const mips = std::min(m_DistanceArray->GetMipLevelCount(), field->GetMipCount());
        for (u32 mip = 0; mip < mips; ++mip)
        {
            auto const data = field->GetMip(mip);
            if (!m_DistanceArray->SetLayerMipData(layer, mip, data.data(), data.size() * sizeof(f32)))
            {
                return false;
            }
        }
        return true;
    }

    void ReflectionProbeArray::PrepareFrame(const glm::mat4& viewMatrix, const glm::mat4& projectionMatrix,
                                            u32 viewportWidth, u32 viewportHeight)
    {
        OLO_PROFILE_FUNCTION();

        m_UploadedCount = 0;
        m_GridValid = false;
        if (!m_Initialized)
        {
            m_Submitted.clear();
            return;
        }

        // Validate + cap the submitted set. A probe without a distance field
        // (pre-#705 bake, or a failed distance capture) is skipped — it still
        // contributes through the irradiance override, just not here.
        std::vector<ReflectionProbeRenderData> wanted;
        wanted.reserve(std::min<sizet>(m_Submitted.size(), UBOStructures::ReflectionProbeUBO::MAX_PROBES));
        for (auto& probe : m_Submitted)
        {
            if (wanted.size() >= UBOStructures::ReflectionProbeUBO::MAX_PROBES)
            {
                break;
            }
            if (!probe.Environment || !probe.Environment->HasIBL() || !probe.Environment->HasProbeDistanceField())
            {
                continue;
            }
            auto const& prefilter = probe.Environment->GetPrefilterMap();
            if (!prefilter)
            {
                continue;
            }
            if (m_RadianceArray &&
                (prefilter->GetCubemapSpecification().Width != m_RadianceArray->GetArraySpecification().Resolution ||
                 prefilter->GetCubemapSpecification().Format != m_RadianceArray->GetArraySpecification().Format))
            {
                if (!m_WarnedPrefilterMismatch)
                {
                    m_WarnedPrefilterMismatch = true;
                    OLO_CORE_WARN("ReflectionProbeArray: probe prefilter {}x{} does not match the array — probe skipped",
                                  prefilter->GetWidth(), prefilter->GetHeight());
                }
                continue;
            }
            wanted.push_back(std::move(probe));
        }
        m_Submitted.clear();

        UBOStructures::ReflectionProbeUBO uboData{};
        if (wanted.empty() ||
            !EnsureArrays(static_cast<u32>(wanted.size()), wanted.front().Environment->GetPrefilterMap()))
        {
            // Keep the UBO current so shaders see zero probes rather than a
            // stale count from the previous scene/frame.
            m_ProbeUBO->SetData(&uboData, sizeof(uboData));
            m_ProbeUBO->Bind();
            return;
        }

        // Release layers whose probe left the set — a freed slot drops its
        // EnvironmentMap reference (the bake result) immediately.
        for (auto& slot : m_Layers)
        {
            if (!slot.Environment)
            {
                continue;
            }
            bool stillWanted = false;
            for (auto const& probe : wanted)
            {
                if (probe.Environment.Raw() == slot.Environment.Raw())
                {
                    stillWanted = true;
                    break;
                }
            }
            if (!stillWanted)
            {
                slot.Environment = nullptr;
            }
        }

        f32 nearPlane = 0.1f;
        f32 farPlane = 1000.0f;
        ClusteredLighting::ExtractClipPlanes(projectionMatrix, nearPlane, farPlane);
        nearPlane = std::max(nearPlane, ClusteredLighting::kMinNearPlane);
        farPlane = std::max(farPlane, nearPlane * (1.0f + 1e-3f));

        glm::vec3 const renderOrigin = Renderer3D::GetRenderOrigin();

        u32 count = 0;
        auto const layerCount = m_Layers.size();
        for (auto const& probe : wanted)
        {
            // Find the probe's layer (identity match on the bake result) or
            // claim a free one and upload into it.
            i32 layer = -1;
            for (sizet i = 0; i < layerCount; ++i)
            {
                if (m_Layers[i].Environment.Raw() == probe.Environment.Raw())
                {
                    layer = static_cast<i32>(i);
                    break;
                }
            }
            if (layer < 0)
            {
                for (sizet i = 0; i < layerCount; ++i)
                {
                    if (!m_Layers[i].Environment)
                    {
                        layer = static_cast<i32>(i);
                        break;
                    }
                }
                if (layer < 0)
                {
                    break; // capacity exhausted (cannot happen: EnsureArrays sized to wanted)
                }
                if (!UploadLayer(static_cast<u32>(layer), *probe.Environment))
                {
                    OLO_CORE_WARN("ReflectionProbeArray: layer upload failed — probe skipped this frame");
                    continue;
                }
                m_Layers[static_cast<sizet>(layer)].Environment = probe.Environment;
            }

            auto& gpuProbe = uboData.Probes[count];
            gpuProbe.PositionRadius = glm::vec4(probe.Position - renderOrigin, probe.InfluenceRadius);
            gpuProbe.Params = glm::vec4(std::max(probe.BlendDistance, 0.001f),
                                        probe.Intensity,
                                        probe.Environment->GetProbeDistanceField()->GetMaxFiniteDistance(),
                                        static_cast<f32>(layer));
            ++count;
        }

        bool const dispatchCull = count > 0 && m_CullShader && m_CullShader->IsValid() && m_GridSSBO;

        auto const slicing = ClusteredLighting::ComputeDepthSliceParams(
            ClusteredLighting::kClusterCountZ, nearPlane, farPlane);
        uboData.Counts = glm::uvec4(count, dispatchCull ? 1u : 0u,
                                    ClusteredLighting::kClusterCountX, ClusteredLighting::kClusterCountY);
        uboData.TileScale = glm::vec4(
            viewportWidth > 0 ? static_cast<f32>(ClusteredLighting::kClusterCountX) / static_cast<f32>(viewportWidth) : 0.0f,
            viewportHeight > 0 ? static_cast<f32>(ClusteredLighting::kClusterCountY) / static_cast<f32>(viewportHeight) : 0.0f,
            static_cast<f32>(ClusteredLighting::kClusterCountZ), 0.0f);
        uboData.DepthSlicing = glm::vec4(slicing.Scale, slicing.Bias, nearPlane, farPlane);

        m_ProbeUBO->SetData(&uboData, sizeof(uboData));
        m_ProbeUBO->Bind();
        m_UploadedCount = count;

        if (dispatchCull)
        {
            // The UBO carries render-relative probe positions, so the cull's
            // view matrix must map relative world to view (the Forward+ rule,
            // issue #429).
            glm::mat4 const viewRelative = MakeViewRelative(viewMatrix, renderOrigin);

            m_GridSSBO->Bind();
            m_CullShader->Bind();
            m_CullShader->SetMat4("u_ViewMatrix", viewRelative);
            m_CullShader->SetMat4("u_InverseProjectionMatrix", glm::inverse(projectionMatrix));
            m_CullShader->SetFloat("u_NearPlane", nearPlane);
            m_CullShader->SetFloat("u_FarPlane", farPlane);
            RenderCommand::DispatchCompute(ClusteredLighting::kClusterCountX,
                                           ClusteredLighting::kClusterCountY,
                                           ClusteredLighting::kClusterCountZ);
            RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderStorage);
            m_CullShader->Unbind();
            m_GridValid = true;
        }
    }

    void ReflectionProbeArray::BindForShading() const
    {
        OLO_PROFILE_FUNCTION();

        if (!m_Initialized)
        {
            return;
        }

        auto const& radiance = (m_UploadedCount > 0 && m_RadianceArray) ? m_RadianceArray : m_PlaceholderRadiance;
        auto const& distance = (m_UploadedCount > 0 && m_DistanceArray) ? m_DistanceArray : m_PlaceholderDistance;
        if (radiance)
        {
            HeapBinding::PublishTextureOffsetAndBind(ShaderBindingLayout::TEX_REFLECTION_PROBE_RADIANCE,
                                                     radiance->GetRHIHandle(), RHI::HeapSlotLifetime::Persistent);
        }
        if (distance)
        {
            HeapBinding::PublishTextureOffsetAndBind(ShaderBindingLayout::TEX_REFLECTION_PROBE_DISTANCE,
                                                     distance->GetRHIHandle(), RHI::HeapSlotLifetime::Persistent);
        }
        if (m_ProbeUBO)
        {
            m_ProbeUBO->Bind();
        }
        if (m_GridSSBO)
        {
            m_GridSSBO->Bind();
        }
    }
} // namespace OloEngine
