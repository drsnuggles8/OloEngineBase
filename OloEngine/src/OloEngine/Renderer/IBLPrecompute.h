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
    
    class IBLPrecompute
    {
    public:
        // Precompute IBL textures from environment map
        static void GenerateIrradianceMap(const Ref<TextureCubemap>& environmentMap, const Ref<TextureCubemap>& irradianceMap, ShaderLibrary& shaderLibrary);
        static void GeneratePrefilterMap(const Ref<TextureCubemap>& environmentMap, const Ref<TextureCubemap>& prefilterMap, ShaderLibrary& shaderLibrary);
        static void GenerateBRDFLut(const Ref<Texture2D>& brdfLutMap, ShaderLibrary& shaderLibrary);
        
        // Convert equirectangular HDR to cubemap
        static Ref<TextureCubemap> ConvertEquirectangularToCubemap(const std::string& filePath, ShaderLibrary& shaderLibrary, u32 resolution = 512);
        
        // Utility to create cubemap from 6 face images
        static Ref<TextureCubemap> CreateCubemapFromFaces(const std::vector<std::string>& facePaths);
        
    private:
        // Render to cubemap helper
        static void RenderToCubemap(const Ref<TextureCubemap>& cubemap, const Ref<Shader>& shader, 
                                   const Ref<Mesh>& cubeMesh, u32 mipLevel = 0);
        
        // Render to texture helper
        static void RenderToTexture(const Ref<Texture2D>& texture, const Ref<Shader>& shader, 
                                   const Ref<Mesh>& quadMesh);
        
        // Get cube mesh for rendering
        static const Ref<Mesh>& GetCubeMesh();
        static const Ref<Mesh>& GetQuadMesh();
        
        // Cached meshes
        static Ref<Mesh> s_CubeMesh;
        static Ref<Mesh> s_QuadMesh;
    };
}
