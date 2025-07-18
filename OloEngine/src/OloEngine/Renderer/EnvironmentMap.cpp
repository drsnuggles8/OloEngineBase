#include "OloEnginePCH.h"
#include "OloEngine/Renderer/EnvironmentMap.h"
#include "OloEngine/Renderer/IBLPrecompute.h"
#include "OloEngine/Renderer/Renderer.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/Commands/RenderCommand.h"
#include "OloEngine/Renderer/Debug/RendererMemoryTracker.h"
#include "OloEngine/Renderer/ShaderLibrary.h"

#include <stb_image/stb_image.h>
#include <filesystem>
#include <fstream>
#include <ctime>

namespace OloEngine
{
    // Static member definition
    ShaderLibrary* EnvironmentMap::s_ShaderLibrary = nullptr;
    void EnvironmentMap::InitializeIBLSystem(ShaderLibrary& shaderLibrary)
    {
        s_ShaderLibrary = &shaderLibrary;
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
        return CreateRef<EnvironmentMap>(spec);
    }

    Ref<EnvironmentMap> EnvironmentMap::CreateFromCubemap(const Ref<TextureCubemap>& cubemap)
    {
        EnvironmentMapSpecification spec;
        spec.Resolution = cubemap->GetWidth();
        spec.Format = ImageFormat::RGB32F;
        spec.GenerateIBL = true;
        
        auto envMap = CreateRef<EnvironmentMap>(spec);
        envMap->m_EnvironmentMap = cubemap;
        
        if (spec.GenerateIBL)
        {
            envMap->GenerateIBLTextures();
        }
        
        return envMap;
    }

    Ref<EnvironmentMap> EnvironmentMap::CreateFromEquirectangular(const std::string& filePath)
    {
        EnvironmentMapSpecification spec;
        spec.FilePath = filePath;
        spec.Resolution = 512;
        spec.Format = ImageFormat::RGB32F;
        spec.GenerateIBL = true;
        spec.GenerateMipmaps = true;
        
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

        // Check if we can load from cache first
        std::string cacheKey = GenerateCacheKey();
        if (m_Specification.IBLConfig.CacheToFile && LoadFromCache(cacheKey))
        {
            OLO_CORE_INFO("IBL textures loaded from cache for key: {}", cacheKey);
            return;
        }

        // Generate IBL textures with enhanced configuration
        GenerateIBLWithConfig(m_Specification.IBLConfig);
        
        // Save to cache if enabled
        if (m_Specification.IBLConfig.CacheToFile)
        {
            SaveToCache(cacheKey);
        }
        
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
        
        OLO_CORE_INFO("Generating IBL textures with quality: {}, importance sampling: {}", 
                     static_cast<int>(config.Quality), config.UseImportanceSampling);
        
        GenerateIrradianceMapWithConfig(config);
        GeneratePrefilterMapWithConfig(config);
        GenerateBRDFLutWithConfig(config);
    }

    void EnvironmentMap::GenerateIrradianceMapWithConfig(const IBLConfiguration& config)
    {
        OLO_PROFILE_FUNCTION();
        
        // Create irradiance map specification with configurable resolution
        CubemapSpecification irradianceSpec;
        irradianceSpec.Width = config.IrradianceResolution;
        irradianceSpec.Height = config.IrradianceResolution;
        irradianceSpec.Format = ImageFormat::RGB32F;
        irradianceSpec.GenerateMips = false;
        
        m_IrradianceMap = TextureCubemap::Create(irradianceSpec);
        
        // Use IBLPrecompute to generate the irradiance map with enhanced settings
        if (s_ShaderLibrary)
        {
            if (config.UseSphericalHarmonics)
            {
                // TODO: Implement spherical harmonics alternative
                OLO_CORE_INFO("Spherical harmonics not yet implemented, falling back to cubemap");
            }
            
            IBLPrecompute::GenerateIrradianceMapAdvanced(m_EnvironmentMap, m_IrradianceMap, 
                                                       *s_ShaderLibrary, config);
        }
        else
        {
            OLO_CORE_ERROR("EnvironmentMap: IBL system not initialized! Call InitializeIBLSystem() first.");
        }
        
        OLO_CORE_INFO("Enhanced irradiance map generated ({}x{}) with {} samples", 
                     config.IrradianceResolution, config.IrradianceResolution, config.IrradianceSamples);
    }

