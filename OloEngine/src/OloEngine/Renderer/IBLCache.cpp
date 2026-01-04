#include "OloEnginePCH.h"
#include "IBLCache.h"

#include "OloEngine/Core/Log.h"
#include "Platform/OpenGL/OpenGLTextureCubemap.h"

#include <fstream>

namespace OloEngine
{
    // Static member definitions
    std::filesystem::path IBLCache::s_CacheDirectory;
    bool IBLCache::s_Initialized = false;
    IBLCache::Statistics IBLCache::s_Stats;

    // Cache file header for version tracking
    struct IBLCacheHeader
    {
        char Magic[4] = {'I', 'B', 'L', 'C'};
        u32 Version = 1;
        u32 Width = 0;
        u32 Height = 0;
        u32 Format = 0;       // ImageFormat enum value
        u32 MipLevels = 0;
        u32 FaceCount = 0;    // 1 for Texture2D, 6 for cubemap
        u64 DataSize = 0;     // Total data size following header
    };

    void IBLCache::Initialize(const std::filesystem::path& cacheDirectory)
    {
        s_CacheDirectory = cacheDirectory;
        s_Initialized = true;
        s_Stats = {};

        // Create cache directory if it doesn't exist
        if (!std::filesystem::exists(s_CacheDirectory))
        {
            std::filesystem::create_directories(s_CacheDirectory);
            OLO_CORE_INFO("IBLCache: Created cache directory: {}", s_CacheDirectory.string());
        }

        // Calculate existing cache size
        for (const auto& entry : std::filesystem::directory_iterator(s_CacheDirectory))
        {
            if (entry.is_regular_file())
            {
                s_Stats.CacheSizeBytes += entry.file_size();
            }
        }

        OLO_CORE_INFO("IBLCache: Initialized with {} bytes cached", s_Stats.CacheSizeBytes);
    }

    void IBLCache::Shutdown()
    {
        s_Initialized = false;
        OLO_CORE_INFO("IBLCache: Shutdown (Hits: {}, Misses: {}, Saves: {})",
                      s_Stats.CacheHits, s_Stats.CacheMisses, s_Stats.SaveCount);
    }

    u64 IBLCache::ComputeHash(const std::string& sourcePath, const IBLConfiguration& config)
    {
        // Simple FNV-1a hash combining all relevant parameters
        u64 hash = 14695981039346656037ULL; // FNV offset basis

        auto hashCombine = [&hash](u64 value)
        {
            hash ^= value;
            hash *= 1099511628211ULL; // FNV prime
        };

        // Hash source path
        for (char c : sourcePath)
        {
            hashCombine(static_cast<u64>(c));
        }

        // Hash all config parameters
        hashCombine(static_cast<u64>(config.Quality));
        hashCombine(config.UseImportanceSampling ? 1ULL : 0ULL);
        hashCombine(config.UseSphericalHarmonics ? 1ULL : 0ULL);
        hashCombine(static_cast<u64>(config.IrradianceResolution));
        hashCombine(static_cast<u64>(config.PrefilterResolution));
        hashCombine(static_cast<u64>(config.BRDFLutResolution));
        hashCombine(static_cast<u64>(config.IrradianceSamples));
        hashCombine(static_cast<u64>(config.PrefilterSamples));

        return hash;
    }

    IBLCache::CachePaths IBLCache::GetCachePaths(u64 hash)
    {
        CachePaths paths;

        std::string hashStr = std::format("{:016x}", hash);

        paths.Irradiance = s_CacheDirectory / (hashStr + "_irradiance.iblcache");
        paths.Prefilter = s_CacheDirectory / (hashStr + "_prefilter.iblcache");
        paths.BRDFLut = s_CacheDirectory / "brdf_lut.iblcache";

        return paths;
    }

