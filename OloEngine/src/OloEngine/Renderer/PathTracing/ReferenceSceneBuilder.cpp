#include "OloEnginePCH.h"

#include "OloEngine/Renderer/PathTracing/ReferenceSceneBuilder.h"

#include "OloEngine/Animation/AnimatedMeshComponents.h"
#include "OloEngine/Renderer/LightCommon.h"
#include "OloEngine/Renderer/Material.h"
#include "OloEngine/Renderer/MeshSource.h"
#include "OloEngine/Renderer/SubmeshMaterialResolve.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Scene.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <tuple>
#include <type_traits>
#include <utility>

namespace OloEngine::PathTracing
{
    namespace
    {
        constexpr u32 kInvalidIndex = std::numeric_limits<u32>::max();

        [[nodiscard]] bool IsFinite(const glm::mat4& m)
        {
            for (glm::length_t column = 0; column < 4; ++column)
            {
                for (glm::length_t row = 0; row < 4; ++row)
                {
                    if (!std::isfinite(m[column][row]))
                        return false;
                }
            }
            return true;
        }

        [[nodiscard]] bool IsFinite(const glm::vec3& v)
        {
            return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
        }

        // Extract one submesh's triangle soup in MESH-LOCAL space.
        //
        // The engine's shared index buffer stores ABSOLUTE vertex indices —
        // the draw path renders with a baseIndex offset only, no baseVertex
        // bias (CommandDispatch::DrawMesh -> DrawBoundIndexed(count,
        // baseIndex)), and the importers emit indices that already include the
        // submesh's vertex offset. So the slice [BaseIndex, BaseIndex +
        // IndexCount) is rebased by -BaseVertex to index the copied vertex
        // window [BaseVertex, BaseVertex + VertexCount).
        //
        // Submesh::m_Transform is deliberately NOT applied — the classic
        // MeshComponent draw path never applies it either (the model matrix
        // submitted is the entity world transform alone), so applying it here
        // would place geometry somewhere the renderer does not.
        //
        // Defensive against imported data: a submesh window that exceeds the
        // arrays fails whole; an individual index outside the submesh's vertex
        // window drops that triangle (warned once per submesh). Returns true
        // iff at least one triangle survived.
        [[nodiscard]] bool ExtractSubmeshTriangles(const MeshSource& meshSource, u32 submeshIndex,
                                                   std::vector<Vertex>& outVertices, std::vector<u32>& outIndices)
        {
            const auto& vertices = meshSource.GetVertices();
            const auto& indices = meshSource.GetIndices();
            const Submesh& submesh = meshSource.GetSubmeshes()[static_cast<i32>(submeshIndex)];

            const u64 vertexEnd = static_cast<u64>(submesh.m_BaseVertex) + submesh.m_VertexCount;
            const u64 indexEnd = static_cast<u64>(submesh.m_BaseIndex) + submesh.m_IndexCount;
            if (submesh.m_VertexCount == 0 || submesh.m_IndexCount < 3 ||
                vertexEnd > static_cast<u64>(vertices.Num()) || indexEnd > static_cast<u64>(indices.Num()))
            {
                OLO_CORE_WARN("ReferenceSceneBuilder: submesh {} has an empty or out-of-range window "
                              "(vertices [{}, {}) of {}, indices [{}, {}) of {}) — skipped",
                              submeshIndex, submesh.m_BaseVertex, vertexEnd, vertices.Num(),
                              submesh.m_BaseIndex, indexEnd, indices.Num());
                return false;
            }

            outVertices.clear();
            outVertices.reserve(submesh.m_VertexCount);
            for (u32 v = 0; v < submesh.m_VertexCount; ++v)
            {
                outVertices.push_back(vertices[static_cast<i32>(submesh.m_BaseVertex + v)]);
            }

            outIndices.clear();
            outIndices.reserve(submesh.m_IndexCount);
            bool warnedOutOfWindow = false;
            for (u32 i = 0; i + 2 < submesh.m_IndexCount; i += 3)
            {
                const u32 i0 = indices[static_cast<i32>(submesh.m_BaseIndex + i + 0)];
                const u32 i1 = indices[static_cast<i32>(submesh.m_BaseIndex + i + 1)];
                const u32 i2 = indices[static_cast<i32>(submesh.m_BaseIndex + i + 2)];

                const auto inWindow = [&submesh, vertexEnd](u32 index)
                {
                    return index >= submesh.m_BaseVertex && static_cast<u64>(index) < vertexEnd;
                };
                if (!inWindow(i0) || !inWindow(i1) || !inWindow(i2))
                {
                    if (!warnedOutOfWindow)
                    {
                        OLO_CORE_WARN("ReferenceSceneBuilder: submesh {} has indices outside its vertex "
                                      "window — dropping the affected triangle(s)",
                                      submeshIndex);
                        warnedOutOfWindow = true;
                    }
                    continue;
                }

                outIndices.push_back(i0 - submesh.m_BaseVertex);
                outIndices.push_back(i1 - submesh.m_BaseVertex);
                outIndices.push_back(i2 - submesh.m_BaseVertex);
            }

            return !outIndices.empty();
        }
    } // namespace

