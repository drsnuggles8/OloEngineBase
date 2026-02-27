#include "OloEnginePCH.h"
#include "OloEngine/Renderer/IBLPrecompute.h"
#include "OloEngine/Renderer/EnvironmentMap.h" // For IBLConfiguration
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/Renderer.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/ShaderLibrary.h"
#include "OloEngine/Renderer/UniformBuffer.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"

#include <stb_image/stb_image.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace OloEngine
{
    // Static member definitions
    Ref<Mesh> IBLPrecompute::s_CubeMesh = nullptr;
    Ref<Mesh> IBLPrecompute::s_QuadMesh = nullptr;

    // Helper method to update camera matrices UBO for IBL rendering
    static void UpdateIBLCameraUBO(const glm::mat4& view, const glm::mat4& projection)
    {
        // Create or get static UBO for IBL camera matrices
        static Ref<UniformBuffer> s_IBLCameraUBO = nullptr;
        if (!s_IBLCameraUBO)
        {
            s_IBLCameraUBO = UniformBuffer::Create(
                ShaderBindingLayout::CameraUBO::GetSize(),
                ShaderBindingLayout::UBO_CAMERA);
        }

        // Prepare camera data
        ShaderBindingLayout::CameraUBO cameraData;
        cameraData.ViewProjection = projection * view;
        cameraData.View = view;
        cameraData.Projection = projection;
        cameraData.Position = glm::vec3(0.0f); // IBL rendering is done from origin
        cameraData._padding0 = 0.0f;

        // Update the UBO
        s_IBLCameraUBO->SetData(&cameraData, ShaderBindingLayout::CameraUBO::GetSize());
    }

    void IBLPrecompute::GenerateIrradianceMap(const Ref<TextureCubemap>& environmentMap, const Ref<TextureCubemap>& irradianceMap, ShaderLibrary& shaderLibrary)
    {
        OLO_PROFILE_FUNCTION();
        OLO_CORE_INFO("Generating irradiance map from environment map");

        if (!shaderLibrary.Exists("IrradianceConvolution"))
        {
            OLO_CORE_ERROR("IBLPrecompute::GenerateIrradianceMap: IrradianceConvolution shader not found");
            return;
        }

        auto shader = shaderLibrary.Get("IrradianceConvolution");

        // Bind environment map
        environmentMap->Bind(ShaderBindingLayout::TEX_ENVIRONMENT);

        // Use the render to cubemap helper
        RenderToCubemap(irradianceMap, shader, GetCubeMesh());

        OLO_CORE_INFO("Irradiance map generation complete");
    }

    void IBLPrecompute::GeneratePrefilterMap(const Ref<TextureCubemap>& environmentMap, const Ref<TextureCubemap>& prefilterMap, ShaderLibrary& shaderLibrary)
    {
        OLO_PROFILE_FUNCTION();
        OLO_CORE_INFO("Generating prefiltered environment map");

        if (!shaderLibrary.Exists("IBLPrefilter"))
        {
            OLO_CORE_ERROR("IBLPrecompute::GeneratePrefilterMap: IBLPrefilter shader not found");
            return;
        }

        auto shader = shaderLibrary.Get("IBLPrefilter");

        // Bind environment map
        environmentMap->Bind(ShaderBindingLayout::TEX_ENVIRONMENT);

        // Create IBL parameters uniform buffer
        auto iblParamsUBO = UniformBuffer::Create(ShaderBindingLayout::IBLParametersUBO::GetSize(), ShaderBindingLayout::UBO_USER_0);

        // Generate each mip level with different roughness values
        const u32 maxMipLevels = 5;                             // 0 to 4
        const u32 sampleCounts[] = { 1024, 512, 256, 128, 64 }; // More samples for lower roughness

        for (u32 mip = 0; mip < maxMipLevels; ++mip)
        {
            f32 roughness = static_cast<f32>(mip) / static_cast<f32>(maxMipLevels - 1);

            // Update IBL parameters with sample count for importance sampling
            ShaderBindingLayout::IBLParametersUBO iblParams;
            iblParams.Roughness = roughness;
            iblParams.ExposureAdjustment = static_cast<f32>(sampleCounts[mip]); // Use exposure for sample count
            iblParams.IBLIntensity = 1.0f;                                      // Default IBL intensity
            iblParams.IBLRotation = 0.0f;                                       // Default rotation

            iblParamsUBO->SetData(&iblParams, sizeof(iblParams));

            shader->Bind();

            RenderToCubemap(prefilterMap, shader, GetCubeMesh(), mip);
        }

        OLO_CORE_INFO("Prefiltered environment map generation complete");
    }

    void IBLPrecompute::GenerateBRDFLut(const Ref<Texture2D>& brdfLutMap, ShaderLibrary& shaderLibrary)
    {
        OLO_PROFILE_FUNCTION();
        OLO_CORE_INFO("Generating BRDF lookup table");

        if (!shaderLibrary.Exists("BRDFLutGeneration"))
        {
            OLO_CORE_ERROR("IBLPrecompute::GenerateBRDFLut: BRDFLutGeneration shader not found");
            return;
        }

        auto shader = shaderLibrary.Get("BRDFLutGeneration");

        RenderToTexture(brdfLutMap, shader, GetQuadMesh());

        OLO_CORE_INFO("BRDF lookup table generation complete");
    }

    Ref<TextureCubemap> IBLPrecompute::ConvertEquirectangularToCubemap(const std::string& filePath, ShaderLibrary& shaderLibrary, u32 resolution)
    {
        OLO_PROFILE_FUNCTION();
        OLO_CORE_INFO("Converting equirectangular HDR to cubemap: {}", filePath);

        // Load HDR image
        stbi_set_flip_vertically_on_load(true);

        i32 width, height, channels;
        f32* data = stbi_loadf(filePath.c_str(), &width, &height, &channels, 0);

        if (!data)
        {
            OLO_CORE_ERROR("Failed to load HDR image: {}", filePath);
            return nullptr;
        }

        // Create HDR texture from loaded data
        TextureSpecification hdrSpec;
        hdrSpec.Width = width;
        hdrSpec.Height = height;
        hdrSpec.Format = channels == 3 ? ImageFormat::RGB32F : ImageFormat::RGBA32F;
        hdrSpec.GenerateMips = false;

        auto hdrTexture = Texture2D::Create(hdrSpec);
        hdrTexture->SetData(data, width * height * channels * sizeof(f32));

        stbi_image_free(data);

        // Create cubemap
        CubemapSpecification cubemapSpec;
        cubemapSpec.Width = resolution;
        cubemapSpec.Height = resolution;
        cubemapSpec.Format = ImageFormat::RGB32F;
        cubemapSpec.GenerateMips = false; // We'll render to mips manually

        auto cubemap = TextureCubemap::Create(cubemapSpec);

        // Get shader for conversion
        if (!shaderLibrary.Exists("EquirectangularToCubemap"))
        {
            OLO_CORE_ERROR("IBLPrecompute::ConvertEquirectangularToCubemap: EquirectangularToCubemap shader not found");
            return nullptr;
        }

        auto shader = shaderLibrary.Get("EquirectangularToCubemap");

        // Bind HDR texture
        hdrTexture->Bind(0);

        // Render to cubemap
        RenderToCubemap(cubemap, shader, GetCubeMesh());

        OLO_CORE_INFO("Equirectangular to cubemap conversion complete");
        return cubemap;
    }

    Ref<TextureCubemap> IBLPrecompute::CreateCubemapFromFaces(const std::vector<std::string>& facePaths)
    {
        OLO_PROFILE_FUNCTION();

        if (facePaths.size() != 6)
        {
            OLO_CORE_ERROR("IBLPrecompute::CreateCubemapFromFaces: Expected 6 face paths, got {}", facePaths.size());
            return nullptr;
        }

        return TextureCubemap::Create(facePaths);
    }

    void IBLPrecompute::RenderToCubemap(const Ref<TextureCubemap>& cubemap, const Ref<Shader>& shader,
                                        const Ref<Mesh>& cubeMesh, u32 mipLevel)
    {
        OLO_PROFILE_FUNCTION();

        // Calculate viewport size for this mip level
        u32 mipWidth = cubemap->GetWidth() >> mipLevel;
        u32 mipHeight = cubemap->GetHeight() >> mipLevel;

        // View matrices for each face
        const glm::mat4 captureViews[] = {
            glm::lookAt(glm::vec3(0.0f), glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f)),  // +X
            glm::lookAt(glm::vec3(0.0f), glm::vec3(-1.0f, 0.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f)), // -X
            glm::lookAt(glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f)),   // +Y
            glm::lookAt(glm::vec3(0.0f), glm::vec3(0.0f, -1.0f, 0.0f), glm::vec3(0.0f, 0.0f, -1.0f)), // -Y
            glm::lookAt(glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, 1.0f), glm::vec3(0.0f, -1.0f, 0.0f)),  // +Z
            glm::lookAt(glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0.0f, -1.0f, 0.0f))  // -Z
        };

        const glm::mat4 captureProjection = glm::perspective(glm::radians(90.0f), 1.0f, 0.1f, 10.0f);

        // Store previous stencil state and disable for IBL rendering
        bool wasStencilTestEnabled = RenderCommand::IsStencilTestEnabled();
        if (wasStencilTestEnabled)
            RenderCommand::DisableStencilTest();

        shader->Bind();

        // Create a framebuffer for rendering each face
        FramebufferSpecification fbSpec;
        fbSpec.Width = mipWidth;
        fbSpec.Height = mipHeight;
        fbSpec.Attachments = { FramebufferTextureFormat::RGBA32F, FramebufferTextureFormat::Depth };
        auto framebuffer = Framebuffer::Create(fbSpec);

        for (u32 i = 0; i < 6; ++i)
        {
            // Update camera matrices UBO for this face
            UpdateIBLCameraUBO(captureViews[i], captureProjection);

            // Bind framebuffer and render to it
            framebuffer->Bind();
            RenderCommand::SetViewport(0, 0, mipWidth, mipHeight);
            RenderCommand::SetClearColor({ 0.0f, 0.0f, 0.0f, 1.0f });

            // Clear framebuffer
            RenderCommand::ClearColorAndDepth();

            // Render cube to framebuffer
            auto vertexArray = cubeMesh->GetVertexArray();
            vertexArray->Bind();
            RenderCommand::DrawIndexed(vertexArray);

            // Now copy from framebuffer to cubemap face
            u32 framebufferColorTexture = framebuffer->GetColorAttachmentRendererID(0);

            RenderCommand::CopyImageSubDataFull(
                framebufferColorTexture, GL_TEXTURE_2D, 0, 0,
                cubemap->GetRendererID(), GL_TEXTURE_CUBE_MAP, 0, static_cast<i32>(i),
                mipWidth, mipHeight);
        }

        framebuffer->Unbind();

        // Restore previous stencil state
        if (wasStencilTestEnabled)
            RenderCommand::EnableStencilTest();
    }

    void IBLPrecompute::RenderToTexture(const Ref<Texture2D>& texture, const Ref<Shader>& shader,
                                        const Ref<Mesh>& quadMesh)
    {
        OLO_PROFILE_FUNCTION();

        // Create framebuffer for rendering to texture
        FramebufferSpecification fbSpec;
        fbSpec.Width = texture->GetWidth();
        fbSpec.Height = texture->GetHeight();
        fbSpec.Attachments = { FramebufferTextureFormat::RG32F, FramebufferTextureFormat::Depth };

        auto framebuffer = Framebuffer::Create(fbSpec);

        // Store previous stencil state and disable for IBL rendering
        bool wasStencilTestEnabled = RenderCommand::IsStencilTestEnabled();
        if (wasStencilTestEnabled)
            RenderCommand::DisableStencilTest();

        // Bind framebuffer and set viewport
        framebuffer->Bind();
        RenderCommand::SetViewport(0, 0, texture->GetWidth(), texture->GetHeight());
        RenderCommand::SetClearColor({ 0.0f, 0.0f, 0.0f, 1.0f });

        // Clear only color and depth buffers for IBL framebuffer
        RenderCommand::ClearColorAndDepth();

        // Render fullscreen quad
        shader->Bind();
        auto vertexArray = quadMesh->GetVertexArray();
        vertexArray->Bind();
        RenderCommand::DrawIndexed(vertexArray);

        framebuffer->Unbind();

        // Copy from framebuffer color attachment to the output texture
        RenderCommand::CopyImageSubDataFull(
            framebuffer->GetColorAttachmentRendererID(0), GL_TEXTURE_2D, 0, 0,
            texture->GetRendererID(), GL_TEXTURE_2D, 0, 0,
            texture->GetWidth(), texture->GetHeight());

        // Restore previous stencil state
        if (wasStencilTestEnabled)
            RenderCommand::EnableStencilTest();
    }

    const Ref<Mesh>& IBLPrecompute::GetCubeMesh()
    {
        if (!s_CubeMesh)
        {
            s_CubeMesh = MeshPrimitives::CreateSkyboxCube();
        }
        return s_CubeMesh;
    }

    const Ref<Mesh>& IBLPrecompute::GetQuadMesh()
    {
        if (!s_QuadMesh)
        {
            s_QuadMesh = MeshPrimitives::CreatePlane(2.0f, 2.0f); // Create a 2x2 quad for fullscreen rendering
        }
        return s_QuadMesh;
    }

    // Enhanced IBL generation methods implementation
    void IBLPrecompute::GenerateIrradianceMapAdvanced(const Ref<TextureCubemap>& environmentMap,
                                                      const Ref<TextureCubemap>& irradianceMap,
                                                      ShaderLibrary& shaderLibrary,
                                                      const IBLConfiguration& config)
    {
        OLO_PROFILE_FUNCTION();
        OLO_CORE_INFO("Generating enhanced irradiance map with {} samples", config.IrradianceSamples);

        Ref<Shader> shader = nullptr;
        if (shaderLibrary.Exists("IrradianceConvolutionAdvanced"))
        {
            shader = shaderLibrary.Get("IrradianceConvolutionAdvanced");
        }
        else if (shaderLibrary.Exists("IrradianceConvolution"))
        {
            // Fallback to standard shader if advanced version not available
            OLO_CORE_WARN("Advanced irradiance shader not found, using standard version");
            shader = shaderLibrary.Get("IrradianceConvolution");
        }
        else
        {
            OLO_CORE_ERROR("IBLPrecompute::GenerateIrradianceMapAdvanced: No irradiance shader available");
            return;
        }

        // Configure shader with enhanced settings
        shader->Bind();
        shader->SetInt("u_EnvironmentMap", 9);
        shader->SetInt("u_SampleCount", config.IrradianceSamples);

        // Set quality-based parameters
        switch (config.Quality)
        {
            case IBLQuality::Low:
                shader->SetFloat("u_QualityMultiplier", 0.5f);
                break;
            case IBLQuality::Medium:
                shader->SetFloat("u_QualityMultiplier", 1.0f);
                break;
            case IBLQuality::High:
                shader->SetFloat("u_QualityMultiplier", 2.0f);
                break;
            case IBLQuality::Ultra:
                shader->SetFloat("u_QualityMultiplier", 4.0f);
                break;
        }

        // Bind environment map
        environmentMap->Bind(ShaderBindingLayout::TEX_ENVIRONMENT);

        // Use enhanced rendering with configuration
        RenderToCubemapAdvanced(irradianceMap, shader, GetCubeMesh(), config);

        OLO_CORE_INFO("Enhanced irradiance map generation complete");
    }

    void IBLPrecompute::GeneratePrefilterMapAdvanced(const Ref<TextureCubemap>& environmentMap,
                                                     const Ref<TextureCubemap>& prefilterMap,
                                                     ShaderLibrary& shaderLibrary,
                                                     const IBLConfiguration& config)
    {
        OLO_PROFILE_FUNCTION();
        OLO_CORE_INFO("Generating enhanced prefilter map with {} samples and importance sampling: {}",
                      config.PrefilterSamples, config.UseImportanceSampling);

        Ref<Shader> shader = nullptr;
        const std::string preferredShader = config.UseImportanceSampling ? "IBLPrefilterImportance" : "IBLPrefilter";

        if (shaderLibrary.Exists(preferredShader))
        {
            shader = shaderLibrary.Get(preferredShader);
        }
        else if (shaderLibrary.Exists("IBLPrefilter"))
        {
            // Fallback to standard shader
            OLO_CORE_WARN("Advanced prefilter shader not found, using standard version");
            shader = shaderLibrary.Get("IBLPrefilter");
        }
        else
        {
            OLO_CORE_ERROR("IBLPrecompute::GeneratePrefilterMapAdvanced: No prefilter shader available");
            return;
        }

        // Bind environment map
        environmentMap->Bind(ShaderBindingLayout::TEX_ENVIRONMENT);

        // Generate mipmaps with varying roughness values and sample counts
        const u32 maxMipLevels = 5;
        for (u32 mip = 0; mip < maxMipLevels; ++mip)
        {
            float roughness = static_cast<float>(mip) / static_cast<float>(maxMipLevels - 1);

            // Calculate sample count based on quality and mip level
            u32 sampleCount = config.PrefilterSamples >> mip; // Reduce samples for higher mips
            sampleCount = std::max(sampleCount, 32u);         // Minimum sample count

            shader->Bind();
            shader->SetInt("u_EnvironmentMap", ShaderBindingLayout::TEX_ENVIRONMENT);
            shader->SetFloat("u_Roughness", roughness);
            shader->SetInt("u_SampleCount", sampleCount);
            shader->SetInt("u_UseImportanceSampling", config.UseImportanceSampling ? 1 : 0);

            // Set quality parameters
            switch (config.Quality)
            {
                case IBLQuality::Low:
                    shader->SetFloat("u_QualityMultiplier", 0.5f);
                    break;
                case IBLQuality::Medium:
                    shader->SetFloat("u_QualityMultiplier", 1.0f);
                    break;
                case IBLQuality::High:
                    shader->SetFloat("u_QualityMultiplier", 1.5f);
                    break;
                case IBLQuality::Ultra:
                    shader->SetFloat("u_QualityMultiplier", 2.0f);
                    break;
            }

            RenderToCubemapAdvanced(prefilterMap, shader, GetCubeMesh(), config, mip);
        }

        OLO_CORE_INFO("Enhanced prefilter map generation complete");
    }

    void IBLPrecompute::GenerateBRDFLutAdvanced(const Ref<Texture2D>& brdfLutMap,
                                                ShaderLibrary& shaderLibrary,
                                                const IBLConfiguration& config)
    {
        OLO_PROFILE_FUNCTION();
        OLO_CORE_INFO("Generating enhanced BRDF LUT");

        Ref<Shader> shader = nullptr;
        if (shaderLibrary.Exists("BRDFIntegrationAdvanced"))
        {
            shader = shaderLibrary.Get("BRDFIntegrationAdvanced");
        }
        else if (shaderLibrary.Exists("BRDFLutGeneration"))
        {
            // Fallback to standard shader
            OLO_CORE_WARN("Advanced BRDF LUT shader not found, using standard version");
            shader = shaderLibrary.Get("BRDFLutGeneration");
        }
        else
        {
            OLO_CORE_ERROR("IBLPrecompute::GenerateBRDFLutAdvanced: No BRDF LUT shader available");
            return;
        }

        // Configure shader with enhanced settings
        shader->Bind();

        // Set quality parameters
        switch (config.Quality)
        {
            case IBLQuality::Low:
                shader->SetInt("u_SampleCount", 256);
                break;
            case IBLQuality::Medium:
                shader->SetInt("u_SampleCount", 512);
                break;
            case IBLQuality::High:
                shader->SetInt("u_SampleCount", 1024);
                break;
            case IBLQuality::Ultra:
                shader->SetInt("u_SampleCount", 2048);
                break;
        }

        RenderToTextureAdvanced(brdfLutMap, shader, GetQuadMesh(), config);

        OLO_CORE_INFO("Enhanced BRDF LUT generation complete");
    }

    // Enhanced rendering methods
    void IBLPrecompute::RenderToCubemapAdvanced(const Ref<TextureCubemap>& cubemap, const Ref<Shader>& shader,
                                                const Ref<Mesh>& cubeMesh, [[maybe_unused]] const IBLConfiguration& config, u32 mipLevel)
    {
        // Use standard render method for now - can be enhanced with parallel rendering if needed
        RenderToCubemap(cubemap, shader, cubeMesh, mipLevel);

        // Future enhancement: implement parallel face rendering if config.EnableMultithreading is true
    }

    void IBLPrecompute::RenderToTextureAdvanced(const Ref<Texture2D>& texture, const Ref<Shader>& shader,
                                                const Ref<Mesh>& quadMesh, [[maybe_unused]] const IBLConfiguration& config)
    {
        // Use standard render method for now - can be enhanced with additional quality parameters
        RenderToTexture(texture, shader, quadMesh);

        // Future enhancement: implement additional quality-based optimizations
    }
} // namespace OloEngine
