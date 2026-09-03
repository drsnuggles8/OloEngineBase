#pragma once

// =============================================================================
// ReferenceSceneBuilder.h — the ECS -> ReferenceScene adapter (issue #439)
//
// ReferenceScene.h promises this file: the reference path tracer's world is a
// literal-constructible, GL-free description, and a SEPARATE adapter populates
// one from a live `Scene` for raster-vs-reference comparisons. This is that
// adapter. Everything it emits follows one rule, stated in
// docs/agent-rules/reference-path-tracer.md §4 and repeated here because every
// judgement call below descends from it:
//
//     The reference must differ from the raster path in TRANSPORT,
//     never in the LIGHT MODEL (or the material model, or the geometry).
//
// So materials are the raster path's resolved FACTORS (resolution order ==
// SubmeshMaterialResolve.h, the one shared rule every submission path uses),
// lights are packed bit-for-bit the way Scene.cpp fills the MultiLight UBO
// (point AttenuationParams = (1, 0, m_Attenuation, m_Range), spot cones as
// cosines, directional direction passed through with the LightData sign
// convention), and geometry is the same triangle soup the draw call indexes.
//
// TRANSFORM HANDLING — why two paths exist
// ----------------------------------------
// ReferenceScene::AddInstance REJECTS non-uniform scale and mirrored bases,
// loudly and by design (its TMax conversion needs a single scalar). An ECS
// scene is free to author either. The builder therefore classifies every
// entity transform with EXACTLY the acceptance test AddInstance applies:
//
//   * rigid + uniform scale, positive determinant  -> the submesh geometry is
//     added ONCE per distinct (MeshSource, submesh) pair — cached — and each
//     entity becomes an instance with its own transform. Instancing preserved.
//   * anything else (non-uniform scale, mirror)    -> positions AND normals
//     (inverse-transpose, renormalized) are pre-transformed to world space and
//     added as a dedicated geometry under an IDENTITY instance. A mirrored
//     basis also flips triangle winding during the pre-transform, so the
//     winding-derived geometric normals still face outward.
//
// Either way AddInstance's rejection is unreachable: a scene that renders is a
// scene that traces.
//
// BUILD IS DEFERRED AND THE BUILDER IS SINGLE-USE
// -----------------------------------------------
// Add* calls only accumulate. `Build(options)` materialises the ReferenceScene
// (applying LambertianDiffuseOnly to every material and installing the uniform
// environment) and consumes the builder — geometry buffers are moved out, and
// every later call is a logged no-op. Build a new builder per snapshot.
//
// DETERMINISM
// -----------
// The path tracer's bit-identical-render contract needs stable geometry and
// light ordering. `AddScene` therefore sorts every gathered entity by UUID
// before adding, so two walks of the same scene produce bit-identical
// ReferenceScenes regardless of EnTT pool packing order.
// =============================================================================

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/PathTracing/ReferenceScene.h"
// Entity.h (which pulls Scene.h and Components.h) rather than forward
// declarations: `std::function<bool(Entity)>` needs Entity complete wherever
// the specialization is instantiated, and this adapter's whole purpose is the
// Scene coupling — hiding it behind forward declarations would only move the
// include burden onto every caller.
#include "OloEngine/Scene/Entity.h"

#include <glm/glm.hpp>

#include <functional>
#include <map>
#include <span>
#include <utility>
#include <vector>

namespace OloEngine
{
    // Scene/SceneLightmapGather.h — forward-declared so this renderer header
    // does not pull the lightmap gather into every path-tracing translation
    // unit; only the definition of the span's element is needed at the call.
    struct LightmapReceiver;
} // namespace OloEngine

namespace OloEngine::PathTracing
{
    struct ReferenceSceneBuildOptions
    {
        // Uniform environment radiance — the only environment ReferenceScene
        // supports (on purpose; see its header). A raster scene's sky/IBL has
        // no exact reference twin, so the caller picks the approximation.
        glm::vec3 EnvironmentRadiance{ 0.0f };

        // Forwarded to EVERY emitted material. Transport-isolation mode: see
        // ReferenceMaterial::LambertianDiffuseOnly for when this is legitimate
        // (comparing against a diffuse-only consumer such as DDGI or a
        // lightmap baker) and why it must be used on BOTH sides of a
        // comparison or on neither.
        bool LambertianDiffuseOnly = false;
    };

    class ReferenceSceneBuilder
    {
      public:
        ReferenceSceneBuilder();
        ~ReferenceSceneBuilder();

        ReferenceSceneBuilder(const ReferenceSceneBuilder&) = delete;
        ReferenceSceneBuilder& operator=(const ReferenceSceneBuilder&) = delete;

        // Adds one entity's static geometry: every submesh of `meshSource`,
        // placed by `worldTransform`, with materials resolved exactly the way
        // render submission does (override -> imported-per-submesh -> the
        // engine default; SubmeshMaterialResolve.h). `overrideMaterial` is the
        // entity's MaterialComponent material, or null for none.
        //
        // Submesh::m_Transform is deliberately NOT composed in: the classic
        // MeshComponent draw path ignores it (Renderer3D::DrawMesh submits
        // `cmd->transform = worldTransform` with baseIndex/indexCount only,
        // and the importers bake node transforms into the vertices), so the
        // builder ignores it too — same geometry, same placement.
        //
        // Returns true if at least one submesh contributed triangles; false
        // (adding nothing) for a null/empty meshSource, a non-finite or
        // near-singular transform, or a mesh whose submeshes are all
        // degenerate.
        bool AddMeshEntity(const Ref<MeshSource>& meshSource, const glm::mat4& worldTransform,
                           const Material* overrideMaterial);

