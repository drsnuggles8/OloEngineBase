#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Renderer3D.h"

#include "OloEngine/Renderer/HeapBindingSeam.h"
#include "OloEngine/Renderer/Material.h"
#include "OloEngine/Renderer/MeshSource.h"
#include "OloEngine/Renderer/RHI/RHIDescriptorHeap.h"
#include "OloEngine/Renderer/SubmeshMaterialResolve.h"

#include <utility>

namespace OloEngine
{
    namespace
    {
        // A texture as the material / environment record carries it: the RHI
        // identity plus the persistent heap offset resolved for it now.
        // `heapEnabled` is read once per extraction so a disabled heap (GL
        // without OLO_RHI_BINDLESS, the default) costs no seam call per texture;
        // the offset then stays GPUSceneHeapOffsetUnresolved, which the record
        // contract documents as "bind through the slot path".
        [[nodiscard]] GPUSceneTextureRef ResolveRecordTexture(RHI::ResourceHandle handle, bool heapEnabled,
                                                              const RHI::SamplerDesc& sampler,
                                                              RHI::NullSamplerKind kind)
        {
            GPUSceneTextureRef texture{ .m_Handle = handle };
            if (handle.IsValid() && heapEnabled)
            {
                texture.m_HeapOffset = HeapBinding::ResolveRecordTextureOffset(handle, sampler, kind).Value;
            }
            return texture;
        }

        [[nodiscard]] GPUSceneTextureRef ResolveRecordTexture2D(const Ref<Texture2D>& texture, bool heapEnabled)
        {
            return ResolveRecordTexture(texture ? texture->GetRHIHandle() : RHI::NullResource, heapEnabled,
                                        HeapBinding::MaterialTexture2DSampler(), RHI::NullSamplerKind::Texture2D);
        }

        // Material -> record input, field for field the same reads as
        // CreatePODMaterialDataForMaterial, minus the global-IBL fallback: the
        // IBL trio is environment data (GPUSceneEnvironment), not material data.
        [[nodiscard]] GPUSceneMaterialInput BuildMaterialInput(const Material& material, bool heapEnabled)
        {
            const bool pbr = material.GetType() == MaterialType::PBR;

            GPUSceneMaterialInput input;
            // One albedo lane: the PBR base colour, or the legacy diffuse colour
            // and diffuse map for a Phong material.
            input.m_BaseColorFactor = pbr ? material.GetBaseColorFactor() : glm::vec4(material.GetDiffuse(), 1.0f);
            input.m_EmissiveFactor = material.GetEmissiveFactor();
            input.m_LegacyAmbient = material.GetAmbient();
            input.m_LegacySpecular = material.GetSpecular();
            input.m_Shininess = material.GetShininess();
            input.m_MetallicFactor = material.GetMetallicFactor();
            input.m_RoughnessFactor = material.GetRoughnessFactor();
            input.m_NormalScale = material.GetNormalScale();
            input.m_OcclusionStrength = material.GetOcclusionStrength();
            input.m_AlphaCutoff = material.GetAlphaCutoff();
            input.m_AlphaMode = static_cast<u32>(std::to_underlying(material.GetAlphaMode()));
            input.m_ClosureVersion = static_cast<u32>(std::to_underlying(material.GetPBRModel()));

            u32 flags = 0;
            if (pbr)
            {
                flags |= GPUSceneMaterialFlagPBR;
            }
            if (material.GetFlag(MaterialFlag::TwoSided))
            {
                flags |= GPUSceneMaterialFlagTwoSided;
            }
            if (material.GetFlag(MaterialFlag::Blend))
            {
                flags |= GPUSceneMaterialFlagBlend;
            }
            if (material.GetFlag(MaterialFlag::DepthTest))
            {
                flags |= GPUSceneMaterialFlagDepthTest;
            }
            if (material.GetFlag(MaterialFlag::DisableShadowCasting))
            {
                flags |= GPUSceneMaterialFlagDisableShadowCasting;
            }
            if (material.IsIBLEnabled())
            {
                flags |= GPUSceneMaterialFlagIBL;
            }
            if (material.IsUsingTextureMaps())
            {
                flags |= GPUSceneMaterialFlagUseTextureMaps;
            }
            input.m_Flags = flags;

            input.m_Albedo = ResolveRecordTexture2D(pbr ? material.GetAlbedoMap() : material.GetDiffuseMap(), heapEnabled);
            input.m_MetallicRoughness = ResolveRecordTexture2D(material.GetMetallicRoughnessMap(), heapEnabled);
            input.m_Normal = ResolveRecordTexture2D(material.GetNormalMap(), heapEnabled);
            input.m_Occlusion = ResolveRecordTexture2D(material.GetAOMap(), heapEnabled);
            input.m_Emissive = ResolveRecordTexture2D(material.GetEmissiveMap(), heapEnabled);
            input.m_Specular = ResolveRecordTexture2D(material.GetSpecularMap(), heapEnabled);
            return input;
        }
    } // namespace

