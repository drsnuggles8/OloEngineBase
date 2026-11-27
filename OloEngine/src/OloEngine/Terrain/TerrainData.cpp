#include "OloEnginePCH.h"
#include "OloEngine/Terrain/TerrainData.h"
#include "OloEngine/Math/Math.h"
#include "OloEngine/Particle/SimplexNoise.h"
#include "OloEngine/Renderer/RenderCommand.h"

#include <stb_image/stb_image.h>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>

namespace OloEngine
{
    std::optional<glm::vec2> TerrainData::GetHeightRange() const
    {
        // Erosion can move heights outside the authored normalized range.
        // Do not force the editor's intentionally deferred stroke-end readback;
        // Pending or failed GPU synchronization keeps bounds unknown until a
        // successful readback or full CPU upload makes the mirror authoritative.
        if (!m_HeightBoundsValid)
            return std::nullopt;
        if (m_CachedHeightRevision != m_HeightRevision)
        {
            m_CachedHeightRange = glm::vec2(0.0f);
            if (!m_Heights.IsEmpty())
            {
                const auto range = std::minmax_element(m_Heights.begin(), m_Heights.end());
                m_CachedHeightRange = glm::vec2(*range.first, *range.second);
            }
            m_CachedHeightRevision = m_HeightRevision;
        }
        return m_CachedHeightRange;
    }

    bool TerrainData::DecodeHeightmapFile(const std::string& path, TArray<f32>& outHeights, u32& outResolution)
    {
        OLO_PROFILE_FUNCTION();

        outHeights.Reset();
        outResolution = 0;

        int width = 0;
        int height = 0;
        int channels = 0;

        // Row 0 of the image is the tile's z = 0 edge. The flip latch is
        // per-thread state another loader may have left set.
        stbi_set_flip_vertically_on_load_thread(0);

        // 16-bit first. stb widens an 8-bit file for this call, so the 8-bit
        // case needs no second decode; an 8-bit grey is simply coarser.
        stbi_us* texels = stbi_load_16(path.c_str(), &width, &height, &channels, 1);
        if (!texels)
        {
            OLO_CORE_ERROR("TerrainData: failed to load heightmap '{}': {}", path, stbi_failure_reason());
            return false;
        }
        if (width != height || width < 2)
        {
            OLO_CORE_ERROR("TerrainData: heightmap '{}' is {}x{}; a heightmap is square and at least 2 texels wide",
                           path, width, height);
            stbi_image_free(texels);
            return false;
        }

        outResolution = static_cast<u32>(width);
        const sizet count = static_cast<sizet>(width) * static_cast<sizet>(height);
        outHeights.SetNum(static_cast<TArray<f32>::SizeType>(count), EAllowShrinking::No);
        constexpr f32 inv65535 = 1.0f / 65535.0f;
        for (sizet i = 0; i < count; ++i)
        {
            outHeights[static_cast<TArray<f32>::SizeType>(i)] = static_cast<f32>(texels[i]) * inv65535;
        }
        stbi_image_free(texels);
        return true;
    }

    Ref<TerrainData> TerrainData::FromCPUHeights(u32 resolution, TArray<f32> heights)
    {
        if (resolution == 0 || heights.Num() != static_cast<sizet>(resolution) * resolution)
            return nullptr;
        auto data = Ref<TerrainData>::Create();
        data->m_Resolution = resolution;
        data->m_Heights = std::move(heights);
        return data;
    }

    bool TerrainData::LoadFromFile(const std::string& path)
    {
        OLO_PROFILE_FUNCTION();

        TArray<f32> heights;
        u32 resolution = 0;
        if (!DecodeHeightmapFile(path, heights, resolution))
        {
            return false;
        }

        m_Resolution = resolution;
        m_Heights = std::move(heights);
        UploadToGPU();
        OLO_CORE_INFO("TerrainData: Loaded {}x{} heightmap from '{}'", m_Resolution, m_Resolution, path);
        return true;
    }

