#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Core/UUID.h"
#include "OloEngine/Asset/Asset.h"
#include "OloEngine/Renderer/Texture.h"

#include <glm/glm.hpp>

#include <unordered_map>

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
        // exists) creates the GPU atlas texture. Safe to call repeatedly; cheap
        // when already resolved against the same asset and key.
        //
        // Self-healing: a lightmap-static mesh that lost its UV2 stream (a
        // procedural primitive recreated at load, or a mesh whose .omesh was
        // never re-saved after the bake) is re-unwrapped here with the shared
        // kLightmapUnwrap* parameters BEFORE the key check. The unwrap is
        // deterministic, so the regenerated stream matches what the bake
        // rasterized; only a genuinely-changed scene reads as stale.
        void Resolve(Scene& scene);

        // Drops all resolved state (asset reloaded, bake re-run, scene edited).
        // The next Resolve() rebuilds from the asset.
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

        // The entity's atlas region, or vec4(0) (the shader's "no lightmap"
        // sentinel) when the entity has none or the bake is invalid.
        [[nodiscard]] glm::vec4 GetScaleOffset(UUID entityUUID) const;

        [[nodiscard]] const Ref<Texture2D>& GetAtlasTexture() const
        {
            return m_AtlasTexture;
        }

        [[nodiscard]] u32 GetAtlasSize() const
        {
            return m_AtlasSize;
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
        bool m_Resolved = false;
        bool m_Stale = false;
        AssetHandle m_ResolvedAsset = 0;
        u64 m_ResolvedBakeKey = 0;
        u32 m_AtlasSize = 0;
        Ref<Texture2D> m_AtlasTexture;
        std::unordered_map<UUID, glm::vec4> m_Regions;
    };
} // namespace OloEngine
