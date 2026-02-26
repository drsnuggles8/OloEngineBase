#include "OloEnginePCH.h"
#include "OloEngine/Terrain/TerrainMaterial.h"

#include <stb_image/stb_image.h>

namespace OloEngine
{
    i32 TerrainMaterial::AddLayer(const TerrainLayer& layer)
    {
        OLO_PROFILE_FUNCTION();

        if (m_LayerCount >= MAX_TERRAIN_LAYERS)
        {
            OLO_CORE_WARN("TerrainMaterial: Cannot add layer — maximum {} reached", MAX_TERRAIN_LAYERS);
            return -1;
        }

        m_Layers[m_LayerCount] = layer;
        return static_cast<i32>(m_LayerCount++);
    }

    void TerrainMaterial::RemoveLayer(u32 index)
    {
        OLO_PROFILE_FUNCTION();

        if (index >= m_LayerCount)
        {
            return;
        }

        // Shift layers down
        for (u32 i = index; i < m_LayerCount - 1; ++i)
        {
            m_Layers[i] = m_Layers[i + 1];
        }
        m_Layers[m_LayerCount - 1] = TerrainLayer{};
        --m_LayerCount;
    }

    const TerrainLayer& TerrainMaterial::GetLayer(u32 index) const
    {
        OLO_CORE_ASSERT(index < m_LayerCount, "Layer index out of bounds");
        return m_Layers[index];
    }

    TerrainLayer& TerrainMaterial::GetLayer(u32 index)
    {
        OLO_CORE_ASSERT(index < m_LayerCount, "Layer index out of bounds");
        return m_Layers[index];
    }

    bool TerrainMaterial::LoadTextureData(const std::string& path, u32 targetSize,
                                          std::vector<u8>& outData)
    {
        OLO_PROFILE_FUNCTION();

        if (path.empty())
        {
            return false;
        }

        i32 w = 0, h = 0, channels = 0;
        stbi_set_flip_vertically_on_load_thread(0);
        u8* pixels = stbi_load(path.c_str(), &w, &h, &channels, 4); // Force RGBA
        if (!pixels)
        {
            OLO_CORE_WARN("TerrainMaterial: Failed to load texture '{}'", path);
            return false;
        }

        if (static_cast<u32>(w) == targetSize && static_cast<u32>(h) == targetSize)
        {
            outData.assign(pixels, pixels + static_cast<sizet>(w) * h * 4);
        }
        else
        {
            // Simple nearest-neighbor resize to target size
            outData.resize(static_cast<sizet>(targetSize) * targetSize * 4);
            for (u32 y = 0; y < targetSize; ++y)
            {
                for (u32 x = 0; x < targetSize; ++x)
                {
                    u32 srcX = x * static_cast<u32>(w) / targetSize;
                    u32 srcY = y * static_cast<u32>(h) / targetSize;
                    sizet srcIdx = (static_cast<sizet>(srcY) * w + srcX) * 4;
                    sizet dstIdx = (static_cast<sizet>(y) * targetSize + x) * 4;
                    outData[dstIdx + 0] = pixels[srcIdx + 0];
                    outData[dstIdx + 1] = pixels[srcIdx + 1];
                    outData[dstIdx + 2] = pixels[srcIdx + 2];
                    outData[dstIdx + 3] = pixels[srcIdx + 3];
                }
            }
        }

        stbi_image_free(pixels);
        return true;
    }

