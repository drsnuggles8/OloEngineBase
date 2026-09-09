#include "OloEnginePCH.h"

#include "OloEngine/Renderer/PathTracing/ReferenceTextureCapture.h"

#include "OloEngine/Renderer/EnvironmentMap.h"
#include "OloEngine/Renderer/Material.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Renderer/TextureCubemap.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Scene.h"

#include <cmath>
#include <cstring>
#include <memory>
#include <span>
#include <vector>

namespace OloEngine::PathTracing
{
    namespace
    {
        // An 8-bit material texture comes back from GetData as the STORED
        // bytes — a readback does not apply the sRGB EOTF — so the decode is
        // ReferenceTexture::FromRgba8's job and `srgb` comes from the
        // texture's own specification. That is the same flag the GPU uses to
        // pick an sRGB internal format, so the two tracers decode one image
        // identically.
        //
        // Only 8-bit colour formats are handled. A float material map is not a
        // thing this engine imports, and a BLOCK-COMPRESSED one (BC7 albedo
        // from a packed asset) cannot be read back at all — OpenGLTexture2D::
        // GetData rejects it. Both leave the slot factor-only and are counted
        // by the caller rather than passed off as success.
        [[nodiscard]] bool DecodeTexture2D(const Ref<Texture2D>& texture, ReferenceTexture& out)
        {
            const TextureSpecification& spec = texture->GetSpecification();
            const u32 width = texture->GetWidth();
            const u32 height = texture->GetHeight();
            if (width == 0 || height == 0)
                return false;

            std::vector<u8> bytes;
            if (!texture->GetData(bytes, 0))
                return false;

            const sizet texelCount = static_cast<sizet>(width) * height;
            if (spec.Format == ImageFormat::RGBA8)
            {
                if (bytes.size() < texelCount * 4u)
                    return false;
                out = ReferenceTexture::FromRgba8(width, height, std::span<const u8>(bytes), spec.SRGB);
            }
            else if (spec.Format == ImageFormat::RGB8)
            {
                if (bytes.size() < texelCount * 3u)
                    return false;
                std::vector<u8> rgba(texelCount * 4u);
                for (sizet i = 0; i < texelCount; ++i)
                {
                    rgba[i * 4 + 0] = bytes[i * 3 + 0];
                    rgba[i * 4 + 1] = bytes[i * 3 + 1];
                    rgba[i * 4 + 2] = bytes[i * 3 + 2];
                    rgba[i * 4 + 3] = 255;
                }
                out = ReferenceTexture::FromRgba8(width, height, std::span<const u8>(rgba), spec.SRGB);
            }
            else
            {
                return false;
            }
            return out.Width == width && out.Height == height;
        }
        // One sky component type, resolved the way the renderer resolves it.
        // Returns whether this tier DRIVES THE SCENE, which is not the same
        // question as "did it capture" — see the call site.
        //
        // The `continue` below is load-bearing and mirrors
        // Scene::LoadAndRenderSkybox exactly: each tier there skips a component
        // whose cubemap has not been baked (or whose bake failed and dropped
        // the stale one) and FALLS THROUGH to the next tier. Returning true for
        // such a component would report "no sky" while an EnvironmentMapComponent
        // further down was actually lighting the scene. Note the asymmetry that
        // makes this subtle: a sky with m_EnableIBL false still stops the
        // fall-through over there — it draws and clears the global IBL — so the
        // predicate is "has a cached EnvironmentMap", not "contributes light".
        template<typename SkyComponent>
        [[nodiscard]] bool TakeFirstSky(Scene& scene, CapturedSky& captured, const char* label)
        {
            auto view = scene.GetAllEntitiesWith<SkyComponent>();
            bool taken = false;
            u32 candidates = 0;
            for (auto entity : view)
            {
                const SkyComponent& sky = view.template get<SkyComponent>(entity);
                if (!sky.m_EnvironmentMap)
                    continue;
                ++candidates;
                if (taken)
                    continue; // counting the rest, not using them — see below
                if (sky.m_EnableIBL && sky.m_EnvironmentMap->GetEnvironmentMap())
                {
                    captured.Cubemap = CaptureEnvironmentCubemap(sky.m_EnvironmentMap->GetEnvironmentMap());
                    if (captured.Cubemap)
                    {
                        captured.Intensity = std::isfinite(sky.m_IBLIntensity) && sky.m_IBLIntensity >= 0.0f
                                                 ? sky.m_IBLIntensity
                                                 : 1.0f;
                    }
                }
                taken = true;
            }
            // "First in the view" is not a stable choice — EnTT's iteration
            // order follows pool packing history, so a scene carrying TWO
            // usable skies of one type can bake against a different one after
            // an unrelated registry change. Deliberately NOT fixed by sorting
            // on UUID here: Scene::LoadAndRenderSkybox picks the first too, and
            // a capture that sorted differently would light the bake with a
            // sky the frame does not show — swapping a nondeterminism for a
            // guaranteed mismatch. Matching the renderer is the contract
            // (reference-path-tracer.md §4: differ in transport, never in the
            // light model). What this CAN do is refuse to be quiet about it.
            if (candidates > 1)
            {
                OLO_CORE_WARN("CaptureSceneSky: the scene has {} usable {} components; both the renderer and "
                              "this capture take whichever the registry lists first, which is not a stable "
                              "choice. Keep one sky per scene.",
                              candidates, label);
            }
            return taken;
        }
    } // namespace