    bool IBLCache::TryLoad(const std::string& sourcePath,
                           const IBLConfiguration& config,
                           CachedIBL& outCached)
    {
        if (!s_Initialized)
        {
            OLO_CORE_WARN("IBLCache: Not initialized");
            s_Stats.CacheMisses++;
            return false;
        }

        u64 hash = ComputeHash(sourcePath, config);
        CachePaths paths = GetCachePaths(hash);

        // Check if all cache files exist
        if (!std::filesystem::exists(paths.Irradiance) ||
            !std::filesystem::exists(paths.Prefilter) ||
            !std::filesystem::exists(paths.BRDFLut))
        {
            OLO_CORE_TRACE("IBLCache: Cache miss for {} (files not found)", sourcePath);
            s_Stats.CacheMisses++;
            return false;
        }

        // Try to load each texture
        outCached.Irradiance = LoadCubemapFromCache(paths.Irradiance);
        if (!outCached.Irradiance)
        {
            OLO_CORE_WARN("IBLCache: Failed to load irradiance from cache");
            s_Stats.CacheMisses++;
            return false;
        }

        outCached.Prefilter = LoadCubemapFromCache(paths.Prefilter);
        if (!outCached.Prefilter)
        {
            OLO_CORE_WARN("IBLCache: Failed to load prefilter from cache");
            s_Stats.CacheMisses++;
            return false;
        }

        outCached.BRDFLut = LoadTexture2DFromCache(paths.BRDFLut);
        if (!outCached.BRDFLut)
        {
            OLO_CORE_WARN("IBLCache: Failed to load BRDF LUT from cache");
            s_Stats.CacheMisses++;
            return false;
        }

        OLO_CORE_INFO("IBLCache: Loaded IBL textures from cache for {}", sourcePath);
        s_Stats.CacheHits++;
        return true;
    }

    bool IBLCache::Save(const std::string& sourcePath,
                        const IBLConfiguration& config,
                        const Ref<TextureCubemap>& irradiance,
                        const Ref<TextureCubemap>& prefilter,
                        const Ref<Texture2D>& brdfLut)
    {
        if (!s_Initialized)
        {
            OLO_CORE_WARN("IBLCache: Not initialized, cannot save");
            return false;
        }

        if (!irradiance || !prefilter || !brdfLut)
        {
            OLO_CORE_ERROR("IBLCache: Cannot save null textures");
            return false;
        }

        u64 hash = ComputeHash(sourcePath, config);
        CachePaths paths = GetCachePaths(hash);

        bool success = true;

        if (!SaveCubemapToCache(irradiance, paths.Irradiance))
        {
            OLO_CORE_WARN("IBLCache: Failed to save irradiance map");
            success = false;
        }

        if (!SaveCubemapToCache(prefilter, paths.Prefilter))
        {
            OLO_CORE_WARN("IBLCache: Failed to save prefilter map");
            success = false;
        }

        // Only save BRDF LUT if it doesn't already exist (it's shared)
        if (!std::filesystem::exists(paths.BRDFLut))
        {
            if (!SaveTexture2DToCache(brdfLut, paths.BRDFLut))
            {
                OLO_CORE_WARN("IBLCache: Failed to save BRDF LUT");
                success = false;
            }
        }

        if (success)
        {
            s_Stats.SaveCount++;
            OLO_CORE_INFO("IBLCache: Saved IBL textures to cache for {}", sourcePath);
        }

        return success;
    }

    bool IBLCache::HasCache(const std::string& sourcePath, const IBLConfiguration& config)
    {
        if (!s_Initialized)
            return false;

        u64 hash = ComputeHash(sourcePath, config);
        CachePaths paths = GetCachePaths(hash);

        return std::filesystem::exists(paths.Irradiance) &&
               std::filesystem::exists(paths.Prefilter) &&
               std::filesystem::exists(paths.BRDFLut);
    }

    void IBLCache::Invalidate(const std::string& sourcePath)
    {
        if (!s_Initialized)
            return;

        // We need to search for files matching this source's hash pattern
        // Since we don't know the config, we can't compute the exact hash
        // For now, just log that we would invalidate
        OLO_CORE_INFO("IBLCache: Invalidate called for {} (manual deletion may be required)", sourcePath);
    }

    void IBLCache::ClearAll()
    {
        if (!s_Initialized || !std::filesystem::exists(s_CacheDirectory))
            return;

        u64 removedBytes = 0;
        u32 removedFiles = 0;

        for (const auto& entry : std::filesystem::directory_iterator(s_CacheDirectory))
        {
            if (entry.is_regular_file())
            {
                removedBytes += entry.file_size();
                std::filesystem::remove(entry.path());
                removedFiles++;
            }
        }

        s_Stats.CacheSizeBytes = 0;

        OLO_CORE_INFO("IBLCache: Cleared {} files ({} bytes)", removedFiles, removedBytes);
    }

    IBLCache::Statistics IBLCache::GetStatistics()
    {
        return s_Stats;
    }

    std::filesystem::path IBLCache::GetCacheDirectory()
    {
        return s_CacheDirectory;
    }

    Ref<TextureCubemap> IBLCache::LoadCubemapFromCache(const std::filesystem::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open())
        {
            OLO_CORE_ERROR("IBLCache: Cannot open cache file: {}", path.string());
            return nullptr;
        }