    void TerrainMaterial::BuildTextureArrays(u32 layerResolution)
    {
        OLO_PROFILE_FUNCTION();

        if (m_LayerCount == 0)
        {
            OLO_CORE_WARN("TerrainMaterial: No layers to build");
            return;
        }

        if (layerResolution == 0)
        {
            OLO_CORE_ERROR("TerrainMaterial: Invalid layer resolution 0");
            return;
        }

        m_LayerResolution = layerResolution;

        // Create RGBA8 texture arrays with mipmaps
        Texture2DArraySpecification spec;
        spec.Width = layerResolution;
        spec.Height = layerResolution;
        spec.Layers = m_LayerCount;
        spec.Format = Texture2DArrayFormat::RGBA8;
        spec.GenerateMipmaps = true;

        m_AlbedoArray = Texture2DArray::Create(spec);
        m_NormalArray = Texture2DArray::Create(spec);
        m_ARMArray = Texture2DArray::Create(spec);

        if (!m_AlbedoArray || !m_NormalArray || !m_ARMArray)
        {
            OLO_CORE_ERROR("TerrainMaterial::BuildTextureArrays - Failed to create one or more texture arrays");
            return;
        }

        // Default data for layers without textures
        std::vector<u8> defaultAlbedo(static_cast<sizet>(layerResolution) * layerResolution * 4, 128);
        std::vector<u8> defaultNormal(static_cast<sizet>(layerResolution) * layerResolution * 4);
        std::vector<u8> defaultARM(static_cast<sizet>(layerResolution) * layerResolution * 4);

        // Fill default normal map (flat: 128, 128, 255, 255 = tangent-space up)
        for (sizet i = 0; i < defaultNormal.size(); i += 4)
        {
            defaultNormal[i + 0] = 128; // X
            defaultNormal[i + 1] = 128; // Y
            defaultNormal[i + 2] = 255; // Z (up)
            defaultNormal[i + 3] = 255; // A
        }

        // Fill default ARM (AO=255, Roughness=204 (~0.8), Metallic=0)
        for (sizet i = 0; i < defaultARM.size(); i += 4)
        {
            defaultARM[i + 0] = 255; // AO
            defaultARM[i + 1] = 204; // Roughness
            defaultARM[i + 2] = 0;   // Metallic
            defaultARM[i + 3] = 255; // A (height for blending)
        }

        std::vector<u8> texData;

        for (u32 i = 0; i < m_LayerCount; ++i)
        {
            const auto& layer = m_Layers[i];

            // Albedo
            if (LoadTextureData(layer.AlbedoPath, layerResolution, texData))
            {
                m_AlbedoArray->SetLayerData(i, texData.data(), layerResolution, layerResolution);
            }
            else
            {
                // Generate solid color from layer's BaseColor
                auto solidColor = defaultAlbedo;
                u8 r = static_cast<u8>(std::clamp(layer.BaseColor.r, 0.0f, 1.0f) * 255.0f);
                u8 g = static_cast<u8>(std::clamp(layer.BaseColor.g, 0.0f, 1.0f) * 255.0f);
                u8 b = static_cast<u8>(std::clamp(layer.BaseColor.b, 0.0f, 1.0f) * 255.0f);
                for (sizet j = 0; j < solidColor.size(); j += 4)
                {
                    solidColor[j + 0] = r;
                    solidColor[j + 1] = g;
                    solidColor[j + 2] = b;
                    solidColor[j + 3] = 255;
                }
                m_AlbedoArray->SetLayerData(i, solidColor.data(), layerResolution, layerResolution);
            }

            // Normal
            if (LoadTextureData(layer.NormalPath, layerResolution, texData))
            {
                m_NormalArray->SetLayerData(i, texData.data(), layerResolution, layerResolution);
            }
            else
            {
                m_NormalArray->SetLayerData(i, defaultNormal.data(), layerResolution, layerResolution);
            }

            // ARM
            if (LoadTextureData(layer.ARMPath, layerResolution, texData))
            {
                m_ARMArray->SetLayerData(i, texData.data(), layerResolution, layerResolution);
            }
            else
            {
                // Generate from layer defaults
                auto armData = defaultARM;
                u8 roughByte = static_cast<u8>(std::clamp(layer.Roughness, 0.0f, 1.0f) * 255.0f);
                u8 metalByte = static_cast<u8>(std::clamp(layer.Metallic, 0.0f, 1.0f) * 255.0f);
                for (sizet j = 0; j < armData.size(); j += 4)
                {
                    armData[j + 1] = roughByte;
                    armData[j + 2] = metalByte;
                }
                m_ARMArray->SetLayerData(i, armData.data(), layerResolution, layerResolution);
            }
        }

        // Generate mipmaps
        m_AlbedoArray->GenerateMipmaps();
        m_NormalArray->GenerateMipmaps();
        m_ARMArray->GenerateMipmaps();

        OLO_CORE_INFO("TerrainMaterial: Built texture arrays ({} layers, {}×{} per layer)",
                      m_LayerCount, layerResolution, layerResolution);
    }

    void TerrainMaterial::SetSplatmapPath(u32 index, const std::string& path)
    {
        if (index < 2)
        {
            m_SplatmapPaths[index] = path;
        }
    }

    const std::string& TerrainMaterial::GetSplatmapPath(u32 index) const
    {
        static const std::string empty;
        return (index < 2) ? m_SplatmapPaths[index] : empty;
    }

    void TerrainMaterial::LoadSplatmaps()
    {
        OLO_PROFILE_FUNCTION();

        for (u32 i = 0; i < 2; ++i)
        {
            if (!m_SplatmapPaths[i].empty())
            {
                m_Splatmaps[i] = Texture2D::Create(m_SplatmapPaths[i]);
            }
        }
    }

