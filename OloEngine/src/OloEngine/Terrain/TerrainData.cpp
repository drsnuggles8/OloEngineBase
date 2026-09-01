#include "OloEnginePCH.h"
#include "OloEngine/Terrain/TerrainData.h"
#include "OloEngine/Particle/SimplexNoise.h"

#include <stb_image/stb_image.h>
#include <algorithm>
#include <cmath>
#include <fstream>

namespace OloEngine
{
    bool TerrainData::LoadFromFile(const std::string& path)
    {
        OLO_PROFILE_FUNCTION();

        // Try loading as 16-bit PNG first
        int width = 0;
        int height = 0;
        int channels = 0;

        stbi_set_flip_vertically_on_load_thread(0);
        if (stbi_us* data16 = stbi_load_16(path.c_str(), &width, &height, &channels, 1); data16)
        {
            if (width != height)
            {
                OLO_CORE_ERROR("TerrainData::LoadFromFile: Non-square heightmap {}x{} not supported in '{}'", width, height, path);
                stbi_image_free(data16);
                return false;
            }

            m_Resolution = static_cast<u32>(width);
            m_Heights.resize(static_cast<sizet>(width) * static_cast<sizet>(height));

            // Normalize 16-bit [0, 65535] → [0, 1]
            constexpr f32 inv65535 = 1.0f / 65535.0f;
            for (sizet i = 0; i < m_Heights.size(); ++i)
            {
                m_Heights[i] = static_cast<f32>(data16[i]) * inv65535;
            }

            stbi_image_free(data16);
            UploadToGPU();
            OLO_CORE_INFO("TerrainData: Loaded {}x{} heightmap from '{}'", m_Resolution, m_Resolution, path);
            return true;
        }

        // Fallback: try 8-bit
        if (stbi_uc* data8 = stbi_load(path.c_str(), &width, &height, &channels, 1); data8)
        {
            if (width != height)
            {
                OLO_CORE_ERROR("TerrainData::LoadFromFile: Non-square heightmap {}x{} not supported in '{}'", width, height, path);
                stbi_image_free(data8);
                return false;
            }

            m_Resolution = static_cast<u32>(width);
            m_Heights.resize(static_cast<sizet>(width) * static_cast<sizet>(height));

            constexpr f32 inv255 = 1.0f / 255.0f;
            for (sizet i = 0; i < m_Heights.size(); ++i)
            {
                m_Heights[i] = static_cast<f32>(data8[i]) * inv255;
            }

            stbi_image_free(data8);
            UploadToGPU();
            OLO_CORE_INFO("TerrainData: Loaded {}x{} (8-bit) heightmap from '{}'", m_Resolution, m_Resolution, path);
            return true;
        }

        OLO_CORE_ERROR("TerrainData::LoadFromFile: Failed to load '{}'", path);
        return false;
    }

    void TerrainData::CreateFlat(u32 resolution, f32 defaultHeight)
    {
        OLO_PROFILE_FUNCTION();

        m_Resolution = resolution;
        m_Heights.assign(static_cast<sizet>(resolution) * static_cast<sizet>(resolution), defaultHeight);
        UploadToGPU();
    }