        // Read header
        IBLCacheHeader header;
        file.read(reinterpret_cast<char*>(&header), sizeof(header));

        // Validate magic
        if (header.Magic[0] != 'I' || header.Magic[1] != 'B' ||
            header.Magic[2] != 'L' || header.Magic[3] != 'C')
        {
            OLO_CORE_ERROR("IBLCache: Invalid cache file magic: {}", path.string());
            return nullptr;
        }

        if (header.Version != 1)
        {
            OLO_CORE_ERROR("IBLCache: Unsupported cache version: {}", header.Version);
            return nullptr;
        }

        if (header.FaceCount != 6)
        {
            OLO_CORE_ERROR("IBLCache: Expected cubemap with 6 faces, got {}", header.FaceCount);
            return nullptr;
        }

        // Create cubemap specification
        CubemapSpecification spec;
        spec.Width = header.Width;
        spec.Height = header.Height;
        spec.Format = static_cast<ImageFormat>(header.Format);
        spec.GenerateMips = header.MipLevels > 1;

        Ref<TextureCubemap> cubemap = TextureCubemap::Create(spec);

        // Determine bytes per pixel
        u32 bytesPerPixel = 4;
        switch (spec.Format)
        {
            case ImageFormat::R8:
                bytesPerPixel = 1;
                break;
            case ImageFormat::RGB8:
                bytesPerPixel = 3;
                break;
            case ImageFormat::RGBA8:
                bytesPerPixel = 4;
                break;
            case ImageFormat::R32F:
                bytesPerPixel = 4;
                break;
            case ImageFormat::RG32F:
                bytesPerPixel = 8;
                break;
            case ImageFormat::RGB32F:
                bytesPerPixel = 12;
                break;
            case ImageFormat::RGBA32F:
                bytesPerPixel = 16;
                break;
            default:
                break;
        }

        // Read each mip level's face data
        for (u32 mip = 0; mip < header.MipLevels; mip++)
        {
            u32 mipWidth = std::max(1u, header.Width >> mip);
            u32 mipHeight = std::max(1u, header.Height >> mip);
            sizet faceSize = static_cast<sizet>(mipWidth) * mipHeight * bytesPerPixel;

            for (u32 face = 0; face < 6; face++)
            {
                std::vector<u8> faceData(faceSize);
                file.read(reinterpret_cast<char*>(faceData.data()), static_cast<std::streamsize>(faceSize));

                if (mip == 0)
                {
                    // SetFaceData only works for mip 0, for now just load base level
                    cubemap->SetFaceData(face, faceData.data(), static_cast<u32>(faceSize));
                }
                // TODO: Support loading mip levels directly if needed
            }
        }

