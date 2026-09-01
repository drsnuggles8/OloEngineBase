#include "OloEnginePCH.h"
#include "OloEngine/Terrain/Editor/TerrainGPUBrush.h"

#include "OloEngine/Renderer/ComputeShader.h"
#include "OloEngine/Renderer/HeapBindingSeam.h"
#include "OloEngine/Renderer/MemoryBarrierFlags.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Renderer/UniformBuffer.h"
#include "OloEngine/Terrain/TerrainData.h"
#include "OloEngine/Terrain/TerrainMaterial.h"

#include <algorithm>
#include <bit>

namespace OloEngine
{
    namespace
    {
        // Matches local_size_x/y in both brush kernels.
        constexpr u32 kBrushGroupSize = 8;

        constexpr u32 GroupsFor(u32 texels)
        {
            return (texels + kBrushGroupSize - 1) / kBrushGroupSize;
        }
    } // namespace

    TerrainGPUBrush::TerrainGPUBrush()
        : m_SculptShader(ComputeShader::Create("assets/shaders/compute/Terrain_SculptBrush.comp")),
          m_PaintShader(ComputeShader::Create("assets/shaders/compute/Terrain_PaintBrush.comp"))
    {
        OLO_PROFILE_FUNCTION();
    }

    bool TerrainGPUBrush::IsSculptReady() const
    {
        return m_SculptShader && m_SculptShader->IsValid();
    }

    bool TerrainGPUBrush::IsPaintReady() const
    {
        return m_PaintShader && m_PaintShader->IsValid();
    }

    bool TerrainGPUBrush::IsReady() const
    {
        return IsSculptReady() && IsPaintReady();
    }

    bool TerrainGPUBrush::EnsureScratch(u32 width, u32 height)
    {
        OLO_PROFILE_FUNCTION();

        const u32 needed = std::max(width, height);
        if (m_HeightScratch && m_ScratchSize >= needed)
        {
            return true;
        }

        // Round up to a power of two so a drag that grows the brush a texel at a
        // time reallocates a handful of times rather than every frame.
        const u32 size = std::bit_ceil(std::max(needed, kBrushGroupSize));

        TextureSpecification spec;
        spec.Width = size;
        spec.Height = size;
        spec.Format = ImageFormat::R32F;
        spec.GenerateMips = false;

        Ref<Texture2D> scratch = Texture2D::Create(spec);
        if (!scratch)
        {
            OLO_CORE_ERROR("TerrainGPUBrush: Failed to create {}x{} sculpt scratch texture", size, size);
            return false;
        }

        m_HeightScratch = scratch;
        m_ScratchSize = size;
        return true;
    }

