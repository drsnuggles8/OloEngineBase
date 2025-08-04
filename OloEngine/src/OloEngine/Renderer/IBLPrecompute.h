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
        static void GenerateIrradianceMap(const AssetRef<TextureCubemap>& environmentMap, const AssetRef<TextureCubemap>& irradianceMap, ShaderLibrary& shaderLibrary);
        static void GeneratePrefilterMap(const AssetRef<TextureCubemap>& environmentMap, const AssetRef<TextureCubemap>& prefilterMap, ShaderLibrary& shaderLibrary);
        static void GenerateBRDFLut(const AssetRef<Texture2D>& brdfLutMap, ShaderLibrary& shaderLibrary);
        
        // Enhanced IBL generation methods with configurable quality
        static void GenerateIrradianceMapAdvanced(const AssetRef<TextureCubemap>& environmentMap, 
                                                 const AssetRef<TextureCubemap>& irradianceMap, 
                                                 ShaderLibrary& shaderLibrary, 
                                                 const IBLConfiguration& config);
        static void GeneratePrefilterMapAdvanced(const AssetRef<TextureCubemap>& environmentMap, 
                                                const AssetRef<TextureCubemap>& prefilterMap, 
                                                ShaderLibrary& shaderLibrary, 
                                                const IBLConfiguration& config);
        static void GenerateBRDFLutAdvanced(const AssetRef<Texture2D>& brdfLutMap, 
                                          ShaderLibrary& shaderLibrary, 
                                          const IBLConfiguration& config);
        
        // Convert equirectangular HDR to cubemap
        static AssetRef<TextureCubemap> ConvertEquirectangularToCubemap(const std::string& filePath, ShaderLibrary& shaderLibrary, u32 resolution = 512);
        
        // Utility to create cubemap from 6 face images
        static AssetRef<TextureCubemap> CreateCubemapFromFaces(const std::vector<std::string>& facePaths);
        
    private:
        // Enhanced rendering methods
        static void RenderToCubemapAdvanced(const AssetRef<TextureCubemap>& cubemap, const AssetRef<Shader>& shader, 
                                          const AssetRef<Mesh>& cubeMesh, const IBLConfiguration& config, u32 mipLevel = 0);
        
        // Render to cubemap helper
        static void RenderToCubemap(const AssetRef<TextureCubemap>& cubemap, const AssetRef<Shader>& shader, 
                                   const AssetRef<Mesh>& cubeMesh, u32 mipLevel = 0);
        
        // Render to texture helper
        static void RenderToTexture(const AssetRef<Texture2D>& texture, const AssetRef<Shader>& shader, 
                                   const AssetRef<Mesh>& quadMesh);
        
        // Render to texture with advanced configuration
        static void RenderToTextureAdvanced(const AssetRef<Texture2D>& texture, const AssetRef<Shader>& shader, 
                                          const AssetRef<Mesh>& quadMesh, const IBLConfiguration& config);
        
        // Get cube mesh for rendering
        static const AssetRef<Mesh>& GetCubeMesh();
        static const AssetRef<Mesh>& GetQuadMesh();
        
        // Cached meshes
        static AssetRef<Mesh> s_CubeMesh;
        static AssetRef<Mesh> s_QuadMesh;
    };
}
