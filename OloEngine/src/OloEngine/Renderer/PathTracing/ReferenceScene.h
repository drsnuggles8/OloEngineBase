#pragma once

// =============================================================================
// ReferenceScene.h — the offline reference path tracer's world (issue #709)
//
// A self-contained, GL-free description of what to trace: triangle geometry
// (each with its own BoundingVolumeHierarchy), instances of that geometry with
// world transforms and materials, punctual lights, and a uniform environment.
// Deliberately NOT the ECS `Scene`: the tracer is a validation instrument that
// must run headless in a unit test with no Application, no asset manager and
// no GPU, and it must be constructible from a handful of literals so a
// Cornell-box fixture is twenty lines rather than a scene file. A separate
// adapter populates one of these from a live `Scene` for raster-vs-reference
// comparisons (ReferenceSceneBuilder.h).
//
// ACCELERATION
// ------------
// Two levels, the usual way round:
//   * Bottom  — `BoundingVolumeHierarchy` per geometry, over its triangle soup.
//               Already existed; its queries are const and thread-safe once
//               built, which is what lets the tracer run wide.
//   * Top     — `ReferenceScene`'s own median-split BVH over instance WORLD
//               AABBs (below). Rays are transformed into instance LOCAL space
//               before descending into the bottom level, so one geometry can be
//               instanced many times without duplicating its triangles.
//
// The bottom-level BVH tightens `ray.TMax` as it finds closer hits, so the
// top-level traversal must feed each instance the distance found SO FAR, and
// must convert that distance across the transform. This implementation keeps
// instance transforms rigid-plus-uniform-scale for exactly that reason — see
// `ReferenceInstance::UniformScale`.
// =============================================================================

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/BoundingVolume.h"
#include "OloEngine/Renderer/BoundingVolumeHierarchy.h"
#include "OloEngine/Renderer/Ray.h"
#include "OloEngine/Renderer/Vertex.h"

#include <glm/glm.hpp>

#include <memory>
#include <vector>

namespace OloEngine::PathTracing
{
    // -------------------------------------------------------------------------
    // Material — the parameter set `CookTorranceBRDF` consumes, plus emission.
    // Textures are deliberately absent: the reference validates TRANSPORT and
    // the BRDF, and a texture fetch would drag in the asset manager and the
    // sampler/mip conventions, which is a separate (and much larger) parity
    // problem. Every consumer here is a factor-only material.
    // -------------------------------------------------------------------------
    struct ReferenceMaterial
    {
        glm::vec3 BaseColor{ 0.8f };
        f32 Metallic = 0.0f;
        f32 Roughness = 1.0f;
        // Emitted radiance in linear units (NOT a colour x intensity pair —
        // the tracer wants radiance directly).
        glm::vec3 Emissive{ 0.0f };
        // Emit from the back face too. Off by default: a one-sided emitter is
        // what a Cornell-box ceiling light is, and a two-sided one silently
        // doubles the light escaping into the room.
        bool TwoSidedEmission = false;

        // DIAGNOSTIC: shade with a bare Lambertian `albedo / pi` instead of
        // PBRCommon's `cookTorranceBRDF`.
        //
        // The reference normally shades with the engine's BRDF — that is the
        // whole point. This flag exists because one engine subsystem does NOT:
        // DDGI's probe radiance cache is deliberately diffuse-only
        // (DDGI_Relight.glsl shades `min(albedo, clamp)/PI * (directE +
        // bounceE)`, no Fresnel, no specular, because a probe hit point has no
        // view direction). Comparing DDGI's output against a cookTorranceBRDF
        // reference would fold that KNOWN modelling difference into the
        // measurement and make the result uninterpretable.
        //
        // So the DDGI parity test renders the reference twice — once with each
        // model — and reports the gap explicitly instead of hiding it. Do not
        // reach for this anywhere else; a reference that quietly uses a
        // different BRDF than the renderer is not a reference.
        bool LambertianDiffuseOnly = false;
    };

    // -------------------------------------------------------------------------
    // Geometry — a triangle soup plus its BVH. Owned by the scene; instances
    // refer to it by index.
    // -------------------------------------------------------------------------
    class ReferenceGeometry
    {
      public:
        ReferenceGeometry(std::vector<Vertex> vertices, std::vector<u32> indices);