    void TerrainData::CreateFlat(u32 resolution, f32 defaultHeight)
    {
        OLO_PROFILE_FUNCTION();

        m_Resolution = resolution;
        m_Heights.Init(defaultHeight, static_cast<sizet>(resolution) * resolution);
        UploadToGPU();
    }

    void TerrainData::GenerateProcedural(u32 resolution, i32 seed, u32 octaves,
                                         f32 frequency, f32 amplitude,
                                         f32 lacunarity, f32 persistence)
    {
        OLO_PROFILE_FUNCTION();

        m_Resolution = resolution;
        sizet totalPixels = static_cast<sizet>(resolution) * resolution;
        m_Heights.SetNum(totalPixels, EAllowShrinking::No);

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

    void TerrainData::SetHeights(u32 resolution, TArray<f32> heights)
    {
        OLO_PROFILE_FUNCTION();

        if (resolution == 0 || heights.Num() != static_cast<sizet>(resolution) * resolution)
        {
            OLO_CORE_ERROR("TerrainData::SetHeights - height count {} does not match resolution {}x{}",
                           heights.Num(), resolution, resolution);
            return;
        }

        m_Resolution = resolution;
        m_Heights = std::move(heights);
        UploadToGPU();
    }

    f32 TerrainData::SampleHeight(const TArray<f32>& heights, u32 resolution,
                                  f32 normalizedX, f32 normalizedZ)
    {
        if (heights.IsEmpty() || resolution == 0 ||
            heights.Num() != static_cast<sizet>(resolution) * resolution)
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

    glm::vec3 TerrainData::SampleNormal(const TArray<f32>& heights, u32 resolution,
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

    f32 TerrainData::SampleSlopeDegrees(const TArray<f32>& heights, u32 resolution,
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

    std::optional<f32> TerrainData::PickRay(glm::vec3 origin, glm::vec3 direction,
                                            glm::vec2 tileSize, f32 heightScale, f32 maxDistance) const
    {
        if (!Math::IsFinite(origin) || !Math::IsFinite(direction) || !std::isfinite(tileSize.x) || !std::isfinite(tileSize.y) ||
            !(tileSize.x > 0.0f && tileSize.y > 0.0f) || !std::isfinite(heightScale) ||
            !std::isfinite(maxDistance) || maxDistance < 0.0f || m_Resolution == 0u)
            return std::nullopt;

        // Clip to the tile footprint first, including fractional entry and exit
        // distances. A clamped height sampled outside the tile cannot bracket a hit.
        f64 start = 0.0, end = maxDistance;
        const auto clip = [&](f64 o, f64 d, f64 extent)
        {
            if (std::fpclassify(d) == FP_ZERO)
                return o >= 0.0 && o <= extent;
            const f64 a = -o / d, b = (extent - o) / d;
            start = std::max(start, std::min(a, b));
            end = std::min(end, std::max(a, b));
            return start <= end;
        };
        if (!clip(origin.x, direction.x, tileSize.x) || !clip(origin.z, direction.z, tileSize.y))
            return std::nullopt;

        SyncFromGPU();
        if (m_Heights.Num() != static_cast<sizet>(m_Resolution) * m_Resolution)
            return std::nullopt;
        const auto cells = static_cast<i32>(std::max(1u, m_Resolution - 1));
        const glm::dvec2 gridScale = f64(cells) / glm::dvec2(tileSize);
        const glm::dvec2 gridOrigin = glm::dvec2(origin.x, origin.z) * gridScale;
        const glm::dvec2 gridDirection = glm::dvec2(direction.x, direction.z) * gridScale;
        glm::ivec2 cell(glm::clamp(gridOrigin + start * gridDirection, glm::dvec2(0), glm::dvec2(cells - 1)));
        const glm::ivec2 step(glm::sign(gridDirection));
        const auto crossing = [&](glm::length_t axis)
        {
            if (step[axis] == 0)
                return std::numeric_limits<f64>::infinity();
            const i32 edge = cell[axis] + (step[axis] > 0 ? 1 : 0);
            return (edge - gridOrigin[axis]) / gridDirection[axis];
        };
        const auto height = [&](i32 x, i32 z)
        {
            const auto last = static_cast<i32>(m_Resolution - 1);
            return static_cast<f64>(m_Heights[static_cast<sizet>(std::min(z, last)) * m_Resolution + std::min(x, last)]) * heightScale;
        };
        const auto firstRoot = [](f64 a, f64 b, f64 c) -> std::optional<f64>
        {
            constexpr f64 tolerance = 8.0 * std::numeric_limits<f64>::epsilon();
            const auto inSegment = [](f64 root)
            { return root >= -tolerance && root <= 1.0 + tolerance; };
            if (std::fpclassify(c) == FP_ZERO)
                return 0.0;
            if (std::fpclassify(a) == FP_ZERO)
            {
                if (std::fpclassify(b) != FP_ZERO && inSegment(-c / b))
                    return std::clamp(-c / b, 0.0, 1.0);
                return std::nullopt;
            }
            const f64 product = 4.0 * a * c;
            const f64 discriminant = b * b - product;
            if (discriminant < -tolerance * (b * b + std::abs(product)))
                return std::nullopt;
            // Compute the second root from the product of the roots to avoid
            // cancellation when one intersection is much closer than the other.
            const f64 q = -0.5 * (b + std::copysign(std::sqrt(std::max(discriminant, 0.0)), b));
            const f64 r0 = q / a;
            const f64 r1 = std::fpclassify(q) == FP_ZERO ? r0 : c / q;
            if (const f64 nearRoot = std::min(r0, r1); inSegment(nearRoot))
                return std::clamp(nearRoot, 0.0, 1.0);
            if (const f64 farRoot = std::max(r0, r1); inSegment(farRoot))
                return std::clamp(farRoot, 0.0, 1.0);
            return std::nullopt;
        };

        while (cell.x >= 0 && cell.x < cells && cell.y >= 0 && cell.y < cells)
        {
            const f64 crossX = crossing(0), crossZ = crossing(1);
            const f64 next = std::clamp(std::min(crossX, crossZ), start, end);
            const f64 span = next - start;
            const glm::dvec2 uv = glm::clamp(gridOrigin + start * gridDirection - glm::dvec2(cell),
                                             glm::dvec2(0), glm::dvec2(1));
            const glm::dvec2 delta = gridDirection * span;
            const f64 h00 = height(cell.x, cell.y);
            const f64 hx = height(cell.x + 1, cell.y) - h00;
            const f64 hz = height(cell.x, cell.y + 1) - h00;
            const f64 hxz = height(cell.x + 1, cell.y + 1) - h00 - hx - hz;
            // The ray and bilinear patch meet where their height difference is
            // zero. Parameterize this cell's segment by a fraction in [0, 1].
            const f64 a = -hxz * delta.x * delta.y;
            const f64 b = direction.y * span - hx * delta.x - hz * delta.y - hxz * (uv.x * delta.y + uv.y * delta.x);
            const f64 c = origin.y + direction.y * start - (h00 + hx * uv.x + hz * uv.y + hxz * uv.x * uv.y);
            if (const auto root = firstRoot(a, b, c))
                return static_cast<f32>(start + *root * span);

            if (next >= end)
                break;
            if (crossX <= crossZ)
                cell.x += step.x;
            if (crossZ <= crossX)
                cell.y += step.y;
            start = next;
        }
        return std::nullopt;
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
        m_HeightBoundsValid = true;
        ++m_HeightRevision; // height content changed — see GetHeightRevision()

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
        m_GPUHeightmap->SetData(m_Heights.GetData(), static_cast<u32>(m_Heights.Num() * sizeof(f32)));
        // A height map covers its tile once. Under the default repeat wrap the
        // filter at u or v = 0 takes half its weight from the OPPOSITE edge, so
        // a tile whose two edges differ grows a wall along the lower one.
        RenderCommand::SetTextureWrap(m_GPUHeightmap->GetRHIHandle(), RHI::AddressMode::ClampToEdge);
    }

    void TerrainData::UploadRegionToGPU(u32 x, u32 y, u32 width, u32 height)
    {
        OLO_PROFILE_FUNCTION();

        // The caller reached the CPU mirror through GetHeightData(), which synced
        // it first, so the mirror is a superset of the GPU state and this partial
        // upload leaves the two in agreement.
        m_CPUMirrorStale = false;
        ++m_HeightRevision; // height content changed — see GetHeightRevision()

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
            m_GPUHeightmap->SetData(m_Heights.GetData(), static_cast<u32>(m_Heights.Num() * sizeof(f32)));
            m_HeightBoundsValid = true;
            return;
        }

        // Extract the sub-region into a contiguous buffer.
        TArray<f32> regionData(static_cast<sizet>(width) * height);
        for (u32 row = 0; row < height; ++row)
        {
            sizet srcOffset = static_cast<sizet>(y + row) * m_Resolution + x;
            sizet dstOffset = static_cast<sizet>(row) * width;
            std::memcpy(&regionData[dstOffset], &m_Heights[srcOffset], width * sizeof(f32));
        }

        // Single-level textures can retain the cheap partial upload.
        u32 dataSize = width * height * static_cast<u32>(sizeof(f32));
        m_GPUHeightmap->SubImage(x, y, width, height, regionData.GetData(), dataSize);
        if (x == 0 && y == 0 && width == m_Resolution && height == m_Resolution)
            m_HeightBoundsValid = true;
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

        TArray64<u8> rawData;
        if (!m_GPUHeightmap->GetData(rawData))
        {
            OLO_CORE_ERROR("TerrainData::SyncFromGPU - Failed to read back the GPU heightmap");
            return;
        }

        const sizet expectedBytes = static_cast<sizet>(m_Resolution) * m_Resolution * sizeof(f32);
        if (rawData.Num() != expectedBytes)
        {
            OLO_CORE_ERROR("TerrainData::SyncFromGPU - Readback size mismatch: got {} bytes, expected {}",
                           rawData.Num(), expectedBytes);
            return;
        }

        m_Heights.SetNum(static_cast<sizet>(m_Resolution) * m_Resolution, EAllowShrinking::No);
        std::memcpy(m_Heights.GetData(), rawData.GetData(), rawData.Num());
        m_HeightBoundsValid = true;
        // The GPU is authoritative during sculpting, so this memcpy is where a
        // brush stroke becomes visible to every CPU consumer — and it lands at
        // the SAME address every time. Bumping here is the only signal a cache
        // keyed on the field's identity can see.
        ++m_HeightRevision;
    }

    bool TerrainData::ExportRawR32F(const std::string& path) const
    {
        OLO_PROFILE_FUNCTION();

        SyncFromGPU();
        if (m_Heights.IsEmpty() || m_Resolution == 0)
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

        file.write(reinterpret_cast<const char*>(m_Heights.GetData()), static_cast<std::streamsize>(m_Heights.Num() * sizeof(f32)));
        OLO_CORE_INFO("TerrainData: Exported R32F heightmap ({0}x{0}) to {1}", m_Resolution, path);
        return file.good();
    }

    bool TerrainData::ExportRawR16(const std::string& path) const
    {
        OLO_PROFILE_FUNCTION();

        SyncFromGPU();
        if (m_Heights.IsEmpty() || m_Resolution == 0)
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

        TArray<u16> quantized(m_Heights.Num());
        for (sizet i = 0; i < m_Heights.Num(); ++i)
        {
            f32 clamped = glm::clamp(m_Heights[i], 0.0f, 1.0f);
            quantized[i] = static_cast<u16>(clamped * 65535.0f + 0.5f);
        }

        file.write(reinterpret_cast<const char*>(quantized.GetData()), static_cast<std::streamsize>(quantized.Num() * sizeof(u16)));
        OLO_CORE_INFO("TerrainData: Exported R16 heightmap ({0}x{0}) to {1}", m_Resolution, path);
        return file.good();
    }
} // namespace OloEngine
