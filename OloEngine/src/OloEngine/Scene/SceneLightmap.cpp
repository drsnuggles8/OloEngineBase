#include "OloEnginePCH.h"
#include "OloEngine/Scene/SceneLightmap.h"

#include "OloEngine/Animation/AnimatedMeshComponents.h"
#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Core/Hash.h"
#include "OloEngine/Renderer/Baking/LightmapUnwrap.h"
#include "OloEngine/Renderer/LightmapAsset.h"
#include "OloEngine/Renderer/LightmapPageEncoding.h"
#include "OloEngine/Renderer/MeshSource.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/SubmeshMaterialResolve.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/SceneLightmapGather.h"

#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <vector>

namespace OloEngine
{
    namespace
    {
        // Bumping this stales every existing bake — do so whenever the bake's
        // semantics change (integrand, texel layout, key inputs).
        // v3 (issue #867): the key walks every RECEIVER, not just MeshComponent
        // entities, and mixes each receiver's sub-key and own world transform —
        // moving one instance of an InstancedMeshComponent must stale the bake.
        constexpr u64 kBakeKeyFormatVersion = 3;

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
        m_FailedUnwraps.clear();
    }

    void SceneLightmapRuntime::ResetResolvedState()
    {
        m_Resolved = false;
        m_Stale = false;
        m_ResolvedAsset = 0;
        m_ResolvedBakeKey = 0;
        m_AtlasSize = 0;
        m_PageCount = 0;
        m_AtlasTexture = nullptr;
        m_Regions.clear();
        m_EntitiesWithRegions.clear();
        // Bumped on the way DOWN as well as up: a consumer caching regions has
        // to notice the table going away just as much as it changing.
        ++m_ResolveGeneration;
    }

    glm::vec4 SceneLightmapRuntime::GetScaleOffset(UUID entityUUID, u64 subKey) const
    {
        if (!IsValid())
            return glm::vec4(0.0f);

        const auto it = m_Regions.find(LightmapRegionKey{ entityUUID, subKey });
        return it != m_Regions.end() ? it->second : glm::vec4(0.0f);
    }