    Ref<Texture2D> TerrainMaterial::GetSplatmap(u32 index) const
    {
        return (index < 2) ? m_Splatmaps[index] : nullptr;
    }

    void TerrainMaterial::InitializeCPUSplatmaps(u32 resolution)
    {
        OLO_PROFILE_FUNCTION();

        if (resolution == 0)
        {
            OLO_CORE_ERROR("TerrainMaterial::InitializeCPUSplatmaps - Cannot initialize with zero resolution");
            return;
        }

        m_SplatmapResolution = resolution;
        sizet totalPixels = static_cast<sizet>(resolution) * resolution * 4; // RGBA8

        for (u32 i = 0; i < 2; ++i)
        {
            m_CPUSplatmaps[i].resize(totalPixels, 0);
        }

        // If splatmaps were loaded from file, read them back into CPU buffers
        for (u32 i = 0; i < 2; ++i)
        {
            if (m_Splatmaps[i])
            {
                std::vector<u8> readback;
                if (m_Splatmaps[i]->GetData(readback))
                {
                    // Resize if needed (readback may differ from target resolution)
                    u32 texW = m_Splatmaps[i]->GetWidth();
                    u32 texH = m_Splatmaps[i]->GetHeight();
                    if (texW == resolution && texH == resolution && readback.size() == totalPixels)
                    {
                        m_CPUSplatmaps[i] = std::move(readback);
                    }
                    else
                    {
                        OLO_CORE_WARN("TerrainMaterial: Splatmap {} size mismatch ({}x{} vs {}), using blank",
                                      i, texW, texH, resolution);
                    }
                }
            }
        }

        // When layers exist (m_LayerCount > 0) but the first splatmap hasn't been loaded
        // (!m_Splatmaps[0]), initialize splatmap 0's R channel to 1.0 so the first layer
        // is fully visible by default
        if (m_LayerCount > 0 && !m_Splatmaps[0])
        {
            for (sizet p = 0; p < totalPixels; p += 4)
            {
                m_CPUSplatmaps[0][p] = 255; // Layer 0 fully on
            }
        }

        // Create GPU splatmap textures from CPU data
        for (u32 i = 0; i < 2; ++i)
        {
            TextureSpecification spec;
            spec.Width = resolution;
            spec.Height = resolution;
            spec.Format = ImageFormat::RGBA8;
            spec.GenerateMips = false;

            m_Splatmaps[i] = Texture2D::Create(spec);
            if (!m_Splatmaps[i])
            {
                OLO_CORE_ERROR("TerrainMaterial::InitializeCPUSplatmaps - Failed to create splatmap texture {}", i);
                continue;
            }
            m_Splatmaps[i]->SetData(m_CPUSplatmaps[i].data(),
                                    static_cast<u32>(m_CPUSplatmaps[i].size()));
        }
    }

    std::vector<u8>& TerrainMaterial::GetSplatmapData(u32 index)
    {
        OLO_CORE_ASSERT(index < 2, "Splatmap index out of bounds");
        return m_CPUSplatmaps[index];
    }

    const std::vector<u8>& TerrainMaterial::GetSplatmapData(u32 index) const
    {
        OLO_CORE_ASSERT(index < 2, "Splatmap index out of bounds");
        return m_CPUSplatmaps[index];
    }

    void TerrainMaterial::UploadSplatmapRegion(u32 splatmapIndex, u32 x, u32 y, u32 w, u32 h)
    {
        OLO_PROFILE_FUNCTION();

        if (splatmapIndex >= 2 || !m_Splatmaps[splatmapIndex] || m_SplatmapResolution == 0)
            return;

        u32 res = m_SplatmapResolution;
        x = std::min(x, res - 1);
        y = std::min(y, res - 1);
        u32 endX = x + std::min(w, res - x);
        u32 endY = y + std::min(h, res - y);
        w = endX - x;
        h = endY - y;

        if (w == 0 || h == 0)
            return;

        // Extract contiguous sub-region
        std::vector<u8> regionData(static_cast<sizet>(w) * h * 4);
        const auto& cpuData = m_CPUSplatmaps[splatmapIndex];
        for (u32 row = 0; row < h; ++row)
        {
            sizet srcOffset = (static_cast<sizet>(y + row) * res + x) * 4;
            sizet dstOffset = static_cast<sizet>(row) * w * 4;
            std::memcpy(&regionData[dstOffset], &cpuData[srcOffset], w * 4);
        }

        u32 dataSize = w * h * 4;
        m_Splatmaps[splatmapIndex]->SubImage(x, y, w, h, regionData.data(), dataSize);
    }

} // namespace OloEngine
