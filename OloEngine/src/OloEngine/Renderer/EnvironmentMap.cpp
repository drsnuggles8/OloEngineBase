#include "OloEnginePCH.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/EnvironmentMap.h"
#include "OloEngine/Renderer/IBLPrecompute.h"
#include "OloEngine/Renderer/IBLCache.h"
#include "OloEngine/Renderer/Renderer.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/Commands/RenderCommand.h"
#include "OloEngine/Renderer/Debug/RendererMemoryTracker.h"
#include "OloEngine/Renderer/ShaderLibrary.h"

#include <stb_image/stb_image.h>

namespace OloEngine
{
    // Static member definition
    ShaderLibrary* EnvironmentMap::s_ShaderLibrary = nullptr;
    void EnvironmentMap::InitializeIBLSystem(ShaderLibrary& shaderLibrary)
    {
        s_ShaderLibrary = &shaderLibrary;

        // Initialize IBL cache for disk caching
        // Use default cache directory "assets/cache/ibl" from IBLCache::Initialize
        const std::filesystem::path cacheDir = "assets/cache/ibl";
        if (!std::filesystem::exists(cacheDir))
        {
            try
            {
                std::filesystem::create_directories(cacheDir);
                OLO_CORE_INFO("EnvironmentMap: Created IBL cache directory: {}", cacheDir.string());
            }
            catch (const std::filesystem::filesystem_error& e)
            {
                OLO_CORE_ERROR("EnvironmentMap: Failed to create IBL cache directory: {}", e.what());
            }
        }
        IBLCache::Initialize(cacheDir);

        OLO_CORE_INFO("EnvironmentMap: IBL system initialized with shader library");
    }

    EnvironmentMap::EnvironmentMap(const EnvironmentMapSpecification& spec)
        : m_Specification(spec)
    {
        OLO_PROFILE_FUNCTION();

        if (!spec.FilePath.empty())
        {
            // Load from file
            m_EnvironmentMap = ConvertEquirectangularToCubemap(spec.FilePath);
        }

        if (spec.GenerateIBL && m_EnvironmentMap)
        {
            GenerateIBLTextures();
        }

        OLO_TRACK_GPU_ALLOC(this,
                            spec.Resolution * spec.Resolution * 6 * 4 * sizeof(float), // Rough estimate
                            RendererMemoryTracker::ResourceType::TextureCubemap,
                            "Environment Map");
    }

    Ref<EnvironmentMap> EnvironmentMap::Create(const EnvironmentMapSpecification& spec)
    {
        return Ref<EnvironmentMap>::Create(spec);
    }

    Ref<EnvironmentMap> EnvironmentMap::CreateFromCubemap(const Ref<TextureCubemap>& cubemap)
    {
        return CreateFromCubemap(cubemap, IBLConfiguration{});
    }

    Ref<EnvironmentMap> EnvironmentMap::CreateFromCubemap(const Ref<TextureCubemap>& cubemap, const IBLConfiguration& iblConfig)
    {
        EnvironmentMapSpecification spec;
        spec.Resolution = cubemap->GetWidth();
        spec.Format = ImageFormat::RGBA32F;
        spec.GenerateIBL = true;
        spec.IBLConfig = iblConfig;

        auto envMap = Ref<EnvironmentMap>::Create(spec);
        envMap->m_EnvironmentMap = cubemap;

        if (spec.GenerateIBL)
        {
            envMap->GenerateIBLTextures();
        }

        return envMap;
    }

    Ref<EnvironmentMap> EnvironmentMap::CreateFromEquirectangular(const std::string& filePath)
    {
        return CreateFromEquirectangular(filePath, IBLConfiguration{});
    }

    Ref<EnvironmentMap> EnvironmentMap::CreateFromEquirectangular(const std::string& filePath, const IBLConfiguration& iblConfig)
    {
        EnvironmentMapSpecification spec;
        spec.FilePath = filePath;
        spec.Resolution = 512;
        spec.Format = ImageFormat::RGBA32F;
        spec.GenerateIBL = true;
        spec.GenerateMipmaps = true;
        spec.IBLConfig = iblConfig;

        return Create(spec);
    }

    void EnvironmentMap::GenerateIBLTextures()
    {
        OLO_PROFILE_FUNCTION();

        if (!m_EnvironmentMap)
        {
            OLO_CORE_ERROR("EnvironmentMap::GenerateIBLTextures: No environment map available");
            return;
        }

        // Generate IBL textures with enhanced configuration
        GenerateIBLWithConfig(m_Specification.IBLConfig);

        OLO_CORE_INFO("IBL textures generated successfully");
    }

