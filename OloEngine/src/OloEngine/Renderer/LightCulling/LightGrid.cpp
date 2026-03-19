#include "OloEnginePCH.h"
#include "OloEngine/Renderer/LightCulling/LightGrid.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"

namespace OloEngine
{
    void LightGrid::Initialize(u32 screenWidth, u32 screenHeight, const LightGridConfig& config)
    {
        OLO_PROFILE_FUNCTION();

        m_Config = config;
        m_ScreenWidth = screenWidth;
        m_ScreenHeight = screenHeight;
        m_TileCountX = (screenWidth + config.TileSizePixels - 1) / config.TileSizePixels;
        m_TileCountY = (screenHeight + config.TileSizePixels - 1) / config.TileSizePixels;

        CreateBuffers();
        m_Initialized = true;

        OLO_CORE_INFO("LightGrid: Initialized {}x{} tiles ({}px), {} depth slices, {} total clusters",
                      m_TileCountX, m_TileCountY, config.TileSizePixels,
                      config.DepthSlices, GetTotalClusters());
    }

    void LightGrid::Resize(u32 screenWidth, u32 screenHeight)
    {
        OLO_PROFILE_FUNCTION();

        if (screenWidth == 0 || screenHeight == 0)
        {
            return;
        }

        if (screenWidth == m_ScreenWidth && screenHeight == m_ScreenHeight)
        {
            return;
        }

        m_ScreenWidth = screenWidth;
        m_ScreenHeight = screenHeight;
        m_TileCountX = (screenWidth + m_Config.TileSizePixels - 1) / m_Config.TileSizePixels;
        m_TileCountY = (screenHeight + m_Config.TileSizePixels - 1) / m_Config.TileSizePixels;

        CreateBuffers();

        OLO_CORE_TRACE("LightGrid: Resized to {}x{} tiles", m_TileCountX, m_TileCountY);
    }

    void LightGrid::Bind() const
    {
        if (m_LightIndexSSBO)
            m_LightIndexSSBO->Bind();
        if (m_LightGridSSBO)
            m_LightGridSSBO->Bind();
        if (m_GlobalIndexSSBO)
            m_GlobalIndexSSBO->Bind();
    }

    void LightGrid::Unbind() const
    {
        if (m_LightIndexSSBO)
            m_LightIndexSSBO->Unbind();
        if (m_LightGridSSBO)
            m_LightGridSSBO->Unbind();
        if (m_GlobalIndexSSBO)
            m_GlobalIndexSSBO->Unbind();
    }

    void LightGrid::ResetAtomicCounter()
    {
        if (m_GlobalIndexSSBO)
        {
            u32 zero = 0;
            m_GlobalIndexSSBO->SetData(&zero, sizeof(u32));
        }
    }

    void LightGrid::CreateBuffers()
    {
        OLO_PROFILE_FUNCTION();

        const u32 totalClusters = GetTotalClusters();

        // Light index list: worst case each tile has MaxLightsPerTile lights
        // In practice, we size for average occupancy. Use totalClusters * MaxLightsPerTile as upper bound.
        const u32 lightIndexCapacity = totalClusters * m_Config.MaxLightsPerTile;
        const u32 lightIndexBufferSize = lightIndexCapacity * sizeof(u32);

        // Light grid: 2 u32s per cluster (offset, count)
        const u32 lightGridBufferSize = totalClusters * 2 * sizeof(u32);

        // Global atomic counter: single u32
        const u32 globalIndexBufferSize = sizeof(u32);

        m_LightIndexSSBO = StorageBuffer::Create(
            lightIndexBufferSize, ShaderBindingLayout::SSBO_FPLUS_LIGHT_INDICES,
            StorageBufferUsage::DynamicCopy);

        m_LightGridSSBO = StorageBuffer::Create(
            lightGridBufferSize, ShaderBindingLayout::SSBO_FPLUS_LIGHT_GRID,
            StorageBufferUsage::DynamicCopy);

        m_GlobalIndexSSBO = StorageBuffer::Create(
            globalIndexBufferSize, ShaderBindingLayout::SSBO_FPLUS_GLOBAL_INDEX,
            StorageBufferUsage::DynamicCopy);

        OLO_CORE_TRACE("LightGrid: Created buffers — index list: {}KB, grid: {}KB",
                       lightIndexBufferSize / 1024, lightGridBufferSize / 1024);
    }
} // namespace OloEngine