    ReferenceSceneBuilder::ReferenceSceneBuilder()
    {
        // Mirror of Scene.cpp's file-static GetDefaultMaterial(): the material
        // a submesh gets when neither an override nor an imported material
        // exists. Same factors, so a default-shaded raster mesh and its
        // reference twin agree.
        m_DefaultMaterial = Material::CreatePBR("ReferenceSceneBuilderDefault",
                                                glm::vec3(0.8f, 0.8f, 0.8f), 0.0f, 0.5f);
    }

    ReferenceSceneBuilder::~ReferenceSceneBuilder() = default;

    u32 ReferenceSceneBuilder::ResolveMaterialIndex(const Material& material)
    {
        if (const auto it = m_MaterialCache.find(&material); it != m_MaterialCache.end())
        {
            return it->second;
        }

        // Stored directly as ReferenceMaterial; LambertianDiffuseOnly keeps
        // its default until Build() stamps the Build-time option onto it.
        ReferenceMaterial pending;
        pending.BaseColor = glm::vec3(material.GetBaseColorFactor());
        pending.Metallic = material.GetMetallicFactor();
        pending.Roughness = material.GetRoughnessFactor();
        // The PBR shaders emit `u_EmissiveFactor.rgb` (times the emissive map,
        // which the factor-only reference has no twin for) with NO separate
        // intensity multiplier — sampleEmissive() in PBRCommon.glsl — so the
        // factor rgb IS the emitted radiance.
        pending.Emissive = glm::vec3(material.GetEmissiveFactor());
        // A two-sided raster material shades both faces; mirror that for the
        // emitter side of the reference.
        pending.TwoSidedEmission = material.GetFlag(MaterialFlag::TwoSided);
        // Carry the versioned closure across (issue #975) — a v2 material must
        // trace as v2 or the reference silently measures the wrong closure.
        pending.Model = material.GetPBRModel();

        const u32 index = static_cast<u32>(m_Materials.size());
        m_Materials.push_back(pending);
        m_MaterialCache.emplace(&material, index);
        return index;
    }

    u32 ReferenceSceneBuilder::GetOrAddSharedGeometry(const MeshSource& meshSource, u32 submeshIndex)
    {
        const std::pair<const MeshSource*, u32> key{ &meshSource, submeshIndex };
        if (const auto it = m_GeometryCache.find(key); it != m_GeometryCache.end())
        {
            return it->second;
        }

        PendingGeometry geometry;
        u32 index = kInvalidIndex;
        if (ExtractSubmeshTriangles(meshSource, submeshIndex, geometry.Vertices, geometry.Indices))
        {
            index = static_cast<u32>(m_Geometries.size());
            m_Geometries.push_back(std::move(geometry));
        }
        // Failures are cached too, so a degenerate submesh instanced N times
        // warns once instead of N times.
        m_GeometryCache.emplace(key, index);
        return index;
    }

