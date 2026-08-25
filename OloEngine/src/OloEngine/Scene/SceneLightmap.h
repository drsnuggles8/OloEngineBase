#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Core/UUID.h"
#include "OloEngine/Asset/Asset.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Renderer/Texture2DArray.h"

#include <glm/glm.hpp>

#include <unordered_map>
#include <unordered_set>

namespace OloEngine
{
    class Scene;

    // Scene-level baked-lightmap settings (issue #439). Serialized with the
    // scene and copied by Scene::Copy() — see
    // docs/agent-rules/scene-copy-must-carry-scene-level-settings.md.
    struct SceneLightmapSettings
    {
        AssetHandle LightmapAsset = 0; // the scene's baked LightmapAsset (.olmap); 0 = never baked
        bool Enabled = true;           // user toggle — staleness is detected separately and always wins
        f32 Intensity = 1.0f;          // global baked-GI multiplier (u_LightmapIntensity)

        // Bake parameters, persisted so a re-bake reproduces the same layout.
        // They are part of the bake key: changing one stales the bake.
        u32 AtlasSize = 1024;
        u32 SamplesPerTexel = 128;
        u32 MaxBounces = 4;
        f32 TexelsPerMeter = 8.0f;
    };

    // Runtime-resolved state derived from a scene's LightmapAsset: the GPU atlas
    // texture and the per-entity atlas regions. Deliberately NOT a component and
    // NOT serialized — Scene::Copy() drops it and the runtime copy re-resolves at
    // Play start, so stale state cannot survive a scene transition.
    //
    // The staleness contract (the signature failure this feature must not have —
    // see HANDOVER/issue #439): Resolve() recomputes the live scene's bake key
    // and compares it against the asset's stored key. On mismatch the lightmap
    // is NOT sampled — IsValid() stays false, the renderer uploads a disabled
    // UBO, and every draw falls back to probes/IBL — and the editor surfaces a
    // warning instead of rendering confidently from stale data.
    class SceneLightmapRuntime : public RefCounted
    {
      public:
        // Loads the settings' LightmapAsset, verifies its bake key against the
        // live scene, builds the UUID→region table, and (when a graphics device
        // exists) creates the GPU atlas texture. Called every frame; the full
        // recheck (UV2 scan + bake-key recompute over every static entity and
        // light) runs only every kResolveRecheckIntervalFrames calls — the
        // frames in between return immediately on cached state, so a scene
        // edit is picked up within a fraction of a second without paying an
        // O(scene) hash per frame. Invalidate() forces the next call to do a
        // full recheck.
        //
        // Self-healing: a lightmap-static mesh that lost its UV2 stream (a
        // procedural primitive recreated at load, or a mesh whose .omesh was
        // never re-saved after the bake) is re-unwrapped here with the shared
        // kLightmapUnwrap* parameters BEFORE the key check. The unwrap is
        // deterministic, so the regenerated stream matches what the bake
        // rasterized; only a genuinely-changed scene reads as stale.
        void Resolve(Scene& scene);

        // Drops all resolved state (asset reloaded, bake re-run, scene edited),
        // clears the failed-unwrap memo, and forces the next Resolve() to do a
        // full recheck immediately (no throttle wait).
        void Invalidate();

        // True when a non-stale bake is resolved and the atlas texture exists.
        [[nodiscard]] bool IsValid() const
        {
            return m_Resolved && !m_Stale && m_AtlasTexture;
        }

        // True when an asset was found but its bake key no longer matches the
        // scene — the "needs re-bake" editor signal.
        [[nodiscard]] bool IsStale() const
        {
            return m_Stale;
        }

        // The entity's ENCODED atlas region (issue #868) — page-local scale/
        // offset with the atlas page folded into the integer part of `.z`, the
        // form InstanceData::LightmapScaleOffset carries and
        // sampleLightmapIrradiance decodes (see LightmapPageEncoding.h).
        // vec4(0) (the shader's "no lightmap" sentinel) when the entity has no
        // region or the bake is invalid; the sentinel survives the encoding
        // because every consumer gates on the untouched `.x` scale lane.
        [[nodiscard]] glm::vec4 GetScaleOffset(UUID entityUUID) const;

        // The atlas as a Texture2DArray with one layer per page (issue #868).
        // Single-page bakes are a 1-layer array — there is no separate
        // single-page path, so the multi-page code runs on every scene.
        [[nodiscard]] const Ref<Texture2DArray>& GetAtlasTexture() const
        {
            return m_AtlasTexture;
        }

        [[nodiscard]] u32 GetAtlasSize() const
        {
            return m_AtlasSize;
        }

        [[nodiscard]] u32 GetPageCount() const
        {
            return m_PageCount;
        }

        // The staleness key: FNV-1a-64 over everything the bake captured —
        // every lightmap-static entity (identity, geometry proxy, world
        // transform, resolved material factors), every light's photometric
        // fields, and the bake settings. Deterministic (entities and lights are
        // visited in UUID order). Any change to a hashed input stales the bake;
        // geometry is proxied by counts + bounds + asset handle rather than a
        // full vertex hash (a Sponza-sized position hash per load is not worth
        // the marginal coverage).
        [[nodiscard]] static u64 ComputeBakeKey(Scene& scene, const SceneLightmapSettings& settings);

      private:
        // Drops the resolved asset state only — keeps the failed-unwrap memo
        // and the warn-once latch, unlike the public Invalidate(). Used by the
        // recheck miss path so a persistently-failing unwrap or a stale bake
        // doesn't retry/re-warn on every recheck.
        void ResetResolvedState();

        bool m_Resolved = false;
        bool m_Stale = false;
        AssetHandle m_ResolvedAsset = 0;
        u64 m_ResolvedBakeKey = 0;
        u32 m_AtlasSize = 0;
        u32 m_PageCount = 0;
        Ref<Texture2DArray> m_AtlasTexture;
        std::unordered_map<UUID, glm::vec4> m_Regions;

        // Resolve() throttle + warn-once latch + failed-unwrap memo (see
        // Resolve()/Invalidate() docs above).
        u32 m_FramesUntilRecheck = 0;
        bool m_WarnedResolveFailure = false;
        std::unordered_set<UUID> m_FailedUnwraps;
    };
} // namespace OloEngine