        return cubemap;
    }

    bool IBLCache::SaveCubemapToCache(const Ref<TextureCubemap>& cubemap,
                                      const std::filesystem::path& path)
    {
        if (!cubemap)
            return false;

        const auto& spec = cubemap->GetCubemapSpecification();
        u32 mipLevels = cubemap->GetMipLevelCount();

        // Determine bytes per pixel
        u32 bytesPerPixel = 4;
        switch (spec.Format)
        {
            case ImageFormat::R8:
                bytesPerPixel = 1;
                break;
            case ImageFormat::RGB8:
                bytesPerPixel = 3;
                break;
            case ImageFormat::RGBA8:
                bytesPerPixel = 4;
                break;
            case ImageFormat::R32F:
                bytesPerPixel = 4;
                break;
            case ImageFormat::RG32F:
                bytesPerPixel = 8;
                break;
            case ImageFormat::RGB32F:
                bytesPerPixel = 12;
                break;
            case ImageFormat::RGBA32F:
                bytesPerPixel = 16;
                break;
            default:
                break;
        }

        // Calculate total data size
        u64 totalDataSize = 0;
        for (u32 mip = 0; mip < mipLevels; mip++)
        {
            u32 mipWidth = std::max(1u, spec.Width >> mip);
            u32 mipHeight = std::max(1u, spec.Height >> mip);
            totalDataSize += static_cast<u64>(mipWidth) * mipHeight * bytesPerPixel * 6;
        }

        // Create header
        IBLCacheHeader header;
        header.Width = spec.Width;
        header.Height = spec.Height;
        header.Format = static_cast<u32>(spec.Format);
        header.MipLevels = mipLevels;
        header.FaceCount = 6;
        header.DataSize = totalDataSize;

        std::ofstream file(path, std::ios::binary);
        if (!file.is_open())
        {
            OLO_CORE_ERROR("IBLCache: Cannot create cache file: {}", path.string());
            return false;
        }

        // Write header
        file.write(reinterpret_cast<const char*>(&header), sizeof(header));

        // Write each mip level's face data
        for (u32 mip = 0; mip < mipLevels; mip++)
        {
            for (u32 face = 0; face < 6; face++)
            {
                std::vector<u8> faceData;
                if (!cubemap->GetFaceData(face, faceData, mip))
                {
                    OLO_CORE_ERROR("IBLCache: Failed to read face {} mip {} for caching", face, mip);
                    return false;
                }
                file.write(reinterpret_cast<const char*>(faceData.data()),
                           static_cast<std::streamsize>(faceData.size()));
            }
        }

        file.close();

        // Update cache size stats
        s_Stats.CacheSizeBytes += sizeof(header) + totalDataSize;

        OLO_CORE_TRACE("IBLCache: Saved cubemap to {} ({} bytes)", path.string(), sizeof(header) + totalDataSize);
        return true;
    }

    Ref<Texture2D> IBLCache::LoadTexture2DFromCache(const std::filesystem::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open())
        {
            OLO_CORE_ERROR("IBLCache: Cannot open cache file: {}", path.string());
            return nullptr;
        }

        // Read header
        IBLCacheHeader header;
        file.read(reinterpret_cast<char*>(&header), sizeof(header));

        // Validate magic
        if (header.Magic[0] != 'I' || header.Magic[1] != 'B' ||
            header.Magic[2] != 'L' || header.Magic[3] != 'C')
        {
            OLO_CORE_ERROR("IBLCache: Invalid cache file magic: {}", path.string());
            return nullptr;
        }

        if (header.FaceCount != 1)
        {
            OLO_CORE_ERROR("IBLCache: Expected 2D texture with 1 face, got {}", header.FaceCount);
            return nullptr;
        }

        // Create texture specification
        TextureSpecification spec;
        spec.Width = header.Width;
        spec.Height = header.Height;
        spec.Format = static_cast<ImageFormat>(header.Format);
        spec.GenerateMips = false;

        Ref<Texture2D> texture = Texture2D::Create(spec);

        // Read texture data
        std::vector<u8> data(header.DataSize);
        file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(header.DataSize));

        texture->SetData(data.data(), static_cast<u32>(header.DataSize));

        return texture;
    }

    bool IBLCache::SaveTexture2DToCache(const Ref<Texture2D>& texture,
                                        const std::filesystem::path& path)
    {
        if (!texture)
            return false;

        const auto& spec = texture->GetSpecification();

        // Determine bytes per pixel
        u32 bytesPerPixel = 4;
        switch (spec.Format)
        {
            case ImageFormat::R8:
                bytesPerPixel = 1;
                break;
            case ImageFormat::RGB8:
                bytesPerPixel = 3;
                break;
            case ImageFormat::RGBA8:
                bytesPerPixel = 4;
                break;
            case ImageFormat::R32F:
                bytesPerPixel = 4;
                break;
            case ImageFormat::RG32F:
                bytesPerPixel = 8;
                break;
            case ImageFormat::RGB32F:
                bytesPerPixel = 12;
                break;
            case ImageFormat::RGBA32F:
                bytesPerPixel = 16;
                break;
            default:
                break;
        }

        u64 dataSize = static_cast<u64>(spec.Width) * spec.Height * bytesPerPixel;

        // Create header
        IBLCacheHeader header;
        header.Width = spec.Width;
        header.Height = spec.Height;
        header.Format = static_cast<u32>(spec.Format);
        header.MipLevels = 1;
        header.FaceCount = 1;
        header.DataSize = dataSize;

        std::ofstream file(path, std::ios::binary);
        if (!file.is_open())
        {
            OLO_CORE_ERROR("IBLCache: Cannot create cache file: {}", path.string());
            return false;
        }

        // Write header
        file.write(reinterpret_cast<const char*>(&header), sizeof(header));

        // Read back texture data
        std::vector<u8> data;
        if (!texture->GetData(data, 0))
        {
            OLO_CORE_ERROR("IBLCache: Failed to read texture data for caching");
            return false;
        }

        file.write(reinterpret_cast<const char*>(data.data()),
                   static_cast<std::streamsize>(data.size()));

        file.close();

        // Update cache size stats
        s_Stats.CacheSizeBytes += sizeof(header) + dataSize;

        OLO_CORE_TRACE("IBLCache: Saved texture to {} ({} bytes)", path.string(), sizeof(header) + dataSize);
        return true;
    }

} // namespace OloEngine
