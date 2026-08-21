#include "OloEnginePCH.h"
#include "OloEngine/Scene/SceneLightmap.h"

#include "OloEngine/Animation/AnimatedMeshComponents.h"
#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Core/Hash.h"
#include "OloEngine/Renderer/Baking/LightmapUnwrap.h"
#include "OloEngine/Renderer/LightmapAsset.h"
#include "OloEngine/Renderer/MeshSource.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/SubmeshMaterialResolve.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Scene.h"

#include <algorithm>
#include <vector>

namespace OloEngine
{
    namespace
    {
        // Bumping this stales every existing bake — do so whenever the bake's
        // semantics change (integrand, texel layout, key inputs).
        constexpr u64 kBakeKeyFormatVersion = 2; // v2: lights hash what the bake consumes (raw Translation, per-type)

        // How many Resolve() calls pass between full rechecks. The recheck is
        // O(scene) (UV2 scan + key hash); a scene edit is still picked up in a
        // quarter-second at 60 fps, and Invalidate() bypasses the wait.
        constexpr u32 kResolveRecheckIntervalFrames = 15;

        struct BakeKeyHasher
        {
            u64 Value = Hash::FNV1a64OffsetBasis;

            void MixBytes(const void* data, sizet size)
            {
                Value = Hash::FNV1a64(data, size, Value);
            }

            template<typename T>
            void Mix(const T& value)
            {
                static_assert(std::is_trivially_copyable_v<T>, "BakeKeyHasher::Mix needs a trivially-copyable value");
                MixBytes(&value, sizeof(T));
            }
        };

        void MixMaterial(BakeKeyHasher& hasher, const Material& material)
        {
            hasher.Mix(material.GetBaseColorFactor());
            hasher.Mix(material.GetEmissiveFactor());
            hasher.Mix(material.GetMetallicFactor());
            hasher.Mix(material.GetRoughnessFactor());
        }
    } // namespace

    void SceneLightmapRuntime::Invalidate()
    {
        ResetResolvedState();
        m_FramesUntilRecheck = 0;
        m_WarnedResolveFailure = false;
        m_WarnedUnsampledPath = false;
        m_FailedUnwraps.clear();
    }

    void SceneLightmapRuntime::WarnIfActivePathCannotSample(const char* pathName)
    {
        if (m_WarnedUnsampledPath || !IsValid())
            return;

        m_WarnedUnsampledPath = true;
        OLO_CORE_WARN("SceneLightmapRuntime: a valid baked lightmap is resolved, but the active {} render path "
                      "cannot sample it — those draws fall back to probes/IBL, so the bake has no visible effect. "
                      "Switch the rendering path to Forward to see baked GI (tracked: issue #865).",
                      pathName);
    }

    void SceneLightmapRuntime::ResetResolvedState()
    {
        m_Resolved = false;
        m_Stale = false;
        m_ResolvedAsset = 0;
        m_ResolvedBakeKey = 0;
        m_AtlasSize = 0;
        m_AtlasTexture = nullptr;
        m_Regions.clear();
    }

    glm::vec4 SceneLightmapRuntime::GetScaleOffset(UUID entityUUID) const
    {
        if (!IsValid())
            return glm::vec4(0.0f);

        const auto it = m_Regions.find(entityUUID);
        return it != m_Regions.end() ? it->second : glm::vec4(0.0f);
    }

