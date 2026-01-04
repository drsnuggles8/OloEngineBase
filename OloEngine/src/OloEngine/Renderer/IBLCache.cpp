#include "OloEnginePCH.h"
#include "IBLCache.h"

#include "OloEngine/Core/Log.h"
#include "OloEngine/Debug/Profiler.h"
#include "Platform/OpenGL/OpenGLTextureCubemap.h"

#include <fstream>

namespace OloEngine
{
    // Static member definitions
    std::filesystem::path IBLCache::s_CacheDirectory;
    bool IBLCache::s_Initialized = false;
    IBLCache::Statistics IBLCache::s_Stats;

    // Cache file header for version tracking
    // Use pragma pack to ensure consistent binary layout across compilers
#pragma pack(push, 1)
    struct IBLCacheHeader
    {
        char Magic[4] = { 'I', 'B', 'L', 'C' };
        u32 Version = 1;
        u32 Width = 0;
        u32 Height = 0;
        u32 Format = 0; // ImageFormat enum value
        u32 MipLevels = 0;
        u32 FaceCount = 0; // 1 for Texture2D, 6 for cubemap
        u64 DataSize = 0;  // Total data size following header
    };
#pragma pack(pop)

    // Verify header size at compile time
    static_assert(sizeof(IBLCacheHeader) == 36, "IBLCacheHeader size mismatch - packing may be incorrect");

    namespace
    {
        /**
         * @brief Get bytes per pixel for an image format
         * @param format The image format
         * @return Bytes per pixel, or 0 for unsupported formats
         */
        [[nodiscard]] u32 GetBytesPerPixel(ImageFormat format)
        {
            switch (format)
            {
                case ImageFormat::R8:
                    return 1;
                case ImageFormat::RGB8:
                    return 3;
                case ImageFormat::RGBA8:
                    return 4;
                case ImageFormat::R32F:
                    return 4;
                case ImageFormat::RG32F:
                    return 8;
                case ImageFormat::RGB32F:
                    return 12;
                case ImageFormat::RGBA32F:
                    return 16;
                default:
                    return 0;
            }
        }
    } // anonymous namespace

    void IBLCache::Initialize(const std::filesystem::path& cacheDirectory)
    {
        OLO_PROFILE_FUNCTION();

        s_CacheDirectory = cacheDirectory;
        s_Initialized = true;
        s_Stats = {};

        try
        {
            // Create cache directory if it doesn't exist
            if (!std::filesystem::exists(s_CacheDirectory))
            {
                std::filesystem::create_directories(s_CacheDirectory);
                OLO_CORE_INFO("IBLCache: Created cache directory: {}", s_CacheDirectory.string());
            }

            // Calculate existing cache size
            std::error_code ec;
            for (const auto& entry : std::filesystem::directory_iterator(s_CacheDirectory, ec))
            {
                if (ec)
                {
                    OLO_CORE_WARN("IBLCache: Error iterating cache directory: {}", ec.message());
                    break;
                }

                std::error_code fileEc;
                if (entry.is_regular_file(fileEc) && !fileEc)
                {
                    s_Stats.CacheSizeBytes += entry.file_size(fileEc);
                    if (fileEc)
                    {
                        OLO_CORE_WARN("IBLCache: Error getting file size for {}: {}", entry.path().string(), fileEc.message());
                    }
                }
            }
        }
        catch (const std::filesystem::filesystem_error& e)
        {
            OLO_CORE_ERROR("IBLCache: Filesystem error during initialization: {}", e.what());
        }
        catch (const std::exception& e)
        {
            OLO_CORE_ERROR("IBLCache: Error during initialization: {}", e.what());
        }

        OLO_CORE_INFO("IBLCache: Initialized with {} bytes cached", s_Stats.CacheSizeBytes);
    }

    void IBLCache::Shutdown()
    {
        OLO_PROFILE_FUNCTION();

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
        // Include hash in BRDF LUT filename to avoid collisions between different resolutions
        // (hash already incorporates BRDFLutResolution from config)
        paths.BRDFLut = s_CacheDirectory / (hashStr + "_brdf.iblcache");

        return paths;
    }