    bool ReferenceSceneBuilder::AddMeshEntity(const Ref<MeshSource>& meshSource, const glm::mat4& worldTransform,
                                              const Material* overrideMaterial)
    {
        if (m_Consumed)
        {
            OLO_CORE_ERROR("ReferenceSceneBuilder::AddMeshEntity on a consumed builder — Build() was already called");
            return false;
        }
        // Same guard shape as Scene.cpp's SubmitMeshSourceClassic: a null or
        // submesh-less source draws nothing, so it traces nothing.
        if (!meshSource || meshSource->GetSubmeshes().IsEmpty())
        {
            return false;
        }
        if (!IsFinite(worldTransform))
        {
            OLO_CORE_WARN("ReferenceSceneBuilder::AddMeshEntity: non-finite world transform — entity skipped");
            return false;
        }

        // Classify with EXACTLY the acceptance test ReferenceScene::AddInstance
        // applies (same 1e-6 floor, same 1e-4 relative tolerance, same
        // triple-product orientation test) so its loud rejection is
        // unreachable from here: transforms that pass go through as instances,
        // everything else takes the pre-transform path.
        const glm::vec3 axisX(worldTransform[0]);
        const glm::vec3 axisY(worldTransform[1]);
        const glm::vec3 axisZ(worldTransform[2]);
        const f32 scaleX = glm::length(axisX);
        const f32 scaleY = glm::length(axisY);
        const f32 scaleZ = glm::length(axisZ);
        const f32 maxScale = std::max({ scaleX, scaleY, scaleZ });
        const f32 minScale = std::min({ scaleX, scaleY, scaleZ });
        const bool uniformScale = (minScale > 1e-6f) && ((maxScale - minScale) <= 1e-4f * maxScale);
        const bool positiveOrientation = glm::dot(glm::cross(axisX, axisY), axisZ) > 0.0f;
        const bool shareable = uniformScale && positiveOrientation;

        glm::mat3 normalMatrix(1.0f);
        bool flipWinding = false;
        if (!shareable)
        {
            // Pre-transforming needs the inverse-transpose for normals, so a
            // (near-)singular basis — a scale collapsed to zero — cannot be
            // represented. It also cannot be rendered meaningfully, so skip.
            const f32 determinant = glm::determinant(glm::mat3(worldTransform));
            if (!(std::abs(determinant) > 1e-12f))
            {
                OLO_CORE_WARN("ReferenceSceneBuilder::AddMeshEntity: near-singular world transform "
                              "(determinant {}) — entity skipped",
                              determinant);
                return false;
            }
            normalMatrix = glm::transpose(glm::inverse(glm::mat3(worldTransform)));
            // A mirrored basis flips triangle winding, and both the emissive
            // list and the intersection code derive outward normals FROM the
            // winding. Swapping two indices per triangle restores agreement
            // between the winding-derived geometric normals and the
            // inverse-transpose-transformed shading normals.
            flipWinding = determinant < 0.0f;
        }

        bool addedAny = false;
        const i32 submeshCount = meshSource->GetSubmeshes().Num();
        for (i32 submeshIndex = 0; submeshIndex < submeshCount; ++submeshIndex)
        {
            // The ONE material-resolution rule every render submission path
            // uses (issue #629): override -> imported-per-submesh -> default.
            const Material& material = ResolveSubmeshMaterial(overrideMaterial, meshSource.get(),
                                                              static_cast<u32>(submeshIndex), *m_DefaultMaterial);
            const u32 materialIndex = ResolveMaterialIndex(material);

            if (shareable)
            {
                const u32 geometryIndex = GetOrAddSharedGeometry(*meshSource, static_cast<u32>(submeshIndex));
                if (geometryIndex == kInvalidIndex)
                {
                    continue;
                }
                m_Instances.push_back({ geometryIndex, materialIndex, worldTransform });
            }
            else
            {
                PendingGeometry geometry;
                if (!ExtractSubmeshTriangles(*meshSource, static_cast<u32>(submeshIndex),
                                             geometry.Vertices, geometry.Indices))
                {
                    continue;
                }

                for (Vertex& vertex : geometry.Vertices)
                {
                    vertex.Position = glm::vec3(worldTransform * glm::vec4(vertex.Position, 1.0f));
                    const glm::vec3 transformedNormal = normalMatrix * vertex.Normal;
                    const f32 lengthSq = glm::dot(transformedNormal, transformedNormal);
                    // A degenerate source normal stays degenerate (zero), so
                    // ReferenceGeometry::InterpolateNormal takes its geometric
                    // fallback — the same guard the raster path applies in
                    // sanitizeSurfaceNormal, for the same reason.
                    vertex.Normal = (lengthSq > 1e-20f) ? transformedNormal * glm::inversesqrt(lengthSq)
                                                        : glm::vec3(0.0f);
                }

                if (flipWinding)
                {
                    for (sizet i = 0; i + 2 < geometry.Indices.size(); i += 3)
                    {
                        std::swap(geometry.Indices[i + 1], geometry.Indices[i + 2]);
                    }
                }

                const u32 geometryIndex = static_cast<u32>(m_Geometries.size());
                m_Geometries.push_back(std::move(geometry));
                m_Instances.push_back({ geometryIndex, materialIndex, glm::mat4(1.0f) });
            }
            addedAny = true;
        }

        return addedAny;
    }

