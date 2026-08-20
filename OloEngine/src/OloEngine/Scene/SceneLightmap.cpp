#include "OloEnginePCH.h"
#include "OloEngine/Scene/SceneLightmap.h"

#include "OloEngine/Animation/AnimatedMeshComponents.h"
#include "OloEngine/Asset/AssetManager.h"
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
        constexpr u64 kBakeKeyFormatVersion = 1;

        constexpr u64 kFnvOffsetBasis = 14695981039346656037ull;
        constexpr u64 kFnvPrime = 1099511628211ull;

        struct BakeKeyHasher
        {
            u64 Value = kFnvOffsetBasis;

            void MixBytes(const void* data, sizet size)
            {
                const auto* bytes = static_cast<const u8*>(data);
                for (sizet i = 0; i < size; ++i)
                {
                    Value ^= bytes[i];
                    Value *= kFnvPrime;
                }
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
            Invalidate();
            return;
        }

        // Self-healing unwrap (see the header): regenerate any lightmap-static
        // mesh's missing UV2 stream deterministically BEFORE the key check, so
        // a reloaded scene (procedural primitives, un-resaved .omesh) matches
        // the layout the bake rasterized instead of reading permanently stale.
        {
            LightmapUnwrapOptions unwrapOptions;
            unwrapOptions.Resolution = kLightmapUnwrapResolution;
            unwrapOptions.Padding = kLightmapUnwrapPadding;

            auto meshView = scene.GetAllEntitiesWith<MeshComponent>();
            for (auto entity : meshView)
            {
                auto& mesh = meshView.get<MeshComponent>(entity);
                if (!mesh.m_LightmapStatic || !mesh.m_MeshSource || mesh.m_MeshSource->HasLightmapUVs())
                    continue;
                if (LightmapUnwrap::Generate(*mesh.m_MeshSource, unwrapOptions))
                {
                    if (RenderCommand::IsDeviceAvailable())
                    {
                        mesh.m_MeshSource->Build();
                    }
                }
            }
        }

        const u64 liveKey = ComputeBakeKey(scene, settings);

        // Cheap re-resolve: same asset, same live key, texture already built.
        if (m_Resolved && m_ResolvedAsset == settings.LightmapAsset && m_ResolvedBakeKey == liveKey && !m_Stale)
            return;

        Invalidate();

        const Ref<LightmapAsset> asset = AssetManager::GetAsset<LightmapAsset>(settings.LightmapAsset);
        if (!asset || !asset->HasBakedData() || !asset->Validate())
        {
            OLO_CORE_WARN("SceneLightmapRuntime: lightmap asset {:x} missing or invalid — baked GI disabled",
                          static_cast<u64>(settings.LightmapAsset));
            return;
        }

        m_ResolvedAsset = settings.LightmapAsset;
        m_ResolvedBakeKey = liveKey;

        // THE staleness gate: a bake whose stored key no longer matches the live
        // scene is never sampled. Falling back to probes/IBL beats rendering
        // confidently from stale data (issue #439's signature failure mode).
        if (asset->GetBakeKey() != liveKey)
        {
            m_Resolved = true;
            m_Stale = true;
            OLO_CORE_WARN("SceneLightmapRuntime: bake key mismatch (asset {:x}, live {:x}) — the scene changed "
                          "since the last bake; baked GI disabled until re-baked",
                          asset->GetBakeKey(), liveKey);
            return;
        }

        m_Regions.reserve(asset->GetEntries().size());
        for (const auto& entry : asset->GetEntries())
        {
            m_Regions.emplace(UUID(entry.EntityUUID), entry.ScaleOffset);
        }
        m_AtlasSize = asset->GetWidth();

        // GPU atlas — RGBA16F, uploaded from the asset's f32 texels (the GL
        // upload path takes GL_FLOAT client data for RGBA16F and converts).
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
            m_AtlasTexture->SetData(const_cast<f32*>(texels.data()), static_cast<u32>(texels.size() * sizeof(f32)));
        }

        m_Resolved = true;
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

        // Lights: every photometric field that feeds the bake, in UUID order.
        std::vector<KeyedEntity> lights;
        auto collectLights = [&](auto view)
        {
            for (auto entity : view)
                lights.push_back({ static_cast<u64>(view.template get<IDComponent>(entity).ID), entity });
        };
        collectLights(scene.GetAllEntitiesWith<IDComponent, DirectionalLightComponent>());
        collectLights(scene.GetAllEntitiesWith<IDComponent, PointLightComponent>());
        collectLights(scene.GetAllEntitiesWith<IDComponent, SpotLightComponent>());
        std::sort(lights.begin(), lights.end(),
                  [](const KeyedEntity& a, const KeyedEntity& b)
                  { return a.Uuid < b.Uuid; });

        for (const auto& keyed : lights)
        {
            Entity e{ keyed.Handle, &scene };
            hasher.Mix(keyed.Uuid);
            const glm::mat4 transform = scene.GetWorldTransform(keyed.Handle);
            hasher.Mix(glm::vec3(transform[3])); // light position

            if (e.HasComponent<DirectionalLightComponent>())
            {
                const auto& l = e.GetComponent<DirectionalLightComponent>();
                hasher.Mix(l.m_Direction);
                hasher.Mix(l.m_Color);
                hasher.Mix(l.m_Intensity);
            }
            if (e.HasComponent<PointLightComponent>())
            {
                const auto& l = e.GetComponent<PointLightComponent>();
                hasher.Mix(l.m_Color);
                hasher.Mix(l.m_Intensity);
                hasher.Mix(l.m_Range);
                hasher.Mix(l.m_Attenuation);
            }
            if (e.HasComponent<SpotLightComponent>())
            {
                const auto& l = e.GetComponent<SpotLightComponent>();
                hasher.Mix(l.m_Direction);
                hasher.Mix(l.m_Color);
                hasher.Mix(l.m_Intensity);
                hasher.Mix(l.m_Range);
                hasher.Mix(l.m_InnerCutoff);
                hasher.Mix(l.m_OuterCutoff);
                hasher.Mix(l.m_Attenuation);
            }
        }

        return hasher.Value;
    }
} // namespace OloEngine