    bool SceneLightmapRuntime::HasAnyRegionForEntity(UUID entityUUID) const
    {
        return IsValid() && m_EntitiesWithRegions.contains(entityUUID);
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
        //
        // Walks the SHARED receiver gather (issue #867) rather than a private
        // MeshComponent loop, so an instanced or model receiver heals on the same
        // terms the classic one always did. Several receivers can share a
        // MeshSource (every instance of one batch does); Generate() is idempotent
        // and HasLightmapUVs() short-circuits the repeats.
        //
        // Gathered ONCE per recheck and handed to ComputeBakeKey below: the walk
        // is O(scene) with an EnsureStableIDs scan per instanced batch, and it
        // sits behind the throttle for that reason.
        const std::vector<LightmapReceiver> receivers = GatherLightmapReceivers(scene);
        {
            LightmapUnwrapOptions unwrapOptions;
            unwrapOptions.Resolution = kLightmapUnwrapResolution;
            unwrapOptions.Padding = kLightmapUnwrapPadding;

            for (const LightmapReceiver& receiver : receivers)
            {
                if (receiver.Mesh->HasLightmapUVs())
                    continue;
                // An unwrap that failed once (unchartable faces, chart overflow)
                // will fail identically on every retry — xatlas runs 100ms+ per
                // mesh, so retrying each recheck is a recurring frame hitch.
                // The memo clears on Invalidate() (re-bake, asset change).
                if (m_FailedUnwraps.contains(receiver.Mesh.Raw()))
                    continue;
                // Through the SHARED helper, so the runtime's self-healing
                // re-unwrap deals with a cooked virtual-mesh blob exactly as the
                // editor's bake does. Without that, a virtual receiver whose
                // mesh lost its UV2 would heal on the classic path and not on
                // the virtual one — baked GI that appears only with the master
                // switch off, which is the asymmetry this whole receiver exists
                // to avoid.
                if (PrepareReceiverForBake(receiver))
                {
                    if (RenderCommand::IsDeviceAvailable())
                    {
                        Ref<MeshSource> mesh = receiver.Mesh;
                        mesh->Build();
                    }
                }
                else
                {
                    m_FailedUnwraps.insert(receiver.Mesh.Raw());
                }
            }
        }

        const u64 liveKey = ComputeBakeKey(scene, settings, receivers);

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

        // Every page is uploaded as its own texture-array layer (issue #868),
        // so the pre-#868 `Page != 0` rejection is gone. What replaces it is a
        // BOUNDS check with the same job: an entry naming a page outside the
        // asset's own PageCount has no layer to sample and must never be
        // served — serving it would read another entity's charts through a
        // valid-looking region, which is exactly the failure the old guard
        // existed to prevent. LightmapAsset::Validate() already rejects such an
        // asset outright, so this is the second line of defence, not the first.
        u32 rejectedPages = 0;
        const u32 pageCount = asset->GetPageCount();
        m_Regions.reserve(asset->GetEntries().size());
        m_EntitiesWithRegions.reserve(asset->GetEntries().size());
        for (const auto& entry : asset->GetEntries())
        {
            // Both halves of the address are bounded, not just the page: the
            // encoding folds the page into the INTEGER PART of the offset's x
            // lane, so an out-of-range offset is as much a wrong-layer read as
            // an out-of-range page. LightmapAsset::Validate() rejects either
            // outright above; this is the second line.
            const glm::vec4& region = entry.ScaleOffset;
            const bool pageInRange = entry.Page < pageCount && entry.Page < kMaxLightmapPages;
            const bool regionInPage = region.x > 0.0f && region.y > 0.0f && region.z >= 0.0f && region.w >= 0.0f &&
                                      region.z + region.x <= 1.0f && region.w + region.y <= 1.0f;
            if (!pageInRange || !regionInPage)
            {
                ++rejectedPages;
                continue;
            }
            // Fold the page into the region here — this is the ONE place the
            // encoding is composed; everything downstream (DrawMeshCommand,
            // the FrameDataBuffer stream, InstanceData) just carries the vec4.
            m_Regions.emplace(LightmapRegionKey{ UUID(entry.EntityUUID), entry.SubKey },
                              EncodeLightmapRegion(entry.ScaleOffset, entry.Page));
            m_EntitiesWithRegions.insert(UUID(entry.EntityUUID));
        }
        ++m_ResolveGeneration;
        if (rejectedPages > 0)
        {
            OLO_CORE_WARN("SceneLightmapRuntime: {} baked regions address texels outside the asset's {} "
                          "page(s) — those receivers fall back to probes/IBL",
                          rejectedPages, pageCount);
        }
        m_AtlasSize = asset->GetWidth();
        m_PageCount = pageCount;

        // GPU atlas — an RGBA16F Texture2DArray with one layer per page. Unlike
        // Texture2D::SetData (which accepts GL_FLOAT client data for an RGBA16F
        // target), Texture2DArray::SetLayerData's contract is NATIVE data per
        // format, so the asset's f32 texels are packed to halves here. Headless
        // processes keep the CPU-side region table; there is nothing to sample
        // without a device anyway.
        if (RenderCommand::IsDeviceAvailable())
        {
            Texture2DArraySpecification spec;
            spec.Width = asset->GetWidth();
            spec.Height = asset->GetHeight();
            spec.Layers = pageCount;
            spec.Format = Texture2DArrayFormat::RGBA16F;
            spec.GenerateMipmaps = false;
            m_AtlasTexture = Texture2DArray::Create(spec);
            if (m_AtlasTexture)
            {
                const auto& texels = asset->GetTexelData();
                const sizet pageFloats = static_cast<sizet>(asset->GetWidth()) * asset->GetHeight() * 4u;
                std::vector<u16> halves(pageFloats);
                for (u32 page = 0; page < pageCount; ++page)
                {
                    const sizet base = static_cast<sizet>(page) * pageFloats;
                    if (base + pageFloats > texels.size())
                    {
                        // Validate() guarantees this cannot happen; refusing to
                        // upload beats reading past the buffer if it ever does.
                        OLO_CORE_ERROR("SceneLightmapRuntime: texel buffer is short of page {} — atlas upload aborted",
                                       page);
                        m_AtlasTexture = nullptr;
                        break;
                    }
                    for (sizet i = 0; i < pageFloats; ++i)
                    {
                        halves[i] = static_cast<u16>(glm::packHalf1x16(texels[base + i]));
                    }
                    m_AtlasTexture->SetLayerData(page, halves.data(), asset->GetWidth(), asset->GetHeight());
                }
            }
        }

        m_Resolved = true;
        m_WarnedResolveFailure = false;
        OLO_CORE_INFO("SceneLightmapRuntime: resolved lightmap {}x{}x{} page(s) with {} entity regions",
                      asset->GetWidth(), asset->GetHeight(), pageCount, m_Regions.size());
    }