    // -------------------------------------------------------------------------
    // Lights. Every packing below is a transcription of the MultiLight UBO fill
    // in Scene::ProcessScene3DSharedLogic (Scene.cpp) — ReferenceBRDF's
    // CalculateAttenuation / CalculateSpotIntensity are ports of the shader
    // functions that consume that packing, so matching it exactly is what
    // keeps a raster-vs-reference comparison about TRANSPORT rather than about
    // falloff conventions.
    // -------------------------------------------------------------------------

    void ReferenceSceneBuilder::AddDirectionalLight(const DirectionalLightComponent& light)
    {
        if (m_Consumed)
        {
            OLO_CORE_ERROR("ReferenceSceneBuilder::AddDirectionalLight on a consumed builder");
            return;
        }
        // A single NaN/Inf in a light poisons every texel the tracer touches,
        // silently — reject at the seam so both the AddScene walk and direct
        // callers are covered. A raster frame lit by such a light is garbage
        // anyway, so skipping it here costs no meaningful parity.
        if (!IsFinite(light.m_Direction) || !IsFinite(light.m_Color) || !std::isfinite(light.m_Intensity))
        {
            OLO_CORE_WARN("ReferenceSceneBuilder::AddDirectionalLight: non-finite direction {}, "
                          "color {}, or intensity {} — light skipped",
                          light.m_Direction, light.m_Color, light.m_Intensity);
            return;
        }
        // Zero-intensity contributes nothing in either world. `!(x > 0)` also
        // drops negative intensities.
        if (!(light.m_Intensity > 0.0f))
        {
            return;
        }

        ReferenceLight refLight;
        refLight.Type = ReferenceLightType::Directional;
        // The direction the light TRAVELS, exactly as authored — Scene.cpp
        // packs m_Direction into LightData::direction unmodified (no sanitize,
        // no normalize) and the shader negates it to get L, which is what the
        // tracer does too.
        refLight.Direction = light.m_Direction;
        refLight.Position = glm::vec3(0.0f);
        refLight.Color = light.m_Color;
        refLight.Intensity = light.m_Intensity;
        // Scene.cpp: AttenuationParams = (1, 0, 0, 0), SpotParams = 0 for the
        // directional row. Neither is read for a directional light, but the
        // mirror is kept literal so a future consumer inherits the same bits.
        refLight.AttenuationParams = glm::vec4(1.0f, 0.0f, 0.0f, 0.0f);
        refLight.SpotParams = glm::vec4(0.0f);
        m_Lights.push_back(refLight);
    }

