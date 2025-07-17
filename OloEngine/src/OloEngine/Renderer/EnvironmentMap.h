#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/TextureCubemap.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Mesh.h"

namespace OloEngine
{
    // Forward declaration
    class ShaderLibrary;
    
    struct EnvironmentMapSpecification
    {
        std::string FilePath;
        u32 Resolution = 512;
        ImageFormat Format = ImageFormat::RGB32F;
        bool GenerateIBL = true;
        bool GenerateMipmaps = true;
    };

    class EnvironmentMap
    {
    public:
        EnvironmentMap(const EnvironmentMapSpecification& spec);
        ~EnvironmentMap() = default;

        // Initialize IBL system with shader library (call once at engine startup)
        static void InitializeIBLSystem(ShaderLibrary& shaderLibrary);

        // Load environment map from HDR file
        static Ref<EnvironmentMap> Create(const EnvironmentMapSpecification& spec);
        static Ref<EnvironmentMap> CreateFromCubemap(const Ref<TextureCubemap>& cubemap);
        static Ref<EnvironmentMap> CreateFromEquirectangular(const std::string& filePath);

        // Get textures
        const Ref<TextureCubemap>& GetEnvironmentMap() const { return m_EnvironmentMap; }
        const Ref<TextureCubemap>& GetIrradianceMap() const { return m_IrradianceMap; }
        const Ref<TextureCubemap>& GetPrefilterMap() const { return m_PrefilterMap; }
        const Ref<Texture2D>& GetBRDFLutMap() const { return m_BRDFLutMap; }

        // Check if IBL is available
        bool HasIBL() const { return m_IrradianceMap && m_PrefilterMap && m_BRDFLutMap; }

        // Get specification
        const EnvironmentMapSpecification& GetSpecification() const { return m_Specification; }

    private:
        void GenerateIBLTextures();
        void GenerateIrradianceMap();
        void GeneratePrefilterMap();
        void GenerateBRDFLut();

        Ref<TextureCubemap> ConvertEquirectangularToCubemap(const std::string& filePath);

    private:
        EnvironmentMapSpecification m_Specification;
        Ref<TextureCubemap> m_EnvironmentMap;
        Ref<TextureCubemap> m_IrradianceMap;
        Ref<TextureCubemap> m_PrefilterMap;
        Ref<Texture2D> m_BRDFLutMap;
        
        // Static shader library for IBL operations
        static ShaderLibrary* s_ShaderLibrary;
    };
}