        [[nodiscard]] const std::vector<Vertex>& GetVertices() const
        {
            return m_Vertices;
        }
        [[nodiscard]] const std::vector<u32>& GetIndices() const
        {
            return m_Indices;
        }
        [[nodiscard]] const BoundingVolumeHierarchy& GetBVH() const
        {
            return m_BVH;
        }
        [[nodiscard]] BoundingBox GetLocalBounds() const
        {
            return m_LocalBounds;
        }
        [[nodiscard]] u32 GetTriangleCount() const
        {
            return static_cast<u32>(m_Indices.size() / 3);
        }

        // Shading normal at a barycentric point of triangle `triangleIndex`
        // (index-buffer offset / 3), interpolated from the vertex normals.
        // Falls back to the geometric normal when the vertex normals are
        // degenerate — the same failure the raster path guards in
        // `sanitizeSurfaceNormal`, and for the same reason: real imported
        // meshes contain zero-length normals.
        [[nodiscard]] glm::vec3 InterpolateNormal(u32 triangleIndex, f32 u, f32 v) const;

      private:
        std::vector<Vertex> m_Vertices;
        std::vector<u32> m_Indices;
        BoundingVolumeHierarchy m_BVH;
        BoundingBox m_LocalBounds{};
    };

    // -------------------------------------------------------------------------
    // Instance — geometry + world placement + material.
    // -------------------------------------------------------------------------
    struct ReferenceInstance
    {
        u32 GeometryIndex = 0;
        u32 MaterialIndex = 0;
        glm::mat4 Transform{ 1.0f };
        glm::mat4 InverseTransform{ 1.0f };
        // inverse-transpose of the upper 3x3, for shading normals.
        glm::mat3 NormalMatrix{ 1.0f };
        // Uniform scale factor extracted from `Transform`. A ray transformed
        // into local space has its direction RE-NORMALIZED, so local `t` and
        // world `t` differ by exactly this factor; the traversal converts the
        // running TMax through it. Non-uniform scale would make that conversion
        // direction-dependent (there would be no single factor), so
        // `AddInstance` rejects it rather than producing subtly wrong hit
        // distances that still look plausible.
        f32 UniformScale = 1.0f;
        BoundingBox WorldBounds{};
    };

    // -------------------------------------------------------------------------
    // Lights.
    //
    // Directional and Point are DELTA distributions — next-event estimation
    // samples them and BSDF sampling can never hit them, so they need no MIS.
    // Their radiometry mirrors PBRCommon's `calculateLightContribution`
    // exactly (see ReferenceBRDF.h's CalculateAttenuation for why).
    //
    // Area light emission is expressed as an EMISSIVE MATERIAL on real geometry
    // instead of a light type — that is what a Cornell box wants, it is what
    // BSDF sampling can hit, and it keeps the emitter's shape honest rather
    // than approximated the way the raster path's representative-point sphere
    // light necessarily is.
    // -------------------------------------------------------------------------
    enum class ReferenceLightType : u32
    {
        Directional = 0,
        Point = 1,
        Spot = 2
    };

    struct ReferenceLight
    {
        ReferenceLightType Type = ReferenceLightType::Directional;
        // Direction the light TRAVELS (matches LightData::direction, which the
        // shader negates to get L).
        glm::vec3 Direction{ 0.0f, -1.0f, 0.0f };
        glm::vec3 Position{ 0.0f };
        glm::vec3 Color{ 1.0f };
        f32 Intensity = 1.0f;
        // (constant, linear, quadratic, range) — LightData::attenuationParams.
        glm::vec4 AttenuationParams{ 1.0f, 0.09f, 0.032f, 50.0f };
        // (innerCutoff, outerCutoff, falloff, enabled) as cosines.
        glm::vec4 SpotParams{ 0.95f, 0.9f, 1.0f, 1.0f };
    };

    // -------------------------------------------------------------------------
    // Environment — a uniform radiance arriving from every direction (the
    // classic white-furnace setup, and the only environment the reference
    // supports on purpose: a cubemap would re-introduce the sampling/mip
    // conventions this instrument is supposed to sit outside of).
    // -------------------------------------------------------------------------
    struct ReferenceEnvironment
    {
        glm::vec3 Radiance{ 0.0f };
    };