    void EnvironmentMap::GenerateIrradianceMap()
    {
        GenerateIrradianceMapWithConfig(m_Specification.IBLConfig);
    }

    void EnvironmentMap::GeneratePrefilterMap()
    {
        GeneratePrefilterMapWithConfig(m_Specification.IBLConfig);
    }

    void EnvironmentMap::GenerateBRDFLut()
    {
        GenerateBRDFLutWithConfig(m_Specification.IBLConfig);
    }

    // Enhanced IBL generation methods
    void EnvironmentMap::GenerateIBLWithConfig(const IBLConfiguration& config)
    {
        OLO_PROFILE_FUNCTION();

        // Generate a unique cache key
        std::string cacheKey;
        if (!m_Specification.FilePath.empty())
        {
            cacheKey = m_Specification.FilePath;
        }
        else if (m_EnvironmentMap)
        {
            // For in-memory cubemaps, use the texture's path + dimensions as a
            // stable key.  The previous pointer-address approach produced a
            // different key every time the scene was reloaded, preventing the
            // cache from ever hitting.
            const auto& texPath = m_EnvironmentMap->GetPath();
            if (texPath.empty())
            {
                // Pathless cubemaps (procedural / render-target) cannot produce
                // a stable cache key across reloads, so skip disk-cache reuse.
                OLO_CORE_WARN("GenerateIBLWithConfig: Pathless cubemap — skipping IBL disk cache");
                cacheKey.clear();
            }
            else
            {
                cacheKey = fmt::format("cubemap_{}_{}x{}_{}",
                                       texPath,
                                       m_EnvironmentMap->GetWidth(),
                                       m_EnvironmentMap->GetHeight(),
                                       static_cast<int>(m_EnvironmentMap->GetSpecification().Format));
            }
        }
        else
        {
            OLO_CORE_ERROR("GenerateIBLWithConfig: No environment map available");
            return;
        }

        // Procedural / runtime-varying sources opt out of the disk cache: the
        // cache key can't see their per-bake parameters (the cubemap path is a
        // constant debug name like "Generated Cubemap"), so a hit would serve
        // stale IBL for freshly-baked pixels — e.g. ProceduralSky changing sun
        // direction / turbidity / exposure.
        if (!config.UseDiskCache)
        {
            cacheKey.clear();
        }

        if (IBLCache::CachedIBL cached; !cacheKey.empty() && IBLCache::TryLoad(cacheKey, config, cached))
        {
            // Use cached IBL textures
            m_IrradianceMap = cached.Irradiance;
            m_PrefilterMap = cached.Prefilter;
            m_BRDFLutMap = cached.BRDFLut;
            OLO_CORE_INFO("IBL textures loaded from cache");
            return;
        }

        OLO_CORE_INFO("Generating IBL textures with quality: {}, importance sampling: {}",
                      static_cast<int>(config.Quality), config.UseImportanceSampling);

        GenerateIrradianceMapWithConfig(config);
        GeneratePrefilterMapWithConfig(config);
        GenerateBRDFLutWithConfig(config);

        // Save to cache for next time
        if (!cacheKey.empty() && m_IrradianceMap && m_PrefilterMap && m_BRDFLutMap)
        {
            IBLCache::Save(cacheKey, config, m_IrradianceMap, m_PrefilterMap, m_BRDFLutMap);
        }
    }