    TerrainBrush::DirtyRegion TerrainGPUBrush::ApplySculpt(TerrainData& terrainData,
                                                           const TerrainBrushSettings& settings,
                                                           const glm::vec3& worldPos,
                                                           f32 worldSizeX, f32 worldSizeZ, f32 heightScale,
                                                           f32 deltaTime, f32 targetHeight)
    {
        OLO_PROFILE_FUNCTION();

        TerrainBrush::DirtyRegion dirty{};

        if (!IsSculptReady())
        {
            return dirty;
        }

        const u32 resolution = terrainData.GetResolution();
        if (resolution <= 1 || heightScale <= 0.0f || worldSizeX <= 0.0f || worldSizeZ <= 0.0f)
        {
            return dirty;
        }

        Ref<Texture2D> heightmap = terrainData.GetGPUHeightmap();
        if (!heightmap)
        {
            return dirty;
        }

        const f32 normX = worldPos.x / worldSizeX;
        const f32 normZ = worldPos.z / worldSizeZ;

        const auto rect = TerrainBrushUtils::ComputeBrushRect(resolution, normX, normZ, settings.Radius,
                                                              worldSizeX, worldSizeZ);
        if (rect.Empty())
        {
            return dirty;
        }

        // Grow the rect by one texel per side for the Smooth tool's neighbour taps,
        // clamped to the map. The shader clamps into this region, so at the terrain
        // edge the boundary texel is duplicated rather than read out of bounds.
        const u32 srcX = (rect.X > 0) ? rect.X - 1 : 0;
        const u32 srcY = (rect.Y > 0) ? rect.Y - 1 : 0;
        const u32 srcMaxX = std::min(rect.X + rect.Width, resolution - 1);
        const u32 srcMaxY = std::min(rect.Y + rect.Height, resolution - 1);
        const u32 srcW = srcMaxX - srcX + 1;
        const u32 srcH = srcMaxY - srcY + 1;

        if (!EnsureScratch(srcW, srcH))
        {
            return dirty;
        }

        // Snapshot the pre-stroke rect. GPU-to-GPU: no readback, and the copy is
        // sized by the brush rather than by the terrain.
        RenderCommand::CopyImageSubDataRegion(
            heightmap->GetRHIHandle(), RendererAPI::TextureTargetType::Texture2D, 0,
            static_cast<i32>(srcX), static_cast<i32>(srcY), 0,
            m_HeightScratch->GetRHIHandle(), RendererAPI::TextureTargetType::Texture2D, 0,
            0, 0, 0, srcW, srcH);
        RenderCommand::MemoryBarrier(MemoryBarrierFlags::TextureUpdate | MemoryBarrierFlags::ShaderImageAccess);

        // Bind the program BEFORE any image — the heap binding seam asks the
        // currently-bound program whether it is bindless, so an image bound first
        // silently takes the slot path and keeps a stale offset. Same ordering
        // contract as TerrainErosion::Apply, and it is load-bearing there too.
        m_SculptShader->Bind();

        if (!m_ParamsUBO)
        {
            m_ParamsUBO = UniformBuffer::Create(UBOStructures::TerrainBrushUBO::GetSize(),
                                                ShaderBindingLayout::UBO_TERRAIN_BRUSH);
        }

        UBOStructures::TerrainBrushUBO params{};
        params.Rect = glm::ivec4(static_cast<i32>(rect.X), static_cast<i32>(rect.Y),
                                 static_cast<i32>(rect.Width), static_cast<i32>(rect.Height));
        params.SrcRect = glm::ivec4(static_cast<i32>(srcX), static_cast<i32>(srcY),
                                    static_cast<i32>(srcW), static_cast<i32>(srcH));
        params.CenterNorm = glm::vec2(normX, normZ);
        params.WorldSize = glm::vec2(worldSizeX, worldSizeZ);
        params.Radius = settings.Radius;
        params.StrengthDt = settings.Strength * deltaTime;
        params.Falloff = settings.Falloff;
        params.TargetHeight = targetHeight;
        params.InvHeightScale = 1.0f / heightScale;
        params.Tool = static_cast<i32>(settings.Tool);
        params.Resolution = static_cast<i32>(resolution);
        params.TargetLayer = 0;
        params.LayerCount = 0;
        m_ParamsUBO->SetData(&params, sizeof(params));
        m_ParamsUBO->Bind();

        HeapBinding::BindImageOrOffset(0, heightmap->GetRHIHandle(), 0, false, 0,
                                       RHI::Access::StorageReadWrite, RHI::Format::R32Float,
                                       RHI::HeapSlotLifetime::Persistent);
        HeapBinding::BindImageOrOffset(1, m_HeightScratch->GetRHIHandle(), 0, false, 0,
                                       RHI::Access::StorageReadWrite, RHI::Format::R32Float,
                                       RHI::HeapSlotLifetime::Persistent);
        HeapBinding::FlushOffsets();

        RenderCommand::DispatchCompute(GroupsFor(rect.Width), GroupsFor(rect.Height), 1);
        // TextureUpdate is not optional here: the next readers of this image are
        // glCopyImageSubData (the undo capture and the next frame's scratch copy)
        // and glGetTexImage (TerrainData::SyncFromGPU), and those are ordered by
        // GL_TEXTURE_UPDATE_BARRIER_BIT rather than by the shader/fetch bits.
        RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderImageAccess | MemoryBarrierFlags::TextureFetch |
                                     MemoryBarrierFlags::TextureUpdate | MemoryBarrierFlags::PixelBuffer);

        // Stage the reserved null image descriptor on both units. Leaving a live
        // offset behind would let a later dispatch that forgot to bind these units
        // go on writing the terrain through a perfectly valid index — the same
        // reasoning as the unbind in TerrainErosion::Apply.
        HeapBinding::BindImageOrOffset(0, RHI::NullResource, 0, false, 0, RHI::Access::StorageReadWrite,
                                       RHI::Format::R32Float, RHI::HeapSlotLifetime::Persistent);
        HeapBinding::BindImageOrOffset(1, RHI::NullResource, 0, false, 0, RHI::Access::StorageReadWrite,
                                       RHI::Format::R32Float, RHI::HeapSlotLifetime::Persistent);
        HeapBinding::FlushOffsets();

        // The kernel wrote level 0 only. The tessellation stage positions terrain
        // vertices from a footprint-selected MIP (include/TerrainHeightSampling.glsl),
        // so without this the stroke is correct near the camera and absent in the
        // distance, snapping in as you approach. The CPU path never had to think
        // about it because TerrainData::UploadRegionToGPU re-uploads the whole
        // image when a chain exists, and SetData rebuilds the chain implicitly.
        heightmap->RegenerateMips();

        // The GPU heightmap is now ahead of the CPU mirror. Nothing is read back
        // here; the next CPU consumer pays for exactly one sync.
        terrainData.MarkGPUModified();

