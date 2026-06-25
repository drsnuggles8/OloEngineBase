#include "OloEnginePCH.h"
#include "OloEngine/Renderer/IBLPrecompute.h"
#include "OloEngine/Renderer/EnvironmentMap.h" // For IBLConfiguration
#include "OloEngine/Renderer/LightProbeBaker.h"
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
#include <glad/gl.h>

#include <chrono>
#include <cstring>

namespace OloEngine
{
    // Static member definitions
    Ref<Mesh> IBLPrecompute::s_CubeMesh = nullptr;

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

        // Scene rendering may have bound a different buffer to UBO_CAMERA since
        // the one-time creation above; SetData (glNamedBufferSubData) only
        // updates the buffer contents without touching the binding point.
        s_IBLCameraUBO->SetData(&cameraData, ShaderBindingLayout::CameraUBO::GetSize());
        s_IBLCameraUBO->Bind();
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

        RenderToTexture(brdfLutMap, shader);

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
        stbi_set_flip_vertically_on_load(false); // reset global flag to avoid polluting later stbi calls

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

        // Create cubemap. Allocate a full mip chain: the advanced specular/diffuse
        // IBL passes read this map at coarser mips (chosen per importance sample)
        // to suppress fireflies from bright HDR pixels. The chain is populated from
        // mip 0 via GenerateMipmaps() once the faces are rendered below.
        CubemapSpecification cubemapSpec;
        cubemapSpec.Width = resolution;
        cubemapSpec.Height = resolution;
        cubemapSpec.Format = ImageFormat::RGBA32F;
        cubemapSpec.GenerateMips = true;

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

        // Render to cubemap (fills mip 0 of each face), then build the mip chain
        // for the advanced IBL passes' mip-biased sampling.
        RenderToCubemap(cubemap, shader, GetCubeMesh());
        cubemap->GenerateMipmaps();

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