        // Mirror one scene light. Packing matches Scene.cpp's MultiLight UBO
        // fill exactly — same attenuation parameterisation, same cone cosines,
        // same direction conventions (the spot direction goes through the
        // SHARED SanitizeSpotLightDirection in Renderer/LightCommon.h, the
        // same function Scene.cpp's packing calls) — because ReferenceBRDF's
        // CalculateAttenuation / CalculateSpotIntensity are ports of the
        // shader functions that consume that packing. Lights with zero (or
        // negative) intensity are skipped silently: they contribute nothing
        // in either world. Lights carrying any OTHER non-finite value
        // (position, color, intensity, attenuation, range, cone angles, a
        // directional direction) are skipped with a warning — a single NaN
        // here would silently poison every traced texel.
        //
        // `position` is the light entity's TransformComponent::Translation —
        // the value Scene.cpp packs (NOT the hierarchy-composed world
        // position; the raster light collection reads the raw translation, so
        // the reference must too).
        void AddDirectionalLight(const DirectionalLightComponent& light);
        void AddPointLight(const PointLightComponent& light, const glm::vec3& position);
        void AddSpotLight(const SpotLightComponent& light, const glm::vec3& position);

        // Convenience: walk a live Scene. Every entity carrying a
        // MeshComponent with a set m_MeshSource — minus skinned entities
        // (SkeletonComponent; the animated path owns those and DDGI likewise
        // excludes them from GI capture) — for which `includeEntity` returns
        // true becomes geometry, transformed by Scene::GetWorldTransform and
        // shaded per its MaterialComponent. Every directional/point/spot
        // light component becomes a light (the predicate does NOT gate
        // lights, only geometry — "static" is a geometry property).
        //
        // An empty `includeEntity` includes every mesh entity.
        //
        // Gathered entities are sorted by UUID before adding — see the
        // determinism note above.
        void AddScene(Scene& scene, const std::function<bool(Entity)>& includeEntity);

        // Capture the world a LIGHTMAP BAKE traces against (issue #867): the
        // geometry is exactly the receiver list the bake is about to consume —
        // instanced placements and model submeshes included, each at its own
        // world transform — plus every scene light, as AddScene adds them.
        //
        // AddScene cannot serve this: it walks MeshComponent entities, so a
        // scene whose static geometry is instanced or virtual would bake against
        // a world those surfaces are missing from. The failure is quiet (less
        // bounce, no occluders) rather than loud, which is exactly why the two
        // walks are one list.
        void AddLightmapReceivers(Scene& scene, std::span<const OloEngine::LightmapReceiver> receivers);

        // The light half of AddScene, on its own. Lights are never gated by a
        // geometry predicate — "static" is a property of geometry.
        void AddSceneLights(Scene& scene);

        // Finalises into a built ReferenceScene (TLAS + emissive list ready to
        // trace) and CONSUMES the builder. Calling anything afterwards is a
        // logged no-op; a second Build returns an empty scene.
        [[nodiscard]] ReferenceScene Build(const ReferenceSceneBuildOptions& options);

        // ---- introspection (tests) ------------------------------------------

        [[nodiscard]] sizet GetPendingGeometryCount() const
        {
            return m_Geometries.size();
        }
        [[nodiscard]] sizet GetPendingInstanceCount() const
        {
            return m_Instances.size();
        }
        [[nodiscard]] sizet GetPendingLightCount() const
        {
            return m_Lights.size();
        }
        [[nodiscard]] bool IsConsumed() const
        {
            return m_Consumed;
        }

      private:
        struct PendingGeometry
        {
            std::vector<Vertex> Vertices;
            std::vector<u32> Indices;
        };

        struct PendingInstance
        {
            u32 GeometryIndex = 0;
            u32 MaterialIndex = 0;
            glm::mat4 Transform{ 1.0f };
        };

        // One entry per distinct resolved Material object (pointer identity —
        // the resolution rule hands back stable references into the
        // MaterialComponent / MeshSource / builder default, all of which
        // outlive the walk).
        [[nodiscard]] u32 ResolveMaterialIndex(const Material& material);

        // Shared-geometry path: extract submesh `submeshIndex` of `meshSource`
        // in mesh-local space, once per distinct (MeshSource, submesh) pair.
        // Returns the pending geometry index, or u32(-1) if the submesh has no
        // valid triangles (the failure is cached too, so it warns once).
        [[nodiscard]] u32 GetOrAddSharedGeometry(const MeshSource& meshSource, u32 submeshIndex);

        // Resolved materials, stored directly as ReferenceMaterial. Their
        // LambertianDiffuseOnly stays at its default here — that is a
        // Build-time option, stamped onto every entry when Build() runs.
        std::vector<ReferenceMaterial> m_Materials;
        std::vector<PendingGeometry> m_Geometries;
        std::vector<PendingInstance> m_Instances;
        std::vector<ReferenceLight> m_Lights;

        std::map<const Material*, u32> m_MaterialCache;
        std::map<std::pair<const MeshSource*, u32>, u32> m_GeometryCache;

        // The engine-default PBR material, mirrored from Scene.cpp's (file-
        // static, inaccessible) GetDefaultMaterial: CreatePBR(0.8 grey,
        // metallic 0, roughness 0.5). Owned here so the resolved reference
        // stays valid for the pointer-keyed material cache.
        Ref<Material> m_DefaultMaterial;

        bool m_Consumed = false;
    };
} // namespace OloEngine::PathTracing