    void SceneLightmapRuntime::Resolve(Scene& scene)
    {
        const SceneLightmapSettings& settings = scene.GetLightmapSettings();
        if (settings.LightmapAsset == 0)
        {
            // Clearing the handle is an explicit "no lightmap" — drop any
            // resolved state right away. This is why the per-frame call site
            // must NOT gate on LightmapAsset != 0: the gate would skip exactly
            // the call that notices the handle went away.
            if (m_Resolved || m_Stale || m_AtlasTexture)
                Invalidate();
            return;
        }

        // Throttle: everything below is O(scene). Between rechecks the cached
        // state (IsValid / regions / atlas) keeps serving.
        if (m_FramesUntilRecheck > 0)
        {
            --m_FramesUntilRecheck;
            return;
        }
        m_FramesUntilRecheck = kResolveRecheckIntervalFrames;

        // Self-healing unwrap (see the header): regenerate any lightmap-static
        // mesh's missing UV2 stream deterministically BEFORE the key check, so
        // a reloaded scene (procedural primitives, un-resaved .omesh) matches
        // the layout the bake rasterized instead of reading permanently stale.
        {
            LightmapUnwrapOptions unwrapOptions;
            unwrapOptions.Resolution = kLightmapUnwrapResolution;
            unwrapOptions.Padding = kLightmapUnwrapPadding;

            auto meshView = scene.GetAllEntitiesWith<IDComponent, MeshComponent>();
            for (auto entity : meshView)
            {
                auto& mesh = meshView.get<MeshComponent>(entity);
                if (!mesh.m_LightmapStatic || !mesh.m_MeshSource || mesh.m_MeshSource->HasLightmapUVs())
                    continue;
                // An unwrap that failed once (unchartable faces, chart overflow)
                // will fail identically on every retry — xatlas runs 100ms+ per
                // mesh, so retrying each recheck is a recurring frame hitch.
                // The memo clears on Invalidate() (re-bake, asset change).
                const UUID uuid = meshView.get<IDComponent>(entity).ID;
                if (m_FailedUnwraps.contains(uuid))
                    continue;
                if (LightmapUnwrap::Generate(*mesh.m_MeshSource, unwrapOptions))
                {
                    if (RenderCommand::IsDeviceAvailable())
                    {
                        mesh.m_MeshSource->Build();
                    }
                }
                else
                {
                    m_FailedUnwraps.insert(uuid);
                }
            }
        }

        const u64 liveKey = ComputeBakeKey(scene, settings);

        // Cheap re-resolve: same asset, same live key, texture already built.
        if (m_Resolved && m_ResolvedAsset == settings.LightmapAsset && m_ResolvedBakeKey == liveKey && !m_Stale)
        {
            m_WarnedResolveFailure = false;
            return;
        }

        ResetResolvedState();

        const Ref<LightmapAsset> asset = AssetManager::GetAsset<LightmapAsset>(settings.LightmapAsset);
        if (!asset || !asset->HasBakedData() || !asset->Validate())
        {
            if (!m_WarnedResolveFailure)
            {
                OLO_CORE_WARN("SceneLightmapRuntime: lightmap asset {:x} missing or invalid — baked GI disabled",
                              static_cast<u64>(settings.LightmapAsset));
                m_WarnedResolveFailure = true;
            }
            return;
        }

        m_ResolvedAsset = settings.LightmapAsset;
        m_ResolvedBakeKey = liveKey;

        // THE staleness gate: a bake whose stored key no longer matches the live
        // scene is never sampled. Falling back to probes/IBL beats rendering
        // confidently from stale data (issue #439's signature failure mode).
        // Warn once per stale episode — staleness is the NORMAL editing state
        // after moving a baked entity, and a per-recheck warning would flood
        // the log for as long as the user keeps editing.
        if (asset->GetBakeKey() != liveKey)
        {
            m_Resolved = true;
            m_Stale = true;
            if (!m_WarnedResolveFailure)
            {
                OLO_CORE_WARN("SceneLightmapRuntime: bake key mismatch (asset {:x}, live {:x}) — the scene changed "
                              "since the last bake; baked GI disabled until re-baked",
                              asset->GetBakeKey(), liveKey);
                m_WarnedResolveFailure = true;
            }
            return;
        }

        // v1 bakes always produce one page and the runtime uploads exactly one:
        // an entry addressing a higher page has no texture to sample and must
        // not be served (it would read page 0's texels — another entity's
        // charts — through a valid-looking region).
        u32 rejectedPages = 0;
        m_Regions.reserve(asset->GetEntries().size());
        for (const auto& entry : asset->GetEntries())
        {
            if (entry.Page != 0)
            {
                ++rejectedPages;
                continue;
            }
            m_Regions.emplace(UUID(entry.EntityUUID), entry.ScaleOffset);
        }
        if (rejectedPages > 0)
        {
            OLO_CORE_WARN("SceneLightmapRuntime: {} entity regions address atlas pages > 0; only page 0 is "
                          "uploaded — those entities fall back to probes/IBL",
                          rejectedPages);
        }
        m_AtlasSize = asset->GetWidth();

        // GPU atlas — RGBA16F, uploaded from the asset's f32 texels (the GL
        // upload path takes GL_FLOAT client data for RGBA16F and converts).
        // Upload is bounded to page 0's slice — the texture is Width×Height,
        // and a (future) multi-page asset's full buffer would overflow it.
        // Headless processes keep the CPU-side region table; there is nothing
        // to sample without a device anyway.
        if (RenderCommand::IsDeviceAvailable())
        {
            TextureSpecification spec;
            spec.Width = asset->GetWidth();
            spec.Height = asset->GetHeight();
            spec.Format = ImageFormat::RGBA16F;
            spec.GenerateMips = false;
            m_AtlasTexture = Texture2D::Create(spec);
            const auto& texels = asset->GetTexelData();
            const sizet pageFloats = static_cast<sizet>(asset->GetWidth()) * asset->GetHeight() * 4u;
            m_AtlasTexture->SetData(const_cast<f32*>(texels.data()),
                                    static_cast<u32>(std::min(texels.size(), pageFloats) * sizeof(f32)));
        }

        m_Resolved = true;
        m_WarnedResolveFailure = false;
        OLO_CORE_INFO("SceneLightmapRuntime: resolved lightmap {}x{} with {} entity regions",
                      asset->GetWidth(), asset->GetHeight(), m_Regions.size());
    }

