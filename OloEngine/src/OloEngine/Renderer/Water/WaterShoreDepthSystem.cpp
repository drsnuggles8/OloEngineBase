#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Water/WaterShoreDepthSystem.h"

#include "OloEngine/Debug/Instrumentor.h"
#include "OloEngine/Renderer/HeapBindingSeam.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Terrain/TerrainData.h"

#include <algorithm>
#include <cmath>

namespace OloEngine
{
    namespace
    {
        struct WaterShoreDepthData
        {
            bool m_Initialized = false;
            bool m_HasField = false;
            Ref<Texture2D> m_FieldTexture;
            WaterShoreSettings m_Settings;
            WaterShoreBakeRequest m_Window;
            u64 m_Signature = 0;
            std::vector<glm::vec4> m_Texels; ///< retained so physics can read the same field
        };

        WaterShoreDepthData s_Data;

        /// FNV-1a over the raw bytes of one value. The signature only has to
        /// detect CHANGE, so hashing float bits directly is right here — two
        /// bit-different transforms are two different seabeds even when they
        /// compare equal, and a NaN that compares unequal to itself would
        /// otherwise rebuild every frame.
        template<typename T>
        void HashInto(u64& hash, const T& value)
        {
            static_assert(std::is_trivially_copyable_v<T>);
            const auto* bytes = reinterpret_cast<const unsigned char*>(&value);
            for (sizet i = 0; i < sizeof(T); ++i)
            {
                hash ^= static_cast<u64>(bytes[i]);
                hash *= 1099511628211ull;
            }
        }

        /// Every non-finite field replaced, once, so the signature, the bake and
        /// the retained window all describe the SAME window.
        ///
        /// BakeField used to sanitise into locals and never write back, which
        /// left a NaN centre in s_Data.m_Window: GetShaderParams then shipped it
        /// to the GPU, where `uv.x < 0.0 || uv.x > 1.0` is false for NaN, so the
        /// out-of-window guard passed and the shader sampled at a NaN uv. Every
        /// float from a scene file is validated (CLAUDE.md -> Conventions) and
        /// this is the boundary that owes it.
        [[nodiscard]] WaterShoreBakeRequest Sanitize(const WaterShoreBakeRequest& request)
        {
            WaterShoreBakeRequest out;
            out.CentreXZ = { std::isfinite(request.CentreXZ.x) ? request.CentreXZ.x : 0.0f,
                             std::isfinite(request.CentreXZ.y) ? request.CentreXZ.y : 0.0f };
            out.ExtentMetres = std::isfinite(request.ExtentMetres) ? request.ExtentMetres : 0.0f;
            out.WaterPlaneY = std::isfinite(request.WaterPlaneY) ? request.WaterPlaneY : 0.0f;
            return out;
        }
    } // namespace

    void WaterShoreDepthSystem::Init()
    {
        OLO_PROFILE_FUNCTION();

        if (s_Data.m_Initialized)
        {
            OLO_CORE_WARN("WaterShoreDepthSystem::Init called when already initialized");
            return;
        }

        // RGBA16F: r = depth (metres), gb = the depth gradient, a reserved. A
        // half float's ~3 decimal digits are plenty for both — the depth drives
        // tanh(kh), which is flat to a part in 1e7 by 25 m, and the gradient is
        // normalised before use, so only its DIRECTION survives.
        TextureSpecification spec;
        spec.Width = WaterShore::kResolution;
        spec.Height = WaterShore::kResolution;
        spec.Format = ImageFormat::RGBA16F;
        // No mips: sampled at LOD 0 by the vertex/tess stages, which have no
        // screen-space derivative to pick a level from in the first place.
        spec.GenerateMips = false;

        s_Data.m_FieldTexture = Texture2D::Create(spec);
        if (!s_Data.m_FieldTexture)
        {
            OLO_CORE_ERROR("WaterShoreDepthSystem::Init — failed to create the seabed depth "
                           "texture; shore wave deformation is disabled for this session");
            return;
        }

        s_Data.m_Initialized = true;
        s_Data.m_HasField = false;
        s_Data.m_Signature = 0;
        OLO_CORE_INFO("WaterShoreDepthSystem initialized ({}x{} RGBA16F seabed depth field)",
                      WaterShore::kResolution, WaterShore::kResolution);
    }

    void WaterShoreDepthSystem::Shutdown()
    {
        s_Data.m_FieldTexture.Reset();
        s_Data.m_Texels.clear();
        s_Data.m_Texels.shrink_to_fit();
        s_Data.m_HasField = false;
        s_Data.m_Signature = 0;
        s_Data.m_Initialized = false;
    }

