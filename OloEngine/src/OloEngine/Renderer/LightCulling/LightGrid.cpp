#include "OloEnginePCH.h"
#include "OloEngine/Renderer/LightCulling/LightGrid.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"

namespace OloEngine
{
    void LightGrid::Initialize(u32 screenWidth, u32 screenHeight, const LightGridConfig& config)
    {
        OLO_PROFILE_FUNCTION();

        if (screenWidth == 0 || screenHeight == 0 ||
            config.ClusterCountX == 0 || config.ClusterCountY == 0 || config.ClusterCountZ == 0 ||
            config.MaxLightsPerCluster == 0)
        {
            OLO_CORE_ERROR("LightGrid::Initialize: Invalid parameters ({}x{}, clusters {}x{}x{}, {} lights/cluster)",
                           screenWidth, screenHeight,
                           config.ClusterCountX, config.ClusterCountY, config.ClusterCountZ,
                           config.MaxLightsPerCluster);
            m_Initialized = false;
            return;
        }

        m_Config = config;
        m_ScreenWidth = screenWidth;
        m_ScreenHeight = screenHeight;

        if (!CreateBuffers())
        {
            OLO_CORE_ERROR("LightGrid::Initialize: Failed to create GPU buffers");
            m_Initialized = false;
            return;
        }

        m_Initialized = true;

        OLO_CORE_INFO("LightGrid: Initialized {}x{}x{} froxel clusters ({} total, {} lights/cluster max)",
                      m_Config.ClusterCountX, m_Config.ClusterCountY, m_Config.ClusterCountZ,
                      GetTotalClusters(), m_Config.MaxLightsPerCluster);
    }

    void LightGrid::Shutdown()
    {
        m_LightIndexSSBO.Reset();
        m_LightGridSSBO.Reset();
        m_GlobalIndexSSBO.Reset();
        m_Initialized = false;
    }

    void LightGrid::Resize(u32 screenWidth, u32 screenHeight)
    {
        OLO_PROFILE_FUNCTION();

        if (screenWidth == 0 || screenHeight == 0)
        {
            return;
        }

        // The cluster grid is fixed-count, so the SSBOs are resolution-
        // independent — only the viewport dimensions (used for the
        // fragment-side tile scale) change.
        m_ScreenWidth = screenWidth;
        m_ScreenHeight = screenHeight;
    }

    void LightGrid::Bind() const
    {
        OLO_PROFILE_FUNCTION();

        if (m_LightIndexSSBO)
            m_LightIndexSSBO->Bind();
        if (m_LightGridSSBO)
            m_LightGridSSBO->Bind();
        if (m_GlobalIndexSSBO)
            m_GlobalIndexSSBO->Bind();
    }

    void LightGrid::Unbind() const
    {
        OLO_PROFILE_FUNCTION();

        if (m_LightIndexSSBO)
            m_LightIndexSSBO->Unbind();
        if (m_LightGridSSBO)
            m_LightGridSSBO->Unbind();
        if (m_GlobalIndexSSBO)
            m_GlobalIndexSSBO->Unbind();
    }

    void LightGrid::ResetCountersAndIndirectArgs()
    {
        OLO_PROFILE_FUNCTION();

        if (m_GlobalIndexSSBO)
        {
            // Word zero is the light-index append counter. Words 1..3 are a
            // DispatchIndirectCommand populated by DepthPrepare.comp; Y/Z
            // remain one while X counts compacted active clusters.
            constexpr std::array<u32, ClusteredLighting::kGlobalCounterAndDispatchWordCount> seed = {
                0u, 0u, 1u, 1u
            };
            m_GlobalIndexSSBO->SetData(seed.data(), static_cast<u32>(sizeof(seed)));
        }
    }

    void LightGrid::ClearLightGrid()
    {
        if (m_LightGridSSBO)
        {
            m_LightGridSSBO->ClearData();
        }
    }

    bool LightGrid::CreateBuffers()
    {
        OLO_PROFILE_FUNCTION();

        const u32 totalClusters = GetTotalClusters();

        // The established shading prefix remains worst-case cluster capacity;
        // one u32 per cluster is appended for the compact active-cluster list.
        const u32 lightIndexStorageWords =
            ClusteredLighting::LightIndexStorageWords(totalClusters, m_Config.MaxLightsPerCluster);
        const u32 lightIndexBufferSize = lightIndexStorageWords * sizeof(u32);

        // The established (offset,count) prefix is followed by four words per
        // screen tile: min depth, max depth, occupancy, active-slice mask.
        const u32 lightGridStorageWords =
            ClusteredLighting::LightGridStorageWords(totalClusters, GetTileCount());
        const u32 lightGridBufferSize = lightGridStorageWords * sizeof(u32);

        // Light-list append counter followed by an indirect uvec3 dispatch.
        const u32 globalIndexBufferSize =
            ClusteredLighting::kGlobalCounterAndDispatchWordCount * sizeof(u32);

        m_LightIndexSSBO = StorageBuffer::Create(
            lightIndexBufferSize, ShaderBindingLayout::SSBO_FPLUS_LIGHT_INDICES,
            StorageBufferUsage::DynamicCopy);

        m_LightGridSSBO = StorageBuffer::Create(
            lightGridBufferSize, ShaderBindingLayout::SSBO_FPLUS_LIGHT_GRID,
            StorageBufferUsage::DynamicCopy);

        m_GlobalIndexSSBO = StorageBuffer::Create(
            globalIndexBufferSize, ShaderBindingLayout::SSBO_FPLUS_GLOBAL_INDEX,
            StorageBufferUsage::DynamicCopy);

        if (!m_LightIndexSSBO || !m_LightGridSSBO || !m_GlobalIndexSSBO)
        {
            OLO_CORE_ERROR("LightGrid::CreateBuffers: Failed to create one or more SSBOs");
            m_LightIndexSSBO.Reset();
            m_LightGridSSBO.Reset();
            m_GlobalIndexSSBO.Reset();
            return false;
        }

        OLO_CORE_TRACE("LightGrid: Created buffers — index list: {}KB, grid: {}KB",
                       lightIndexBufferSize / 1024, lightGridBufferSize / 1024);
        return true;
    }
} // namespace OloEngine