    std::shared_ptr<const ReferenceTexture> ReferenceTextureCaptor::Capture(const Ref<Texture2D>& texture)
    {
        if (!texture)
            return nullptr;

        if (const auto it = m_Cache.find(texture.Raw()); it != m_Cache.end())
        {
            // A cached miss is cached too, so twenty materials sharing one
            // unreadable BC7 albedo attempt the readback once — and are
            // counted once, which is what makes the tally readable.
            return it->second;
        }

        ReferenceTexture decoded;
        std::shared_ptr<const ReferenceTexture> result;
        if (DecodeTexture2D(texture, decoded))
        {
            result = std::make_shared<const ReferenceTexture>(std::move(decoded));
            ++m_Stats.Captured;
        }
        else
        {
            ++m_Stats.Failed;
            OLO_CORE_WARN("ReferenceTextureCaptor: could not read back '{}' ({}x{}) — the reference will "
                          "trace that slot factor-only",
                          texture->GetPath().empty() ? "<generated>" : texture->GetPath(), texture->GetWidth(),
                          texture->GetHeight());
        }
        m_Cache.emplace(texture.Raw(), result);
        return result;
    }

    ReferenceMaterialMapProvider ReferenceTextureCaptor::MakeMaterialMapProvider()
    {
        return [this](const Material& material) -> ReferenceMaterialMaps
        {
            ReferenceMaterialMaps maps;
            maps.Albedo = Capture(material.GetAlbedoMap());
            maps.MetallicRoughness = Capture(material.GetMetallicRoughnessMap());
            maps.Normal = Capture(material.GetNormalMap());
            maps.Emissive = Capture(material.GetEmissiveMap());
            return maps;
        };
    }