    void ReferenceSceneBuilder::AddPointLight(const PointLightComponent& light, const glm::vec3& position)
    {
        if (m_Consumed)
        {
            OLO_CORE_ERROR("ReferenceSceneBuilder::AddPointLight on a consumed builder");
            return;
        }
        // Non-finite rejection at the seam — see AddDirectionalLight.
        if (!IsFinite(position) || !IsFinite(light.m_Color) || !std::isfinite(light.m_Intensity) ||
            !std::isfinite(light.m_Attenuation) || !std::isfinite(light.m_Range))
        {
            OLO_CORE_WARN("ReferenceSceneBuilder::AddPointLight: non-finite position {}, color {}, "
                          "intensity {}, attenuation {}, or range {} — light skipped",
                          position, light.m_Color, light.m_Intensity, light.m_Attenuation, light.m_Range);
            return;
        }
        if (!(light.m_Intensity > 0.0f))
        {
            return;
        }

        ReferenceLight refLight;
        refLight.Type = ReferenceLightType::Point;
        refLight.Position = position;
        refLight.Color = light.m_Color;
        refLight.Intensity = light.m_Intensity;
        // THE point-light packing: (constant=1, linear=0, quadratic=
        // m_Attenuation, range=m_Range) — the way Scene.cpp packs it, and the
        // parameterisation CalculateAttenuation expects.
        refLight.AttenuationParams = glm::vec4(1.0f, 0.0f, light.m_Attenuation, light.m_Range);
        // Unused for a point light; mirrors the UBO row's direction default.
        refLight.Direction = glm::vec3(0.0f, -1.0f, 0.0f);
        refLight.SpotParams = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
        m_Lights.push_back(refLight);
    }

    void ReferenceSceneBuilder::AddSpotLight(const SpotLightComponent& light, const glm::vec3& position)
    {
        if (m_Consumed)
        {
            OLO_CORE_ERROR("ReferenceSceneBuilder::AddSpotLight on a consumed builder");
            return;
        }
        // Non-finite rejection at the seam — see AddDirectionalLight.
        // m_Direction is deliberately NOT rejected: SanitizeSpotLightDirection
        // owns that case (non-finite/zero falls back to -Z, matching
        // Scene.cpp's packing bit-for-bit).
        if (!IsFinite(position) || !IsFinite(light.m_Color) || !std::isfinite(light.m_Intensity) ||
            !std::isfinite(light.m_Attenuation) || !std::isfinite(light.m_Range) ||
            !std::isfinite(light.m_InnerCutoff) || !std::isfinite(light.m_OuterCutoff))
        {
            OLO_CORE_WARN("ReferenceSceneBuilder::AddSpotLight: non-finite position {}, color {}, "
                          "intensity {}, attenuation {}, range {}, or cutoffs ({}, {}) — light skipped",
                          position, light.m_Color, light.m_Intensity, light.m_Attenuation, light.m_Range,
                          light.m_InnerCutoff, light.m_OuterCutoff);
            return;
        }
        if (!(light.m_Intensity > 0.0f))
        {
            return;
        }

        ReferenceLight refLight;
        refLight.Type = ReferenceLightType::Spot;
        refLight.Position = position;
        refLight.Color = light.m_Color;
        refLight.Intensity = light.m_Intensity;
        refLight.AttenuationParams = glm::vec4(1.0f, 0.0f, light.m_Attenuation, light.m_Range);
        // Sanitized by the SAME function Scene.cpp's packing uses
        // (Renderer/LightCommon.h), and passed UNnormalized like the UBO —
        // CalculateSpotIntensity normalizes, same as the shader.
        refLight.Direction = SanitizeSpotLightDirection(light.m_Direction);
        // Cone params as COSINES, inner then outer, falloff 1 — the x/y/z the
        // UBO carries. The UBO's w is the light-type tag (2 = spot), which
        // ReferenceLight expresses through Type instead; its w is the
        // "enabled" convention documented on ReferenceLight::SpotParams.
        refLight.SpotParams = glm::vec4(SpotConeCosine(light.m_InnerCutoff), SpotConeCosine(light.m_OuterCutoff),
                                        1.0f, 1.0f);
        m_Lights.push_back(refLight);
    }