    void EnvironmentMap::GenerateIrradianceMapWithConfig(const IBLConfiguration& config)
    {
        OLO_PROFILE_FUNCTION();

        // Create irradiance map specification with configurable resolution
        CubemapSpecification irradianceSpec;
        irradianceSpec.Width = config.IrradianceResolution;
        irradianceSpec.Height = config.IrradianceResolution;
        irradianceSpec.Format = ImageFormat::RGBA32F;
        irradianceSpec.GenerateMips = false;

        m_IrradianceMap = TextureCubemap::Create(irradianceSpec);

        // Use IBLPrecompute to generate the irradiance map with enhanced settings
        if (s_ShaderLibrary)
        {
            if (config.UseSphericalHarmonics)
            {
                // L2 SH projection path. Output is bit-compatible with the
                // convolution path (same RGBA32F cubemap layout), so all PBR
                // shaders consume the result identically. Generation is ~100x
                // faster: a single CPU-side projection over the source cubemap
                // pixels followed by per-texel SH evaluation, vs Monte Carlo
                // summation of `config.IrradianceSamples` directions per output
                // texel.
                IBLPrecompute::GenerateIrradianceMapFromSH(m_EnvironmentMap, m_IrradianceMap,
                                                           *s_ShaderLibrary, config);
                OLO_CORE_INFO("Irradiance map generated via L2 SH ({}x{})",
                              config.IrradianceResolution, config.IrradianceResolution);
            }
            else
            {
                IBLPrecompute::GenerateIrradianceMapAdvanced(m_EnvironmentMap, m_IrradianceMap,
                                                             *s_ShaderLibrary, config);
                OLO_CORE_INFO("Enhanced irradiance map generated ({}x{}) with {} samples",
                              config.IrradianceResolution, config.IrradianceResolution, config.IrradianceSamples);
            }
        }
        else
        {
            OLO_CORE_ERROR("EnvironmentMap: IBL system not initialized! Call InitializeIBLSystem() first.");
        }
    }

    void EnvironmentMap::GeneratePrefilterMapWithConfig(const IBLConfiguration& config)
    {
        OLO_PROFILE_FUNCTION();

        // Create prefilter map specification with configurable resolution
        CubemapSpecification prefilterSpec;
        prefilterSpec.Width = config.PrefilterResolution;
        prefilterSpec.Height = config.PrefilterResolution;
        prefilterSpec.Format = ImageFormat::RGBA32F;
        prefilterSpec.GenerateMips = true;

        m_PrefilterMap = TextureCubemap::Create(prefilterSpec);

        // Use IBLPrecompute to generate the prefiltered environment map with enhanced settings
        if (s_ShaderLibrary)
        {
            IBLPrecompute::GeneratePrefilterMapAdvanced(m_EnvironmentMap, m_PrefilterMap,
                                                        *s_ShaderLibrary, config);
        }
        else
        {
            OLO_CORE_ERROR("EnvironmentMap: IBL system not initialized! Call InitializeIBLSystem() first.");
        }

        OLO_CORE_INFO("Enhanced prefilter map generated ({}x{}) with {} samples and importance sampling: {}",
                      config.PrefilterResolution, config.PrefilterResolution,
                      config.PrefilterSamples, config.UseImportanceSampling);
    }

    void EnvironmentMap::GenerateBRDFLutWithConfig(const IBLConfiguration& config)
    {
        OLO_PROFILE_FUNCTION();

        // Create BRDF LUT specification with configurable resolution
        TextureSpecification brdfSpec;
        brdfSpec.Width = config.BRDFLutResolution;
        brdfSpec.Height = config.BRDFLutResolution;
        brdfSpec.Format = ImageFormat::RG32F;
        brdfSpec.GenerateMips = false;

        m_BRDFLutMap = Texture2D::Create(brdfSpec);

        // Use IBLPrecompute to generate the BRDF lookup table with enhanced settings
        if (s_ShaderLibrary)
        {
            IBLPrecompute::GenerateBRDFLutAdvanced(m_BRDFLutMap, *s_ShaderLibrary, config);
        }
        else
        {
            OLO_CORE_ERROR("EnvironmentMap: IBL system not initialized! Call InitializeIBLSystem() first.");
        }

        OLO_CORE_INFO("Enhanced BRDF LUT generated ({}x{})",
                      config.BRDFLutResolution, config.BRDFLutResolution);
    }

    // Configuration management
    void EnvironmentMap::SetIBLConfiguration(const IBLConfiguration& config)
    {
        m_Specification.IBLConfig = config;
    }

    void EnvironmentMap::RegenerateIBL(const IBLConfiguration& config)
    {
        m_Specification.IBLConfig = config;
        GenerateIBLWithConfig(config);
    }

    Ref<TextureCubemap> EnvironmentMap::ConvertEquirectangularToCubemap(const std::string& filePath)
    {
        OLO_PROFILE_FUNCTION();

        // Use IBLPrecompute to handle the conversion
        if (s_ShaderLibrary)
        {
            return IBLPrecompute::ConvertEquirectangularToCubemap(filePath, *s_ShaderLibrary, m_Specification.Resolution);
        }
        else
        {
            OLO_CORE_ERROR("EnvironmentMap: IBL system not initialized! Call InitializeIBLSystem() first.");
            return nullptr;
        }
    }
} // namespace OloEngine