    void EnvironmentMap::GeneratePrefilterMapWithConfig(const IBLConfiguration& config)
    {
        OLO_PROFILE_FUNCTION();
        
        // Create prefilter map specification with configurable resolution
        CubemapSpecification prefilterSpec;
        prefilterSpec.Width = config.PrefilterResolution;
        prefilterSpec.Height = config.PrefilterResolution;
        prefilterSpec.Format = ImageFormat::RGB32F;
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

    // Cache management
    std::string EnvironmentMap::GenerateCacheKey() const
    {
        const auto& config = m_Specification.IBLConfig;
        
        // Create a hash from configuration parameters
        std::string key = m_Specification.FilePath + "_" +
                         std::to_string(static_cast<int>(config.Quality)) + "_" +
                         std::to_string(config.IrradianceResolution) + "_" +
                         std::to_string(config.PrefilterResolution) + "_" +
                         std::to_string(config.BRDFLutResolution) + "_" +
                         std::to_string(config.IrradianceSamples) + "_" +
                         std::to_string(config.PrefilterSamples) + "_" +
                         (config.UseImportanceSampling ? "1" : "0") + "_" +
                         (config.UseSphericalHarmonics ? "1" : "0");
        
        // Simple hash function for cache key
        std::hash<std::string> hasher;
        return std::to_string(hasher(key));
    }

    bool EnvironmentMap::LoadFromCache(const std::string& cacheKey)
    {
        OLO_PROFILE_FUNCTION();
        
        const auto& config = m_Specification.IBLConfig;
        if (!config.CacheToFile)
        {
            return false;
        }
        
        // Create cache directory path
        std::filesystem::path cacheDir = config.CacheDirectory;
        if (!std::filesystem::exists(cacheDir))
        {
            return false;
        }
        
        std::filesystem::path cachePath = cacheDir / cacheKey;
        
        try
        {
            // Check if all required cache files exist
            std::filesystem::path irradiancePath = cachePath / "irradiance.hdr";
            std::filesystem::path prefilterPath = cachePath / "prefilter.hdr";
            std::filesystem::path brdfPath = cachePath / "brdf.png";
            
            if (!std::filesystem::exists(irradiancePath) || 
                !std::filesystem::exists(prefilterPath) || 
                !std::filesystem::exists(brdfPath))
            {
                return false;
            }
            
            // TODO: Load the cached textures from files
            // For now, we would need to implement texture loading from HDR/PNG files
            // This would involve:
            // 1. Loading irradiance cubemap from irradiance.hdr
            // 2. Loading prefilter cubemap from prefilter.hdr  
            // 3. Loading BRDF LUT from brdf.png
            // 4. Setting m_IrradianceMap, m_PrefilterMap, m_BRDFLutMap
            
            OLO_CORE_INFO("EnvironmentMap: Found cache files for key '{}', but loading not yet implemented", cacheKey);
            return false; // Return false until loading is implemented
        }
        catch (const std::filesystem::filesystem_error& e)
        {
            OLO_CORE_ERROR("EnvironmentMap: Cache loading failed: {}", e.what());
            return false;
        }
    }

    void EnvironmentMap::SaveToCache(const std::string& cacheKey) const
    {
        OLO_PROFILE_FUNCTION();
        
        const auto& config = m_Specification.IBLConfig;
        if (!config.CacheToFile)
        {
            return;
        }
        
        // Don't cache if IBL textures are not available
        if (!HasIBL())
        {
            OLO_CORE_WARN("EnvironmentMap: Cannot cache IBL textures - they are not generated");
            return;
        }
        
        // Create cache directory path
        std::filesystem::path cacheDir = config.CacheDirectory;
        std::filesystem::path cachePath = cacheDir / cacheKey;
        
        try
        {
            // Create cache directories if they don't exist
            std::filesystem::create_directories(cachePath);
            
            // TODO: Save the IBL textures to cache files
            // This would involve:
            // 1. Saving irradiance cubemap as HDR files (6 faces)
            // 2. Saving prefilter cubemap as HDR files (6 faces, multiple mip levels)
            // 3. Saving BRDF LUT as PNG file
            // Implementation requires texture data extraction from GPU and file writing
            
            OLO_CORE_INFO("EnvironmentMap: Created cache directory for key '{}', but saving not yet implemented", cacheKey);
            
            // Create a metadata file to track what's cached
            std::filesystem::path metadataPath = cachePath / "metadata.txt";
            std::ofstream metadataFile(metadataPath);
            if (metadataFile.is_open())
            {
                metadataFile << "Cache Key: " << cacheKey << std::endl;
                metadataFile << "Generated: " << std::time(nullptr) << std::endl;
                metadataFile << "Irradiance Resolution: " << config.IrradianceResolution << std::endl;
                metadataFile << "Prefilter Resolution: " << config.PrefilterResolution << std::endl;
                metadataFile << "BRDF LUT Resolution: " << config.BRDFLutResolution << std::endl;
                metadataFile << "Quality: " << static_cast<int>(config.Quality) << std::endl;
                metadataFile.close();
            }
        }
        catch (const std::filesystem::filesystem_error& e)
        {
            OLO_CORE_ERROR("EnvironmentMap: Cache saving failed: {}", e.what());
        }
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
}
