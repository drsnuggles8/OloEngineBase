#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/TextureCubemap.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Mesh.h"

namespace OloEngine
{
    class ShaderLibrary;
    
    // Forward declaration for enhanced IBL configuration
    struct IBLConfiguration;
    
    class IBLPrecompute
    {
    public:
        // Standard IBL generation methods (backward compatibility)
        static void GenerateIrradianceMap(const Ref<TextureCubemap>& environmentMap, const Ref<TextureCubemap>& irradianceMap, ShaderLibrary& shaderLibrary);
        static void GeneratePrefilterMap(const Ref<TextureCubemap>& environmentMap, const Ref<TextureCubemap>& prefilterMap, ShaderLibrary& shaderLibrary);
        static void GenerateBRDFLut(const Ref<Texture2D>& brdfLutMap, ShaderLibrary& shaderLibrary);
        
        // Enhanced IBL generation methods with configurable quality
        static void GenerateIrradianceMapAdvanced(const Ref<TextureCubemap>& environmentMap, 
                                                 const Ref<TextureCubemap>& irradianceMap, 
                                                 ShaderLibrary& shaderLibrary, 
                                                 const IBLConfiguration& config);
        static void GeneratePrefilterMapAdvanced(const Ref<TextureCubemap>& environmentMap, 
                                                const Ref<TextureCubemap>& prefilterMap, 
                                                ShaderLibrary& shaderLibrary, 
                                                const IBLConfiguration& config);
        static void GenerateBRDFLutAdvanced(const Ref<Texture2D>& brdfLutMap, 
                                          ShaderLibrary& shaderLibrary, 
                                          const IBLConfiguration& config);
        
        // Convert equirectangular HDR to cubemap
        static Ref<TextureCubemap> ConvertEquirectangularToCubemap(const std::string& filePath, ShaderLibrary& shaderLibrary, u32 resolution = 512);
        
        // Utility to create cubemap from 6 face images
        static Ref<TextureCubemap> CreateCubemapFromFaces(const std::vector<std::string>& facePaths);
        
    private:
        // Enhanced rendering methods
        static void RenderToCubemapAdvanced(const Ref<TextureCubemap>& cubemap, const AssetRef<Shader>& shader, 
                                          const AssetRef<Mesh>& cubeMesh, const IBLConfiguration& config, u32 mipLevel = 0);
        
        // Render to cubemap helper
        static void RenderToCubemap(const Ref<TextureCubemap>& cubemap, const AssetRef<Shader>& shader, 
                                   const AssetRef<Mesh>& cubeMesh, u32 mipLevel = 0);
        
        // Render to texture helper
        static void RenderToTexture(const Ref<Texture2D>& texture, const AssetRef<Shader>& shader, 
                                   const AssetRef<Mesh>& quadMesh);
        
        // Render to texture with advanced configuration
        static void RenderToTextureAdvanced(const Ref<Texture2D>& texture, const AssetRef<Shader>& shader, 
                                          const AssetRef<Mesh>& quadMesh, const IBLConfiguration& config);
        
        // Get cube mesh for rendering
        static const AssetRef<Mesh>& GetCubeMesh();
        static const AssetRef<Mesh>& GetQuadMesh();
        
        // Cached meshes
        static AssetRef<Mesh> s_CubeMesh;
        static AssetRef<Mesh> s_QuadMesh;
    };
}
