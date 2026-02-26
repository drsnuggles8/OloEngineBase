#include "OloEnginePCH.h"
#include "TerrainErosion.h"
#include "OloEngine/Renderer/ComputeShader.h"
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
            return;

        // Bind heightmap as image unit 0 for read/write
        RenderCommand::BindImageTexture(0, heightmap->GetRendererID(), 0, false, 0, GL_READ_WRITE, GL_R32F);

        // Bind and configure the compute shader
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

        // Dispatch — one thread per droplet
        if (settings.DropletCount == 0)
            return;
        u32 groups = (settings.DropletCount + 255) / 256;
        RenderCommand::DispatchCompute(groups, 1, 1);
        RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderImageAccess | MemoryBarrierFlags::TextureFetch);

        // Unbind image
        RenderCommand::BindImageTexture(0, 0, 0, false, 0, GL_READ_WRITE, GL_R32F);

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