    u64 SceneLightmapRuntime::ComputeBakeKey(Scene& scene, const SceneLightmapSettings& settings)
    {
        return ComputeBakeKey(scene, settings, GatherLightmapReceivers(scene));
    }

    u64 SceneLightmapRuntime::ComputeBakeKey(Scene& scene, const SceneLightmapSettings& settings,
                                             std::span<const LightmapReceiver> receivers)
    {
        BakeKeyHasher hasher;
        hasher.Mix(kBakeKeyFormatVersion);
        hasher.Mix(settings.AtlasSize);
        hasher.Mix(settings.SamplesPerTexel);
        hasher.Mix(settings.MaxBounces);
        hasher.Mix(settings.TexelsPerMeter);

        // Registry iteration order is not a contract and the key must be, so
        // both walks below sort. The RECEIVERS come pre-sorted by (UUID, SubKey)
        // from the shared gather; hashing the SAME list the bake consumes is the
        // point of that gather, because a key computed over a different walk
        // than the bake's does not render wrongly — it renders with no baked GI
        // and no error at all. The LIGHTS are gathered and sorted here.
        struct KeyedEntity
        {
            u64 Uuid;
            entt::entity Handle;
        };

        const Material defaultMaterial{};
        for (const LightmapReceiver& receiver : receivers)
        {
            const Ref<MeshSource>& source = receiver.Mesh;

            hasher.Mix(static_cast<u64>(receiver.EntityUUID));
            // The sub-key is hashed for its own sake, not just as a
            // disambiguator: adding, removing or re-identifying an instance
            // changes which regions the bake owes, and the atlas layout with it.
            hasher.Mix(receiver.SubKey);
            hasher.Mix(static_cast<u64>(source->GetHandle()));

            // Geometry proxy: counts + bounds. A vertex-level edit that keeps
            // both identical slips through — accepted; a full position hash per
            // scene load is not worth that marginal coverage.
            hasher.Mix(source->GetVertices().Num());
            hasher.Mix(source->GetIndices().Num());
            const auto& bounds = source->GetBoundingBox();
            hasher.Mix(bounds.Min);
            hasher.Mix(bounds.Max);

            // The RECEIVER's transform, not the entity's: an instance's
            // world transform is its own, and moving one instance of a batch
            // has to stale the bake exactly as moving a mesh entity does.
            hasher.Mix(receiver.WorldTransform);

            const auto& submeshes = source->GetSubmeshes();
            for (i32 s = 0; s < submeshes.Num(); ++s)
            {
                MixMaterial(hasher, ResolveSubmeshMaterial(receiver.OverrideMaterial, source.get(),
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