    void Renderer3D::BeginGPUSceneExtraction(u64 ownerToken)
    {
        OLO_CORE_ASSERT(!s_Data.GPUSceneExtractionActive,
                        "Renderer3D::BeginGPUSceneExtraction called twice before EndScene");
        s_Data.SceneGPU.BeginExtraction(ownerToken, s_Data.RenderOrigin);
        s_Data.GPUSceneExtractionActive = true;
    }

    GPUSceneMaterialKey Renderer3D::ResolveGPUSceneMaterialKey(const Material* overrideMaterial, u64 stableEntityId,
                                                               const Ref<MeshSource>& meshSource, u32 submeshIndex,
                                                               GPUSceneMaterialOverrideLane overrideLane)
    {
        // The same decision that picks the material the draw shades with
        // (SubmeshMaterialResolve.h), so the key cannot name one source while
        // the draw uses another.
        switch (ResolveSubmeshMaterialOrigin(overrideMaterial, meshSource.get(), submeshIndex))
        {
            case SubmeshMaterialOrigin::Override:
                return GPUSceneMaterialKey{
                    .m_Owner = stableEntityId,
                    .m_Slot = std::to_underlying(overrideLane),
                    .m_Source = std::to_underlying(GPUSceneMaterialSource::EntityOverride),
                };
            case SubmeshMaterialOrigin::Imported:
            {
                // The authored identity of an imported material is its mesh
                // source: the asset handle when the source is an asset (a GPU
                // rebuild keeps it), else the vertex-buffer identity the geometry
                // key already uses (procedural sources). A source with no GPU
                // buffer yet is not extractable at all (ExtractGPUSceneMesh
                // rejects it) and gets no record rather than a made-up identity.
                const auto assetId = static_cast<u64>(meshSource->GetHandle());
                const Ref<VertexBuffer>& vertexBuffer = meshSource->GetVertexBuffer();
                const RHI::ResourceHandle vertexHandle =
                    vertexBuffer ? vertexBuffer->GetRHIHandle() : RHI::NullResource;
                u64 owner = 0;
                if (assetId != 0)
                {
                    owner = assetId;
                }
                else if (vertexHandle.IsValid())
                {
                    owner = RHI::HashKey(vertexHandle);
                }
                else
                {
                    return GPUSceneMaterialKey{
                        .m_Source = std::to_underlying(GPUSceneMaterialSource::Unresolvable),
                    };
                }
                return GPUSceneMaterialKey{
                    .m_Owner = owner,
                    .m_Slot = meshSource->GetSubmeshes()[static_cast<i32>(submeshIndex)].m_MaterialIndex,
                    .m_Source = std::to_underlying(GPUSceneMaterialSource::Imported),
                };
            }
            case SubmeshMaterialOrigin::Default:
                break;
        }
        return GPUSceneMaterialKey{
            .m_Owner = 0,
            .m_Slot = 0,
            .m_Source = std::to_underlying(GPUSceneMaterialSource::Default),
        };
    }

    void Renderer3D::ExtractGPUSceneMaterial(const GPUSceneMaterialKey& key, const Material& material)
    {
        if (!s_Data.GPUSceneExtractionActive ||
            key.m_Source == std::to_underlying(GPUSceneMaterialSource::Unresolvable) ||
            s_Data.SceneGPU.IsMaterialStaged(key))
        {
            return;
        }
        s_Data.SceneGPU.ExtractMaterial(key, BuildMaterialInput(material, RHI::DescriptorHeap::Get().IsEnabled()));
    }

