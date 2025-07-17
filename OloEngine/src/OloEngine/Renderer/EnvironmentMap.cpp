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

        GenerateIrradianceMap();
        GeneratePrefilterMap();
        GenerateBRDFLut();
        
        OLO_CORE_INFO("IBL textures generated successfully");
    }

    void EnvironmentMap::GenerateIrradianceMap()
    {
        OLO_PROFILE_FUNCTION();
        
        // Create irradiance map specification
        CubemapSpecification irradianceSpec;
        irradianceSpec.Width = 32;
        irradianceSpec.Height = 32;
        irradianceSpec.Format = ImageFormat::RGB32F;
        irradianceSpec.GenerateMips = false;
        
        m_IrradianceMap = TextureCubemap::Create(irradianceSpec);
        
        // Use IBLPrecompute to generate the irradiance map
        if (s_ShaderLibrary)
        {
            IBLPrecompute::GenerateIrradianceMap(m_EnvironmentMap, m_IrradianceMap, *s_ShaderLibrary);
        }
        else
        {
            OLO_CORE_ERROR("EnvironmentMap: IBL system not initialized! Call InitializeIBLSystem() first.");
        }
        
        OLO_CORE_INFO("Irradiance map generated (32x32)");
    }

    void EnvironmentMap::GeneratePrefilterMap()
    {
        OLO_PROFILE_FUNCTION();
        
        // Create prefilter map specification
        CubemapSpecification prefilterSpec;
        prefilterSpec.Width = 128;
        prefilterSpec.Height = 128;
        prefilterSpec.Format = ImageFormat::RGB32F;
        prefilterSpec.GenerateMips = true;
        
        m_PrefilterMap = TextureCubemap::Create(prefilterSpec);
        
        // Use IBLPrecompute to generate the prefiltered environment map
        if (s_ShaderLibrary)
        {
            IBLPrecompute::GeneratePrefilterMap(m_EnvironmentMap, m_PrefilterMap, *s_ShaderLibrary);
        }
        else
        {
            OLO_CORE_ERROR("EnvironmentMap: IBL system not initialized! Call InitializeIBLSystem() first.");
        }
        
        OLO_CORE_INFO("Prefilter map generated (128x128) with mipmaps");
    }

    void EnvironmentMap::GenerateBRDFLut()
    {
        OLO_PROFILE_FUNCTION();
        
        // Create BRDF LUT specification
        TextureSpecification brdfSpec;
        brdfSpec.Width = 512;
        brdfSpec.Height = 512;
        brdfSpec.Format = ImageFormat::RG32F;
        brdfSpec.GenerateMips = false;
        
        m_BRDFLutMap = Texture2D::Create(brdfSpec);
        
        // Use IBLPrecompute to generate the BRDF lookup table
        if (s_ShaderLibrary)
        {
            IBLPrecompute::GenerateBRDFLut(m_BRDFLutMap, *s_ShaderLibrary);
        }
        else
        {
            OLO_CORE_ERROR("EnvironmentMap: IBL system not initialized! Call InitializeIBLSystem() first.");
        }
        
        OLO_CORE_INFO("BRDF LUT generated (512x512)");
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