    bool IBLCache::TryLoad(const std::string& sourcePath,
                           const IBLConfiguration& config,
                           CachedIBL& outCached)
    {
        OLO_PROFILE_FUNCTION();

        if (!s_Initialized)
        {
            OLO_CORE_WARN("IBLCache: Not initialized");
            s_Stats.CacheMisses++;
            return false;
        }

        u64 hash = ComputeHash(sourcePath, config);
        CachePaths paths = GetCachePaths(hash);

        try
        {
            // Check if all cache files exist
            if (!std::filesystem::exists(paths.Irradiance) ||
                !std::filesystem::exists(paths.Prefilter) ||
                !std::filesystem::exists(paths.BRDFLut))
            {
                OLO_CORE_TRACE("IBLCache: Cache miss for {} (files not found)", sourcePath);
                s_Stats.CacheMisses++;
                return false;
            }
        }
        catch (const std::filesystem::filesystem_error& e)
        {
            OLO_CORE_ERROR("IBLCache: Filesystem error checking cache: {}", e.what());
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
        OLO_PROFILE_FUNCTION();

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

        // Save BRDF LUT (resolution-specific via hash, so always save)
        if (!SaveTexture2DToCache(brdfLut, paths.BRDFLut))
        {
            OLO_CORE_WARN("IBLCache: Failed to save BRDF LUT");
            success = false;
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
        OLO_PROFILE_FUNCTION();

        if (!s_Initialized)
            return false;

        u64 hash = ComputeHash(sourcePath, config);
        CachePaths paths = GetCachePaths(hash);

        try
        {
            return std::filesystem::exists(paths.Irradiance) &&
                   std::filesystem::exists(paths.Prefilter) &&
                   std::filesystem::exists(paths.BRDFLut);
        }
        catch (const std::filesystem::filesystem_error& e)
        {
            OLO_CORE_ERROR("IBLCache: Filesystem error checking cache existence: {}", e.what());
            return false;
        }
    }

    void IBLCache::Invalidate(const std::string& sourcePath, const IBLConfiguration& config)
    {
        OLO_PROFILE_FUNCTION();

        if (!s_Initialized)
            return;

        u64 hash = ComputeHash(sourcePath, config);
        CachePaths paths = GetCachePaths(hash);

        try
        {
            u64 removedBytes = 0;
            u32 removedFiles = 0;

            auto removeFile = [&](const std::filesystem::path& path)
            {
                if (std::filesystem::exists(path))
                {
                    removedBytes += std::filesystem::file_size(path);
                    std::filesystem::remove(path);
                    removedFiles++;
                }
            };

            removeFile(paths.Irradiance);
            removeFile(paths.Prefilter);
            removeFile(paths.BRDFLut);

            if (removedFiles > 0)
            {
                s_Stats.CacheSizeBytes = (s_Stats.CacheSizeBytes > removedBytes)
                                             ? s_Stats.CacheSizeBytes - removedBytes
                                             : 0;
                OLO_CORE_INFO("IBLCache: Invalidated cache for {} ({} files, {} bytes)",
                              sourcePath, removedFiles, removedBytes);
            }
        }
        catch (const std::filesystem::filesystem_error& e)
        {
            OLO_CORE_ERROR("IBLCache: Filesystem error during invalidation: {}", e.what());
        }
    }

    void IBLCache::ClearAll()
    {
        OLO_PROFILE_FUNCTION();

        if (!s_Initialized)
            return;

        try
        {
            if (!std::filesystem::exists(s_CacheDirectory))
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
        catch (const std::filesystem::filesystem_error& e)
        {
            OLO_CORE_ERROR("IBLCache: Filesystem error during clear: {}", e.what());
        }
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

        // Validate header read
        if (!file || file.gcount() != static_cast<std::streamsize>(sizeof(header)))
        {
            OLO_CORE_ERROR("IBLCache: Failed to read cache file header: {}", path.string());
            return nullptr;
        }

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

        u32 bytesPerPixel = GetBytesPerPixel(spec.Format);

        // Read each mip level's face data
        for (u32 mip = 0; mip < header.MipLevels; mip++)
        {
            u32 mipWidth = std::max(1u, header.Width >> mip);
            u32 mipHeight = std::max(1u, header.Height >> mip);
            sizet faceSize = static_cast<sizet>(mipWidth) * mipHeight * bytesPerPixel;

            for (u32 face = 0; face < 6; face++)
            {
                if (mip == 0)
                {
                    std::vector<u8> faceData(faceSize);
                    file.read(reinterpret_cast<char*>(faceData.data()), static_cast<std::streamsize>(faceSize));

                    if (!file.good())
                    {
                        OLO_CORE_ERROR("IBLCache: Failed to read base mip data for face {} from {}", face, path.string());
                        return nullptr;
                    }

                    // SetFaceData only works for mip 0, for now just load base level
                    cubemap->SetFaceData(face, faceData.data(), static_cast<u32>(faceSize));
                }
                else
                {
                    // Skip higher mip levels for now to avoid unnecessary I/O
                    file.seekg(static_cast<std::streamoff>(faceSize), std::ios::cur);
                    if (!file.good())
                    {
                        OLO_CORE_ERROR("IBLCache: Failed to skip mip {} data for face {} in {}", mip, face, path.string());
                        return nullptr;
                    }
                }
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

        u32 bytesPerPixel = GetBytesPerPixel(spec.Format);

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

        file.flush();
        if (!file.good())
        {
            OLO_CORE_ERROR("IBLCache: Failed to write cubemap to cache: {}", path.string());
            return false;
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

        // Validate header read
        if (!file || file.gcount() != static_cast<std::streamsize>(sizeof(header)))
        {
            OLO_CORE_ERROR("IBLCache: Failed to read cache file header: {}", path.string());
            return nullptr;
        }

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

        if (!file.good())
        {
            OLO_CORE_ERROR("IBLCache: Failed to read texture data from cache: {}", path.string());
            return nullptr;
        }

        texture->SetData(data.data(), static_cast<u32>(header.DataSize));

        return texture;
    }

    bool IBLCache::SaveTexture2DToCache(const Ref<Texture2D>& texture,
                                        const std::filesystem::path& path)
    {
        if (!texture)
            return false;

        const auto& spec = texture->GetSpecification();

        u32 bytesPerPixel = GetBytesPerPixel(spec.Format);

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

        file.flush();
        if (!file.good())
        {
            OLO_CORE_ERROR("IBLCache: Failed to write texture to cache: {}", path.string());
            return false;
        }
        file.close();

        // Update cache size stats
        s_Stats.CacheSizeBytes += sizeof(header) + dataSize;

        OLO_CORE_TRACE("IBLCache: Saved texture to {} ({} bytes)", path.string(), sizeof(header) + dataSize);
        return true;
    }

} // namespace OloEngine