    void Renderer3D::ExtractGPUSceneMesh(u64 stableEntityId, u64 stableInstanceId,
                                         const Ref<MeshSource>& meshSource, u32 submeshIndex,
                                         const glm::mat4& worldTransform, const GPUSceneMaterialKey& materialKey,
                                         u32 visibilityMask, u32 flags)
    {
        if (!s_Data.GPUSceneExtractionActive || !meshSource || !meshSource->GetVertexArray() || submeshIndex >= static_cast<u32>(meshSource->GetSubmeshes().Num()))
        {
            return;
        }

        const Ref<VertexBuffer>& vertexBuffer = meshSource->GetVertexBuffer();
        const Ref<IndexBuffer>& indexBuffer = meshSource->GetIndexBuffer();
        if (!vertexBuffer || !indexBuffer)
        {
            return;
        }

        const RHI::ResourceHandle vertexHandle = vertexBuffer->GetRHIHandle();
        const RHI::ResourceHandle indexHandle = indexBuffer->GetRHIHandle();
        if (!vertexHandle.IsValid() || !indexHandle.IsValid())
        {
            return;
        }

        const Submesh& submesh = meshSource->GetSubmeshes()[static_cast<i32>(submeshIndex)];
        const GPUSceneGeometryKey geometryKey{
            .m_VertexBuffer = RHI::HashKey(vertexHandle),
            .m_IndexBuffer = RHI::HashKey(indexHandle),
            .m_SubmeshIndex = submeshIndex,
        };
        s_Data.SceneGPU.ExtractGeometry(
            geometryKey,
            GPUSceneGeometryInput{
                .m_VertexBuffer = vertexHandle,
                .m_IndexBuffer = indexHandle,
                .m_VertexAddress = vertexBuffer->GetDeviceAddress(),
                .m_IndexAddress = indexBuffer->GetDeviceAddress(),
                .m_VertexFormat = std::to_underlying(GPUSceneVertexFormat::OloVertex),
                .m_IndexFormat = std::to_underlying(GPUSceneIndexFormat::UInt32),
                .m_FirstIndex = submesh.m_BaseIndex,
                .m_IndexCount = submesh.m_IndexCount,
                .m_BaseVertex = static_cast<i32>(submesh.m_BaseVertex),
                .m_VertexCount = submesh.m_VertexCount,
            });
        s_Data.SceneGPU.ExtractInstance(
            GPUSceneInstanceKey{
                .m_EntityId = stableEntityId,
                .m_Geometry = geometryKey,
                .m_InstanceId = stableInstanceId,
            },
            GPUSceneInstanceInput{
                .m_WorldTransform = worldTransform,
                .m_Material = materialKey,
                .m_VisibilityMask = visibilityMask,
                .m_Flags = flags,
            });
    }

    void Renderer3D::ExtractGPUSceneLight(const GPUSceneLightKey& key, const GPUSceneLightInput& input)
    {
        if (!s_Data.GPUSceneExtractionActive)
        {
            return;
        }
        s_Data.SceneGPU.ExtractLight(key, input);
    }

    void Renderer3D::ExtractGPUSceneGlobalEnvironment()
    {
        if (!s_Data.GPUSceneExtractionActive)
        {
            return;
        }

        // The cubemap descriptors keep the object's own sampler state (the
        // default SamplerDesc{}), exactly as the per-draw material UBO mints
        // them; the BRDF LUT is a 2D texture and takes the material sampler.
        const bool heapEnabled = RHI::DescriptorHeap::Get().IsEnabled();
        const RHI::SamplerDesc cubeSampler{};
        GPUSceneEnvironmentInput input;
        input.m_Environment = ResolveRecordTexture(s_Data.GlobalEnvironmentMapID, heapEnabled, cubeSampler,
                                                   RHI::NullSamplerKind::Cube);
        input.m_Irradiance = ResolveRecordTexture(s_Data.GlobalIrradianceMapID, heapEnabled, cubeSampler,
                                                  RHI::NullSamplerKind::Cube);
        input.m_Prefilter = ResolveRecordTexture(s_Data.GlobalPrefilterMapID, heapEnabled, cubeSampler,
                                                 RHI::NullSamplerKind::Cube);
        input.m_BRDFLut = ResolveRecordTexture(s_Data.GlobalBRDFLutMapID, heapEnabled,
                                               HeapBinding::MaterialTexture2DSampler(), RHI::NullSamplerKind::Texture2D);
        input.m_Intensity = s_Data.GlobalIBLIntensity;

        // No published environment means no record: the slot is removed (and
        // retired) rather than carrying an all-invalid entry.
        const bool published = input.m_Environment.m_Handle.IsValid() || input.m_Irradiance.m_Handle.IsValid() ||
                               input.m_Prefilter.m_Handle.IsValid() || input.m_BRDFLut.m_Handle.IsValid();
        if (published)
        {
            s_Data.SceneGPU.ExtractEnvironment(GPUSceneEnvironmentKey{ .m_Owner = 0 }, input);
        }
    }

    void Renderer3D::ReportUnsupportedGPUScene(GPUSceneUnsupportedCategory category, u32 count)
    {
        if (s_Data.GPUSceneExtractionActive && count > 0)
        {
            s_Data.SceneGPU.ReportUnsupported(category, count);
        }
    }

    void Renderer3D::ResetGPUScene()
    {
        s_Data.SceneGPU.Reset();
        s_Data.GPUSceneExtractionActive = false;
    }

    const GPUSceneFrameStats& Renderer3D::GetGPUSceneStats()
    {
        return s_Data.SceneGPU.GetLastFrameUpdate().m_Stats;
    }
} // namespace OloEngine