    std::shared_ptr<const ReferenceEnvironmentCubemap> CaptureEnvironmentCubemap(const Ref<TextureCubemap>& cubemap)
    {
        if (!cubemap)
            return nullptr;

        const CubemapSpecification& spec = cubemap->GetCubemapSpecification();
        const u32 faceSize = cubemap->GetWidth();
        if (faceSize == 0 || cubemap->GetHeight() != faceSize)
        {
            OLO_CORE_WARN("CaptureEnvironmentCubemap: face is {}x{} — the reference cube sampler assumes "
                          "square faces; tracing with no sky",
                          cubemap->GetWidth(), cubemap->GetHeight());
            return nullptr;
        }

        std::vector<u8> bytes;
        if (!cubemap->GetData(bytes, 0))
        {
            OLO_CORE_WARN("CaptureEnvironmentCubemap: the backend could not read back a {}x{} sky cubemap "
                          "in format {} — the bake will run with no sky contribution",
                          cubemap->GetWidth(), cubemap->GetHeight(), static_cast<i32>(spec.Format));
            return nullptr;
        }

        const sizet faceTexels = static_cast<sizet>(faceSize) * faceSize;
        const sizet totalTexels = faceTexels * ReferenceEnvironmentCubemap::kFaceCount;

        // RGBA32F is what every generated sky and HDR load produces, and it
        // comes back as four linear f32 per texel — no transfer function to
        // undo. RGBA16F is accepted here for a backend that can hand it back,
        // but note that the GL one CANNOT: OpenGLTextureCubemap's format table
        // has no RGBA16F entry, so GetData fails above and a 16F sky never
        // reaches this branch. That is why the failure below names the format
        // rather than shrugging — an unexplained "no sky" in a bake log is the
        // hard version of this bug to find.
        std::vector<f32> rgba;
        if (spec.Format == ImageFormat::RGBA32F || spec.Format == ImageFormat::RGBA16F)
        {
            if (bytes.size() < totalTexels * 4u * sizeof(f32))
                return nullptr;
            rgba.resize(totalTexels * 4u);
            std::memcpy(rgba.data(), bytes.data(), rgba.size() * sizeof(f32));
        }
        else if (spec.Format == ImageFormat::RGBA8 || spec.Format == ImageFormat::RGB8)
        {
            // An 8-bit sky is LINEAR here, not sRGB — deliberately. A
            // face-path cubemap is created with GL_RGB8 / GL_RGBA8 internal
            // formats (OpenGLTextureCubemap), never the sRGB variants, so the
            // skybox and IBL bake read those bytes as linear values. The
            // reference must match the raster path's light model, so it reads
            // them the same way; decoding sRGB here would make every 8-bit sky
            // darker in the bake than on screen, which is a convention
            // divergence dressed up as a transport result.
            const sizet channels = spec.Format == ImageFormat::RGBA8 ? 4u : 3u;
            if (bytes.size() < totalTexels * channels)
                return nullptr;
            rgba.resize(totalTexels * 4u);
            for (sizet i = 0; i < totalTexels; ++i)
            {
                rgba[i * 4 + 0] = static_cast<f32>(bytes[i * channels + 0]) / 255.0f;
                rgba[i * 4 + 1] = static_cast<f32>(bytes[i * channels + 1]) / 255.0f;
                rgba[i * 4 + 2] = static_cast<f32>(bytes[i * channels + 2]) / 255.0f;
                rgba[i * 4 + 3] = 1.0f;
            }
        }
        else
        {
            OLO_CORE_WARN("CaptureEnvironmentCubemap: sky cubemap format is not one this can decode — "
                          "the bake will run with no sky contribution");
            return nullptr;
        }

        auto cube = std::make_shared<const ReferenceEnvironmentCubemap>(
            ReferenceEnvironmentCubemap::FromFacesRgba32F(faceSize, std::span<const f32>(rgba)));
        if (!cube->IsValid())
            return nullptr;
        return cube;
    }

    CapturedSky CaptureSceneSky(Scene& scene)
    {
        CapturedSky captured;
        // The precedence order of Scene::LoadAndRenderSkybox, and the same
        // "first one found drives the scene" rule: TakeFirstSky reports
        // whether a sky component EXISTS, not whether it captured, so a
        // procedural sky that has not baked its cubemap yet still shadows the
        // EnvironmentMapComponent behind it — exactly as it does on screen.
        if (TakeFirstSky<StarNestSkyComponent>(scene, captured, "StarNestSky"))
            return captured;
        if (TakeFirstSky<ProceduralSkyComponent>(scene, captured, "ProceduralSky"))
            return captured;
        // The last rung has nothing to fall through to, so its answer is not
        // interesting — but discarding a [[nodiscard]] is a warning here.
        static_cast<void>(TakeFirstSky<EnvironmentMapComponent>(scene, captured, "EnvironmentMap"));
        return captured;
    }
} // namespace OloEngine::PathTracing