    bool WaterShoreDepthSystem::IsInitialized()
    {
        return s_Data.m_Initialized;
    }

    void WaterShoreDepthSystem::SetSettings(const WaterShoreSettings& settings)
    {
        s_Data.m_Settings = settings;
    }

    const WaterShoreSettings& WaterShoreDepthSystem::GetSettings()
    {
        return s_Data.m_Settings;
    }

    u64 WaterShoreDepthSystem::BuildSignature(const WaterShoreBakeRequest& request,
                                              std::span<const SeabedTerrain> terrains)
    {
        u64 hash = 14695981039346656037ull;
        HashInto(hash, request.CentreXZ);
        HashInto(hash, request.ExtentMetres);
        HashInto(hash, request.WaterPlaneY);
        HashInto(hash, terrains.size());
        for (const SeabedTerrain& terrain : terrains)
        {
            HashInto(hash, terrain.OriginXZ);
            HashInto(hash, terrain.SizeXZ);
            HashInto(hash, terrain.BaseY);
            HashInto(hash, terrain.HeightScale);
            HashInto(hash, terrain.Resolution);
            // The height field's IDENTITY, not its contents. Hashing 512^2
            // floats per terrain every frame would cost more than the bake it is
            // meant to avoid, so this hashes the vector's address and size for a
            // REPLACED field, plus TerrainData's revision counter for a field
            // rewritten in place.
            //
            // The revision is not belt-and-braces. An in-place edit is the COMMON
            // case, not the exotic one: sculpting is GPU-resident, and
            // SyncFromGPU() refreshes an already-correctly-sized mirror with
            // resize() + memcpy, so every sample changes at the same address and
            // the size does not move either. Without the revision this hash is
            // constant across a whole sculpting session and the surf line stays
            // pinned to the coastline the scene loaded with — silently, with the
            // terrain visibly changing next to it.
            HashInto(hash, terrain.Heights);
            HashInto(hash, terrain.Heights != nullptr ? terrain.Heights->size() : sizet{ 0 });
            HashInto(hash, terrain.HeightRevision);
        }
        return hash;
    }

