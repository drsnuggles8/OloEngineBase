#include "OloEnginePCH.h"
#include <limits>
#include "OloEngine/Terrain/TerrainMaterial.h"
#include "OloEngine/Project/ContentPath.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/RendererAPI.h"

#include <stb_image/stb_image.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <filesystem>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>

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

    namespace
    {
        // Decoded sRGB bytes, and the linear values at which the encoded byte
        // steps from i to i + 1 (the decode of i + 0.5). Encoding is then a
        // search over the second table, which rounds exactly as
        // round(encode(x) * 255) does without a pow per texel.
        struct SRGBTables
        {
            std::array<f32, 256> Decode{};
            std::array<f32, 255> Step{};

            SRGBTables()
            {
                const auto decode = [](const f64 encoded)
                {
                    return encoded <= 0.04045 ? encoded / 12.92 : std::pow((encoded + 0.055) / 1.055, 2.4);
                };
                for (u32 i = 0; i < 256; ++i)
                    Decode[i] = static_cast<f32>(decode(static_cast<f64>(i) / 255.0));
                for (u32 i = 0; i < 255; ++i)
                    Step[i] = static_cast<f32>(decode((static_cast<f64>(i) + 0.5) / 255.0));
            }
        };

        const SRGBTables& GetSRGBTables()
        {
            static const SRGBTables s_Tables;
            return s_Tables;
        }

        // Which source texels one destination texel reads along an axis, and
        // with what weights (they sum to 1).
        struct AxisFilter
        {
            TArray<u32> First; // per destination texel: offset of its first tap
            TArray<u32> Count; // per destination texel: number of taps
            TArray<u32> Index; // source texel of each tap
            TArray<f32> Weight;
        };

        AxisFilter BuildAxisFilter(const u32 srcSize, const u32 dstSize)
        {
            AxisFilter filter;
            filter.First.Reserve(dstSize);
            filter.Count.Reserve(dstSize);
            const f64 scale = static_cast<f64>(srcSize) / static_cast<f64>(dstSize);
            for (u32 d = 0; d < dstSize; ++d)
            {
                filter.First.Add(static_cast<u32>(filter.Index.Num()));
                if (dstSize < srcSize)
                {
                    // AREA: the destination texel covers [lo, hi) of the source
                    // axis, and each source texel contributes the length of it
                    // that lies inside.
                    const f64 lo = static_cast<f64>(d) * scale;
                    const f64 hi = static_cast<f64>(d + 1u) * scale;
                    const u32 first = static_cast<u32>(std::floor(lo));
                    const u32 last = std::min(static_cast<u32>(std::ceil(hi)), srcSize);
                    for (u32 src = first; src < last; ++src)
                    {
                        const f64 covered = std::min(hi, static_cast<f64>(src + 1u)) - std::max(lo, static_cast<f64>(src));
                        if (covered > 0.0)
                        {
                            filter.Index.Add(src);
                            filter.Weight.Add(static_cast<f32>(covered / scale));
                        }
                    }
                }
                else if (dstSize == srcSize)
                {
                    filter.Index.Add(d);
                    filter.Weight.Add(1.0f);
                }
                else
                {
                    // LINEAR between the two nearest texel centres, wrapping at
                    // the edges: a layer tiles, so the texel past the last one
                    // is the first.
                    const f64 centre = (static_cast<f64>(d) + 0.5) * scale - 0.5;
                    const f64 base = std::floor(centre);
                    const f32 t = static_cast<f32>(centre - base);
                    const i64 size = static_cast<i64>(srcSize);
                    const i64 first = ((static_cast<i64>(base) % size) + size) % size;
                    filter.Index.Add(static_cast<u32>(first));
                    filter.Weight.Add(1.0f - t);
                    filter.Index.Add(static_cast<u32>((first + 1) % size));
                    filter.Weight.Add(t);
                }
                filter.Count.Add(static_cast<u32>(filter.Index.Num()) - filter.First[d]);
            }
            return filter;
        }

        // Decode an image file to RGBA8, row 0 first (no vertical flip). The
        // stored path is resolved by the caller. False, with a warning naming
        // the file, when it does not decode.
        bool DecodeRGBA8File(const std::filesystem::path& file, std::string_view storedPath, TArray<u8>& outPixels,
                             u32& outWidth, u32& outHeight)
        {
            i32 w = 0;
            i32 h = 0;
            i32 channels = 0;
            stbi_set_flip_vertically_on_load_thread(0);
            const std::string fileString = file.string();
            u8* pixels = stbi_load(fileString.c_str(), &w, &h, &channels, 4); // Force RGBA
            if (!pixels || w <= 0 || h <= 0)
            {
                OLO_CORE_WARN("TerrainMaterial: Failed to load texture '{}' ({})", storedPath, fileString);
                if (pixels)
                    stbi_image_free(pixels);
                return false;
            }
            outWidth = static_cast<u32>(w);
            outHeight = static_cast<u32>(h);
            outPixels.Reset();
            outPixels.Append(pixels, static_cast<TArray<u8>::SizeType>(static_cast<sizet>(w) * static_cast<sizet>(h) * 4));
            stbi_image_free(pixels);
            return true;
        }

        // One layer's three files, resolved once: the result feeds both the
        // shared-array key and the load, and ResolveContentPath logs an
        // unresolvable path each time it is asked.
        struct LayerFiles
        {
            std::filesystem::path Albedo;
            std::filesystem::path Normal;
            std::filesystem::path ARM;
        };

        std::filesystem::path ResolveLayerFile(const FString& storedPath)
        {
            // Project-relative or working-directory-relative (#1496); an
            // unresolvable path is logged by the resolver and loads nothing.
            return storedPath.IsEmpty() ? std::filesystem::path{} : ResolveContentPath(storedPath.ToView());
        }

        // Decode `file` and resample it to targetSize x targetSize RGBA8.
        bool LoadLayerTexture(const std::filesystem::path& file, const FString& storedPath, const u32 targetSize,
                              const bool srgbColour, TArray<u8>& outData)
        {
            OLO_PROFILE_FUNCTION();

            if (file.empty())
            {
                return false;
            }

            TArray<u8> decoded;
            u32 width = 0;
            u32 height = 0;
            if (!DecodeRGBA8File(file, storedPath.ToView(), decoded, width, height))
            {
                return false;
            }

            if (width == targetSize && height == targetSize)
            {
                outData = std::move(decoded);
                return true;
            }
            TerrainMaterial::ResampleRGBA8(std::span<const u8>(decoded.GetData(), static_cast<sizet>(decoded.Num())),
                                           width, height, targetSize, targetSize, srgbColour, outData);
            return true;
        }

        void AppendFileIdentity(std::string& key, const std::filesystem::path& file, const FString& storedPath)
        {
            if (file.empty())
            {
                // No file, or one that did not resolve. The stored spelling
                // still separates "no texture" from "a texture that is
                // missing": both take the fallback, but a later rebuild with
                // the file in place must not find this entry.
                key += storedPath.IsEmpty() ? "-" : "?";
                key += '|';
                return;
            }
            std::error_code ec;
            const auto size = std::filesystem::file_size(file, ec);
            const auto written = ec ? std::filesystem::file_time_type{} : std::filesystem::last_write_time(file, ec);
            key += file.generic_string();
            key += '@';
            key += std::to_string(ec ? 0u : static_cast<u64>(size));
            key += '@';
            key += std::to_string(ec ? 0 : static_cast<i64>(written.time_since_epoch().count()));
            key += '|';
        }

        // What a set of layer arrays is built from, as one string.
        std::string BuildLayerArrayKey(std::span<const TerrainLayer> layers, std::span<const LayerFiles> files,
                                       const u32 layerResolution)
        {
            std::string key = std::to_string(layerResolution);
            key += '#';
            key += std::to_string(static_cast<int>(RendererAPI::GetAPI()));
            key += '#';
            const sizet count = layers.size();
            for (sizet i = 0; i < count; ++i)
            {
                const TerrainLayer& layer = layers[i];
                AppendFileIdentity(key, files[i].Albedo, layer.AlbedoPath);
                AppendFileIdentity(key, files[i].Normal, layer.NormalPath);
                AppendFileIdentity(key, files[i].ARM, layer.ARMPath);
                // The fallback values, bit for bit: they are what a layer with
                // no file is filled with.
                for (const f32 value : { layer.BaseColor.r, layer.BaseColor.g, layer.BaseColor.b, layer.Roughness, layer.Metallic })
                {
                    key += std::to_string(std::bit_cast<u32>(value));
                    key += ',';
                }
                key += ';';
            }
            return key;
        }

        Ref<TerrainLayerArrays> CreateLayerArrays(std::span<const TerrainLayer> layers, std::span<const LayerFiles> files,
                                                  const u32 layerResolution)
        {
            OLO_PROFILE_FUNCTION();

            const u32 layerCount = static_cast<u32>(layers.size());

            // RGBA8 texture arrays with mipmaps. They TILE: the shaders address
            // them with uv * tiling, far outside [0,1], so the wrap mode is
            // repeat (a clamped array draws one tile and smears its border
            // across the rest of the terrain), and they are seen at grazing
            // angles, so they are filtered anisotropically.
            Texture2DArraySpecification spec;
            spec.Width = layerResolution;
            spec.Height = layerResolution;
            spec.Layers = layerCount;
            spec.Format = Texture2DArrayFormat::RGBA8;
            spec.GenerateMipmaps = true;
            spec.RepeatWrap = true;
            spec.MaxAnisotropy = TerrainMaterial::kLayerAnisotropy;

            // Albedo is sRGB-authored colour: stored as it is and decoded to
            // linear by the sampler (and by the mip chain's filter). Normal and
            // ARM are data and stay linear.
            Texture2DArraySpecification albedoSpec = spec;
            albedoSpec.Format = Texture2DArrayFormat::RGBA8_SRGB;

            auto arrays = Ref<TerrainLayerArrays>::Create();
            arrays->Resolution = layerResolution;
            arrays->Albedo = Texture2DArray::Create(albedoSpec);
            arrays->Normal = Texture2DArray::Create(spec);
            arrays->ARM = Texture2DArray::Create(spec);

            if (!arrays->Albedo || !arrays->Normal || !arrays->ARM)
            {
                OLO_CORE_ERROR("TerrainMaterial::BuildTextureArrays - Failed to create one or more texture arrays");
                return nullptr;
            }

            // Default data for layers without textures
            const sizet layerBytes = static_cast<sizet>(layerResolution) * layerResolution * 4;
            TArray<u8> solidColor(layerBytes);
            TArray<u8> defaultNormal(layerBytes);
            TArray<u8> defaultARM(layerBytes);

            // Fill default normal map (flat: 128, 128, 255, 255 = tangent-space up)
            for (sizet i = 0; i < layerBytes; i += 4)
            {
                defaultNormal[i + 0] = 128; // X
                defaultNormal[i + 1] = 128; // Y
                defaultNormal[i + 2] = 255; // Z (up)
                defaultNormal[i + 3] = 255; // A
            }

            // Fill default ARM (AO=255, Roughness=204 (~0.8), Metallic=0)
            for (sizet i = 0; i < layerBytes; i += 4)
            {
                defaultARM[i + 0] = 255; // AO
                defaultARM[i + 1] = 204; // Roughness
                defaultARM[i + 2] = 0;   // Metallic
                defaultARM[i + 3] = 255; // A (height for blending)
            }

            TArray<u8> texData;

            for (u32 i = 0; i < layerCount; ++i)
            {
                const auto& layer = layers[i];

                // Albedo
                if (LoadLayerTexture(files[i].Albedo, layer.AlbedoPath, layerResolution, true, texData))
                {
                    arrays->Albedo->SetLayerData(i, texData.GetData(), layerResolution, layerResolution);
                }
                else
                {
                    // A solid colour from the layer's BaseColor. BaseColor is
                    // LINEAR and the array is sRGB, so it is encoded here: the
                    // sampler decodes the byte back to the colour that was
                    // authored.
                    const u8 r = TerrainMaterial::LinearToSRGB8(layer.BaseColor.r);
                    const u8 g = TerrainMaterial::LinearToSRGB8(layer.BaseColor.g);
                    const u8 b = TerrainMaterial::LinearToSRGB8(layer.BaseColor.b);
                    for (sizet j = 0; j < layerBytes; j += 4)
                    {
                        solidColor[j + 0] = r;
                        solidColor[j + 1] = g;
                        solidColor[j + 2] = b;
                        solidColor[j + 3] = 255;
                    }
                    arrays->Albedo->SetLayerData(i, solidColor.GetData(), layerResolution, layerResolution);
                }

                // Normal
                if (LoadLayerTexture(files[i].Normal, layer.NormalPath, layerResolution, false, texData))
                {
                    arrays->Normal->SetLayerData(i, texData.GetData(), layerResolution, layerResolution);
                }
                else
                {
                    arrays->Normal->SetLayerData(i, defaultNormal.GetData(), layerResolution, layerResolution);
                }

                // ARM
                if (LoadLayerTexture(files[i].ARM, layer.ARMPath, layerResolution, false, texData))
                {
                    arrays->ARM->SetLayerData(i, texData.GetData(), layerResolution, layerResolution);
                }
                else
                {
                    // Generate from layer defaults
                    auto armData = defaultARM;
                    const u8 roughByte = static_cast<u8>(std::clamp(layer.Roughness, 0.0f, 1.0f) * 255.0f);
                    const u8 metalByte = static_cast<u8>(std::clamp(layer.Metallic, 0.0f, 1.0f) * 255.0f);
                    for (sizet j = 0; j < layerBytes; j += 4)
                    {
                        armData[j + 1] = roughByte;
                        armData[j + 2] = metalByte;
                    }
                    arrays->ARM->SetLayerData(i, armData.GetData(), layerResolution, layerResolution);
                }
            }

            // Generate mipmaps
            arrays->Albedo->GenerateMipmaps();
            arrays->Normal->GenerateMipmaps();
            arrays->ARM->GenerateMipmaps();
            return arrays;
        }

        // The live layer-array sets, by what they were built from. Weak: a set
        // lives exactly as long as some material holds it.
        struct LayerArrayCache
        {
            std::mutex Mutex;
            std::unordered_map<std::string, WeakRef<TerrainLayerArrays>> Entries;
        };

        LayerArrayCache& GetLayerArrayCache()
        {
            static LayerArrayCache s_Cache;
            return s_Cache;
        }
    } // namespace

    u8 TerrainMaterial::LinearToSRGB8(const f32 linear)
    {
        if (!(linear > 0.0f)) // also catches NaN
        {
            return 0;
        }
        const auto& steps = GetSRGBTables().Step;
        // The number of byte steps at or below the value is the encoded byte.
        return static_cast<u8>(std::upper_bound(steps.begin(), steps.end(), linear) - steps.begin());
    }

    f32 TerrainMaterial::SRGB8ToLinear(const u8 encoded)
    {
        return GetSRGBTables().Decode[encoded];
    }

    u32 TerrainMaterial::SanitizeLayerResolution(const u32 requested)
    {
        return std::bit_floor(std::clamp(requested, kMinLayerResolution, kMaxLayerResolution));
    }

    void TerrainMaterial::ResampleRGBA8(const std::span<const u8> source, const u32 srcWidth, const u32 srcHeight,
                                        const u32 dstWidth, const u32 dstHeight, const bool srgbColour,
                                        TArray<u8>& outPixels)
    {
        OLO_PROFILE_FUNCTION();

        outPixels.Reset();
        if (srcWidth == 0 || srcHeight == 0 || dstWidth == 0 || dstHeight == 0 ||
            source.size() < static_cast<sizet>(srcWidth) * srcHeight * 4)
        {
            OLO_CORE_ERROR("TerrainMaterial::ResampleRGBA8 - invalid image ({}x{} -> {}x{}, {} bytes)", srcWidth,
                           srcHeight, dstWidth, dstHeight, source.size());
            return;
        }

        const AxisFilter columns = BuildAxisFilter(srcWidth, dstWidth);
        const AxisFilter rows = BuildAxisFilter(srcHeight, dstHeight);
        const auto& decode = GetSRGBTables().Decode;

        outPixels.SetNum(static_cast<TArray<u8>::SizeType>(static_cast<sizet>(dstWidth) * dstHeight * 4),
                         EAllowShrinking::No);
        for (u32 y = 0; y < dstHeight; ++y)
        {
            const u32 rowFirst = rows.First[y];
            const u32 rowCount = rows.Count[y];
            for (u32 x = 0; x < dstWidth; ++x)
            {
                const u32 columnFirst = columns.First[x];
                const u32 columnCount = columns.Count[x];
                f32 sum[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
                for (u32 ry = 0; ry < rowCount; ++ry)
                {
                    const sizet rowOffset = static_cast<sizet>(rows.Index[rowFirst + ry]) * srcWidth * 4;
                    const f32 rowWeight = rows.Weight[rowFirst + ry];
                    for (u32 cx = 0; cx < columnCount; ++cx)
                    {
                        const u8* texel = source.data() + rowOffset + static_cast<sizet>(columns.Index[columnFirst + cx]) * 4;
                        const f32 weight = rowWeight * columns.Weight[columnFirst + cx];
                        if (srgbColour)
                        {
                            sum[0] += decode[texel[0]] * weight;
                            sum[1] += decode[texel[1]] * weight;
                            sum[2] += decode[texel[2]] * weight;
                        }
                        else
                        {
                            sum[0] += static_cast<f32>(texel[0]) * weight;
                            sum[1] += static_cast<f32>(texel[1]) * weight;
                            sum[2] += static_cast<f32>(texel[2]) * weight;
                        }
                        sum[3] += static_cast<f32>(texel[3]) * weight;
                    }
                }

                u8* out = outPixels.GetData() + (static_cast<sizet>(y) * dstWidth + x) * 4;
                const auto toByte = [](const f32 value)
                {
                    return static_cast<u8>(std::clamp(value + 0.5f, 0.0f, 255.0f));
                };
                if (srgbColour)
                {
                    out[0] = LinearToSRGB8(sum[0]);
                    out[1] = LinearToSRGB8(sum[1]);
                    out[2] = LinearToSRGB8(sum[2]);
                }
                else
                {
                    out[0] = toByte(sum[0]);
                    out[1] = toByte(sum[1]);
                    out[2] = toByte(sum[2]);
                }
                out[3] = toByte(sum[3]);
            }
        }
    }

    void TerrainMaterial::BuildTextureArrays(const u32 layerResolution)
    {
        if (layerResolution == 0)
        {
            OLO_CORE_ERROR("TerrainMaterial: Invalid layer resolution 0");
            return;
        }
        SetLayerResolution(layerResolution);
        BuildTextureArrays();
    }

    void TerrainMaterial::BuildTextureArrays()
    {
        OLO_PROFILE_FUNCTION();

        if (m_LayerCount == 0)
        {
            OLO_CORE_WARN("TerrainMaterial: No layers to build");
            return;
        }

        m_LayerResolution = SanitizeLayerResolution(m_LayerResolution);

        const std::span<const TerrainLayer> layers(m_Layers.data(), m_LayerCount);
        std::array<LayerFiles, MAX_TERRAIN_LAYERS> files;
        for (u32 i = 0; i < m_LayerCount; ++i)
        {
            files[i].Albedo = ResolveLayerFile(m_Layers[i].AlbedoPath);
            files[i].Normal = ResolveLayerFile(m_Layers[i].NormalPath);
            files[i].ARM = ResolveLayerFile(m_Layers[i].ARMPath);
        }
        const std::span<const LayerFiles> layerFiles(files.data(), m_LayerCount);
        const std::string key = BuildLayerArrayKey(layers, layerFiles, m_LayerResolution);

        // Share the arrays of a material already built from the same layers
        // (see TerrainLayerArrays). Looked up and inserted under one lock, so
        // two materials building at once cannot each build a set.
        auto& cache = GetLayerArrayCache();
        std::scoped_lock lock(cache.Mutex);
        if (const auto it = cache.Entries.find(key); it != cache.Entries.end())
        {
            if (Ref<TerrainLayerArrays> shared = it->second.Lock())
            {
                m_LayerArrays = std::move(shared);
                OLO_CORE_INFO("TerrainMaterial: Sharing texture arrays ({} layers, {}×{} per layer)", m_LayerCount,
                              m_LayerResolution, m_LayerResolution);
                return;
            }
        }

        Ref<TerrainLayerArrays> built = CreateLayerArrays(layers, layerFiles, m_LayerResolution);
        if (!built)
        {
            return;
        }
        m_LayerArrays = std::move(built);

        // Drop the entries whose arrays have died, then record this set.
        std::erase_if(cache.Entries, [](const auto& entry)
                      { return !entry.second.IsValid(); });
        cache.Entries[key] = WeakRef<TerrainLayerArrays>(m_LayerArrays);

        OLO_CORE_INFO("TerrainMaterial: Built texture arrays ({} layers, {}×{} per layer)",
                      m_LayerCount, m_LayerResolution, m_LayerResolution);
    }

    void TerrainMaterial::SetSplatmapPath(u32 index, const FString& path)
    {
        if (index < 2)
        {
            m_SplatmapPaths[index] = path;
        }
    }

    const FString& TerrainMaterial::GetSplatmapPath(u32 index) const
    {
        static const FString empty;
        return (index < 2) ? m_SplatmapPaths[index] : empty;
    }

    void TerrainMaterial::LoadSplatmaps()
    {
        OLO_PROFILE_FUNCTION();

        if (m_SplatmapPaths[0].IsEmpty() && m_SplatmapPaths[1].IsEmpty())
        {
            return;
        }

        // Decode both planes on the CPU first. Nothing of the material changes
        // until every check below has passed, so a refused load leaves the
        // weights it had.
        std::array<TArray<u8>, 2> decoded;
        std::array<u32, 2> width{ 0, 0 };
        std::array<u32, 2> height{ 0, 0 };
        std::array<bool, 2> present{ false, false };
        for (u32 i = 0; i < 2; ++i)
        {
            if (m_SplatmapPaths[i].IsEmpty())
            {
                continue;
            }
            // A stored content path, like the layer textures (#1496).
            const std::filesystem::path file = ResolveContentPath(m_SplatmapPaths[i].ToView());
            if (file.empty() || !DecodeRGBA8File(file, m_SplatmapPaths[i].ToView(), decoded[i], width[i], height[i]))
            {
                OLO_CORE_ERROR("TerrainMaterial: splatmap {} ('{}') did not load; the material keeps the weights it had",
                               i, m_SplatmapPaths[i].ToView());
                return;
            }
            if (width[i] != height[i])
            {
                OLO_CORE_ERROR("TerrainMaterial: splatmap {} ('{}') is {}x{}; a splatmap must be square. "
                               "The material keeps the weights it had.",
                               i, m_SplatmapPaths[i].ToView(), width[i], height[i]);
                return;
            }
            present[i] = true;
        }

        // The two planes are one weight field on one grid. Choosing either
        // size would resample the other behind the author's back.
        if (present[0] && present[1] && width[0] != width[1])
        {
            OLO_CORE_ERROR("TerrainMaterial: splatmap 0 ('{}') is {}x{} and splatmap 1 ('{}') is {}x{}; both planes "
                           "must have the same size. The material keeps the weights it had.",
                           m_SplatmapPaths[0].ToView(), width[0], height[0], m_SplatmapPaths[1].ToView(), width[1],
                           height[1]);
            return;
        }
        const u32 resolution = present[0] ? width[0] : width[1];

        // A paint/foliage mirror has a fixed common grid. Refuse a
        // differently sized reload coherently, rather than displaying
        // new GPU weights while CPU consumers silently keep old ones.
        // Reinitializing the material establishes a different grid.
        if (HasCPUSplatmaps() && resolution != m_SplatmapResolution)
        {
            OLO_CORE_WARN("TerrainMaterial: splatmap reload refused ({}x{}); the active CPU/GPU grid is {}x{}. "
                          "Retaining its prior weights; reinitialize the material to change grid size.",
                          resolution, resolution, m_SplatmapResolution, m_SplatmapResolution);
            return;
        }

        // The CPU mirror uses a signed 32-bit element count.
        const u64 totalBytes = static_cast<u64>(resolution) * resolution * 4;
        if (totalBytes > static_cast<u64>(std::numeric_limits<i32>::max()))
        {
            OLO_CORE_ERROR("TerrainMaterial: splatmap resolution {} exceeds CPU storage capacity", resolution);
            return;
        }

        for (u32 i = 0; i < 2; ++i)
        {
            if (present[i])
            {
                m_CPUSplatmaps[i] = std::move(decoded[i]);
            }
            else
            {
                // A plane with no file carries no weight for its four layers.
                m_CPUSplatmaps[i].Reset();
                m_CPUSplatmaps[i].SetNumZeroed(static_cast<TArray<u8>::SizeType>(totalBytes));
            }
        }
        m_SplatmapResolution = resolution;
        ++m_SplatmapRevision;
        // The GPU textures are uploaded from the mirror just below, so the two
        // agree and nothing needs reading back.
        m_CPUSplatmapsStale = false;
        CreateSplatmapTexturesFromCPU();
    }

    void TerrainMaterial::CreateSplatmapTexturesFromCPU()
    {
        for (u32 i = 0; i < 2; ++i)
        {
            // Weights, so a LINEAR format: an sRGB one would decode them on
            // sampling and every blend would be wrong on screen.
            TextureSpecification spec;
            spec.Width = m_SplatmapResolution;
            spec.Height = m_SplatmapResolution;
            spec.Format = ImageFormat::RGBA8;
            spec.GenerateMips = false;
            spec.SRGB = false;

            m_Splatmaps[i] = Texture2D::Create(spec);
            if (!m_Splatmaps[i])
            {
                OLO_CORE_ERROR("TerrainMaterial - Failed to create splatmap texture {}", i);
                continue;
            }
            m_Splatmaps[i]->SetData(m_CPUSplatmaps[i].GetData(),
                                    static_cast<u32>(m_CPUSplatmaps[i].Num()));
            // A splatmap covers the terrain once. With the default repeat wrap
            // the filter at u or v = 0 took half its weight from the texels of
            // the OPPOSITE edge.
            RenderCommand::SetTextureWrap(m_Splatmaps[i]->GetRHIHandle(), RHI::AddressMode::ClampToEdge);
        }
    }

    void TerrainMaterial::LoadMacroMap()
    {
        OLO_PROFILE_FUNCTION();

        m_MacroMap = nullptr;
        if (m_MacroMapPath.IsEmpty())
        {
            return;
        }

        const std::filesystem::path file = ResolveContentPath(m_MacroMapPath.ToView());
        TArray<u8> pixels;
        u32 width = 0;
        u32 height = 0;
        if (file.empty() || !DecodeRGBA8File(file, m_MacroMapPath.ToView(), pixels, width, height))
        {
            OLO_CORE_ERROR("TerrainMaterial: macro map '{}' did not load; the terrain is drawn without one",
                           m_MacroMapPath.ToView());
            return;
        }

        // Data, not colour to be decoded: the tint is a multiplier with 128 as
        // one, and alpha is dryness. Linear RGBA8, mipped so a distant terrain
        // does not alias it.
        TextureSpecification spec;
        spec.Width = width;
        spec.Height = height;
        spec.Format = ImageFormat::RGBA8;
        spec.GenerateMips = true;
        spec.SRGB = false;

        Ref<Texture2D> texture = Texture2D::Create(spec);
        if (!texture)
        {
            OLO_CORE_ERROR("TerrainMaterial: failed to create the macro map texture for '{}'", m_MacroMapPath.ToView());
            return;
        }
        texture->SetData(pixels.GetData(), static_cast<u32>(pixels.Num()));
        // It covers the terrain once, so its edge texels must not blend with
        // the opposite edge.
        RenderCommand::SetTextureWrap(texture->GetRHIHandle(), RHI::AddressMode::ClampToEdge);
        m_MacroMap = std::move(texture);
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

        // The CPU mirror uses a signed 32-bit element count. Reject an oversized
        // image before narrowing its byte count or changing the active resolution.
        const u64 pixelCount = static_cast<u64>(resolution) * resolution;
        if (pixelCount > static_cast<u64>(std::numeric_limits<i32>::max()) / 4)
        {
            OLO_CORE_ERROR("TerrainMaterial::InitializeCPUSplatmaps - Resolution {} exceeds CPU storage capacity", resolution);
            return;
        }

        const u64 totalPixels = pixelCount * 4; // RGBA8
        m_SplatmapResolution = resolution;
        ++m_SplatmapRevision;
        // Initialisation loads the mirror and the GPU textures together, so any
        // pending GPU-newer flag from a previous material configuration is void.
        m_CPUSplatmapsStale = false;

        for (u32 i = 0; i < 2; ++i)
        {
            m_CPUSplatmaps[i].SetNum(static_cast<i32>(totalPixels), EAllowShrinking::No);
        }

        // If splatmaps were loaded from file, read them back into CPU buffers
        for (u32 i = 0; i < 2; ++i)
        {
            if (m_Splatmaps[i])
            {
                TArray64<u8> readback;
                if (m_Splatmaps[i]->GetData(readback))
                {
                    // Resize if needed (readback may differ from target resolution)
                    u32 texW = m_Splatmaps[i]->GetWidth();
                    u32 texH = m_Splatmaps[i]->GetHeight();
                    if (texW == resolution && texH == resolution && readback.Num() == totalPixels)
                    {
                        m_CPUSplatmaps[i].Reset();
                        m_CPUSplatmaps[i].Append(readback.GetData(), static_cast<i32>(readback.Num()));
                    }
                    else
                    {
                        OLO_CORE_WARN("TerrainMaterial: Splatmap {} size mismatch ({}x{} vs {}), using blank",
                                      i, texW, texH, resolution);
                        // Blank, as the message says. The mirror can hold the
                        // weights of a file loaded at the OTHER size (issue
                        // #1721: LoadSplatmaps fills it), and resizing an array
                        // keeps its bytes — in the old size's row layout.
                        m_CPUSplatmaps[i].Reset();
                        m_CPUSplatmaps[i].SetNumZeroed(static_cast<i32>(totalPixels));
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
        CreateSplatmapTexturesFromCPU();
    }

    TArray<u8>& TerrainMaterial::GetSplatmapData(u32 index)
    {
        OLO_CORE_ASSERT(index < 2, "Splatmap index out of bounds");
        SyncSplatmapsFromGPU();
        return m_CPUSplatmaps[index];
    }

    const TArray<u8>& TerrainMaterial::GetSplatmapData(u32 index) const
    {
        OLO_CORE_ASSERT(index < 2, "Splatmap index out of bounds");
        SyncSplatmapsFromGPU();
        return m_CPUSplatmaps[index];
    }

    void TerrainMaterial::SyncSplatmapsFromGPU() const
    {
        OLO_PROFILE_FUNCTION();

        if (!m_CPUSplatmapsStale)
        {
            return;
        }

        // Cleared first and unconditionally: every failure below is permanent for
        // this configuration, and leaving the flag set would retry the same doomed
        // readback on every subsequent splatmap read.
        m_CPUSplatmapsStale = false;

        if (m_SplatmapResolution == 0)
        {
            return;
        }

        const sizet expectedBytes = static_cast<sizet>(m_SplatmapResolution) * m_SplatmapResolution * 4;
        for (u32 i = 0; i < 2; ++i)
        {
            if (!m_Splatmaps[i])
            {
                continue;
            }

            TArray64<u8> readback;
            if (!m_Splatmaps[i]->GetData(readback))
            {
                OLO_CORE_ERROR("TerrainMaterial::SyncSplatmapsFromGPU - Failed to read back splatmap {}", i);
                continue;
            }
            if (readback.Num() != expectedBytes)
            {
                OLO_CORE_ERROR("TerrainMaterial::SyncSplatmapsFromGPU - Splatmap {} readback size mismatch: "
                               "got {} bytes, expected {}",
                               i, readback.Num(), expectedBytes);
                continue;
            }
            m_CPUSplatmaps[i].Reset();
            m_CPUSplatmaps[i].Append(readback.GetData(), static_cast<i32>(readback.Num()));
        }
    }

    void TerrainMaterial::UploadSplatmapRegion(u32 splatmapIndex, u32 x, u32 y, u32 w, u32 h)
    {
        OLO_PROFILE_FUNCTION();

        if (splatmapIndex >= 2 || !m_Splatmaps[splatmapIndex] || m_SplatmapResolution == 0)
            return;

        // The caller reached the mirror through GetSplatmapData(), which synced it
        // first, so pushing it back puts the two in agreement again.
        m_CPUSplatmapsStale = false;

        u32 res = m_SplatmapResolution;
        x = std::min(x, res - 1);
        y = std::min(y, res - 1);
        u32 endX = x + std::min(w, res - x);
        u32 endY = y + std::min(h, res - y);
        w = endX - x;
        h = endY - y;

        if (w == 0 || h == 0)
            return;

        ++m_SplatmapRevision;

        // Extract contiguous sub-region
        TArray<u8> regionData(static_cast<sizet>(w) * h * 4);
        const auto& cpuData = m_CPUSplatmaps[splatmapIndex];
        for (u32 row = 0; row < h; ++row)
        {
            sizet srcOffset = (static_cast<sizet>(y + row) * res + x) * 4;
            sizet dstOffset = static_cast<sizet>(row) * w * 4;
            std::memcpy(&regionData[dstOffset], &cpuData[srcOffset], w * 4);
        }

        u32 dataSize = w * h * 4;
        m_Splatmaps[splatmapIndex]->SubImage(x, y, w, h, regionData.GetData(), dataSize);
    }

} // namespace OloEngine