    // -------------------------------------------------------------------------
    // A ray/surface interaction, in world space.
    // -------------------------------------------------------------------------
    struct SurfaceInteraction
    {
        bool Hit = false;
        f32 Distance = 0.0f;
        glm::vec3 Position{ 0.0f };
        // Geometric normal, oriented along the triangle winding (NOT flipped
        // toward the ray — the integrator needs the true orientation to decide
        // whether it is looking at an emitter's front face).
        glm::vec3 GeometricNormal{ 0.0f, 1.0f, 0.0f };
        // Interpolated shading normal, same orientation convention.
        glm::vec3 ShadingNormal{ 0.0f, 1.0f, 0.0f };
        bool FrontFace = false;
        u32 InstanceIndex = 0;
        u32 MaterialIndex = 0;
        u32 TriangleIndex = 0;
    };

    // -------------------------------------------------------------------------
    // One sampleable emissive triangle, in WORLD space. Built by `Build()` from
    // every instance whose material emits.
    // -------------------------------------------------------------------------
    struct EmissiveTriangle
    {
        glm::vec3 V0{ 0.0f };
        glm::vec3 V1{ 0.0f };
        glm::vec3 V2{ 0.0f };
        glm::vec3 Normal{ 0.0f, 1.0f, 0.0f }; // world geometric normal
        f32 Area = 0.0f;
        u32 InstanceIndex = 0;
        u32 MaterialIndex = 0;
        u32 TriangleIndex = 0;
    };

    // =========================================================================
    // ReferenceScene
    // =========================================================================
    class ReferenceScene
    {
      public:
        ReferenceScene() = default;

        // ---- construction ---------------------------------------------------

        u32 AddMaterial(const ReferenceMaterial& material);

        // Takes ownership of the triangle soup and builds its BVH. Returns the
        // geometry index for `AddInstance`.
        u32 AddGeometry(std::vector<Vertex> vertices, std::vector<u32> indices);

        // Convenience: an axis-aligned quad with a constant normal, the
        // building block of every box-shaped reference scene. Corners are given
        // in winding order; the normal is derived from the winding.
        u32 AddQuadGeometry(const glm::vec3& p0, const glm::vec3& p1, const glm::vec3& p2, const glm::vec3& p3);

        // `transform` must be rigid plus UNIFORM scale (see
        // ReferenceInstance::UniformScale). Returns the instance index, or
        // u32(-1) if the transform was rejected or the geometry index is out of
        // range.
        u32 AddInstance(u32 geometryIndex, const glm::mat4& transform, u32 materialIndex);

        void AddLight(const ReferenceLight& light);

        void SetEnvironment(const ReferenceEnvironment& environment)
        {
            m_Environment = environment;
        }

        // Build the top-level acceleration structure and the emissive-triangle
        // list. Must be called after the last Add*(); calling it again rebuilds
        // from scratch.
        void Build();

        [[nodiscard]] bool IsBuilt() const
        {
            return m_Built;
        }

        // ---- queries --------------------------------------------------------

        // Closest hit. `ray.Direction` must be normalized; `outHit` is only
        // meaningful when the call returns true.
        [[nodiscard]] bool Intersect(const Ray& ray, SurfaceInteraction& outHit) const;

        // Any-hit shadow query between two world points, with both ends inset
        // by `epsilon` along the segment so the shading point and the light
        // sample cannot self-shadow.
        [[nodiscard]] bool IsOccluded(const glm::vec3& from, const glm::vec3& to, f32 epsilon = 1e-3f) const;

        // ---- accessors ------------------------------------------------------

        [[nodiscard]] const std::vector<ReferenceMaterial>& GetMaterials() const
        {
            return m_Materials;
        }
        [[nodiscard]] const ReferenceMaterial& GetMaterial(u32 index) const;
        [[nodiscard]] const std::vector<ReferenceInstance>& GetInstances() const
        {
            return m_Instances;
        }
        [[nodiscard]] const std::vector<ReferenceLight>& GetLights() const
        {
            return m_Lights;
        }
        [[nodiscard]] const ReferenceEnvironment& GetEnvironment() const
        {
            return m_Environment;
        }
        [[nodiscard]] const std::vector<EmissiveTriangle>& GetEmissiveTriangles() const
        {
            return m_EmissiveTriangles;
        }
        [[nodiscard]] f32 GetTotalEmissiveArea() const
        {
            return m_TotalEmissiveArea;
        }
        [[nodiscard]] BoundingBox GetWorldBounds() const
        {
            return m_WorldBounds;
        }
        [[nodiscard]] u32 GetGeometryCount() const
        {
            return static_cast<u32>(m_Geometries.size());
        }