    u64 SceneLightmapRuntime::ComputeBakeKey(Scene& scene, const SceneLightmapSettings& settings)
    {
        BakeKeyHasher hasher;
        hasher.Mix(kBakeKeyFormatVersion);
        hasher.Mix(settings.AtlasSize);
        hasher.Mix(settings.SamplesPerTexel);
        hasher.Mix(settings.MaxBounces);
        hasher.Mix(settings.TexelsPerMeter);

        // Deterministic visit order: gather then sort by UUID — registry
        // iteration order is not a contract, the key must be.
        struct KeyedEntity
        {
            u64 Uuid;
            entt::entity Handle;
        };
        std::vector<KeyedEntity> staticEntities;

        auto meshView = scene.GetAllEntitiesWith<IDComponent, MeshComponent>();
        for (auto entity : meshView)
        {
            const auto& mesh = meshView.get<MeshComponent>(entity);
            if (!mesh.m_LightmapStatic || !mesh.m_MeshSource)
                continue;
            staticEntities.push_back({ static_cast<u64>(meshView.get<IDComponent>(entity).ID), entity });
        }
        std::sort(staticEntities.begin(), staticEntities.end(),
                  [](const KeyedEntity& a, const KeyedEntity& b)
                  { return a.Uuid < b.Uuid; });

        const Material defaultMaterial{};
        for (const auto& keyed : staticEntities)
        {
            const auto& mesh = meshView.get<MeshComponent>(keyed.Handle);
            const Ref<MeshSource>& source = mesh.m_MeshSource;

            hasher.Mix(keyed.Uuid);
            hasher.Mix(static_cast<u64>(source->GetHandle()));

            // Geometry proxy: counts + bounds. A vertex-level edit that keeps
            // both identical slips through — accepted; a full position hash per
            // scene load is not worth that marginal coverage.
            hasher.Mix(source->GetVertices().Num());
            hasher.Mix(source->GetIndices().Num());
            const auto& bounds = source->GetBoundingBox();
            hasher.Mix(bounds.Min);
            hasher.Mix(bounds.Max);

            hasher.Mix(scene.GetWorldTransform(keyed.Handle));

            const Material* overrideMaterial = nullptr;
            if (Entity e{ keyed.Handle, &scene }; e.HasComponent<MaterialComponent>())
                overrideMaterial = &e.GetComponent<MaterialComponent>().m_Material;
            const auto& submeshes = source->GetSubmeshes();
            for (i32 s = 0; s < submeshes.Num(); ++s)
            {
                MixMaterial(hasher, ResolveSubmeshMaterial(overrideMaterial, source.get(),
                                                           static_cast<u32>(s), defaultMaterial));
            }
        }

        // Lights: hash EXACTLY what the bake consumes, per type, in the order
        // ReferenceSceneBuilder::AddScene gathers them. Two deliberate parity
        // choices, both mirrored from the builder:
        //  - positions are the RAW TransformComponent::Translation (the value
        //    the raster light collection packs — NOT the parent-composed world
        //    transform), so re-parenting a light under a moved parent neither
        //    stales nor un-stales a bake the light never changed in;
        //  - a light the builder rejects (`!(intensity > 0)` — zero, negative
        //    or NaN) contributes nothing to the bake, so it contributes
        //    nothing to the key either.
        // A per-type tag keeps e.g. a point light and a spot light with
        // coincidentally-equal field bytes from colliding.
        const auto hashLightsOfType = [&](auto view, auto&& mixEntity)
        {
            std::vector<KeyedEntity> lights;
            for (auto entity : view)
                lights.push_back({ static_cast<u64>(view.template get<IDComponent>(entity).ID), entity });
            std::sort(lights.begin(), lights.end(),
                      [](const KeyedEntity& a, const KeyedEntity& b)
                      { return a.Uuid < b.Uuid; });
            for (const auto& keyed : lights)
                mixEntity(view, keyed);
        };

        hashLightsOfType(scene.GetAllEntitiesWith<IDComponent, TransformComponent, DirectionalLightComponent>(),
                         [&](auto& view, const KeyedEntity& keyed)
                         {
                             const auto& l = view.template get<DirectionalLightComponent>(keyed.Handle);
                             if (!(l.m_Intensity > 0.0f))
                                 return; // invisible to the bake ⇒ invisible to the key
                             hasher.Mix(1u);
                             hasher.Mix(keyed.Uuid);
                             hasher.Mix(l.m_Direction);
                             hasher.Mix(l.m_Color);
                             hasher.Mix(l.m_Intensity);
                         });
        hashLightsOfType(scene.GetAllEntitiesWith<IDComponent, TransformComponent, PointLightComponent>(),
                         [&](auto& view, const KeyedEntity& keyed)
                         {
                             const auto& l = view.template get<PointLightComponent>(keyed.Handle);
                             if (!(l.m_Intensity > 0.0f))
                                 return;
                             hasher.Mix(2u);
                             hasher.Mix(keyed.Uuid);
                             hasher.Mix(view.template get<TransformComponent>(keyed.Handle).Translation);
                             hasher.Mix(l.m_Color);
                             hasher.Mix(l.m_Intensity);
                             hasher.Mix(l.m_Range);
                             hasher.Mix(l.m_Attenuation);
                         });
        hashLightsOfType(scene.GetAllEntitiesWith<IDComponent, TransformComponent, SpotLightComponent>(),
                         [&](auto& view, const KeyedEntity& keyed)
                         {
                             const auto& l = view.template get<SpotLightComponent>(keyed.Handle);
                             if (!(l.m_Intensity > 0.0f))
                                 return;
                             hasher.Mix(3u);
                             hasher.Mix(keyed.Uuid);
                             hasher.Mix(view.template get<TransformComponent>(keyed.Handle).Translation);
                             hasher.Mix(l.m_Direction);
                             hasher.Mix(l.m_Color);
                             hasher.Mix(l.m_Intensity);
                             hasher.Mix(l.m_Range);
                             hasher.Mix(l.m_InnerCutoff);
                             hasher.Mix(l.m_OuterCutoff);
                             hasher.Mix(l.m_Attenuation);
                         });

        return hasher.Value;
    }
} // namespace OloEngine