    void TerrainData::GenerateProcedural(u32 resolution, i32 seed, u32 octaves,
                                         f32 frequency, f32 amplitude,
                                         f32 lacunarity, f32 persistence)
    {
        OLO_PROFILE_FUNCTION();

        m_Resolution = resolution;
        sizet totalPixels = static_cast<sizet>(resolution) * resolution;
        m_Heights.resize(totalPixels);

        f32 seedOffset = static_cast<f32>(seed) * 13.37f;
        f32 minH = std::numeric_limits<f32>::max();
        f32 maxH = std::numeric_limits<f32>::lowest();

        for (u32 z = 0; z < resolution; ++z)
        {
            for (u32 x = 0; x < resolution; ++x)
            {
                f32 nx = static_cast<f32>(x) / static_cast<f32>(resolution);
                f32 nz = static_cast<f32>(z) / static_cast<f32>(resolution);

                f32 value = 0.0f;
                f32 freq = frequency;
                f32 amp = amplitude;
                for (u32 o = 0; o < octaves; ++o)
                {
                    value += SimplexNoise3D(nx * freq + seedOffset,
                                            0.0f,
                                            nz * freq + seedOffset) *
                             amp;
                    freq *= lacunarity;
                    amp *= persistence;
                }

                sizet idx = static_cast<sizet>(z) * resolution + x;
                m_Heights[idx] = value;
                minH = std::min(minH, value);
                maxH = std::max(maxH, value);
            }
        }

        // Normalize to [0, 1]
        if (f32 range = maxH - minH; range > 1e-6f)
        {
            f32 invRange = 1.0f / range;
            for (auto& h : m_Heights)
            {
                h = (h - minH) * invRange;
            }
        }

        UploadToGPU();
        OLO_CORE_INFO("TerrainData: Generated {}x{} procedural terrain (seed={}, octaves={}, freq={:.1f})",
                      resolution, resolution, seed, octaves, frequency);
    }

    void TerrainData::SetHeights(u32 resolution, std::vector<f32> heights)
    {
        OLO_PROFILE_FUNCTION();

        if (resolution == 0 || heights.size() != static_cast<sizet>(resolution) * resolution)
        {
            OLO_CORE_ERROR("TerrainData::SetHeights - height count {} does not match resolution {}x{}",
                           heights.size(), resolution, resolution);
            return;
        }

        m_Resolution = resolution;
        m_Heights = std::move(heights);
        UploadToGPU();
    }

    f32 TerrainData::SampleHeight(const std::vector<f32>& heights, u32 resolution,
                                  f32 normalizedX, f32 normalizedZ)
    {
        if (heights.empty() || resolution == 0 ||
            heights.size() != static_cast<sizet>(resolution) * resolution)
        {
            return 0.0f;
        }

        f32 fx = std::clamp(normalizedX, 0.0f, 1.0f) * static_cast<f32>(resolution - 1);
        f32 fz = std::clamp(normalizedZ, 0.0f, 1.0f) * static_cast<f32>(resolution - 1);

        u32 x0 = static_cast<u32>(fx);
        u32 z0 = static_cast<u32>(fz);
        u32 x1 = std::min(x0 + 1, resolution - 1);
        u32 z1 = std::min(z0 + 1, resolution - 1);

        f32 fracX = fx - static_cast<f32>(x0);
        f32 fracZ = fz - static_cast<f32>(z0);

        f32 h00 = heights[static_cast<sizet>(z0) * resolution + x0];
        f32 h10 = heights[static_cast<sizet>(z0) * resolution + x1];
        f32 h01 = heights[static_cast<sizet>(z1) * resolution + x0];
        f32 h11 = heights[static_cast<sizet>(z1) * resolution + x1];

        f32 h0 = h00 + fracX * (h10 - h00);
        f32 h1 = h01 + fracX * (h11 - h01);
        return h0 + fracZ * (h1 - h0);
    }

    glm::vec3 TerrainData::SampleNormal(const std::vector<f32>& heights, u32 resolution,
                                        f32 normalizedX, f32 normalizedZ, f32 worldSizeX,
                                        f32 worldSizeZ, f32 heightScale)
    {
        if (resolution == 0)
            return { 0.0f, 1.0f, 0.0f };

        f32 texelSize = 1.0f / static_cast<f32>(resolution);

        f32 hL = SampleHeight(heights, resolution, normalizedX - texelSize, normalizedZ) * heightScale;
        f32 hR = SampleHeight(heights, resolution, normalizedX + texelSize, normalizedZ) * heightScale;
        f32 hD = SampleHeight(heights, resolution, normalizedX, normalizedZ - texelSize) * heightScale;
        f32 hU = SampleHeight(heights, resolution, normalizedX, normalizedZ + texelSize) * heightScale;

        f32 dx = 2.0f * worldSizeX * texelSize;
        f32 dz = 2.0f * worldSizeZ * texelSize;

        if (constexpr f32 epsilon = 1e-6f; dx < epsilon || dz < epsilon)
            return { 0.0f, 1.0f, 0.0f };

        glm::vec3 normal(
            (hL - hR) / dx,
            1.0f,
            (hD - hU) / dz);

        return glm::normalize(normal);
    }