        // One next-event-estimation sample on the emissive set.
        struct EmissiveSample
        {
            glm::vec3 Position{ 0.0f };
            glm::vec3 Normal{ 0.0f, 1.0f, 0.0f }; // world geometric normal
            glm::vec3 Radiance{ 0.0f };
            f32 PdfArea = 0.0f; // density w.r.t. surface AREA (== 1 / totalArea)
            bool TwoSided = false;
        };

        // Sample a point uniformly by AREA over the whole emissive set.
        // `xiSelect` picks the triangle (area-proportional), `xiPoint` places
        // the point within it. Returns false when the scene has no emitters.
        [[nodiscard]] bool SampleEmissive(f32 xiSelect, const glm::vec2& xiPoint, EmissiveSample& outSample) const;

        // Area-measure density of having sampled the given emissive triangle
        // point via `SampleEmissive`. Constant across the emitter set (uniform
        // area sampling), but exposed as a function so the MIS weight computed
        // when a BSDF ray *hits* an emitter uses the same expression the NEE
        // side does rather than an independently-derived copy.
        [[nodiscard]] f32 EmissivePdfArea() const;

      private:
        // Flat median-split BVH over instance world AABBs. Same shape as
        // BoundingVolumeHierarchy's node array (right child == left + 1) so the
        // traversal reads the same way.
        struct TLASNode
        {
            BoundingBox Bounds{};
            u32 LeftFirst = 0; // internal: left child index; leaf: first instance ref
            u32 Count = 0;     // 0 == internal node

            [[nodiscard]] bool IsLeaf() const
            {
                return Count > 0;
            }
        };

        void BuildTLAS();
        void BuildEmissiveList();
        void SubdivideTLAS(u32 nodeIndex, u32 depth);
        void UpdateTLASNodeBounds(u32 nodeIndex);

        // Intersect one instance, transforming into local space. `ray` is in
        // WORLD space; `outHit` is filled in world space. `ray.TMax` bounds the
        // search.
        [[nodiscard]] bool IntersectInstance(u32 instanceIndex, const Ray& ray, SurfaceInteraction& outHit) const;
        [[nodiscard]] bool OccludedInstance(u32 instanceIndex, const Ray& ray) const;

        std::vector<ReferenceMaterial> m_Materials;
        std::vector<std::unique_ptr<ReferenceGeometry>> m_Geometries;
        std::vector<ReferenceInstance> m_Instances;
        std::vector<ReferenceLight> m_Lights;
        ReferenceEnvironment m_Environment{};

        std::vector<TLASNode> m_TLASNodes;
        // Instance indices, reordered by the TLAS build (leaves index a range
        // of this array, not of m_Instances directly).
        std::vector<u32> m_TLASInstanceRefs;
        // Centroids of the instance world AABBs, parallel to m_Instances —
        // cached so the split pass does not recompute them per level.
        std::vector<glm::vec3> m_InstanceCentroids;

        std::vector<EmissiveTriangle> m_EmissiveTriangles;
        // Prefix sums of triangle area, for area-proportional selection.
        std::vector<f32> m_EmissiveAreaCdf;
        f32 m_TotalEmissiveArea = 0.0f;

        BoundingBox m_WorldBounds{};
        bool m_Built = false;

        static constexpr u32 s_MaxLeafInstances = 2;
        static constexpr u32 s_MaxDepth = 60;
        static constexpr u32 s_TraversalStackSize = 64;

        // A depth-first traversal pops one node and pushes two, so peak stack
        // occupancy is depth + 1. The relation below is what lets the traversals
        // push unconditionally; without it, raising s_MaxDepth would turn their
        // capacity guard into SILENT geometry loss — rays would simply miss a
        // subtree and the image would be plausible and wrong.
        static_assert(s_TraversalStackSize >= s_MaxDepth + 2,
                      "TLAS traversal stack must hold the deepest descent plus its sibling");
    };
} // namespace OloEngine::PathTracing
