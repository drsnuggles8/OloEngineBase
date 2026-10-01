#include "OloEnginePCH.h"
#include "OloEngine/Renderer/RendererMemoryOwners.h"

#include "OloEngine/Renderer/Debug/RendererMemoryReport.h"
#include "OloEngine/Renderer/GPUScene/GPUSceneTypes.h"
#include "OloEngine/Renderer/Passes/GroomRenderPass.h"
#include "OloEngine/Renderer/RayTracing/DeformedSurfaceCache.h"
#include "OloEngine/Renderer/RayTracing/GroomSurfaceCache.h"
#include "OloEngine/Renderer/RayTracing/RayTracingScene.h"
#include "OloEngine/Renderer/RayTracing/VegetationSurfaceCache.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/RendererAPI.h"
#include "OloEngine/Renderer/Shadow/ShadowMap.h"
#include "OloEngine/Renderer/Texture2DArray.h"

#include <optional>

namespace OloEngine::RendererMemoryOwners
{
    namespace
    {
        // Leaked with the tracker's own lifetime rules in mind: Unregister() resets it at
        // Renderer::Shutdown, so it never outlives the statics its reporter reads.
        RendererMemoryReporterHandle& Handle()
        {
            static auto* s_Handle = new RendererMemoryReporterHandle();
            return *s_Handle;
        }

        [[nodiscard]] std::optional<u64> ArrayBytes(const Ref<Texture2DArray>& texture)
        {
            if (!texture)
                return 0;
            const auto& spec = texture->GetSpecification();
            u64 texel = 0;
            switch (spec.Format)
            {
                case Texture2DArrayFormat::DEPTH_COMPONENT32F:
                case Texture2DArrayFormat::RGBA8:
                    texel = 4;
                    break;
                case Texture2DArrayFormat::RGBA16F:
                    texel = 8;
                    break;
                case Texture2DArrayFormat::RGBA32F:
                case Texture2DArrayFormat::RGBA32UI:
                    texel = 16;
                    break;
                default:
                    return std::nullopt;
            }
            // Shadow depth arrays carry one level; a colour array with mips is not a
            // shadow owner and is not reported here.
            return static_cast<u64>(spec.Width) * spec.Height * std::max(spec.Layers, 1u) * texel;
        }

        void AppendShadowRows(TArray<MemoryCapacityRow>& rows)
        {
            const ShadowMap& shadows = Renderer3D::GetShadowMap();

            // Cascaded shadow map: every cascade layer is allocated; all are drawn when the
            // directional light casts this frame.
            {
                MemoryCapacityRow row;
                row.Owner = "ShadowMap";
                row.Category = "Directional CSM cascades";
                row.Lifetime = MemoryLifetime::Persistent;
                row.Source = MemorySizeSource::FormatEstimate;
                row.CapacityBytes = ArrayBytes(shadows.GetCSMTextureArray());
                if (row.CapacityBytes)
                    row.ActiveDemandBytes = shadows.IsDirectionalShadowRequested() ? *row.CapacityBytes : 0u;
                else
                    row.UnknownReason = "the CSM array's format has no known size";
                rows.Add(std::move(row));
            }

            // Local-light atlas: the whole atlas is allocated; demand is the tiles this
            // frame's shadowed lights occupy.
            {
                MemoryCapacityRow row;
                row.Owner = "ShadowMap";
                row.Category = "Local-light shadow atlas (demand = occupied tiles)";
                row.Lifetime = MemoryLifetime::Persistent;
                row.Source = MemorySizeSource::FormatEstimate;
                row.CapacityBytes = ArrayBytes(shadows.GetAtlasTextureArray());
                if (row.CapacityBytes)
                {
                    u64 tiles = 0;
                    for (u32 i = 0; i < shadows.GetAtlasEntryCount(); ++i)
                    {
                        const auto& rect = shadows.GetAtlasEntryRect(i);
                        tiles += static_cast<u64>(rect.Size) * rect.Size * 4u; // D32F
                    }
                    row.ActiveDemandBytes = std::min(tiles, *row.CapacityBytes);
                }
                else
                {
                    row.UnknownReason = "the atlas format has no known size";
                }
                rows.Add(std::move(row));
            }

            // Virtual shadow map page pool and tables (issue #702).
            {
                MemoryCapacityRow row;
                row.Owner = "ShadowMap";
                row.Category = "Virtual shadow map pool and page tables";
                row.Lifetime = MemoryLifetime::Persistent;
                row.Source = MemorySizeSource::FormatEstimate;
                row.CapacityBytes = shadows.GetVirtualShadowMap().GetVRAMBytes();
                row.ActiveDemandBytes = shadows.IsVirtualShadowMapActive() ? *row.CapacityBytes : 0u;
                rows.Add(std::move(row));
            }
        }