    void WaterShoreDepthSystem::BakeField(const WaterShoreBakeRequest& request,
                                          std::span<const SeabedTerrain> terrains,
                                          std::vector<glm::vec4>& outTexels)
    {
        OLO_PROFILE_FUNCTION();

        const u32 res = WaterShore::kResolution;
        outTexels.assign(static_cast<sizet>(res) * res, glm::vec4(0.0f));

        // Idempotent: Rebuild() has already normalised what it stores, and this
        // is here so a direct BakeField() call (the tests) cannot be handed a
        // window the retained one would not have accepted.
        const WaterShoreBakeRequest sane = Sanitize(request);
        const f32 extent = sane.ExtentMetres;
        const f32 planeY = sane.WaterPlaneY;
        const glm::vec2 centre = sane.CentreXZ;
        if (!(extent > 0.0f))
        {
            for (glm::vec4& texel : outTexels)
                texel = glm::vec4(WaterShore::kDeepSentinelMetres, 0.0f, 0.0f, 0.0f);
            return;
        }

        const f32 texelMetres = extent / static_cast<f32>(res);
        const glm::vec2 minCorner = centre - glm::vec2(extent * 0.5f);

        // ---- pass 1: depth ---------------------------------------------------
        for (u32 z = 0; z < res; ++z)
        {
            for (u32 x = 0; x < res; ++x)
            {
                // Texel centres, matching the half-texel convention the GLSL
                // side samples with: storage texel s has its centre at
                // uv = (s + 0.5) / N.
                const glm::vec2 worldXZ = minCorner + (glm::vec2(x, z) + 0.5f) * texelMetres;

                f32 depth = WaterShore::kDeepSentinelMetres;
                for (const SeabedTerrain& terrain : terrains)
                {
                    if (terrain.Heights == nullptr || terrain.Resolution == 0)
                        continue;
                    if (!(terrain.SizeXZ.x > 0.0f) || !(terrain.SizeXZ.y > 0.0f))
                        continue;

                    const glm::vec2 local = (worldXZ - terrain.OriginXZ) / terrain.SizeXZ;
                    if (local.x < 0.0f || local.x > 1.0f || local.y < 0.0f || local.y > 1.0f)
                        continue;

                    // The RAW-height overload, deliberately: TerrainData's member
                    // query needs a GL context to sync its CPU mirror, and this
                    // has to run headlessly. The caller hands over the already
                    // synced field.
                    const f32 normalized = TerrainData::SampleHeight(
                        *terrain.Heights, terrain.Resolution, local.x, local.y);
                    if (!std::isfinite(normalized))
                        continue;

                    const f32 seabedY = terrain.BaseY + normalized * terrain.HeightScale;
                    if (!std::isfinite(seabedY))
                        continue;

                    // Overlapping tiles: the HIGHEST seabed wins, because the
                    // water column ends at the first solid thing under it.
                    depth = std::min(depth, planeY - seabedY);
                }

                outTexels[static_cast<sizet>(z) * res + x].x =
                    std::clamp(depth, WaterShore::kMinDepthMetres, WaterShore::kDeepSentinelMetres);
            }
        }

        // ---- pass 2: gradient ------------------------------------------------
        //
        // Central differences over the finished depth field, clamped at the
        // borders. A one-sided difference at the edge would be a different
        // operator there; the field's border is open sea in every scene this
        // serves, so a replicated neighbour (gradient 0) is both cheaper and
        // more honest than pretending to measure a slope off the end of it.
        //
        // A large spurious gradient where a terrain tile's edge meets the deep
        // sentinel is harmless and deliberately not smoothed away: refraction is
        // scaled by c/c0, which is 1 to within f32 at any depth that deep, so a
        // gradient out there turns nothing. Only the direction of the gradient
        // in SHALLOW water reaches the wave.
        std::vector<f32> depths(outTexels.size());
        for (sizet i = 0; i < outTexels.size(); ++i)
            depths[i] = outTexels[i].x;

        auto depthAt = [&](u32 x, u32 z)
        { return depths[static_cast<sizet>(z) * res + x]; };

        for (u32 z = 0; z < res; ++z)
        {
            for (u32 x = 0; x < res; ++x)
            {
                const u32 xm = (x > 0) ? x - 1 : x;
                const u32 xp = (x + 1 < res) ? x + 1 : x;
                const u32 zm = (z > 0) ? z - 1 : z;
                const u32 zp = (z + 1 < res) ? z + 1 : z;

                // Span in texels, so a clamped border is a one-sided difference
                // over one texel rather than a two-texel difference scaled as
                // though it spanned two.
                const f32 spanX = static_cast<f32>(xp - xm) * texelMetres;
                const f32 spanZ = static_cast<f32>(zp - zm) * texelMetres;

                glm::vec4& texel = outTexels[static_cast<sizet>(z) * res + x];
                texel.y = (spanX > 0.0f) ? (depthAt(xp, z) - depthAt(xm, z)) / spanX : 0.0f;
                texel.z = (spanZ > 0.0f) ? (depthAt(x, zp) - depthAt(x, zm)) / spanZ : 0.0f;
                texel.w = 0.0f;
            }
        }
    }

    WaterShore::Sample WaterShoreDepthSystem::SampleBaked(std::span<const glm::vec4> texels,
                                                          const WaterShoreBakeRequest& window,
                                                          glm::vec2 worldXZ)
    {
        const u32 res = WaterShore::kResolution;
        if (texels.size() != static_cast<sizet>(res) * res)
            return WaterShore::DisabledSample();
        if (!(window.ExtentMetres > 0.0f) || !std::isfinite(worldXZ.x) || !std::isfinite(worldXZ.y))
            return WaterShore::DisabledSample();

        // The same addressing WaterShoreCommon.glsl :: waterShoreSample uses.
        const glm::vec2 uv = (worldXZ - window.CentreXZ) / window.ExtentMetres + 0.5f;
        if (uv.x < 0.0f || uv.x > 1.0f || uv.y < 0.0f || uv.y > 1.0f)
            return WaterShore::DisabledSample();

        // Bilinear with the half-texel convention, mirroring hardware filtering.
        const f32 fx = std::clamp(uv.x * static_cast<f32>(res) - 0.5f, 0.0f, static_cast<f32>(res - 1));
        const f32 fz = std::clamp(uv.y * static_cast<f32>(res) - 0.5f, 0.0f, static_cast<f32>(res - 1));
        const auto x0 = static_cast<u32>(fx);
        const auto z0 = static_cast<u32>(fz);
        const u32 x1 = std::min(x0 + 1u, res - 1u);
        const u32 z1 = std::min(z0 + 1u, res - 1u);
        const f32 tx = fx - static_cast<f32>(x0);
        const f32 tz = fz - static_cast<f32>(z0);

        auto at = [&](u32 x, u32 z)
        { return texels[static_cast<sizet>(z) * res + x]; };
        const glm::vec4 top = glm::mix(at(x0, z0), at(x1, z0), tx);
        const glm::vec4 bottom = glm::mix(at(x0, z1), at(x1, z1), tx);
        const glm::vec4 value = glm::mix(top, bottom, tz);

        WaterShore::Sample sample;
        sample.Depth =
            std::clamp(value.x, WaterShore::kMinDepthMetres, WaterShore::kDeepSentinelMetres);
        sample.Gradient = { value.y, value.z };
        sample.Enabled = true;
        return sample;
    }

