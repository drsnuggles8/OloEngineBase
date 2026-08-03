#include "OloEnginePCH.h"
#include "TerrainErosion.h"
#include "OloEngine/Renderer/ComputeShader.h"
#include "OloEngine/Renderer/HeapBindingSeam.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/MemoryBarrierFlags.h"
#include "OloEngine/Terrain/TerrainData.h"
#include "OloEngine/Core/FastRandom.h"

namespace OloEngine
{
    TerrainErosion::TerrainErosion()
        : m_IterationSeed(static_cast<u32>(RandomUtils::Int32(0, std::numeric_limits<i32>::max()))), m_ErosionShader(ComputeShader::Create("assets/shaders/compute/Terrain_Erosion.comp"))
    {
        OLO_PROFILE_FUNCTION();
    }

    bool TerrainErosion::IsReady() const
    {
        return m_ErosionShader && m_ErosionShader->IsValid();
    }

    void TerrainErosion::Apply(TerrainData& terrainData, const ErosionSettings& settings, bool skipReadback)
    {
        OLO_PROFILE_FUNCTION();

        if (!IsReady())
        {
            OLO_CORE_ERROR("TerrainErosion::Apply - Compute shader not ready");
            return;
        }

        auto heightmap = terrainData.GetGPUHeightmap();
        if (!heightmap)
        {
            OLO_CORE_ERROR("TerrainErosion::Apply - No GPU heightmap available");
            return;
        }

        u32 resolution = terrainData.GetResolution();
        if (resolution == 0)
        {
            return;
        }

        // Early-out if no droplets requested (before binding resources)
        if (settings.DropletCount == 0)
        {
            return;
        }

        // Bind and configure the compute shader.
        //
        // THE SHADER MUST BE BOUND BEFORE THE IMAGE, and under the heap that
        // ordering is load-bearing where the slot-based path did not care. The
        // binding seam asks `Shader::IsBoundProgramBindless()` to decide between
        // writing an offset and issuing a bind, and that flag describes the
        // program currently in flight — so an image bound first would silently
        // take the fallback path even with the heap enabled, and the offset table
        // would keep whatever this unit pointed at last.
        m_ErosionShader->Bind();
        m_ErosionShader->SetUint("u_Resolution", resolution);
        m_ErosionShader->SetUint("u_MaxDropletSteps", settings.MaxDropletSteps);
        m_ErosionShader->SetFloat("u_Inertia", settings.Inertia);
        m_ErosionShader->SetFloat("u_SedimentCapacity", settings.SedimentCapacity);
        m_ErosionShader->SetFloat("u_MinSedimentCapacity", settings.MinSedimentCapacity);
        m_ErosionShader->SetFloat("u_DepositSpeed", settings.DepositSpeed);
        m_ErosionShader->SetFloat("u_ErodeSpeed", settings.ErodeSpeed);
        m_ErosionShader->SetFloat("u_EvaporateSpeed", settings.EvaporateSpeed);
        m_ErosionShader->SetFloat("u_Gravity", settings.Gravity);
        m_ErosionShader->SetFloat("u_InitialWater", settings.InitialWater);
        m_ErosionShader->SetFloat("u_InitialSpeed", settings.InitialSpeed);
        m_ErosionShader->SetInt("u_ErosionRadius", static_cast<i32>(settings.ErosionRadius));
        m_ErosionShader->SetUint("u_Seed", m_IterationSeed);
        m_ErosionShader->SetUint("u_DropletCount", settings.DropletCount);

        // Bind the heightmap as image unit 0 for read/write. Persistent: the
        // heightmap is an editor-owned terrain asset, not a graph-owned target.
        HeapBinding::BindImageOrOffset(0, heightmap->GetRHIHandle(), 0, false, 0,
                                       RHI::Access::StorageReadWrite, RHI::Format::R32Float,
                                       RHI::HeapSlotLifetime::Persistent);
        HeapBinding::FlushOffsets();

        // Dispatch — one thread per droplet
        u32 groups = (settings.DropletCount + 255) / 256;
        RenderCommand::DispatchCompute(groups, 1, 1);
        RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderImageAccess | MemoryBarrierFlags::TextureFetch);

        // Unbind the image. Under the heap there is no bind to clear — the shader
        // reads an OFFSET — so this stages the reserved null IMAGE descriptor
        // instead, and the flush publishes it. Leaving the previous offset would
        // let a later dispatch that forgot to bind this unit go on writing the
        // heightmap through a perfectly valid index.
        HeapBinding::BindImageOrOffset(0, RHI::NullResource, 0, false, 0, RHI::Access::StorageReadWrite,
                                       RHI::Format::R32Float, RHI::HeapSlotLifetime::Persistent);
        HeapBinding::FlushOffsets();

        // Read back GPU heightmap to CPU for chunk rebuilding and serialization
        if (!skipReadback)
        {
            std::vector<u8> rawData;
            if (!heightmap->GetData(rawData))
            {
                OLO_CORE_ERROR("TerrainErosion::Apply - Failed to read back GPU heightmap data");
                return;
            }

            auto& heights = terrainData.GetHeightData();
            if (rawData.size() != heights.size() * sizeof(f32))
            {
                OLO_CORE_ERROR("TerrainErosion::Apply - Readback size mismatch: got {} bytes, expected {} bytes",
                               rawData.size(), heights.size() * sizeof(f32));
                return;
            }
            std::memcpy(heights.data(), rawData.data(), rawData.size());
        }

        // Advance seed so each iteration produces different droplet positions
        ++m_IterationSeed;
    }

    void TerrainErosion::ApplyIterations(TerrainData& terrainData, const ErosionSettings& settings, u32 iterations)
    {
        OLO_PROFILE_FUNCTION();

        for (u32 i = 0; i < iterations; ++i)
        {
            Apply(terrainData, settings, i < iterations - 1);
        }
    }
} // namespace OloEngine
