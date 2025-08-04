#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/TextureCubemap.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Mesh.h"

namespace OloEngine
{
    // Forward declaration
    class ShaderLibrary;
    
    enum class IBLQuality
    {
        Low = 0,     // Fast generation, lower quality
        Medium = 1,  // Balanced quality/performance
        High = 2,    // High quality, slower generation
        Ultra = 3    // Maximum quality, longest generation time
    };

    struct IBLConfiguration
    {
        // Quality settings
        IBLQuality Quality = IBLQuality::Medium;
        bool UseImportanceSampling = true;
        bool UseSphericalHarmonics = false; // Alternative to irradiance cubemap
        
        // Resolution settings
        u32 IrradianceResolution = 32;      // Diffuse irradiance map resolution
        u32 PrefilterResolution = 128;      // Specular prefilter map resolution
        u32 BRDFLutResolution = 512;        // BRDF lookup table resolution
        
        // Sample counts for Monte Carlo integration
        u32 IrradianceSamples = 1024;       // Samples for irradiance generation
        u32 PrefilterSamples = 1024;        // Samples for prefilter generation
        
        // Performance optimization
        bool EnableMultithreading = true;   // Use multiple threads for generation
    };

    struct EnvironmentMapSpecification
    {
        std::string FilePath;
        u32 Resolution = 512;
        ImageFormat Format = ImageFormat::RGB32F;
        bool GenerateIBL = true;
        bool GenerateMipmaps = true;
        IBLConfiguration IBLConfig;         // Enhanced IBL configuration
    };

    class EnvironmentMap : public RefCounted
    {
    public:
        EnvironmentMap(const EnvironmentMapSpecification& spec);
        ~EnvironmentMap() = default;

        // Initialize IBL system with shader library (call once at engine startup)
        static void InitializeIBLSystem(ShaderLibrary& shaderLibrary);

        // Load environment map from HDR file
        static AssetRef<EnvironmentMap> Create(const EnvironmentMapSpecification& spec);
        static AssetRef<EnvironmentMap> CreateFromCubemap(const AssetRef<TextureCubemap>& cubemap);
        static AssetRef<EnvironmentMap> CreateFromEquirectangular(const std::string& filePath);

        // Get textures
        const AssetRef<TextureCubemap>& GetEnvironmentMap() const { return m_EnvironmentMap; }
        const AssetRef<TextureCubemap>& GetIrradianceMap() const { return m_IrradianceMap; }
        const AssetRef<TextureCubemap>& GetPrefilterMap() const { return m_PrefilterMap; }
        const AssetRef<Texture2D>& GetBRDFLutMap() const { return m_BRDFLutMap; }

        // Check if IBL is available
        bool HasIBL() const { return m_IrradianceMap && m_PrefilterMap && m_BRDFLutMap; }

        // Get specification
        const EnvironmentMapSpecification& GetSpecification() const { return m_Specification; }
        
        // Enhanced IBL configuration
        void SetIBLConfiguration(const IBLConfiguration& config);
        const IBLConfiguration& GetIBLConfiguration() const { return m_Specification.IBLConfig; }
        
        // Generate IBL with custom settings
        void RegenerateIBL(const IBLConfiguration& config);

    private:
        void GenerateIBLTextures();
        void GenerateIrradianceMap();
        void GeneratePrefilterMap();
        void GenerateBRDFLut();
        
        // Enhanced IBL generation with configurable quality
        void GenerateIBLWithConfig(const IBLConfiguration& config);
        void GenerateIrradianceMapWithConfig(const IBLConfiguration& config);
        void GeneratePrefilterMapWithConfig(const IBLConfiguration& config);
        void GenerateBRDFLutWithConfig(const IBLConfiguration& config);

        AssetRef<TextureCubemap> ConvertEquirectangularToCubemap(const std::string& filePath);

    private:
        EnvironmentMapSpecification m_Specification;
        AssetRef<TextureCubemap> m_EnvironmentMap;
        AssetRef<TextureCubemap> m_IrradianceMap;
        AssetRef<TextureCubemap> m_PrefilterMap;
        AssetRef<Texture2D> m_BRDFLutMap;
        
        // Static shader library for IBL operations
        static ShaderLibrary* s_ShaderLibrary;
    };
}
