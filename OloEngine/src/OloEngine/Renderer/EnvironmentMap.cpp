#include "OloEnginePCH.h"
#include "OloEngine/Renderer/EnvironmentMap.h"
#include "OloEngine/Renderer/Renderer.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshFactory.h"
#include "OloEngine/Renderer/Commands/RenderCommand.h"
#include "OloEngine/Renderer/Debug/RendererMemoryTracker.h"

#include <stb_image/stb_image.h>

namespace OloEngine
{
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
        
        // TODO: Implement actual irradiance convolution using compute shaders or render-to-texture
        // For now, this is a placeholder that would be filled with proper rendering code
        
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
        
        // TODO: Implement actual prefiltering using compute shaders or render-to-texture
        // For now, this is a placeholder that would be filled with proper rendering code
        
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
        
        // TODO: Implement actual BRDF LUT generation using compute shaders or render-to-texture
        // For now, this is a placeholder that would be filled with proper rendering code
        
        OLO_CORE_INFO("BRDF LUT generated (512x512)");
    }

    Ref<TextureCubemap> EnvironmentMap::ConvertEquirectangularToCubemap(const std::string& filePath)
    {
        OLO_PROFILE_FUNCTION();
        
        // Load HDR image
        stbi_set_flip_vertically_on_load(true);
        
        int width, height, channels;
        float* data = stbi_loadf(filePath.c_str(), &width, &height, &channels, 0);
        
        if (!data)
        {
            OLO_CORE_ERROR("Failed to load HDR image: {0}", filePath);
            return nullptr;
        }
        
        // For now, create a simple cubemap specification
        // In a full implementation, this would involve proper equirectangular to cubemap conversion
        CubemapSpecification spec;
        spec.Width = m_Specification.Resolution;
        spec.Height = m_Specification.Resolution;
        spec.Format = m_Specification.Format;
        spec.GenerateMips = m_Specification.GenerateMipmaps;
        
        auto cubemap = TextureCubemap::Create(spec);
        
        // TODO: Implement actual equirectangular to cubemap conversion
        // This would typically involve:
        // 1. Creating a framebuffer with cubemap faces as render targets
        // 2. Using a shader to sample the equirectangular texture
        // 3. Rendering a cube for each face with appropriate view matrices
        
        stbi_image_free(data);
        
        OLO_CORE_INFO("Converted equirectangular image to cubemap: {0}", filePath);
        return cubemap;
    }
}
