#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/TextureCubemap.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Renderer/EnvironmentMap.h"

#include <filesystem>
#include <string>

namespace OloEngine
{
    /**
     * @brief Cache for IBL (Image-Based Lighting) textures
     *
     * IBL texture generation (irradiance, prefilter, BRDF LUT) is computationally
     * expensive (~500ms+ per environment map). This cache saves the generated
     * textures to disk and loads them on subsequent runs, dramatically reducing
     * startup time.
     *
     * Cache Structure:
     * ```
     * assets/cache/ibl/
     * ├── {hash}_irradiance.iblcache    (diffuse irradiance cubemap)
     * ├── {hash}_prefilter.iblcache     (specular prefilter cubemap with mips)
     * └── {hash}_brdf_{resolution}.iblcache  (BRDF LUT, resolution-specific)
     * ```
     *
     * The hash is computed from the source cubemap path + IBL configuration,
     * so cache is invalidated when settings change.
     *
     * Usage:
     * @code
     * // Before generating IBL
     * IBLCache::CachedIBL cached;
     * if (IBLCache::TryLoad(envMapPath, config, cached))
     * {
     *     // Use cached.Irradiance, cached.Prefilter, cached.BRDFLut
     * }
     * else
     * {
     *     // Generate IBL textures
     *     GenerateIBLTextures();
     *     // Save to cache
     *     IBLCache::Save(envMapPath, config, irradiance, prefilter, brdfLut);
     * }
     * @endcode
     */
    class IBLCache
    {
      public:
        /**
         * @brief Container for cached IBL textures
         */
        struct CachedIBL
        {
            Ref<TextureCubemap> Irradiance;
            Ref<TextureCubemap> Prefilter;
            Ref<Texture2D> BRDFLut;

            bool IsValid() const
            {
                return Irradiance && Prefilter && BRDFLut;
            }
        };

        /**
         * @brief Initialize the cache system
         * @param cacheDirectory Base directory for cache files
         */
        static void Initialize(const std::filesystem::path& cacheDirectory = "assets/cache/ibl");

        /**
         * @brief Shutdown and cleanup
         */
        static void Shutdown();

        /**
         * @brief Try to load cached IBL textures
         *
         * @param sourcePath Path to the source environment map
         * @param config IBL configuration used for generation
         * @param outCached Output structure for loaded textures
         * @return true if cache hit, false if miss or invalid
         */
        static bool TryLoad(const std::string& sourcePath,
                            const IBLConfiguration& config,
                            CachedIBL& outCached);

        /**
         * @brief Save IBL textures to cache
         *
         * @param sourcePath Path to the source environment map
         * @param config IBL configuration used for generation
         * @param irradiance Irradiance cubemap to cache
         * @param prefilter Prefilter cubemap to cache
         * @param brdfLut BRDF LUT texture to cache
         * @return true if save succeeded
         */
        static bool Save(const std::string& sourcePath,
                         const IBLConfiguration& config,
                         const Ref<TextureCubemap>& irradiance,
                         const Ref<TextureCubemap>& prefilter,
                         const Ref<Texture2D>& brdfLut);

        /**
         * @brief Check if valid cache exists for an environment map
         *
         * This is a fast check that doesn't load the textures.
         */
        static bool HasCache(const std::string& sourcePath, const IBLConfiguration& config);

        /**
         * @brief Invalidate cache for a specific environment map
         *
         * @param sourcePath Path to the source environment map
         * @param config IBL configuration to invalidate (needed for hash computation)
         */
        static void Invalidate(const std::string& sourcePath, const IBLConfiguration& config);

        /**
         * @brief Clear entire IBL cache
         */
        static void ClearAll();

        /**
         * @brief Get cache statistics
         */
        struct Statistics
        {
            u64 CacheHits = 0;
            u64 CacheMisses = 0;
            u64 SaveCount = 0;
            u64 CacheSizeBytes = 0;
        };

        static Statistics GetStatistics();

        /**
         * @brief Get the cache directory path
         */
        static std::filesystem::path GetCacheDirectory();

      private:
        /**
         * @brief Generate a hash for cache key
         *
         * Hash includes source path and all IBL config settings.
         */
        static u64 ComputeHash(const std::string& sourcePath, const IBLConfiguration& config);

        /**
         * @brief Get cache file paths for a given source
         */
        struct CachePaths
        {
            std::filesystem::path Irradiance;
            std::filesystem::path Prefilter;
            std::filesystem::path BRDFLut;
        };

        static CachePaths GetCachePaths(u64 hash);

        /**
         * @brief Load a cubemap from cache file
         */
        static Ref<TextureCubemap> LoadCubemapFromCache(const std::filesystem::path& path);

        /**
         * @brief Save a cubemap to cache file
         *
         * Saves as raw binary format for fast loading (not KTX2 for simplicity).
         */
        static bool SaveCubemapToCache(const Ref<TextureCubemap>& cubemap,
                                       const std::filesystem::path& path);

        /**
         * @brief Load a 2D texture from cache file
         */
        static Ref<Texture2D> LoadTexture2DFromCache(const std::filesystem::path& path);

        /**
         * @brief Save a 2D texture to cache file
         */
        static bool SaveTexture2DToCache(const Ref<Texture2D>& texture,
                                         const std::filesystem::path& path);

        static std::filesystem::path s_CacheDirectory;
        static bool s_Initialized;
        static Statistics s_Stats;
    };

} // namespace OloEngine