        void AppendGpuSceneRow(TArray<MemoryCapacityRow>& rows)
        {
            const GPUSceneFrameStats& stats = Renderer3D::GetGPUSceneStats();
            u64 capacity = 0;
            u64 demand = 0;
            const auto add = [&](const GPUSceneKindStats& kind, const u64 recordBytes)
            {
                capacity += static_cast<u64>(kind.m_BufferCapacity) * recordBytes;
                demand += static_cast<u64>(kind.m_Live) * recordBytes;
            };
            add(stats.m_Instances, sizeof(GPUSceneInstance));
            add(stats.m_Geometries, sizeof(GPUSceneGeometry));
            add(stats.m_Materials, sizeof(GPUSceneMaterial));
            add(stats.m_Lights, sizeof(GPUSceneLight));
            add(stats.m_Environments, sizeof(GPUSceneEnvironment));

            MemoryCapacityRow row;
            row.Owner = "GPUScene";
            row.Category = "Instance, geometry, material, light and environment tables";
            row.Lifetime = MemoryLifetime::Persistent;
            row.Source = MemorySizeSource::FormatEstimate;
            row.CapacityBytes = capacity;
            row.ActiveDemandBytes = demand;
            if (RendererAPI::GetAPI() == RendererAPI::API::Vulkan)
            {
                // Each edited frame publishes a whole replacement table; the previous ones
                // are retiring allocations in the physical totals, not in this row.
                row.UnknownReason = "Vulkan replacement tables awaiting reclaim are counted as retiring, not here";
            }
            rows.Add(std::move(row));
        }

        void AppendRayTracingRows(TArray<MemoryCapacityRow>& rows)
        {
            const bool haveBackend = RendererAPI::GetAPI() == RendererAPI::API::Vulkan;
            const RayTracing::SceneStats& stats = Renderer3D::GetRayTracingScene().GetStats();

            MemoryCapacityRow structures;
            structures.Owner = "RayTracingScene";
            structures.Category = "Acceleration structures (BLAS, TLAS, instance ring)";
            structures.Lifetime = MemoryLifetime::Persistent;
            structures.Source = MemorySizeSource::Committed;
            structures.CapacityBytes = stats.Resident.AccelerationStructureBytes;
            structures.ActiveDemandBytes = stats.Resident.AccelerationStructureBytes;
            // Compaction gives bytes back to the allocator; report them as what a
            // non-compacted scene would have held on top.
            structures.AliasSavingsBytes = stats.Resident.CompactionSavedBytes;
            if (!haveBackend)
                structures.UnknownReason = "OpenGL has no ray-tracing backend: no acceleration structure exists";
            rows.Add(std::move(structures));

            MemoryCapacityRow scratch;
            scratch.Owner = "RayTracingScene";
            scratch.Category = "Acceleration-structure build scratch (grow-only)";
            scratch.Lifetime = MemoryLifetime::Persistent;
            scratch.Source = MemorySizeSource::Committed;
            scratch.CapacityBytes = stats.Resident.ScratchBytes;
            scratch.UnknownReason = haveBackend ? "active demand: the per-build scratch size is not retained"
                                                : "OpenGL has no ray-tracing backend";
            rows.Add(std::move(scratch));

            const auto proxyRow = [&rows](const char* category, const u64 resident)
            {
                MemoryCapacityRow row;
                row.Owner = "RayTracingScene";
                row.Category = category;
                row.Lifetime = MemoryLifetime::Persistent;
                row.Source = MemorySizeSource::FormatEstimate;
                row.CapacityBytes = resident;
                row.ActiveDemandBytes = resident;
                rows.Add(std::move(row));
            };
            proxyRow("Deformed-surface RT geometry (skinned/morphed)", Renderer3D::GetDeformedSurfaceCache().GetStats().ResidentBytes);
            proxyRow("Groom RT proxies (independent of the strand cache)", Renderer3D::GetGroomSurfaceCache().GetStats().ResidentBytes);
            {
                // The bound coats' rest streams the proxies refit from (#1533), on
                // the CPU and reported apart from the GPU bytes above.
                MemoryCapacityRow row;
                row.Owner = "RayTracingScene";
                row.Category = "Groom RT proxy rest streams and conversion scratch (CPU)";
                row.Lifetime = MemoryLifetime::Persistent;
                row.Source = MemorySizeSource::FormatEstimate;
                row.IsGpu = false;
                row.CapacityBytes = Renderer3D::GetGroomSurfaceCache().GetCpuBytes();
                row.UnknownReason = "active demand: retained streams and reused scratch have no per-frame demand figure";
                rows.Add(std::move(row));
            }
            proxyRow("Vegetation RT proxies", Renderer3D::GetVegetationSurfaceCache().GetStats().ResidentBytes);
        }
    } // namespace

    void Register()
    {
        Handle() = RendererMemoryReporterHandle([](TArray<MemoryCapacityRow>& rows)
                                                {
            // Renderer3D initializes lazily (the editor's 3D mode); nothing to read before.
            if (!Renderer3D::HasInitialized())
                return;
            AppendShadowRows(rows);
            AppendGpuSceneRow(rows);
            AppendRayTracingRows(rows);
            if (const GroomRenderPass* groom = Renderer3D::GetGroomRenderPass())
                groom->AppendMemoryCapacityRows(rows); });
    }

    void Unregister()
    {
        Handle().Reset();
    }
} // namespace OloEngine::RendererMemoryOwners