    // -------------------------------------------------------------------------
    // AddScene
    // -------------------------------------------------------------------------

    void ReferenceSceneBuilder::AddScene(Scene& scene, const std::function<bool(Entity)>& includeEntity)
    {
        if (m_Consumed)
        {
            OLO_CORE_ERROR("ReferenceSceneBuilder::AddScene on a consumed builder");
            return;
        }

        // Deterministic gather: EnTT view iteration order depends on pool
        // packing history, which two otherwise-identical scenes (or two runs)
        // need not share. The path tracer's bit-identical contract needs
        // stable geometry AND light ordering (both change the floating-point
        // accumulation order), so everything is sorted by UUID before adding.
        struct GatheredEntity
        {
            u64 SortKey = 0;
            entt::entity Handle = entt::null;
        };
        const auto sortKeyFor = [&scene](entt::entity handle)
        {
            const Entity entity{ handle, &scene };
            // Every Scene-created entity carries an IDComponent; the entt id
            // fallback only exists so a hand-assembled registry cannot crash
            // the walk.
            return entity.HasComponent<IDComponent>() ? static_cast<u64>(entity.GetComponent<IDComponent>().ID)
                                                      : static_cast<u64>(std::to_underlying(handle));
        };
        const auto byKey = [](const GatheredEntity& a, const GatheredEntity& b)
        {
            // Handle as tie-break keeps the order strict even under a UUID
            // collision.
            return std::tie(a.SortKey, a.Handle) < std::tie(b.SortKey, b.Handle);
        };

        // ---- geometry -------------------------------------------------------
        {
            std::vector<GatheredEntity> meshEntities;
            auto view = scene.GetAllEntitiesWith<TransformComponent, MeshComponent>();
            for (const auto entityHandle : view)
            {
                Entity entity{ entityHandle, &scene };
                // Skinned entities belong to the animated path (and GI capture
                // deliberately excludes them — receive-only dynamics, ADR
                // 0007); mirror the classic mesh loop's skip.
                if (entity.HasComponent<SkeletonComponent>())
                {
                    continue;
                }
                if (!view.get<MeshComponent>(entityHandle).m_MeshSource)
                {
                    continue;
                }
                // The caller decides what "static" means; an empty predicate
                // includes everything.
                if (includeEntity && !includeEntity(entity))
                {
                    continue;
                }
                meshEntities.push_back({ sortKeyFor(entityHandle), entityHandle });
            }
            std::sort(meshEntities.begin(), meshEntities.end(), byKey);

            for (const GatheredEntity& gathered : meshEntities)
            {
                Entity entity{ gathered.Handle, &scene };
                const auto& mesh = entity.GetComponent<MeshComponent>();
                // A MaterialComponent overrides EVERY submesh, mirroring the
                // classic mesh loop; per-submesh imported materials apply
                // otherwise, inside AddMeshEntity.
                const Material* overrideMaterial = entity.HasComponent<MaterialComponent>()
                                                       ? &entity.GetComponent<MaterialComponent>().m_Material
                                                       : nullptr;
                AddMeshEntity(mesh.m_MeshSource, scene.GetWorldTransform(gathered.Handle), overrideMaterial);
            }
        }

        // ---- lights ---------------------------------------------------------
        //
        // Gathered per type in the order Scene.cpp fills the MultiLight UBO
        // (directional, point, spot), each type sorted by UUID. The positions
        // handed over are the raw TransformComponent::Translation — the value
        // the raster light collection packs; it does NOT compose the parent
        // hierarchy, so neither does the reference (light-model parity beats
        // "correctness" here, by the §4 rule).
        {
            // One shape shared by all three per-type loops: view over
            // (TransformComponent, LightComponent), UUID-sort, add in sorted
            // order. The translation handed to `addOne` is the raw
            // TransformComponent::Translation (see the note above); the
            // directional callback ignores it, mirroring Scene.cpp's packing.
            const auto addLightsOfType = [&]<typename LightComponent>(std::type_identity<LightComponent>,
                                                                      auto&& addOne)
            {
                std::vector<GatheredEntity> lights;
                auto view = scene.GetAllEntitiesWith<TransformComponent, LightComponent>();
                for (const auto entityHandle : view)
                {
                    lights.push_back({ sortKeyFor(entityHandle), entityHandle });
                }
                std::sort(lights.begin(), lights.end(), byKey);
                for (const GatheredEntity& gathered : lights)
                {
                    addOne(view.template get<LightComponent>(gathered.Handle),
                           view.template get<TransformComponent>(gathered.Handle).Translation);
                }
            };

            // Type order is part of the packing contract: directional, then
            // point, then spot — the order Scene.cpp fills the MultiLight UBO.
            addLightsOfType(std::type_identity<DirectionalLightComponent>{},
                            [this](const DirectionalLightComponent& light, const glm::vec3&)
                            { AddDirectionalLight(light); });
            addLightsOfType(std::type_identity<PointLightComponent>{},
                            [this](const PointLightComponent& light, const glm::vec3& position)
                            { AddPointLight(light, position); });
            addLightsOfType(std::type_identity<SpotLightComponent>{},
                            [this](const SpotLightComponent& light, const glm::vec3& position)
                            { AddSpotLight(light, position); });
        }
    }