    f32 TerrainData::SampleSlopeDegrees(const std::vector<f32>& heights, u32 resolution,
                                        f32 normalizedX, f32 normalizedZ, f32 worldSizeX,
                                        f32 worldSizeZ, f32 heightScale)
    {
        const glm::vec3 normal =
            SampleNormal(heights, resolution, normalizedX, normalizedZ, worldSizeX, worldSizeZ, heightScale);
        return glm::degrees(std::acos(std::clamp(normal.y, -1.0f, 1.0f)));
    }

    f32 TerrainData::GetHeightAt(f32 normalizedX, f32 normalizedZ) const
    {
        SyncFromGPU();
        return SampleHeight(m_Heights, m_Resolution, normalizedX, normalizedZ);
    }

    glm::vec3 TerrainData::GetNormalAt(f32 normalizedX, f32 normalizedZ, f32 worldSizeX, f32 worldSizeZ, f32 heightScale) const
    {
        SyncFromGPU();
        return SampleNormal(m_Heights, m_Resolution, normalizedX, normalizedZ, worldSizeX, worldSizeZ, heightScale);
    }

    void TerrainData::UploadToGPU()
    {
        OLO_PROFILE_FUNCTION();

        // A CPU-side write that reaches the GPU puts the two back in agreement,
        // so any pending GPU-newer flag is discharged rather than left to trigger
        // a readback that would undo this upload.
        m_CPUMirrorStale = false;

        if (m_Resolution == 0)
        {
            return;
        }

        // Create or recreate the GPU texture
        TextureSpecification spec;
        spec.Width = m_Resolution;
        spec.Height = m_Resolution;
        spec.Format = ImageFormat::R32F;
        spec.GenerateMips = true;

        m_GPUHeightmap = Texture2D::Create(spec);
        m_GPUHeightmap->SetData(m_Heights.data(), static_cast<u32>(m_Heights.size() * sizeof(f32)));
    }

    void TerrainData::UploadRegionToGPU(u32 x, u32 y, u32 width, u32 height)
    {
        OLO_PROFILE_FUNCTION();

        // The caller reached the CPU mirror through GetHeightData(), which synced
        // it first, so the mirror is a superset of the GPU state and this partial
        // upload leaves the two in agreement.
        m_CPUMirrorStale = false;

        if (!m_GPUHeightmap || m_Resolution == 0)
        {
            UploadToGPU();
            return;
        }

        // Clamp region to heightmap bounds
        x = std::min(x, m_Resolution - 1);
        y = std::min(y, m_Resolution - 1);
        u32 endX = x + std::min(width, m_Resolution - x);
        u32 endY = y + std::min(height, m_Resolution - y);
        width = endX - x;
        height = endY - y;

        if (width == 0 || height == 0)
            return;

        if (m_GPUHeightmap->GetMipLevelCount() > 1)
        {
            // Every coarser level depends on the edited base region. Texture2D has
            // no backend-neutral partial-mip regeneration command, so refresh the
            // complete base image and let SetData rebuild the chain on both APIs.
            // Terrain sculpt uploads happen before the frame is recorded.
            m_GPUHeightmap->SetData(m_Heights.data(), static_cast<u32>(m_Heights.size() * sizeof(f32)));
            return;
        }

        // Extract the sub-region into a contiguous buffer.
        std::vector<f32> regionData(static_cast<sizet>(width) * height);
        for (u32 row = 0; row < height; ++row)
        {
            sizet srcOffset = static_cast<sizet>(y + row) * m_Resolution + x;
            sizet dstOffset = static_cast<sizet>(row) * width;
            std::memcpy(&regionData[dstOffset], &m_Heights[srcOffset], width * sizeof(f32));
        }

        // Single-level textures can retain the cheap partial upload.
        u32 dataSize = width * height * static_cast<u32>(sizeof(f32));
        m_GPUHeightmap->SubImage(x, y, width, height, regionData.data(), dataSize);
    }

