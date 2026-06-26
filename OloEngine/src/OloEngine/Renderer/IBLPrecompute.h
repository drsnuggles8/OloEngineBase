#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/SphericalHarmonics.h"
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

        // Spherical-harmonics path for the IBL diffuse irradiance map.
        //
        // Reads back the source cubemap on the CPU, projects it to a 9-coefficient
        // L2 SH basis (Ramamoorthi-Hanrahan "An Efficient Representation for
        // Irradiance Environment Maps"), applies the analytic cosine-lobe band
        // scaling to convert radiance SH into irradiance SH, uploads the
        // coefficients to UBO_SH_COEFFICIENTS, and rasterises the output cubemap
        // via the IrradianceFromSH shader — bit-compatible output layout with
        // the Monte-Carlo convolution path so no PBR shader changes are needed.
        //
        // Returns the irradiance-scaled SH so callers (tests) can validate.
        // Returns a zeroed SHCoefficients on failure.
        static SHCoefficients GenerateIrradianceMapFromSH(const Ref<TextureCubemap>& environmentMap,
                                                          const Ref<TextureCubemap>& irradianceMap,
                                                          ShaderLibrary& shaderLibrary,
                                                          const IBLConfiguration& config);

        // Helper: read back a cubemap's mip-0 face data and project to L2 SH.
        // Returned coefficients are *radiance* SH (no cosine-lobe scaling) so
        // they match LightProbeBaker::ProjectToSH semantics. Callers that need
        // irradiance must apply Ramamoorthi's per-band constants themselves.
        // Exposed here (rather than as a private detail of GenerateIrradianceMapFromSH)
        // because the projection is independently testable without a render context.
        static SHCoefficients ProjectCubemapToSH(const Ref<TextureCubemap>& environmentMap);
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
        // Render to cubemap helper. Serial, one pass per face by design — the
        // engine's single GL context rules out a multithreaded bake, and the
        // bake is fragment-bound and disk-cached; see the definition for the
        // full rationale.
        static void RenderToCubemap(const Ref<TextureCubemap>& cubemap, const Ref<Shader>& shader,
                                    const Ref<Mesh>& cubeMesh, u32 mipLevel = 0);

        // Render to texture helper
        static void RenderToTexture(const Ref<Texture2D>& texture, const Ref<Shader>& shader);

        // Get cube mesh for rendering
        static const Ref<Mesh>& GetCubeMesh();

        // Cached meshes
        static Ref<Mesh> s_CubeMesh;
    };
} // namespace OloEngine