    void WaterShoreDepthSystem::Rebuild(const WaterShoreBakeRequest& request,
                                        std::span<const SeabedTerrain> terrains)
    {
        OLO_PROFILE_FUNCTION();

        // Normalised BEFORE the signature, so a scene that hands over a NaN
        // centre on one frame and a NaN centre on the next is one window rather
        // than two (NaN compares unequal to itself, and the raw bytes of two
        // different NaNs differ), and so the window this stores is the window
        // the bake actually used.
        const WaterShoreBakeRequest sane = Sanitize(request);

        const u64 signature = BuildSignature(sane, terrains);
        if (s_Data.m_HasField && signature == s_Data.m_Signature)
            return;

        BakeField(sane, terrains, s_Data.m_Texels);
        s_Data.m_Window = sane;
        s_Data.m_Signature = signature;
        s_Data.m_HasField = true;

        if (s_Data.m_Initialized && s_Data.m_FieldTexture)
        {
            // RGBA16F takes its upload as full floats (the driver converts), so
            // this is 16 B per texel — 4 MB once per seabed change, not per
            // frame. See the class comment for why that is the right trade.
            s_Data.m_FieldTexture->SetData(s_Data.m_Texels.data(),
                                           static_cast<u32>(s_Data.m_Texels.size() *
                                                            sizeof(glm::vec4)));
        }
    }

    void WaterShoreDepthSystem::Invalidate()
    {
        s_Data.m_HasField = false;
        s_Data.m_Signature = 0;
        s_Data.m_Texels.clear();
    }

    glm::vec4 WaterShoreDepthSystem::GetShaderParams()
    {
        if (!s_Data.m_Initialized || !s_Data.m_HasField || !s_Data.m_Settings.m_Enabled)
            return glm::vec4(0.0f);
        if (!(s_Data.m_Window.ExtentMetres > 0.0f))
            return glm::vec4(0.0f);

        return { s_Data.m_Window.CentreXZ.x, s_Data.m_Window.CentreXZ.y,
                 1.0f / s_Data.m_Window.ExtentMetres, 1.0f };
    }

    glm::vec4 WaterShoreDepthSystem::GetShaderParams2()
    {
        const WaterShoreSettings& settings = s_Data.m_Settings;
        auto sane = [](f32 value, f32 lo, f32 hi, f32 fallback)
        {
            return std::isfinite(value) ? std::clamp(value, lo, hi) : fallback;
        };
        const f32 fadeStart = sane(settings.m_FoamFadeStartMetres, 0.0f, 5000.0f, 120.0f);
        const f32 fadeEnd = sane(settings.m_FoamFadeEndMetres, 0.0f, 5000.0f, 400.0f);
        return { sane(settings.m_BreakerIndex, 0.02f, 2.0f, WaterShore::kBreakerIndex),
                 sane(settings.m_FoamGain, 0.0f, 4.0f, 1.0f),
                 fadeStart,
                 // A fade whose end is not past its start is a divide the
                 // smoothstep would answer with a step; keep them ordered here
                 // so the shader needs no guard of its own.
                 std::max(fadeEnd, fadeStart + 1.0f) };
    }

    RHI::ResourceHandle WaterShoreDepthSystem::GetFieldTextureHandle()
    {
        if (s_Data.m_Initialized && s_Data.m_HasField && s_Data.m_FieldTexture)
            return s_Data.m_FieldTexture->GetRHIHandle();
        return RHI::NullResource;
    }

    void WaterShoreDepthSystem::PublishFieldTexture()
    {
        OLO_PROFILE_FUNCTION();

        if (!s_Data.m_Initialized || !s_Data.m_HasField || !s_Data.m_FieldTexture)
            return;

        HeapBinding::PublishTextureOffsetAndBind(ShaderBindingLayout::TEX_WATER_SHORE_DEPTH,
                                                 s_Data.m_FieldTexture->GetRHIHandle(),
                                                 RHI::HeapSlotLifetime::Persistent);
    }

    WaterShore::Sample WaterShoreDepthSystem::SampleWorld(glm::vec2 worldXZ)
    {
        if (!s_Data.m_HasField || !s_Data.m_Settings.m_Enabled)
            return WaterShore::DisabledSample();
        return SampleBaked(s_Data.m_Texels, s_Data.m_Window, worldXZ);
    }
} // namespace OloEngine