    // The cubemap bake is intentionally a serial, one-pass-per-face render.
    //
    // Why not multithread it: the engine renders on a single OpenGL 4.6
    // context, so GL commands for the six faces cannot be submitted from
    // multiple threads. A former IBLConfiguration::EnableMultithreading flag
    // promised exactly that and was a permanent no-op (no code ever read it);
    // it has been removed rather than left to mislead.
    //
    // Why not single-pass layered rendering: the genuine single-context win
    // would be drawing all six faces in one pass via a geometry shader writing
    // gl_Layer into a whole-cubemap attachment. But the shaderc->SPIR-V shader
    // pipeline here only supports vertex/fragment/tess stages (there is no
    // geometry stage anywhere in the project), and the bake is fragment-bound
    // (Monte-Carlo importance sampling, up to 2048 samples per texel) — layered
    // rendering would cut only the per-face CPU draw/FBO overhead, not the
    // dominant per-texel sampling cost. The bake is also a cold path: it runs
    // once per unique (environment, IBLConfiguration) and is then served from
    // IBLCache on disk. So the serial per-face loop below is by design.
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
                framebufferColorTexture, RendererAPI::TextureTargetType::Texture2D, 0, 0,
                cubemap->GetRendererID(), RendererAPI::TextureTargetType::TextureCubeMap, static_cast<i32>(mipLevel), static_cast<i32>(i),
                mipWidth, mipHeight);
        }

        framebuffer->Unbind();

        // Restore previous stencil state
        if (wasStencilTestEnabled)
            RenderCommand::EnableStencilTest();
    }

    void IBLPrecompute::RenderToTexture(const Ref<Texture2D>& texture, const Ref<Shader>& shader)
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

        // Render a fullscreen triangle. The old path used CreatePlane(), which
        // lives in the XZ plane and is degenerate in clip-space for this shader;
        // the BRDF LUT stayed black, making fully metallic PBR materials black.
        shader->Bind();
        auto vertexArray = MeshPrimitives::GetFullscreenTriangle();
        vertexArray->Bind();
        RenderCommand::DrawIndexed(vertexArray);

        framebuffer->Unbind();

        // Copy from framebuffer color attachment to the output texture
        RenderCommand::CopyImageSubDataFull(
            framebuffer->GetColorAttachmentRendererID(0), RendererAPI::TextureTargetType::Texture2D, 0, 0,
            texture->GetRendererID(), RendererAPI::TextureTargetType::Texture2D, 0, 0,
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

    namespace
    {
        // Force a GPU sync so the elapsed-time log lines actually reflect the
        // generator's GPU cost rather than just CPU-side command submission.
        // Both paths funnel through here so the numbers are apples-to-apples.
        f64 MeasureMillisecondsWithGPUSync(auto&& work)
        {
            const auto start = std::chrono::steady_clock::now();
            std::forward<decltype(work)>(work)();
            // Sync — flushes the GL command queue so the elapsed time covers
            // the actual rasterisation work, not just submission.
            ::glFinish();
            const auto end = std::chrono::steady_clock::now();
            return std::chrono::duration<f64, std::milli>(end - start).count();
        }
    } // namespace

    // Enhanced IBL generation methods implementation
    void IBLPrecompute::GenerateIrradianceMapAdvanced(const Ref<TextureCubemap>& environmentMap,
                                                      const Ref<TextureCubemap>& irradianceMap,
                                                      ShaderLibrary& shaderLibrary,
                                                      const IBLConfiguration& config)
    {
        OLO_PROFILE_FUNCTION();
        OLO_CORE_INFO("Generating enhanced irradiance map with {} samples", config.IrradianceSamples);

        const bool advancedAvailable = shaderLibrary.Exists("IrradianceConvolutionAdvanced");
        Ref<Shader> shader = nullptr;
        if (advancedAvailable)
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

        // The advanced shader reads bright source texels at coarser mips to cut
        // Monte-Carlo noise — refresh the source mip chain so that bias has valid
        // data. A no-op for single-level (e.g. procedural) environment maps.
        environmentMap->GenerateMipmaps();

        // The advanced shader is parameterised through the IBLAdvancedParams UBO
        // (binding 7). The strict Vulkan-SPIR-V pipeline rejects loose default-
        // block uniforms, so the parameters cannot be set via SetInt/SetFloat.
        // The legacy IrradianceConvolution fallback ignores the UBO entirely.
        shader->Bind();
        if (advancedAvailable)
        {
            const auto qualityMultiplier = [&]() -> f32
            {
                switch (config.Quality)
                {
                    case IBLQuality::Low:
                        return 0.5f;
                    case IBLQuality::Medium:
                        return 1.0f;
                    case IBLQuality::High:
                        return 2.0f;
                    case IBLQuality::Ultra:
                        return 4.0f;
                    default:
                        OLO_CORE_WARN("IBLPrecompute::GenerateIrradianceMapAdvanced: Unhandled IBLQuality value, defaulting to Medium");
                        return 1.0f;
                }
            }();

            ShaderBindingLayout::IBLAdvancedParamsUBO params{};
            params.Roughness = 0.0f; // unused by the irradiance path
            params.QualityMultiplier = qualityMultiplier;
            params.SampleCount = static_cast<i32>(config.IrradianceSamples);
            params.UseImportanceSampling = config.UseImportanceSampling ? 1 : 0;
            params.SourceResolution = static_cast<i32>(environmentMap->GetWidth());

            auto paramsUBO = UniformBuffer::Create(ShaderBindingLayout::IBLAdvancedParamsUBO::GetSize(), ShaderBindingLayout::UBO_USER_0);
            paramsUBO->SetData(&params, ShaderBindingLayout::IBLAdvancedParamsUBO::GetSize());
            paramsUBO->Bind();
        }

        // Bind environment map
        environmentMap->Bind(ShaderBindingLayout::TEX_ENVIRONMENT);

        const f64 elapsedMs = MeasureMillisecondsWithGPUSync([&irradianceMap, &shader]()
                                                             {
            // Single-context, single-pass-per-face bake (see RenderToCubemap).
            RenderToCubemap(irradianceMap, shader, GetCubeMesh()); });

        OLO_CORE_INFO("Enhanced irradiance map generation complete ({:.2f} ms, Monte-Carlo, {} samples)",
                      elapsedMs, config.IrradianceSamples);
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
        bool usingAdvancedShader = false;
        if (const std::string preferredShader = config.UseImportanceSampling ? "IBLPrefilterImportance" : "IBLPrefilter"; shaderLibrary.Exists(preferredShader))
        {
            shader = shaderLibrary.Get(preferredShader);
            usingAdvancedShader = (preferredShader == "IBLPrefilterImportance");
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

        // Each importance sample reads a source mip chosen from its solid angle;
        // refresh the chain so that lookup has valid data (no-op for single-level
        // sources such as the procedural sky cubemap).
        environmentMap->GenerateMipmaps();

        // Bind environment map
        environmentMap->Bind(ShaderBindingLayout::TEX_ENVIRONMENT);

        // The advanced importance shader is driven by IBLAdvancedParams; the
        // legacy IBLPrefilter fallback by IBLParametersUBO. Both live at
        // UBO_USER_0 (binding 7) — only one prefilter shader is bound at a time.
        const f32 qualityMultiplier = [&]() -> f32
        {
            switch (config.Quality)
            {
                case IBLQuality::Low:
                    return 0.5f;
                case IBLQuality::Medium:
                    return 1.0f;
                case IBLQuality::High:
                    return 1.5f;
                case IBLQuality::Ultra:
                    return 2.0f;
                default:
                    OLO_CORE_WARN("IBLPrecompute::GeneratePrefilterMapAdvanced: Unhandled IBLQuality value, defaulting to Medium");
                    return 1.0f;
            }
        }();
        const i32 sourceResolution = static_cast<i32>(environmentMap->GetWidth());

        Ref<UniformBuffer> paramsUBO = usingAdvancedShader
                                           ? UniformBuffer::Create(ShaderBindingLayout::IBLAdvancedParamsUBO::GetSize(), ShaderBindingLayout::UBO_USER_0)
                                           : UniformBuffer::Create(ShaderBindingLayout::IBLParametersUBO::GetSize(), ShaderBindingLayout::UBO_USER_0);

        // Generate each roughness mip with its own sample budget.
        const u32 maxMipLevels = 5;
        for (u32 mip = 0; mip < maxMipLevels; ++mip)
        {
            const f32 roughness = static_cast<f32>(mip) / static_cast<f32>(maxMipLevels - 1);

            // Halve the sample budget per mip — higher roughness lobes are
            // smoother and need fewer samples to resolve.
            const u32 sampleCount = std::max(config.PrefilterSamples >> mip, 32u);

            shader->Bind();
            if (usingAdvancedShader)
            {
                ShaderBindingLayout::IBLAdvancedParamsUBO params{};
                params.Roughness = roughness;
                params.QualityMultiplier = qualityMultiplier;
                params.SampleCount = static_cast<i32>(sampleCount);
                params.UseImportanceSampling = 1;
                params.SourceResolution = sourceResolution;
                paramsUBO->SetData(&params, ShaderBindingLayout::IBLAdvancedParamsUBO::GetSize());
            }
            else
            {
                // Legacy IBLPrefilter layout: sample count is smuggled through
                // ExposureAdjustment to match GeneratePrefilterMap().
                ShaderBindingLayout::IBLParametersUBO params{};
                params.Roughness = roughness;
                params.ExposureAdjustment = static_cast<f32>(sampleCount);
                params.IBLIntensity = 1.0f;
                params.IBLRotation = 0.0f;
                paramsUBO->SetData(&params, ShaderBindingLayout::IBLParametersUBO::GetSize());
            }
            paramsUBO->Bind();

            RenderToCubemap(prefilterMap, shader, GetCubeMesh(), mip);
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
        bool usingAdvancedShader = false;
        if (shaderLibrary.Exists("BRDFIntegrationAdvanced"))
        {
            shader = shaderLibrary.Get("BRDFIntegrationAdvanced");
            usingAdvancedShader = true;
        }
        else if (shaderLibrary.Exists("BRDFLutGeneration"))
        {
            // Fallback to standard shader (fixed 1024 samples, no parameters).
            OLO_CORE_WARN("Advanced BRDF LUT shader not found, using standard version");
            shader = shaderLibrary.Get("BRDFLutGeneration");
        }
        else
        {
            OLO_CORE_ERROR("IBLPrecompute::GenerateBRDFLutAdvanced: No BRDF LUT shader available");
            return;
        }

        shader->Bind();

        // Drive the advanced integrator's sample count through IBLAdvancedParams
        // (binding 7). The legacy BRDFLutGeneration fallback hard-codes its count
        // and ignores the UBO entirely.
        if (usingAdvancedShader)
        {
            const i32 sampleCount = [&]() -> i32
            {
                switch (config.Quality)
                {
                    case IBLQuality::Low:
                        return 256;
                    case IBLQuality::Medium:
                        return 512;
                    case IBLQuality::High:
                        return 1024;
                    case IBLQuality::Ultra:
                        return 2048;
                    default:
                        OLO_CORE_WARN("IBLPrecompute::GenerateBRDFLutAdvanced: Unhandled IBLQuality value, defaulting to Medium");
                        return 512;
                }
            }();

            ShaderBindingLayout::IBLAdvancedParamsUBO params{};
            params.Roughness = 0.0f;         // unused by the BRDF path (swept via UV)
            params.QualityMultiplier = 1.0f; // sample count is already final here
            params.SampleCount = sampleCount;
            params.UseImportanceSampling = 1;
            params.SourceResolution = 1; // unused by the BRDF path

            auto paramsUBO = UniformBuffer::Create(ShaderBindingLayout::IBLAdvancedParamsUBO::GetSize(), ShaderBindingLayout::UBO_USER_0);
            paramsUBO->SetData(&params, ShaderBindingLayout::IBLAdvancedParamsUBO::GetSize());
            paramsUBO->Bind();
        }

        // config.Quality already selected the integrator's sample budget above
        // (256/512/1024/2048). The BRDF LUT is view-independent and cached on
        // disk, and the split-sum integral converges well at these counts, so
        // no extra multi-pass accumulation / higher-precision intermediate is
        // pursued — the straight render is the production path.
        RenderToTexture(brdfLutMap, shader);

        OLO_CORE_INFO("Enhanced BRDF LUT generation complete");
    }

    namespace
    {
        // Convert one row of raw bytes into vec3 pixels based on the cubemap
        // format. The IBL pipeline normalises every loaded HDR to RGBA32F via
        // EnvironmentMap::ConvertEquirectangularToCubemap, so RGBA32F is the
        // expected hot path; the other branches cover hand-authored cubemaps
        // that bypass the HDR conversion (face-list constructors etc.).
        bool DecodeCubemapBytesToVec3(const std::vector<u8>& bytes,
                                      ImageFormat format,
                                      u32 pixelCount,
                                      glm::vec3* outPixels)
        {
            switch (format)
            {
                case ImageFormat::RGBA32F:
                {
                    if (bytes.size() < pixelCount * 16)
                        return false;
                    // Use memcpy rather than `reinterpret_cast<const f32*>` to
                    // avoid the strict-aliasing UB of reading floats through
                    // an unrelated pointer type — the alignment of the byte
                    // buffer is not guaranteed to match `f32`, and the
                    // standard does not let us punning-read through a
                    // mismatched type.
                    for (u32 i = 0; i < pixelCount; ++i)
                    {
                        f32 rgb[3];
                        std::memcpy(rgb, bytes.data() + i * 16, sizeof(rgb));
                        outPixels[i] = glm::vec3(rgb[0], rgb[1], rgb[2]);
                    }
                    return true;
                }
                case ImageFormat::RGB32F:
                {
                    if (bytes.size() < pixelCount * 12)
                        return false;
                    for (u32 i = 0; i < pixelCount; ++i)
                    {
                        f32 rgb[3];
                        std::memcpy(rgb, bytes.data() + i * 12, sizeof(rgb));
                        outPixels[i] = glm::vec3(rgb[0], rgb[1], rgb[2]);
                    }
                    return true;
                }
                case ImageFormat::RGBA8:
                {
                    if (bytes.size() < pixelCount * 4)
                        return false;
                    constexpr f32 inv255 = 1.0f / 255.0f;
                    for (u32 i = 0; i < pixelCount; ++i)
                    {
                        outPixels[i] = glm::vec3(bytes[i * 4 + 0], bytes[i * 4 + 1], bytes[i * 4 + 2]) * inv255;
                    }
                    return true;
                }
                case ImageFormat::RGB8:
                {
                    if (bytes.size() < pixelCount * 3)
                        return false;
                    constexpr f32 inv255 = 1.0f / 255.0f;
                    for (u32 i = 0; i < pixelCount; ++i)
                    {
                        outPixels[i] = glm::vec3(bytes[i * 3 + 0], bytes[i * 3 + 1], bytes[i * 3 + 2]) * inv255;
                    }
                    return true;
                }
                default:
                    OLO_CORE_ERROR("DecodeCubemapBytesToVec3: unsupported cubemap format ({})", static_cast<u32>(format));
                    return false;
            }
        }
    } // anonymous namespace

    SHCoefficients IBLPrecompute::ProjectCubemapToSH(const Ref<TextureCubemap>& environmentMap)
    {
        OLO_PROFILE_FUNCTION();

        SHCoefficients zero;
        zero.Zero();

        if (!environmentMap)
        {
            OLO_CORE_ERROR("IBLPrecompute::ProjectCubemapToSH: null environment map");
            return zero;
        }

        const u32 width = environmentMap->GetWidth();
        const u32 height = environmentMap->GetHeight();
        if (width == 0 || width != height)
        {
            OLO_CORE_ERROR("IBLPrecompute::ProjectCubemapToSH: cubemap must be square (got {}x{})", width, height);
            return zero;
        }

        const auto& spec = environmentMap->GetCubemapSpecification();
        const u32 faceTexels = width * height;

        std::vector<glm::vec3> pixels;
        pixels.resize(static_cast<sizet>(6) * faceTexels);

        std::vector<u8> faceBytes;
        for (u32 face = 0; face < 6; ++face)
        {
            faceBytes.clear();
            if (!environmentMap->GetFaceData(face, faceBytes, /*mipLevel=*/0))
            {
                OLO_CORE_ERROR("IBLPrecompute::ProjectCubemapToSH: GetFaceData failed for face {}", face);
                return zero;
            }

            glm::vec3* faceDst = pixels.data() + static_cast<sizet>(face) * faceTexels;
            if (!DecodeCubemapBytesToVec3(faceBytes, spec.Format, faceTexels, faceDst))
            {
                OLO_CORE_ERROR("IBLPrecompute::ProjectCubemapToSH: byte decode failed for face {}", face);
                return zero;
            }
        }

        return LightProbeBaker::ProjectToSH(pixels, width);
    }

    SHCoefficients IBLPrecompute::GenerateIrradianceMapFromSH(const Ref<TextureCubemap>& environmentMap,
                                                              const Ref<TextureCubemap>& irradianceMap,
                                                              ShaderLibrary& shaderLibrary,
                                                              [[maybe_unused]] const IBLConfiguration& config)
    {
        OLO_PROFILE_FUNCTION();

        SHCoefficients zero;
        zero.Zero();

        if (!environmentMap || !irradianceMap)
        {
            OLO_CORE_ERROR("IBLPrecompute::GenerateIrradianceMapFromSH: null inputs");
            return zero;
        }

        if (!shaderLibrary.Exists("IrradianceFromSH"))
        {
            OLO_CORE_ERROR("IBLPrecompute::GenerateIrradianceMapFromSH: IrradianceFromSH shader not found");
            return zero;
        }

        OLO_CORE_INFO("Generating irradiance map via L2 SH projection (no convolution)");

        const auto pathStart = std::chrono::steady_clock::now();

        // 1. Project the source cubemap onto the L2 SH basis. The result is
        //    radiance SH; we will convert to irradiance SH next.
        SHCoefficients radianceSH = ProjectCubemapToSH(environmentMap);

        // 2. Apply Ramamoorthi-Hanrahan per-band cosine-lobe scaling, divided
        //    by π to match the convention IrradianceConvolution.glsl outputs.
        //    The Ramamoorthi-Hanrahan analytic cosine-lobe constants are
        //    (π, 2π/3, π/4) — those produce the *raw* Lambertian irradiance
        //    integral E(n) = ∫ L(ω) max(N·ω, 0) dω, which for uniform-white
        //    L=1 evaluates to π. But the production convolution shader divides
        //    by π so the stored cubemap reads as 1.0 for uniform-white input
        //    (see `PbrIrradianceTest.UniformWhiteYieldsNormalisedUnity`); the
        //    PBR pipeline then does `diffuse = irradiance * albedo` *without*
        //    re-dividing by π. To stay bit-compatible with that convention
        //    every coefficient is divided by π here:
        //       a_0 = 1            (was π)
        //       a_1 = 2/3          (was 2π/3)
        //       a_2 = 1/4          (was π/4)
        //    Output for uniform-white input is now 1.0, identical to the
        //    convolution path. A previous version of this code used the raw
        //    constants and over-brightened SH-mode by π× — visible as a
        //    ~3x exposure bump on every diffuse surface.
        constexpr f32 kCosineLobeA0 = 1.0f;
        constexpr f32 kCosineLobeA1 = 2.0f / 3.0f;
        constexpr f32 kCosineLobeA2 = 1.0f / 4.0f;

        SHCoefficients irradianceSH = radianceSH;
        irradianceSH.Coefficients[0] *= kCosineLobeA0;
        for (u32 i = 1; i <= 3; ++i)
            irradianceSH.Coefficients[i] *= kCosineLobeA1;
        for (u32 i = 4; i <= 8; ++i)
            irradianceSH.Coefficients[i] *= kCosineLobeA2;

        // 3. Upload to the shader's UBO. The GenerateIrradianceMapFromSH path
        //    runs at most once per environment-map regen, so a fresh ad-hoc
        //    UBO (released when the local Ref drops) is fine — no need to
        //    cache it across calls.
        UBOStructures::SHCoefficientsUBO uboData;
        for (u32 i = 0; i < SH_COEFFICIENT_COUNT; ++i)
        {
            uboData.Coefficients[i] = glm::vec4(irradianceSH.Coefficients[i], 0.0f);
        }
        uboData.Coefficients[0].w = 1.0f; // validity flag
        auto shUBO = UniformBuffer::Create(UBOStructures::SHCoefficientsUBO::GetSize(),
                                           ShaderBindingLayout::UBO_SH_COEFFICIENTS);
        shUBO->SetData(&uboData, UBOStructures::SHCoefficientsUBO::GetSize());
        shUBO->Bind();

        // 4. Rasterise the irradiance cubemap with the SH-eval shader. Output
        //    layout matches the convolution path (per-face render -> copy to
        //    cubemap face), so callers consume the result identically.
        auto shader = shaderLibrary.Get("IrradianceFromSH");
        RenderToCubemap(irradianceMap, shader, GetCubeMesh());

        // Sync so the elapsed-time measurement covers GPU work, not just
        // command submission — matches MeasureMillisecondsWithGPUSync above.
        ::glFinish();
        const auto pathEnd = std::chrono::steady_clock::now();
        const f64 elapsedMs = std::chrono::duration<f64, std::milli>(pathEnd - pathStart).count();
        OLO_CORE_INFO("SH-based irradiance map generation complete ({:.2f} ms, L2 SH, 9 coefficients)", elapsedMs);
        return irradianceSH;
    }
} // namespace OloEngine