    // -------------------------------------------------------------------------
    // Build
    // -------------------------------------------------------------------------

    ReferenceScene ReferenceSceneBuilder::Build(const ReferenceSceneBuildOptions& options)
    {
        ReferenceScene scene;
        if (m_Consumed)
        {
            OLO_CORE_ERROR("ReferenceSceneBuilder::Build called twice — the builder is single-use; "
                           "returning an empty scene");
            scene.Build();
            return scene;
        }
        m_Consumed = true;

        for (ReferenceMaterial& material : m_Materials)
        {
            // LambertianDiffuseOnly is a Build-time option, not a per-material
            // property here — stamp it on the way out (the builder is consumed
            // anyway, so mutating in place is fine).
            material.LambertianDiffuseOnly = options.LambertianDiffuseOnly;
            scene.AddMaterial(material);
        }

        for (PendingGeometry& pending : m_Geometries)
        {
            scene.AddGeometry(std::move(pending.Vertices), std::move(pending.Indices));
        }

        for (const PendingInstance& pending : m_Instances)
        {
            // Unreachable by construction: AddMeshEntity classified every
            // transform with AddInstance's own acceptance test. Kept loud
            // anyway — a silent drop here would be a hole in the world that
            // reads as a transport bug.
            if (scene.AddInstance(pending.GeometryIndex, pending.Transform, pending.MaterialIndex) == kInvalidIndex)
            {
                OLO_CORE_ERROR("ReferenceSceneBuilder::Build: ReferenceScene rejected an instance the "
                               "builder classified as acceptable (geometry {}, material {})",
                               pending.GeometryIndex, pending.MaterialIndex);
            }
        }

        for (const ReferenceLight& light : m_Lights)
        {
            scene.AddLight(light);
        }

        ReferenceEnvironment environment;
        environment.Radiance = options.EnvironmentRadiance;
        scene.SetEnvironment(environment);

        scene.Build();
        return scene;
    }
} // namespace OloEngine::PathTracing