        dirty.X = rect.X;
        dirty.Y = rect.Y;
        dirty.Width = rect.Width;
        dirty.Height = rect.Height;
        return dirty;
    }

    TerrainPaintBrush::DirtyRegion TerrainGPUBrush::ApplyPaint(TerrainMaterial& material,
                                                               const TerrainPaintSettings& settings,
                                                               const glm::vec3& worldPos,
                                                               f32 worldSizeX, f32 worldSizeZ,
                                                               f32 deltaTime)
    {
        OLO_PROFILE_FUNCTION();

        TerrainPaintBrush::DirtyRegion dirty{};

        if (!IsPaintReady() || settings.TargetLayer >= material.GetLayerCount())
        {
            return dirty;
        }

        const u32 resolution = material.GetSplatmapResolution();
        if (resolution <= 1 || worldSizeX <= 0.0f || worldSizeZ <= 0.0f)
        {
            return dirty;
        }

        Ref<Texture2D> splat0 = material.GetSplatmap(0);
        Ref<Texture2D> splat1 = material.GetSplatmap(1);
        if (!splat0 || !splat1)
        {
            return dirty;
        }

        const f32 normX = worldPos.x / worldSizeX;
        const f32 normZ = worldPos.z / worldSizeZ;

        const auto rect = TerrainBrushUtils::ComputeBrushRect(resolution, normX, normZ, settings.Radius,
                                                              worldSizeX, worldSizeZ);
        if (rect.Empty())
        {
            return dirty;
        }

        m_PaintShader->Bind();

        if (!m_ParamsUBO)
        {
            m_ParamsUBO = UniformBuffer::Create(UBOStructures::TerrainBrushUBO::GetSize(),
                                                ShaderBindingLayout::UBO_TERRAIN_BRUSH);
        }

        UBOStructures::TerrainBrushUBO params{};
        params.Rect = glm::ivec4(static_cast<i32>(rect.X), static_cast<i32>(rect.Y),
                                 static_cast<i32>(rect.Width), static_cast<i32>(rect.Height));
        params.SrcRect = glm::ivec4(0);
        params.CenterNorm = glm::vec2(normX, normZ);
        params.WorldSize = glm::vec2(worldSizeX, worldSizeZ);
        params.Radius = settings.Radius;
        params.StrengthDt = settings.Strength * deltaTime;
        params.Falloff = settings.Falloff;
        params.TargetHeight = 0.0f;
        params.InvHeightScale = 0.0f;
        params.Tool = 0;
        params.Resolution = static_cast<i32>(resolution);
        params.TargetLayer = static_cast<i32>(settings.TargetLayer);
        params.LayerCount = static_cast<i32>(material.GetLayerCount());
        m_ParamsUBO->SetData(&params, sizeof(params));
        m_ParamsUBO->Bind();

        HeapBinding::BindImageOrOffset(0, splat0->GetRHIHandle(), 0, false, 0,
                                       RHI::Access::StorageReadWrite, RHI::Format::RGBA8UNorm,
                                       RHI::HeapSlotLifetime::Persistent);
        HeapBinding::BindImageOrOffset(1, splat1->GetRHIHandle(), 0, false, 0,
                                       RHI::Access::StorageReadWrite, RHI::Format::RGBA8UNorm,
                                       RHI::HeapSlotLifetime::Persistent);
        HeapBinding::FlushOffsets();

        RenderCommand::DispatchCompute(GroupsFor(rect.Width), GroupsFor(rect.Height), 1);
        // See the sculpt dispatch: the undo capture copies these images with
        // glCopyImageSubData and the mirror sync reads them with glGetTexImage.
        RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderImageAccess | MemoryBarrierFlags::TextureFetch |
                                     MemoryBarrierFlags::TextureUpdate | MemoryBarrierFlags::PixelBuffer);

        HeapBinding::BindImageOrOffset(0, RHI::NullResource, 0, false, 0, RHI::Access::StorageReadWrite,
                                       RHI::Format::RGBA8UNorm, RHI::HeapSlotLifetime::Persistent);
        HeapBinding::BindImageOrOffset(1, RHI::NullResource, 0, false, 0, RHI::Access::StorageReadWrite,
                                       RHI::Format::RGBA8UNorm, RHI::HeapSlotLifetime::Persistent);
        HeapBinding::FlushOffsets();

        // Splatmaps are sampled with ordinary filtering, so their chain matters for
        // the same reason the heightmap's does — a stale coarse level shows the
        // pre-stroke layer blend at distance.
        splat0->RegenerateMips();
        if (material.GetLayerCount() > 4)
        {
            splat1->RegenerateMips();
        }

        material.MarkSplatmapsGPUModified();

        dirty.X = rect.X;
        dirty.Y = rect.Y;
        dirty.Width = rect.Width;
        dirty.Height = rect.Height;
        return dirty;
    }
} // namespace OloEngine