    void TerrainData::SyncFromGPU() const
    {
        OLO_PROFILE_FUNCTION();

        if (!m_CPUMirrorStale)
        {
            return;
        }

        // Cleared FIRST and unconditionally. Every early-out below is a case where
        // the mirror cannot be refreshed at all (no GPU texture, a failed
        // readback, a size disagreement), and leaving the flag set there would
        // re-attempt the same doomed readback on every subsequent height query —
        // turning a one-off failure into a per-frame stall.
        m_CPUMirrorStale = false;

        if (!m_GPUHeightmap || m_Resolution == 0)
        {
            return;
        }

        std::vector<u8> rawData;
        if (!m_GPUHeightmap->GetData(rawData))
        {
            OLO_CORE_ERROR("TerrainData::SyncFromGPU - Failed to read back the GPU heightmap");
            return;
        }

        const sizet expectedBytes = static_cast<sizet>(m_Resolution) * m_Resolution * sizeof(f32);
        if (rawData.size() != expectedBytes)
        {
            OLO_CORE_ERROR("TerrainData::SyncFromGPU - Readback size mismatch: got {} bytes, expected {}",
                           rawData.size(), expectedBytes);
            return;
        }

        m_Heights.resize(static_cast<sizet>(m_Resolution) * m_Resolution);
        std::memcpy(m_Heights.data(), rawData.data(), rawData.size());
    }

    bool TerrainData::ExportRawR32F(const std::string& path) const
    {
        OLO_PROFILE_FUNCTION();

        SyncFromGPU();
        if (m_Heights.empty() || m_Resolution == 0)
        {
            OLO_CORE_ERROR("TerrainData::ExportRawR32F - No heightmap data to export");
            return false;
        }

        std::ofstream file(path, std::ios::binary);
        if (!file.is_open())
        {
            OLO_CORE_ERROR("TerrainData::ExportRawR32F - Failed to open file: {}", path);
            return false;
        }

        file.write(reinterpret_cast<const char*>(m_Heights.data()), static_cast<std::streamsize>(m_Heights.size() * sizeof(f32)));
        OLO_CORE_INFO("TerrainData: Exported R32F heightmap ({0}x{0}) to {1}", m_Resolution, path);
        return file.good();
    }

    bool TerrainData::ExportRawR16(const std::string& path) const
    {
        OLO_PROFILE_FUNCTION();

        SyncFromGPU();
        if (m_Heights.empty() || m_Resolution == 0)
        {
            OLO_CORE_ERROR("TerrainData::ExportRawR16 - No heightmap data to export");
            return false;
        }

        std::ofstream file(path, std::ios::binary);
        if (!file.is_open())
        {
            OLO_CORE_ERROR("TerrainData::ExportRawR16 - Failed to open file: {}", path);
            return false;
        }

        std::vector<u16> quantized(m_Heights.size());
        for (sizet i = 0; i < m_Heights.size(); ++i)
        {
            f32 clamped = glm::clamp(m_Heights[i], 0.0f, 1.0f);
            quantized[i] = static_cast<u16>(clamped * 65535.0f + 0.5f);
        }

        file.write(reinterpret_cast<const char*>(quantized.data()), static_cast<std::streamsize>(quantized.size() * sizeof(u16)));
        OLO_CORE_INFO("TerrainData: Exported R16 heightmap ({0}x{0}) to {1}", m_Resolution, path);
        return file.good();
    }
} // namespace OloEngine
