#include "OloEnginePCH.h"
#include "OloEngine/Renderer/IBLPrecompute.h"
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
#include <glad/gl.h>

namespace OloEngine
{
    // Static member definitions
    Ref<Mesh> IBLPrecompute::s_CubeMesh = nullptr;
    Ref<Mesh> IBLPrecompute::s_QuadMesh = nullptr;

    void IBLPrecompute::GenerateIrradianceMap(const Ref<TextureCubemap>& environmentMap, const Ref<TextureCubemap>& irradianceMap, ShaderLibrary& shaderLibrary)
    {
        OLO_PROFILE_FUNCTION();
        OLO_CORE_INFO("Generating irradiance map from environment map");

        auto shader = shaderLibrary.Get("IrradianceConvolution");
        if (!shader)
        {
            OLO_CORE_ERROR("IBLPrecompute::GenerateIrradianceMap: IrradianceConvolution shader not found");
            return;
        }

        // Bind environment map
        environmentMap->Bind(9);
        
        // Use the render to cubemap helper
        RenderToCubemap(irradianceMap, shader, GetCubeMesh());
        
        OLO_CORE_INFO("Irradiance map generation complete");
    }

    void IBLPrecompute::GeneratePrefilterMap(const Ref<TextureCubemap>& environmentMap, const Ref<TextureCubemap>& prefilterMap, ShaderLibrary& shaderLibrary)
    {
        OLO_PROFILE_FUNCTION();
        OLO_CORE_INFO("Generating prefiltered environment map");

        auto shader = shaderLibrary.Get("IBLPrefilter");
        if (!shader)
        {
            OLO_CORE_ERROR("IBLPrecompute::GeneratePrefilterMap: IBLPrefilter shader not found");
            return;
        }

        // Bind environment map
        environmentMap->Bind(9);
        
        // Create IBL parameters uniform buffer
        auto iblParamsUBO = UniformBuffer::Create(ShaderBindingLayout::IBLParametersUBO::GetSize(), ShaderBindingLayout::UBO_USER_0);
        
        // Generate each mip level with different roughness values
        const u32 maxMipLevels = 5; // 0 to 4
        for (u32 mip = 0; mip < maxMipLevels; ++mip)
        {
            f32 roughness = static_cast<f32>(mip) / static_cast<f32>(maxMipLevels - 1);
            
            // Update IBL parameters
            ShaderBindingLayout::IBLParametersUBO iblParams;
            iblParams.Roughness = roughness;
            iblParams._padding0 = 0.0f;
            iblParams._padding1 = 0.0f;
            iblParams._padding2 = 0.0f;
            
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

        auto shader = shaderLibrary.Get("BRDFLutGeneration");
        if (!shader)
        {
            OLO_CORE_ERROR("IBLPrecompute::GenerateBRDFLut: BRDFLutGeneration shader not found");
            return;
        }

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
        cubemapSpec.GenerateMips = true;
        
        auto cubemap = TextureCubemap::Create(cubemapSpec);

        // Get shader for conversion
        auto shader = shaderLibrary.Get("EquirectangularToCubemap");
        if (!shader)
        {
            OLO_CORE_ERROR("IBLPrecompute::ConvertEquirectangularToCubemap: EquirectangularToCubemap shader not found");
            return nullptr;
        }

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
        
        // Create framebuffer specification
        FramebufferSpecification fbSpec;
        fbSpec.Width = mipWidth;
        fbSpec.Height = mipHeight;
        fbSpec.Attachments = { FramebufferTextureFormat::RGBA8 };
        
        auto framebuffer = Framebuffer::Create(fbSpec);

        // View matrices for each face
        const glm::mat4 captureViews[] = {
            glm::lookAt(glm::vec3(0.0f), glm::vec3( 1.0f,  0.0f,  0.0f), glm::vec3(0.0f, -1.0f,  0.0f)), // +X
            glm::lookAt(glm::vec3(0.0f), glm::vec3(-1.0f,  0.0f,  0.0f), glm::vec3(0.0f, -1.0f,  0.0f)), // -X
            glm::lookAt(glm::vec3(0.0f), glm::vec3( 0.0f,  1.0f,  0.0f), glm::vec3(0.0f,  0.0f,  1.0f)), // +Y
            glm::lookAt(glm::vec3(0.0f), glm::vec3( 0.0f, -1.0f,  0.0f), glm::vec3(0.0f,  0.0f, -1.0f)), // -Y
            glm::lookAt(glm::vec3(0.0f), glm::vec3( 0.0f,  0.0f,  1.0f), glm::vec3(0.0f, -1.0f,  0.0f)), // +Z
            glm::lookAt(glm::vec3(0.0f), glm::vec3( 0.0f,  0.0f, -1.0f), glm::vec3(0.0f, -1.0f,  0.0f))  // -Z
        };

        const glm::mat4 captureProjection = glm::perspective(glm::radians(90.0f), 1.0f, 0.1f, 10.0f);

        shader->Bind();

        for (u32 i = 0; i < 6; ++i)
        {
            // Set view-projection matrix for this face
            glm::mat4 viewProjection = captureProjection * captureViews[i];
            shader->SetMat4("u_ViewProjection", viewProjection);
            shader->SetMat4("u_View", captureViews[i]);
            shader->SetMat4("u_Projection", captureProjection);

            // Bind framebuffer and set viewport
            framebuffer->Bind();
            RenderCommand::SetViewport(0, 0, mipWidth, mipHeight);
            RenderCommand::SetClearColor({ 0.0f, 0.0f, 0.0f, 1.0f });
            RenderCommand::Clear();

            // Render cube
            Renderer::Submit(shader, cubeMesh->GetVertexArray(), glm::mat4(1.0f));

            // Copy framebuffer to cubemap face using DSA
            framebuffer->Unbind();
            
            // Use OpenGL DSA to copy from framebuffer to cubemap face
            u32 framebufferColorTexture = framebuffer->GetColorAttachmentRendererID(0);
            
            // Copy from framebuffer color attachment to the specific cubemap face
            // GL_TEXTURE_CUBE_MAP_POSITIVE_X + i gives us the correct face
            glCopyTextureSubImage3D(
                cubemap->GetRendererID(),           // Destination cubemap texture
                mipLevel,                           // Mip level
                0, 0,                              // X and Y offsets in destination
                i,                                 // Z offset (face index)
                0, 0,                              // X and Y offsets in source
                static_cast<GLsizei>(mipWidth),    // Width
                static_cast<GLsizei>(mipHeight)    // Height
            );
        }
    }

    void IBLPrecompute::RenderToTexture(const Ref<Texture2D>& texture, const Ref<Shader>& shader, 
                                       const Ref<Mesh>& quadMesh)
    {
        OLO_PROFILE_FUNCTION();

        // Create framebuffer for rendering to texture
        FramebufferSpecification fbSpec;
        fbSpec.Width = texture->GetWidth();
        fbSpec.Height = texture->GetHeight();
        fbSpec.Attachments = { FramebufferTextureFormat::RGBA8 };
        
        auto framebuffer = Framebuffer::Create(fbSpec);

        // Bind framebuffer and set viewport
        framebuffer->Bind();
        RenderCommand::SetViewport(0, 0, texture->GetWidth(), texture->GetHeight());
        RenderCommand::SetClearColor({ 0.0f, 0.0f, 0.0f, 1.0f });
        RenderCommand::Clear();

        // Render fullscreen quad
        shader->Bind();
        Renderer::Submit(shader, quadMesh->GetVertexArray(), glm::mat4(1.0f));

        framebuffer->Unbind();
        
        // Copy framebuffer to texture using DSA
        u32 framebufferColorTexture = framebuffer->GetColorAttachmentRendererID(0);
        
        // Copy from framebuffer color attachment to the texture
        glCopyTextureSubImage2D(
            texture->GetRendererID(),              // Destination texture
            0,                                     // Mip level
            0, 0,                                  // X and Y offsets in destination
            0, 0,                                  // X and Y offsets in source
            static_cast<GLsizei>(texture->GetWidth()),  // Width
            static_cast<GLsizei>(texture->GetHeight())  // Height
        );
    }

    const Ref<Mesh>& IBLPrecompute::GetCubeMesh()
    {
        if (!s_CubeMesh)
        {
            s_CubeMesh = Mesh::CreateSkyboxCube();
        }
        return s_CubeMesh;
    }

    const Ref<Mesh>& IBLPrecompute::GetQuadMesh()
    {
        if (!s_QuadMesh)
        {
            s_QuadMesh = Mesh::CreatePlane(2.0f, 2.0f); // Create a 2x2 quad for fullscreen rendering
        }
        return s_QuadMesh;
    }
}
