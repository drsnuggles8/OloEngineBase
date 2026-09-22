#include "OloEnginePCH.h"
#include "OloEngine/Animation/Skeleton.h"
#include "OloEngine/Renderer/GltfPhysicalMaterial.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/Renderer3DInternal.h"
#include "OloEngine/Renderer/Renderer3DDrawHelpers.h"
#include "OloEngine/Renderer/Instancing/GPUFrustumCuller.h"
#include "OloEngine/Renderer/MeshSource.h"
#include "OloEngine/Renderer/Occlusion/OcclusionCuller.h"
#include "OloEngine/Renderer/Occlusion/OcclusionQueryPool.h"
#include "OloEngine/Renderer/Occlusion/OcclusionState.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/SkinLayeredSpecular.h"
#include "OloEngine/Renderer/SkinOcularSurface.h"
#include "OloEngine/Renderer/SkinOralSurface.h"
#include "OloEngine/Renderer/SkinTransmission.h"
#include "OloEngine/Renderer/SubmeshMaterialResolve.h"
#include "OloEngine/Renderer/Commands/CommandDispatch.h"
#include "OloEngine/Renderer/Commands/DrawKey.h"
#include "OloEngine/Renderer/Commands/FrameDataBuffer.h"
#include "OloEngine/Renderer/Commands/RenderCommand.h"
#include "OloEngine/Renderer/VirtualGeometry/VirtualMeshRegistry.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Containers/Array.h"
#include "OloEngine/Task/ParallelFor.h"

#include <algorithm>
#include <numeric>

#include <atomic>

namespace
{
    // THE ONE PLACE the "transmission has no G-Buffer representation" rule lives
    // (issue #970).
    //
    // The G-Buffer carries albedo/metallic, normal/roughness/AO, emissive/flags,
    // velocity, entity-ID and baked GI — and nothing that could hold a
    // transmission factor, an IOR, a thickness or an extinction coefficient. A
    // transmissive material written into it comes back out of
    // DeferredLightingPass as an ordinary opaque surface.
    //
    // So it is rerouted exactly the way a forward-only shader override already
    // is: ForwardOverlayPass binds the scene framebuffer and runs AFTER the
    // deferred composite, so the forward PBR shader — which does have the
    // closure — shades the surface over the finished deferred image, and
    // Deferred and Forward show the same glass.
    //
    // WHY IT IS A FUNCTION RATHER THAN REPEATED AT EACH SITE. Five submission
    // paths choose a shader — DrawMesh, SelectInstancedShaderRouting,
    // DrawAnimatedMesh and DrawMeshParallel — and every
    // one of them has to make the SAME decision, or a transmissive material
    // renders opaque on whichever path a given scene happens to take. The first
    // cut of this feature wrote the rule inline at two of the five and left the
    // skinned and parallel paths silently opaque; that is the same class of
    // divergence issue #515's routing bug came from.
    //
    // Returns true when the caller must shade with `forwardShader` and reroute
    // the draw to ForwardOverlayPass. When there is nowhere correct to send it,
    // the draw is COUNTED and warned once rather than quietly shaded opaque
    // (CLAUDE.md, no silent fallbacks).
    [[nodiscard]] bool ShouldRerouteTransmissiveToForwardOverlay(const OloEngine::Material& material, bool deferred,
                                                                 bool hasForwardOverlayPass,
                                                                 const OloEngine::Ref<OloEngine::Shader>& forwardShader)
    {
        if (!deferred || !material.IsTransmissive())
            return false;

        if (hasForwardOverlayPass && forwardShader)
            return true;

        OloEngine::NoteTransmissiveDrawWithoutForwardOverlay();
        return false;
    }
} // namespace

namespace OloEngine
{
    namespace
    {
        // World-space slack a morph target set can add to a mesh's rest-pose bounds.
        //
        // Culling an animated mesh has always padded the rest-pose sphere by a blanket
        // 2x for skinning. Morph deformation is a SECOND source of motion the rest
        // bounds know nothing about, and it is one the CPU morph pass bakes straight
        // into the vertex buffer without recomputing bounds — so a strongly displaced
        // expression can leave the sphere entirely and pop out of frame at the screen
        // edge. MorphTargetSet::GetMaxDisplacement is the conservative object-space
        // bound (weights are clamped to [0,1] and combine additively, so no vertex can
        // move further than the sum of the per-target maxima); this converts it to
        // world space with the same max-axis scale BoundingSphere::Transform uses, so
        // the two agree about what "scale" means (#1227).
        [[nodiscard]] f32 MorphBoundsSlack(const Ref<Mesh>& mesh, const glm::mat4& modelMatrix)
        {
            if (!mesh)
                return 0.0f;
            // By value: Mesh::GetMeshSource() returns a Ref by value.
            const Ref<MeshSource> source = mesh->GetMeshSource();
            if (!source || !source->HasMorphTargets())
                return 0.0f;

            const f32 displacement = source->GetMorphTargets()->GetMaxDisplacement();
            if (!std::isfinite(displacement) || displacement <= 0.0f)
                return 0.0f;

            const f32 maxScale = glm::max(glm::max(glm::length(glm::vec3(modelMatrix[0])),
                                                   glm::length(glm::vec3(modelMatrix[1]))),
                                          glm::length(glm::vec3(modelMatrix[2])));
            const f32 slack = displacement * maxScale;
            return std::isfinite(slack) ? slack : 0.0f;
        }
    } // namespace

    namespace
    {
        // Stable per-draw debug label for frame capture / olo_perf_capture_frame:
        // the submesh's node (or mesh) name. Returns a pointer into the
        // MeshSource's own strings — valid for the mesh's lifetime, which spans
        // the frame the packet lives in (PacketMetadata::m_DebugName is a
        // non-owning const char*).
        const char* GetMeshDebugName(const Ref<Mesh>& mesh)
        {
            if (!mesh || !mesh->GetMeshSource())
                return nullptr;

            const auto& submeshes = mesh->GetMeshSource()->GetSubmeshes();
            const u32 submeshIndex = mesh->GetSubmeshIndex();
            if (submeshIndex >= static_cast<u32>(submeshes.Num()))
                return nullptr;

            const Submesh& submesh = submeshes[submeshIndex];
            // UE spells the raw-buffer accessor `*Str`; it always returns a
            // valid null-terminated pointer, and the storage outlives the call
            // because `submesh` is a reference into the MeshSource's array.
            if (!submesh.m_NodeName.IsEmpty())
                return *submesh.m_NodeName;
            if (!submesh.m_MeshName.IsEmpty())
                return *submesh.m_MeshName;
            return nullptr;
        }
    } // namespace

    auto Renderer3D::ValidateDrawMeshResources(const char* context, const RHI::ResourceHandle vertexArray,
                                               const RHI::ResourceHandle shader) -> bool
    {
        if (vertexArray.IsValid() && shader.IsValid())
            return true;

        if (static std::atomic<u64> s_InvalidResourceWarnCount{ 0 }; s_InvalidResourceWarnCount.fetch_add(1, std::memory_order_relaxed) < 1)
        {
            // The handles format as #Index:Generation, which is more useful here
            // than a driver name was: a <null> tells you the producer never
            // minted, a stale one tells you it was retired underneath the draw.
            OLO_CORE_WARN("{}: Dropping draw with invalid resources (VAO={}, Shader={})",
                          context, vertexArray, shader);
        }

        return false;
    }

    bool Renderer3D::IsDeferredCapableShader(const Ref<Shader>& shader)
    {
        if (!shader)
            return false;
        // Primary path: ask the shader itself. `Shader::IsDeferredCapable()`
        // is populated by the backend's reflection pass (for OpenGL, at
        // Reflect() time by scanning the fragment stage's declared outputs
        // for the G-Buffer marker names — see `OpenGLShader::Reflect`).
        // This correctly classifies CUSTOM shaders that weren't loaded via
        // the built-in handle set, as long as they follow the engine's
        // opt-in naming convention (o_GBuffer* / gAlbedo / gNormalRoughAO /
        // gEmissive).
        if (shader->IsDeferredCapable())
            return true;

        // Compatibility shim: some built-in shaders are cached in s_Data
        // and may be queried before their reflection has run (e.g. during
        // engine startup, before the first Bind()/EnsureLinked()). Fall
        // back to identity comparison against the known deferred handle
        // set so those queries don't misclassify a not-yet-reflected shader
        // as forward-only.
        //
        // Built-in shaders can exist without an asset registration. Their
        // object identity is authoritative even when the asset handle is zero.
        // Registered aliases may also match by nonzero AssetHandle; never
        // equate two unrelated unregistered shaders through their zero handles.
        const u64 handle = static_cast<u64>(shader->GetHandle());
        const auto matches = [&shader, handle](const Ref<Shader>& candidate)
        {
            return candidate && (candidate == shader ||
                                 (handle != 0 && static_cast<u64>(candidate->GetHandle()) == handle));
        };
        return matches(s_Data.PBRGBufferShader) || matches(s_Data.PBRGBufferSkinnedShader) ||
               matches(s_Data.SkyboxGBufferShader) || matches(s_Data.LightCubeGBufferShader) ||
               matches(s_Data.InfiniteGridGBufferShader) || matches(s_Data.TerrainGBufferShader) ||
               matches(s_Data.VoxelGBufferShader) || matches(s_Data.VoxelGreedyGBufferShader) ||
               matches(s_Data.FoliageGBufferShader) ||
               matches(s_Data.DecalGBufferShader) || matches(s_Data.DecalGBufferNormalShader) ||
               matches(s_Data.DecalGBufferRMAShader) || matches(s_Data.DecalGBufferEmissiveShader);
    }

    namespace
    {
        // Stage ONE virtual-mesh part's ray-tracing proxy into the canonical
        // GPU Scene (issue #1144), and count the outcome either way.
        //
        // The material record is resolved through the SAME two calls the
        // classic path makes (Scene.cpp's StageGPUSceneSubmesh), from the same
        // (override, imported) pair the raster material above was resolved
        // from — so the proxy a ray hits shades with the material the raster
        // path drew, and a mesh shared with a classic MeshComponent entity
        // resolves to one shared material record rather than two.
        void StageVirtualProxy(VirtualMeshRegistry& registry, u32 entryIndex, u32 partIndex, AssetHandle meshHandle,
                               const Ref<MeshSource>& meshSource, u32 submeshIndex, u64 stableEntityId,
                               const glm::mat4& modelMatrix, const Material* overrideMaterial,
                               const Material& resolvedMaterial, bool castShadows,
                               VirtualMeshRegistry::SubmissionDiagnostics& diagnostics)
        {
            if (!registry.EnsureProxyGeometry(entryIndex))
            {
                // Counted here rather than inside the registry: the registry
                // has no idea whether anyone wanted to trace this. The GPU
                // Scene category is reported by ExtractGPUSceneVirtualProxy on
                // the paths that reach it; this branch never does, so it
                // reports it itself and the two cannot double-count.
                ++diagnostics.ProxylessParts;
                Renderer3D::ReportUnsupportedGPUScene(GPUSceneUnsupportedCategory::Virtualized);
                return;
            }

            const VirtualMeshRegistry::MeshEntry& entry = registry.GetEntry(entryIndex);
            const GPUSceneMaterialKey materialKey =
                Renderer3D::ResolveGPUSceneMaterialKey(overrideMaterial, stableEntityId, meshSource, submeshIndex);
            Renderer3D::ExtractGPUSceneMaterial(materialKey, resolvedMaterial);

            // The registry's own caster rule, applied to the proxy so the two
            // agree: an alpha-masked part does not cast a raster shadow either
            // (a cutout leaf would project as a solid quad), and it must not
            // cast a ray-traced one for the same reason.
            const bool proxyCastsShadow = castShadows && resolvedMaterial.GetAlphaMode() != AlphaMode::Mask;
            const bool staged = Renderer3D::ExtractGPUSceneVirtualProxy(
                stableEntityId, partIndex, entry.ProxyVertexBuffer, entry.ProxyIndexBuffer,
                static_cast<u32>(entry.Proxy.Indices.Num()), static_cast<u32>(entry.Proxy.Vertices.Num()),
                modelMatrix, materialKey, proxyCastsShadow);
            if (staged)
            {
                ++diagnostics.ProxyParts;
            }
            else
            {
                ++diagnostics.ProxylessParts;
            }

            // An emissive virtual mesh is hit by rays and shades correctly, but
            // the path tracer's next-event estimation will not sample it as an
            // area light — the emissive table gathers triangles from a
            // MeshSource submesh, and those are the full-resolution triangles,
            // not the proxy the TLAS holds. Warn once per mesh: this is a
            // permanent property of the asset and this runs per instance per
            // frame.
            const glm::vec3 emissive(resolvedMaterial.GetEmissiveFactor());
            if (std::max({ emissive.x, emissive.y, emissive.z }) > 0.0f)
            {
                static std::unordered_set<u64> s_WarnedEmissiveVirtualMeshes;
                if (s_WarnedEmissiveVirtualMeshes.insert(static_cast<u64>(meshHandle)).second)
                {
                    OLO_CORE_WARN_TAG("Renderer3D",
                                      "virtual mesh {:x} has an EMISSIVE material. Its ray-tracing proxy is in the "
                                      "TLAS, so rays hit it and it shades correctly, but the path tracer's "
                                      "area-light table does not gather virtual geometry (issue #1144) — it will "
                                      "not be sampled by next-event estimation. Warned once per mesh.",
                                      static_cast<u64>(meshHandle));
                }
            }
        }
    } // namespace

    bool Renderer3D::SubmitVirtualMesh(AssetHandle meshHandle, const Ref<MeshSource>& meshSource,
                                       const glm::mat4& modelMatrix, const Material* overrideMaterial,
                                       const Material& defaultMaterial, i32 entityID, u64 stableEntityId,
                                       f32 errorThresholdPixels, bool castShadows,
                                       const glm::vec4& lightmapScaleOffset,
                                       std::span<const glm::mat4> boneMatrices,
                                       std::span<const glm::mat4> prevBoneMatrices)
    {
        OLO_PROFILE_FUNCTION();

        if (static_cast<u64>(meshHandle) == 0 || !meshSource)
        {
            return false;
        }

        auto& registry = VirtualMeshRegistry::Get();
        if (!registry.IsRegistered(meshHandle) && !registry.RegisterMeshSource(meshHandle, *meshSource))
        {
            return false; // unsupported source — warned once at registration
        }

        VirtualMeshRegistry::MeshParts const parts = registry.FindParts(meshHandle);
        if (!parts.Valid)
        {
            return false;
        }

        VirtualMeshSubmission submission;
        submission.Mesh = meshHandle;
        submission.Transform = modelMatrix;
        submission.PrevTransform = GetAndRecordPrevTransform(entityID, modelMatrix);
        submission.EntityID = entityID;
        if (!std::isfinite(errorThresholdPixels))
        {
            errorThresholdPixels = 1.0f;
        }
        submission.ErrorThresholdPixels = std::clamp(errorThresholdPixels, 0.05f, 64.0f);
        submission.CastShadows = castShadows;

        // Bone palette (issue #1150), and ONLY when this mesh's cooked DAG
        // actually carries the skin bindings the shaders would read — the same
        // cook-vs-runtime disagreement the lightmap region below guards against,
        // with a sharper failure: publishing a palette for a rigid cook would
        // deform the cull's BOUNDS while the vertices stayed in the rest pose,
        // i.e. geometry culled against a volume it is not in. The registry warns
        // once per mesh when it sees that, so the drop is not silent.
        if (!boneMatrices.empty() && registry.MeshIsSkinned(meshHandle))
        {
            submission.BoneMatrices.Append(boneMatrices.data(), static_cast<i32>(boneMatrices.size()));
            submission.PrevBoneMatrices.Append(prevBoneMatrices.data(), static_cast<i32>(prevBoneMatrices.size()));
        }
        else if (!boneMatrices.empty())
        {
            // A skeleton posed this entity and NO part of its cooked DAG carries
            // a skin binding: the cook predates issue #1150. The mesh renders in
            // its rest pose, which is a character standing in a T-pose while its
            // skeleton animates — obviously wrong on screen, but with nothing
            // anywhere to say why.
            //
            // Warn-once per mesh: the condition is permanent until the DAG is
            // re-cooked and this runs per instance per frame, so an unmemoized
            // warning fills OloEngine.log at frame rate and buries every other
            // diagnostic — the same failure the lightmap warning below avoids.
            static std::unordered_set<u64> s_WarnedRigidCookSkinnedMeshes;
            if (s_WarnedRigidCookSkinnedMeshes.insert(static_cast<u64>(meshHandle)).second)
            {
                OLO_CORE_WARN_TAG("Renderer3D",
                                  "virtual mesh {:x} is submitted with a bone palette but no part of its cooked "
                                  "cluster DAG carries a skinning payload — it renders in its REST POSE through the "
                                  "virtual path. The cook predates issue #1150; re-cook it (touch the source, or "
                                  "delete its .omesh cache entry). Warned once per asset.",
                                  static_cast<u64>(meshHandle));
            }
        }

        // Baked lightmap region (issue #867), but ONLY when this mesh's cooked
        // DAG actually carries the uv2 stream the shader would read.
        //
        // The two can genuinely disagree: the cluster DAG is cooked when the
        // mesh is first registered, while the unwrap that creates uv2 happens
        // at bake time. A cook that predates its unwrap leaves a valid-looking
        // region pointing into an arena tail that holds some other mesh's
        // charts — a wrong-address read that renders as a plausible patch of
        // light rather than as anything obviously broken. Dropping the region
        // instead degrades to "no baked GI", which is loud in a comparison and
        // silent in a frame, and is the only safe direction.
        if (registry.MeshHasLightmapUVs(meshHandle))
        {
            submission.LightmapScaleOffset = lightmapScaleOffset;
        }
        else if (lightmapScaleOffset.x > 0.0f)
        {
            // Warn-once per mesh: this condition is PERMANENT until the DAG is
            // re-cooked, and this runs per instance per frame — an unmemoized
            // warning here fills OloEngine.log at frame rate and buries every
            // other diagnostic, which is the failure the same pattern in
            // Scene.cpp's virtual loop already exists to avoid.
            static std::unordered_set<u64> s_WarnedUncookedLightmapMeshes;
            if (s_WarnedUncookedLightmapMeshes.insert(static_cast<u64>(meshHandle)).second)
            {
                OLO_CORE_WARN_TAG("Renderer3D",
                                  "virtual mesh {:x} has a baked lightmap region but its cooked cluster DAG "
                                  "carries no UV2 stream — the cook predates the bake's unwrap, so this mesh "
                                  "falls back to probes/IBL. Re-bake the scene to re-cook it.",
                                  static_cast<u64>(meshHandle));
            }
        }

        // Ray-tracing proxy coverage is counted PER PART, beside the rest of the
        // virtual-geometry submission diagnostics, so "N of M virtual parts are
        // in the TLAS" is answerable from one place (issue #1144).
        //
        // Skipped WHOLE when nobody is extracting — the same rule
        // ExtractGPUSceneMesh applies. Staging and counting are one decision
        // here: counting a part as proxyless because no extraction was open
        // would report a fault where there is only an idle frame.
        auto& vgDiagnostics = registry.GetMutableSubmissionDiagnostics();
        const bool stageProxies = s_Data.GPUSceneExtractionActive;

        // One material slot per part. Precedence: an explicit MaterialComponent overrides
        // everything, else the material the SUBMESH was imported with (so a multi-material
        // mesh like Sponza shades each part correctly), else the caller's default.
        submission.MaterialDataIndices.Reserve(parts.Count);
        submission.PartAlphaMasked.Reserve(parts.Count);
        submission.PartTwoSided.Reserve(parts.Count);
        for (u32 partIndex = 0; partIndex < parts.Count; ++partIndex)
        {
            const auto& entry = registry.GetEntry(parts.FirstEntry + partIndex);

            const Material& resolved = ResolveSubmeshMaterial(overrideMaterial, meshSource.get(), entry.SubmeshIndex, defaultMaterial);
            const Material* material = &resolved;

            PODMaterialData const materialData = CreatePODMaterialDataForMaterial(*material, RHI::NullResource);
            submission.MaterialDataIndices.Add(FrameDataBufferManager::Get().AllocateMaterialData(materialData));

            // Anything that is not fully opaque needs the cutout/blend test, which only the
            // hardware fragment shader can run — flag it so the cull keeps it off the compute
            // rasterizer (VirtualInstanceGpuRecord::kFlagAlphaMasked).
            submission.PartAlphaMasked.Add(material->GetAlphaMode() != AlphaMode::Opaque ? 1u : 0u);

            // Two-sided geometry (foliage sheets) must not be backface-culled — the classic
            // path does the same in Renderer3DDrawHelpers::BuildRenderState.
            submission.PartTwoSided.Add(material->GetFlag(MaterialFlag::TwoSided) ? 1u : 0u);

            if (stageProxies)
            {
                // AlphaMode::Blend is the one part kind the virtual raster path
                // REFUSES: VirtualMeshRegistry::PrepareFrame skips it, because
                // the deferred G-Buffer has nowhere to put a blended fragment
                // and drawing it opaque is worse than not drawing it. Staging a
                // proxy for it anyway would put geometry in the TLAS that is on
                // no screen — an invisible caster, which is precisely the
                // failure the Scene loop's Forward/Forward+ gate exists to
                // prevent. It is counted and reported like any other part that
                // could not be represented.
                //
                // The predicate is the same one PrepareFrame applies, read off
                // the SAME resolved Material rather than off the round-tripped
                // PODMaterialData copy. If one of them ever moves, the other
                // has to move with it or a Blend part is traced without being
                // drawn again.
                //
                // A SKINNED part is refused for a different reason and with the
                // same outcome (issue #1150). The proxy is the DAG's coarsest
                // cut baked once at registration — it is fixed geometry, which
                // is exactly why it can be built once and never refitted. A
                // deforming mesh has no fixed geometry, so the proxy would be
                // the REST POSE: a T-posed character casting ray-traced shadows
                // and appearing in reflections while the raster path draws it
                // mid-stride. Wrong geometry in the TLAS is worse than none,
                // because none is counted and this would not be.
                //
                // It leaves skinned virtual geometry outside the TLAS, which is
                // where every other skinned mesh already is — though for a
                // different reason since #1228. A classic skinned mesh now DOES
                // reach the canonical GPU Scene; RayTracingScene::Classify
                // refuses it there, on this same argument, because its record
                // describes a rest surface and no deformed one exists yet.
                // #1144 is the issue that owns closing this for virtual
                // geometry; #1229 owns it for classic meshes.
                if (material->GetAlphaMode() == AlphaMode::Blend || submission.IsSkinned())
                {
                    ++vgDiagnostics.ProxylessParts;
                    ReportUnsupportedGPUScene(GPUSceneUnsupportedCategory::Virtualized);
                }
                else
                {
                    StageVirtualProxy(registry, parts.FirstEntry + partIndex, partIndex, meshHandle, meshSource,
                                      entry.SubmeshIndex, stableEntityId, modelMatrix, overrideMaterial, *material,
                                      castShadows, vgDiagnostics);
                }
            }
        }

        registry.Submit(submission);
        return true;
    }

    auto Renderer3D::CreatePODMaterialDataForMaterial(const Material& material, RHI::ResourceHandle shaderRendererID) -> PODMaterialData
    {
        PODMaterialData data{};
        data.shaderRendererID = shaderRendererID;

        // Legacy material properties.
        data.ambient = material.GetAmbient();
        data.diffuse = material.GetDiffuse();
        data.specular = material.GetSpecular();
        data.shininess = material.GetShininess();
        data.useTextureMaps = material.IsUsingTextureMaps();
        data.diffuseMapID = material.GetDiffuseMap() ? material.GetDiffuseMap()->GetRHIHandle() : RHI::NullResource;
        data.specularMapID = material.GetSpecularMap() ? material.GetSpecularMap()->GetRHIHandle() : RHI::NullResource;

        // PBR material properties.
        data.enablePBR = (material.GetType() == MaterialType::PBR);
        data.baseColorFactor = material.GetBaseColorFactor();
        data.emissiveFactor = material.GetEmissiveFactor();
        data.metallicFactor = material.GetMetallicFactor();
        data.roughnessFactor = material.GetRoughnessFactor();
        data.normalScale = material.GetNormalScale();
        data.occlusionStrength = material.GetOcclusionStrength();
        data.enableIBL = material.IsIBLEnabled();
        data.alphaMode = std::to_underlying(material.GetAlphaMode());
        data.alphaCutoff = material.GetAlphaCutoff();
        data.pbrModel = std::to_underlying(material.GetPBRModel());

        // Material kind + skin profile (issue #1231). The KIND is the material's
        // own field; the PROFILE is RESOLVED here, once per submission, into the
        // small slot the G-Buffer can name per pixel plus the parameters this
        // pass needs — so nothing downstream of submission touches the asset
        // manager, and a missing or unloadable profile is reported and counted
        // at exactly one site (SkinProfileTable::Resolve) rather than becoming a
        // silent neutral default in the shader.
        //
        // Non-skin materials do not consult the table at all: they keep
        // kSkinProfileSlotNone and a neutral tint, so their uploaded bytes are
        // identical to what they were before this feature existed.
        data.materialKind = std::to_underlying(material.GetMaterialKind());
        if (material.GetMaterialKind() == MaterialKind::Skin)
        {
            // A LEGACY (Phong) material has no skin transport to reach: the skin
            // lanes live in PBRMaterialUBO, and CommandDispatch only uploads
            // that block when enablePBR is set, so a legacy skin material would
            // take the legacy UBO and lose the kind, the profile and the tint
            // with nothing to show for it. Reported, counted, and DOWNGRADED to
            // Generic here so the kind this struct carries is the kind that will
            // actually be transported — a silent Skin that shades generic is
            // exactly the failure this whole file argues against.
            if (!data.enablePBR)
            {
                Renderer3D::GetSkinProfileTable().ReportFallback(SkinProfileFallbackReason::MaterialNotPBR,
                                                                 material.GetSkinProfileHandle());
                data.materialKind = std::to_underlying(MaterialKind::Generic);
            }
            else
            {
                const SkinProfileResolution profile = Renderer3D::GetSkinProfileTable().Resolve(material.GetSkinProfileHandle());
                data.skinProfileSlot = profile.Slot;
                data.skinSpecularTint = profile.Parameters.SpecularTint;
                data.skinEvaluationModel = std::to_underlying(profile.Parameters.EvaluationModel);

                // THIN-REGION TRANSMISSION (issue #1242). The lanes are packed
                // here, once per submission, for the same reason the tint is
                // resolved here: the Burley albedo fit behind
                // SkinTransmissionScalingLane is a physical decision, and
                // Renderer/SkinTransmission.h's opening rule puts those on the
                // CPU where a test can look at them.
                //
                // ONLY AT TRANSPORT VERSION 2, and the branch is here rather
                // than only in the shader so a version-1 profile does not even
                // upload a lobe. A version this code has no arm for leaves the
                // lanes zero, which shades as no transmission.
                // THE LAYERED SURFACE RESPONSE (issue #1243). Packed here for
                // the reason the transmission lanes are: the deferred path's
                // per-frame table packs the SAME lane with the SAME function,
                // so the two cannot disagree about the order of its four
                // numbers.
                //
                // ONLY AT TRANSPORT VERSION 3, and the branch is here rather
                // than only in the shader so a version-2 profile does not even
                // upload a lobe. A version this code has no arm for leaves the
                // lane zero, which shades as one lobe and no filtering.
                if (SkinEvaluatesLayeredSpecular(profile.Parameters.EvaluationModel))
                {
                    data.skinSpecularLane = SkinSpecularLane(profile.Parameters);
                    // The expression half. `GetSkinExpressionDetail()` was set
                    // by the animated-mesh submission from the entity's APPLIED
                    // morph weights and is 0 for every static mesh and every
                    // head at a neutral expression, so this reduces to the
                    // profile's base gain wherever no face is emoting.
                    data.skinDetailStrength =
                        SkinDetailStrength(profile.Parameters.Specular, material.GetSkinExpressionDetail());
                }

                // THE ORAL SURFACE (issue #1245). Packed here for the reason
                // every lane above it is: the deferred path's per-frame table
                // packs the SAME lane with the SAME function, and the IOR ->
                // F0 conversion inside SkinOralLane is a physical decision that
                // Renderer/SkinOralSurface.h's opening rule keeps on the CPU.
                //
                // ONLY AT TRANSPORT VERSION 4, so a version-3 profile does not
                // even upload a coat. A version this code has no arm for leaves
                // the lane zero, which shades as dry with the transmitted term
                // exactly as #1242 shipped it.
                if (SkinEvaluatesOralSurface(profile.Parameters.EvaluationModel))
                    data.skinOralLane = SkinOralLane(profile.Parameters);

                // THE EYE (issue #1244). Packed here for the reason every lane
                // above it is, with one difference worth naming: there is no
                // deferred table to keep in step, because the ocular terms
                // resolve in the MATERIAL stage on all three paths. So this is
                // the only site that packs them, and the "two tables could
                // disagree" hazard the comments above guard against does not
                // exist for these three.
                //
                // ONLY AT TRANSPORT VERSION 5, so a version-4 profile does not
                // even upload an eye. A version this code has no arm for leaves
                // the lanes zero, whose master component is zero, which shades
                // as version-4 skin.
                //
                // ALL FOUR OR NONE. They are packed together and gated once,
                // because a cornea lane without its iris lane is an eye whose
                // refraction lands on a disc of radius zero — a division this
                // code refuses and a frame nobody would be able to read.
                if (SkinEvaluatesOcularSurface(profile.Parameters.EvaluationModel))
                {
                    data.skinOcularCorneaLane = SkinOcularCorneaLane(profile.Parameters);
                    data.skinOcularIrisLane = SkinOcularIrisLane(profile.Parameters);
                    data.skinOcularResponseLane = SkinOcularResponseLane(profile.Parameters);
                    data.skinOcularTintLane = SkinOcularTintLane(profile.Parameters);
                }

                // TRANSMISSION IS TESTED WITH `>=`-IN-SPIRIT AND SPELLED OUT,
                // because the versions are CUMULATIVE: version 3 is "everything
                // version 2 does, plus the layered specular", so a version-3
                // profile must still transmit. Testing only for version 2 here
                // would have made moving a profile to version 3 silently turn
                // transmission OFF while turning the lobes on — a head that
                // gains a sheen and loses its backlit ears in one authoring
                // click, which reads as "the new feature broke transmission".
                // The same trap #1242 documented one version earlier, in
                // oloSkinDiffusionOutput.
                if (SkinEvaluatesThicknessTransmission(profile.Parameters.EvaluationModel))
                {
                    data.skinTransmitScatter = SkinTransmissionScatterLane(profile.Parameters);
                    data.skinTransmitScaling = SkinTransmissionScalingLane(profile.Parameters);
                    // The metres -> millimetres conversion, done HERE and not in
                    // GLSL. It is the one number this feature is most likely to
                    // get wrong, and a unit slip in a shader is a thing no test
                    // can reach (Renderer/SkinTransmission.h, opening rule).
                    data.skinThicknessBaseMM =
                        SkinThicknessBaseMM(material.GetThicknessFactor(), profile.Parameters.ThicknessScale);

                    // THE TWO AUTHORING FAULTS, COUNTED AND LOGGED HERE — the
                    // only place that can see them, because it is the only place
                    // that has the material AND the resolved profile together.
                    // Neither is silently absorbed (CLAUDE.md house rule): a head
                    // that quietly stopped transmitting looks exactly like a head
                    // that never should have.
                    if (!material.HasAuthoredThickness())
                    {
                        // No thicknessFactor, so nothing for a map to modulate.
                        // The conservative fallback is NO transmission — see
                        // SkinTransmittance for why the other reading of a zero
                        // thickness is the uniformly emissive head.
                        Renderer3D::GetSkinProfileTable().ReportTransmissionFallback(
                            SkinTransmissionFallbackReason::NoThickness, material.GetSkinProfileHandle());
                    }
                    if (material.IsTransmissive())
                    {
                        // KHR_materials_transmission AND skin transport on one
                        // surface is two transmission closures over the same
                        // energy — the double-count the issue's third criterion
                        // forbids, arriving by the authoring path rather than by
                        // the maths. Skin's term wins because that is what the
                        // material kind asked for; the author is told which one
                        // was dropped.
                        Renderer3D::GetSkinProfileTable().ReportTransmissionFallback(
                            SkinTransmissionFallbackReason::RefractiveTransmissionConflict,
                            material.GetSkinProfileHandle());
                    }
                }
            }
        }

        // Physical transmission / IOR / volume (issue #970). GetAttenuationSigma
        // does the -log(colour)/distance derivation here, once per submission,
        // so the +infinity default distance never reaches the UBO or GLSL.
        data.transmissionFactor = material.GetTransmissionFactor();
        data.ior = material.GetIOR();
        data.thicknessFactor = material.GetThicknessFactor();
        data.attenuationSigma = material.GetAttenuationSigma();

        // PBR texture renderer IDs.
        data.albedoMapID = material.GetAlbedoMap() ? material.GetAlbedoMap()->GetRHIHandle() : RHI::NullResource;
        data.metallicRoughnessMapID = material.GetMetallicRoughnessMap() ? material.GetMetallicRoughnessMap()->GetRHIHandle() : RHI::NullResource;
        data.normalMapID = material.GetNormalMap() ? material.GetNormalMap()->GetRHIHandle() : RHI::NullResource;
        data.aoMapID = material.GetAOMap() ? material.GetAOMap()->GetRHIHandle() : RHI::NullResource;
        data.emissiveMapID = material.GetEmissiveMap() ? material.GetEmissiveMap()->GetRHIHandle() : RHI::NullResource;
        // The thickness map (issue #1242). Carried for EVERY material kind, not
        // only skin: it is KHR_materials_volume data that a material owns, and
        // gating the upload on the kind would mean a material switched to Skin in
        // the editor sampled nothing until the next resubmission.
        data.thicknessMapID = material.GetThicknessMap() ? material.GetThicknessMap()->GetRHIHandle() : RHI::NullResource;
        data.environmentMapID = material.GetEnvironmentMap() ? material.GetEnvironmentMap()->GetRHIHandle() : RHI::NullResource;
        data.irradianceMapID = material.GetIrradianceMap() ? material.GetIrradianceMap()->GetRHIHandle() : RHI::NullResource;
        data.prefilterMapID = material.GetPrefilterMap() ? material.GetPrefilterMap()->GetRHIHandle() : RHI::NullResource;
        data.brdfLutMapID = material.GetBRDFLutMap() ? material.GetBRDFLutMap()->GetRHIHandle() : RHI::NullResource;

        // Fall back to global IBL when the material has no IBL configured.
        if (data.enablePBR && !data.irradianceMapID.IsValid() && Renderer3D::GetGlobalIrradianceMapHandle().IsValid())
        {
            data.irradianceMapID = Renderer3D::GetGlobalIrradianceMapHandle();
            data.prefilterMapID = Renderer3D::GetGlobalPrefilterMapHandle();
            data.brdfLutMapID = Renderer3D::GetGlobalBRDFLutMapHandle();
            if (!data.environmentMapID.IsValid())
                data.environmentMapID = Renderer3D::GetGlobalEnvironmentMapHandle();
            data.enableIBL = true;
            data.iblIntensity = Renderer3D::GetGlobalIBLIntensity();
        }

        return data;
    }

    CommandPacket* Renderer3D::DrawMesh(const Ref<Mesh>& mesh, const glm::mat4& modelMatrix, const Material& material, bool isStatic, i32 entityID, const LODGroup* lodGroup, u32 gpuSceneDrawLink)
    {
        OLO_PROFILE_FUNCTION();
        if (!s_Data.Pipeline->FrameCorePasses.Scene)
        {
            OLO_CORE_ERROR("Renderer3D::DrawMesh: ScenePass is null!");
            return nullptr;
        }
        ++s_Data.Stats.TotalMeshes;

        if (s_Data.FrustumCullingEnabled && (isStatic || s_Data.DynamicCullingEnabled))
        {
            if (mesh && !IsVisibleInFrustum(mesh, modelMatrix))
            {
                ++s_Data.Stats.CulledMeshes;
                return nullptr;
            }
        }

        // Temporal occlusion culling: skip objects that were occluded last frame,
        // and submit proxy bounding boxes for re-testing.
        if (s_Data.OcclusionCullingEnabled && s_Data.OcclusionResultsAvailable && entityID >= 0 && mesh)
        {
            auto& stateMgr = OcclusionStateManager::GetInstance();
            auto& state = stateMgr.GetOrCreate(static_cast<u64>(entityID));

            // Allocate query index if this is a new object.
            if (state.QueryIndex == UINT32_MAX)
            {
                state.QueryIndex = stateMgr.AllocateQueryIndex();
            }

            // Read back previous frame's result.
            if (state.QueryIndex != UINT32_MAX)
            {
                const auto& queryPool = OcclusionQueryPool::GetInstance();
                const bool visible = queryPool.WasVisible(state.QueryIndex);
                state.WasVisible = visible;

                if (!visible)
                {
                    ++state.InvisibleFrameCount;
                    // Re-test periodically to detect when occluded objects become visible.
                    if (state.InvisibleFrameCount % kOcclusionRetestInterval == 0)
                    {
                        BoundingSphere bs = mesh->GetTransformedBoundingSphere(modelMatrix);
                        BoundingBox worldBounds;
                        worldBounds.Min = bs.Center - glm::vec3(bs.Radius);
                        worldBounds.Max = bs.Center + glm::vec3(bs.Radius);
                        OcclusionCuller::GetInstance().QueueBoundingBox(state.QueryIndex, worldBounds);
                    }
                    ++s_Data.Stats.CulledMeshes;
                    return nullptr;
                }

                state.InvisibleFrameCount = 0;
                // Queue visible objects for occlusion testing so they can
                // transition to occluded when something moves in front of them.
                BoundingSphere bs = mesh->GetTransformedBoundingSphere(modelMatrix);
                BoundingBox worldBounds;
                worldBounds.Min = bs.Center - glm::vec3(bs.Radius);
                worldBounds.Max = bs.Center + glm::vec3(bs.Radius);
                OcclusionCuller::GetInstance().QueueBoundingBox(state.QueryIndex, worldBounds);
            }
        }

        // LOD selection.
        Ref<Mesh> meshToUse;
        // s_Data.LODView is built from the CULLING camera, not the render camera
        // (issue #726): LOD is part of the cut, so a frozen frame must keep the LOD
        // levels it was frozen with. Flying the observer closer would otherwise swap
        // in a finer mesh and the picture would stop being the one the culling
        // camera produced.
        if (auto lodResult = SelectLODMesh(mesh, modelMatrix, s_Data.LODView, lodGroup, meshToUse); lodResult.SelectedLODIndex >= 0)
        {
            if (lodResult.SelectedLODIndex >= s_Data.Stats.ObjectsPerLODLevel.Num())
            {
                s_Data.Stats.ObjectsPerLODLevel.SetNumZeroed(static_cast<i64>(lodResult.SelectedLODIndex + 1));
            }
            ++s_Data.Stats.ObjectsPerLODLevel[lodResult.SelectedLODIndex];
            if (lodResult.Switched)
            {
                ++s_Data.Stats.LODSwitches;
            }
        }

        if (!meshToUse || !meshToUse->GetVertexArray())
        {
            OLO_CORE_ERROR("Renderer3D::DrawMesh: Invalid mesh or vertex array!");
            return nullptr;
        }

        Ref<Shader> shaderToUse;
        // Deferred mode demands that every ScenePass draw write the full
        // G-Buffer layout (Albedo/Metallic, Normal/Roughness/AO, Emissive/
        // Flags, Velocity, EntityID, BakedGI). Non-PBR materials selecting
        // s_Data.DefaultForwardShader
        // would instead write the legacy forward outputs (o_Color / o_EntityID
        // / o_ViewNormal / o_Velocity), which alias onto the G-Buffer slots
        // and corrupt lighting for every subsequent pixel.
        //
        // Until a Lighting3D_GBuffer variant lands, reroute such draws to the
        // ForwardOverlayPass — which binds the scene framebuffer (matching
        // MRT layout) and runs *after* DeferredLightingPass composites the
        // G-Buffer, so the non-PBR surface shades itself and blits over the
        // lit deferred image unscathed. Mirrors the same pattern DrawSkybox
        // uses for the skybox-on-deferred fallback.
        bool overlayRoute = false;
        if (material.GetShader())
        {
            shaderToUse = material.GetShader();
            // Forward-only override on the Deferred path would alias its
            // output locations onto G-Buffer slots. Reroute to
            // ForwardOverlayPass so the override gets the forward FB layout
            // it was authored against.
            if (s_Data.Settings.Path == RenderingPath::Deferred && s_Data.Pipeline->RenderStreamPasses.ForwardOverlay &&
                !IsDeferredCapableShader(shaderToUse))
            {
                overlayRoute = true;
            }
        }
        else if (material.GetType() == MaterialType::PBR)
        {
            // Transmission has no G-Buffer representation (issue #970) — see
            // ShouldRerouteTransmissiveToForwardOverlay at the top of this file.
            const bool deferred = s_Data.Settings.Path == RenderingPath::Deferred;
            if (ShouldRerouteTransmissiveToForwardOverlay(
                    material, deferred, s_Data.Pipeline->RenderStreamPasses.ForwardOverlay != nullptr, s_Data.PBRShader))
            {
                shaderToUse = s_Data.PBRShader;
                overlayRoute = true;
            }
            // Route PBR default shader to the G-Buffer write variant when the
            // deferred path is active. Material overrides still win (so
            // custom shaders, e.g. terrain/foliage, keep their forward
            // pipeline until their own G-Buffer variants land in later phases).
            else if (deferred && s_Data.PBRGBufferShader)
                shaderToUse = s_Data.PBRGBufferShader;
            else
                shaderToUse = s_Data.PBRShader;
        }
        else
        {
            shaderToUse = s_Data.DefaultForwardShader;
            if (s_Data.Settings.Path == RenderingPath::Deferred && s_Data.Pipeline->RenderStreamPasses.ForwardOverlay)
                overlayRoute = true;
        }

        if (!shaderToUse)
        {
            OLO_CORE_ERROR("Renderer3D::DrawMesh: No shader available!");
            return nullptr;
        }

        const RHI::ResourceHandle vertexArrayID = meshToUse->GetVertexArray()->GetRHIHandle();
        const RHI::ResourceHandle shaderRendererID = shaderToUse->GetRHIHandle();
        if (!ValidateDrawMeshResources("Renderer3D::DrawMesh", vertexArrayID, shaderRendererID))
            return nullptr;

        // Create POD command using asset handles and renderer IDs.
        CommandPacket* packet = overlayRoute
                                    ? CreateForwardOverlayDrawCall<DrawMeshCommand>()
                                    : CreateDrawCall<DrawMeshCommand>();
        if (!packet)
            return nullptr;
        auto* cmd = packet->GetCommandData<DrawMeshCommand>();
        cmd->header.type = CommandType::DrawMesh;

        // Store asset handles and renderer IDs (POD).
        cmd->meshHandle = meshToUse->GetHandle();
        cmd->vertexArrayID = vertexArrayID;
        cmd->indexCount = meshToUse->GetIndexCount();
        cmd->baseIndex = meshToUse->GetBaseIndex();
        cmd->transform = glm::mat4(modelMatrix);
        // Prev-transform is recorded for every path — forward PBR shaders
        // now emit screen-space velocity into scene FB RT3 alongside the
        // deferred G-Buffer variant, so TAA consumes per-object motion in
        // Forward / Forward+ too. Static meshes self-alias (prev == curr)
        // so their velocity reads zero.
        cmd->prevTransform = GetAndRecordPrevTransform(entityID, cmd->transform);
        // The canonical link (issue #994). It is carried, not consumed, here:
        // the record's slot only becomes final at EndScene, so the dispatcher
        // is the first place that can read the transforms it names. `transform`
        // and `prevTransform` above stay filled because an unresolved link must
        // still draw, and because the depth prepass and every legacy adapter
        // read them.
        cmd->gpuSceneDrawLink = gpuSceneDrawLink;
        cmd->entityID = entityID;
        cmd->shaderHandle = shaderToUse->GetHandle();

        // Material data via table.
        cmd->materialDataIndex = FrameDataBufferManager::Get().AllocateMaterialData(
            CreatePODMaterialDataForMaterial(material, shaderRendererID));

        // Render state via table.
        cmd->renderStateIndex = FrameDataBufferManager::Get().AllocateRenderState(CreatePODRenderStateForMaterial(material));

        // No bone matrices for non-animated mesh.
        cmd->isAnimatedMesh = false;
        cmd->boneBufferOffset = 0;
        cmd->boneCount = 0;

        // Store occlusion query index for conditional rendering in dispatch.
        if (s_Data.OcclusionCullingEnabled && entityID >= 0)
        {
            auto& stateMgr = OcclusionStateManager::GetInstance();
            if (stateMgr.Has(static_cast<u64>(entityID)))
            {
                u32 queryIdx = stateMgr.GetOrCreate(static_cast<u64>(entityID)).QueryIndex;
                if (queryIdx != UINT32_MAX)
                {
                    cmd->occlusionQueryIndex = queryIdx;
                }
            }
        }

        packet->SetCommandType(cmd->header.type);
        packet->SetDispatchFunction(CommandDispatch::GetDispatchFunction(cmd->header.type));

        // Set sort key for optimal command sorting.
        PacketMetadata metadata = packet->GetMetadata();
        u32 shaderID = shaderRendererID.Index & 0xFFFF; // 16-bit shader ID.
        u32 materialID = ComputeMaterialID(material);
        u32 depth = ComputeDepthForSortKey(modelMatrix);
        if (material.GetFlag(MaterialFlag::Blend))
            metadata.m_SortKey = DrawKey::CreateTransparent(0, ViewLayerType::ThreeD, shaderID, materialID, depth);
        else
            metadata.m_SortKey = DrawKey::CreateOpaque(0, ViewLayerType::ThreeD, shaderID, materialID, depth);
        metadata.m_IsStatic = isStatic;
        metadata.m_DebugName = GetMeshDebugName(meshToUse);
        packet->SetMetadata(metadata);

        if (overlayRoute)
        {
            // Submit to the overlay bucket directly; return nullptr so the
            // caller's follow-up SubmitPacket(packet) becomes a safe no-op
            // (same pattern DrawSkybox/DrawInfiniteGrid use).
            SubmitForwardOverlayPacket(packet);
            return nullptr;
        }

        return packet;
    }

    Renderer3D::InstancedShaderRouting Renderer3D::SelectInstancedShaderRouting(const Material& material)
    {
        // Deferred G-Buffer routing — mirrors DrawMesh() exactly (see its
        // comment for the full rationale). Instanced draws share the same
        // InstanceBlock_Vertex.glsl / SSBO_INSTANCE_DATA mechanism as
        // non-instanced draws (gl_InstanceIndex resolves to 0 for a
        // single-instance draw), so PBRGBufferShader (PBR_GBuffer.glsl) is
        // already instancing-capable — no separate shader variant needed.
        // Shared by DrawMeshInstanced's CPU-cull path and
        // SubmitGPUCulledInstanced's GPU-cull path so the two can't drift
        // apart the way the pre-#515 routing bug did.
        InstancedShaderRouting routing;
        if (material.GetShader())
        {
            routing.ShaderToUse = material.GetShader();
            // Forward-only override on the Deferred path would alias its
            // output locations onto G-Buffer slots. Reroute to
            // ForwardOverlayPass so the override gets the forward FB layout
            // it was authored against.
            if (s_Data.Settings.Path == RenderingPath::Deferred && s_Data.Pipeline->RenderStreamPasses.ForwardOverlay &&
                !IsDeferredCapableShader(routing.ShaderToUse))
            {
                routing.OverlayRoute = true;
            }
        }
        else if (material.GetType() == MaterialType::PBR)
        {
            // Transmission has no G-Buffer representation (issue #970) — see
            // ShouldRerouteTransmissiveToForwardOverlay at the top of this file.
            const bool deferred = s_Data.Settings.Path == RenderingPath::Deferred;
            if (ShouldRerouteTransmissiveToForwardOverlay(
                    material, deferred, s_Data.Pipeline->RenderStreamPasses.ForwardOverlay != nullptr, s_Data.PBRShader))
            {
                routing.ShaderToUse = s_Data.PBRShader;
                routing.OverlayRoute = true;
            }
            else if (deferred && s_Data.PBRGBufferShader)
                routing.ShaderToUse = s_Data.PBRGBufferShader;
            else
                routing.ShaderToUse = s_Data.PBRShader;
        }
        else
        {
            routing.ShaderToUse = s_Data.DefaultForwardShader;
            if (s_Data.Settings.Path == RenderingPath::Deferred && s_Data.Pipeline->RenderStreamPasses.ForwardOverlay)
                routing.OverlayRoute = true;
        }
        return routing;
    }

    CommandPacket* Renderer3D::DrawMeshInstanced(const Ref<Mesh>& mesh, std::span<const glm::mat4> transforms, const Material& material, bool isStatic, u64 ownerKey)
    {
        OLO_PROFILE_FUNCTION();
        bool overlayRoute = false;
        CommandPacket* packet = BuildDrawMeshInstancedPacket(mesh, transforms, material, isStatic, ownerKey, overlayRoute);
        if (packet && overlayRoute)
        {
            // Submit to the overlay bucket directly; return nullptr so the
            // caller's follow-up SubmitPacket(packet) becomes a safe no-op
            // (same pattern DrawMesh uses).
            SubmitForwardOverlayPacket(packet);
            return nullptr;
        }
        return packet;
    }

    CommandPacket* Renderer3D::BuildDrawMeshInstancedPacket(const Ref<Mesh>& mesh, std::span<const glm::mat4> transforms,
                                                            const Material& material, bool isStatic, u64 ownerKey,
                                                            bool& outOverlayRoute)
    {
        outOverlayRoute = false;
        if (!s_Data.Pipeline->FrameCorePasses.Scene)
        {
            OLO_CORE_ERROR("Renderer3D::DrawMeshInstanced: ScenePass is null!");
            return nullptr;
        }
        if (transforms.empty())
        {
            OLO_CORE_WARN("Renderer3D::DrawMeshInstanced: No transforms provided");
            return nullptr;
        }
        s_Data.Stats.TotalMeshes += static_cast<u32>(transforms.size());

        // GPU-side frustum cull pre-pass: when the input count is large
        // enough that the CPU sphere loop dominates submission cost, route
        // through the compute-shader path. Threshold defaults to 1024 (tuned
        // for the breakeven of dispatch+memory-barrier overhead vs the linear
        // CPU test) and can be raised at runtime via `s_Data.GPUCullThreshold`.
        const bool cullEnabled = s_Data.FrustumCullingEnabled && (isStatic || s_Data.DynamicCullingEnabled);
        if (cullEnabled && s_Data.GPUFrustumCuller &&
            transforms.size() >= static_cast<sizet>(s_Data.GPUCullThreshold) &&
            mesh)
        {
            // SubmitGPUCulledInstanced owns its shader routing and
            // ForwardOverlayPass submission end-to-end (overlay or not);
            // `outOverlayRoute` stays false here so callers never try to
            // submit its result a second time.
            return SubmitGPUCulledInstanced(mesh, transforms, material, isStatic, ownerKey);
        }

        std::span<const glm::mat4> activeTransforms = transforms;
        std::vector<glm::mat4> filteredTransforms;
        // Index map from post-cull visible slot -> pre-cull stable instance
        // index. Passed to GetAndRecordPrevInstanceTransforms so history
        // lookup uses the full pre-cull array for identity stability, then
        // projects prev onto the visible subset. Empty when no culling ran.
        std::vector<u32> visibleIndices;

        if (cullEnabled)
        {
            // Extract the local bounding sphere once and transform per-instance
            // instead of recomputing from mesh source each time.
            BoundingSphere localSphere = mesh->GetBoundingSphere();
            localSphere.Radius *= 1.3f; // Match expansion factor from IsVisibleInFrustum.
            filteredTransforms.reserve(transforms.size());
            visibleIndices.reserve(transforms.size());
            for (sizet i = 0; i < transforms.size(); ++i)
            {
                const auto& t = transforms[i];
                BoundingSphere worldSphere = localSphere.Transform(t);
                if (s_Data.ViewFrustum.IsBoundingSphereVisible(worldSphere))
                {
                    filteredTransforms.push_back(t);
                    visibleIndices.push_back(static_cast<u32>(i));
                }
            }
            s_Data.Stats.CulledMeshes += static_cast<u32>(transforms.size() - filteredTransforms.size());
            if (filteredTransforms.empty())
            {
                return nullptr;
            }
            activeTransforms = filteredTransforms;
        }

        // Allocate space in FrameDataBuffer for instance transforms.
        FrameDataBuffer& frameBuffer = FrameDataBufferManager::Get();
        u32 transformCount = static_cast<u32>(activeTransforms.size());
        u32 transformOffset = frameBuffer.AllocateTransforms(transformCount);
        if (transformOffset == UINT32_MAX)
        {
            OLO_CORE_ERROR("Renderer3D::DrawMeshInstanced: Failed to allocate transform buffer space");
            return nullptr;
        }
        frameBuffer.WriteTransforms(transformOffset, activeTransforms.data(), transformCount);

        // Previous-frame transforms (Deferred per-instance velocity). Cache keyed by
        // (mesh, ownerKey) so two submission sources that render the same mesh with
        // independent instance arrays don't overwrite each other's history;
        // caller-ordering stability within an owner is still required. First frame
        // / count mismatches alias the current data -> zero velocity.
        // Record per-instance transform history on every render path, not
        // just Deferred: in Forward/Forward+ the PBR shader writes velocity
        // into scene-FB RT3 via the CameraMatrices UBO, and instanced draws
        // without `prevTransformBufferOffset` set fall back to the
        // static-geometry path (prev == curr) — which silently zeroes
        // per-object motion for TAA when the velocity source is scene FB RT3
        // (Forward/Forward+). See EndScene velocity-source selection: the
        // scene FB path is now taken whenever RenderingPath != Deferred.
        u32 prevTransformOffset = UINT32_MAX;
        if (mesh)
        {
            const u64 meshKey = static_cast<u64>(mesh->GetHandle());
            bool usedFallback = false;
            // Pass the **full pre-cull** transform list so history is keyed by
            // stable per-instance identity. When culling dropped instances,
            // `visibleIndices` projects the prev array onto the visible subset
            // so slot i in prevTransforms lines up with slot i in
            // activeTransforms. Without this, a different frustum-visible
            // subset next frame would silently alias unrelated instances.
            const auto visible = visibleIndices.empty() ? std::nullopt : std::optional{ std::span<const u32>(visibleIndices) };
            auto prevTransforms = GetAndRecordPrevInstanceTransforms(meshKey, ownerKey, transforms, visible, &usedFallback);
            // Use the explicit flag rather than pointer identity — the function
            // returns a projected vector by value in the fallback path, so
            // prevTransforms.GetData() is always a distinct buffer.
            if (!usedFallback && static_cast<sizet>(prevTransforms.Num()) == activeTransforms.size())
            {
                u32 prevOffset = frameBuffer.AllocateTransforms(transformCount);
                if (prevOffset != UINT32_MAX)
                {
                    frameBuffer.WriteTransforms(prevOffset, prevTransforms.GetData(), transformCount);
                    prevTransformOffset = prevOffset;
                }
            }
        }

        const InstancedShaderRouting routing = SelectInstancedShaderRouting(material);
        const Ref<Shader>& shaderToUse = routing.ShaderToUse;
        if (!shaderToUse)
        {
            OLO_CORE_ERROR("Renderer3D::DrawMeshInstanced: No shader available!");
            return nullptr;
        }

        // Validate before allocating a packet so an invalid draw doesn't
        // consume packet-arena storage it will never submit (matches
        // DrawMesh's ordering).
        const RHI::ResourceHandle vertexArrayID = mesh->GetVertexArray()->GetRHIHandle();
        const RHI::ResourceHandle shaderRendererID = shaderToUse->GetRHIHandle();
        if (!ValidateDrawMeshResources("Renderer3D::DrawMeshInstanced", vertexArrayID, shaderRendererID))
            return nullptr;

        // Create POD command.
        CommandPacket* packet = routing.OverlayRoute
                                    ? CreateForwardOverlayDrawCall<DrawMeshInstancedCommand>()
                                    : CreateDrawCall<DrawMeshInstancedCommand>();
        if (!packet)
            return nullptr;
        auto* cmd = packet->GetCommandData<DrawMeshInstancedCommand>();
        cmd->header.type = CommandType::DrawMeshInstanced;

        // Store asset handles and renderer IDs (POD).
        cmd->meshHandle = mesh->GetHandle();
        cmd->vertexArrayID = vertexArrayID;
        cmd->indexCount = mesh->GetIndexCount();
        cmd->baseIndex = mesh->GetBaseIndex();
        cmd->instanceCount = transformCount;
        cmd->transformBufferOffset = transformOffset;
        cmd->prevTransformBufferOffset = prevTransformOffset;
        cmd->transformCount = transformCount;
        cmd->shaderHandle = shaderToUse->GetHandle();

        // Material data via table.
        cmd->materialDataIndex = FrameDataBufferManager::Get().AllocateMaterialData(
            CreatePODMaterialDataForMaterial(material, shaderRendererID));

        // Render state via table.
        cmd->renderStateIndex = FrameDataBufferManager::Get().AllocateRenderState(CreatePODRenderStateForMaterial(material));

        cmd->isAnimatedMesh = false;
        cmd->boneBufferOffset = 0;
        cmd->boneCountPerInstance = 0;

        packet->SetCommandType(cmd->header.type);
        packet->SetDispatchFunction(CommandDispatch::GetDispatchFunction(cmd->header.type));

        // Set sort key for instanced mesh commands (use first transform for depth).
        PacketMetadata metadata = packet->GetMetadata();
        u32 shaderID = shaderRendererID.Index & 0xFFFF;
        u32 materialID = ComputeMaterialID(material);
        u32 depth = activeTransforms.empty() ? 0 : ComputeDepthForSortKey(activeTransforms[0]);
        if (material.GetFlag(MaterialFlag::Blend))
            metadata.m_SortKey = DrawKey::CreateTransparent(0, ViewLayerType::ThreeD, shaderID, materialID, depth);
        else
            metadata.m_SortKey = DrawKey::CreateOpaque(0, ViewLayerType::ThreeD, shaderID, materialID, depth);
        metadata.m_IsStatic = isStatic;
        metadata.m_DebugName = GetMeshDebugName(mesh);
        packet->SetMetadata(metadata);

        // Report the routing decision rather than submitting here: the
        // std::span<const InstanceData> overload needs to patch
        // Color/Custom/EntityID onto this packet before it is handed off to
        // ForwardOverlayPass, so the transform-only public overload performs
        // the actual submission once it (or the InstanceData overload) is
        // done with the packet.
        outOverlayRoute = routing.OverlayRoute;
        return packet;
    }

    // InstanceData overload — extracts transforms for the existing pipeline
    // (frustum cull, FrameDataBuffer allocation, prev-transform history,
    // command construction) then patches the resulting packet with per-instance
    // EntityID / Color / Custom streams pulled straight from the InstanceData
    // span, before performing any ForwardOverlayPass submission the CPU-cull
    // path's routing decided on. Keeps the single source of truth for
    // instancing logic in BuildDrawMeshInstancedPacket.
    CommandPacket* Renderer3D::DrawMeshInstanced(const Ref<Mesh>& mesh, std::span<const InstanceData> instances, const Material& material, bool isStatic, u64 ownerKey)
    {
        OLO_PROFILE_FUNCTION();
        if (instances.empty())
            return nullptr;

        std::vector<glm::mat4> transforms;
        transforms.reserve(instances.size());
        for (const auto& inst : instances)
            transforms.push_back(inst.Transform);

        // Called directly rather than through the public transform-only
        // overload so the packet isn't submitted to ForwardOverlayPass until
        // the Color/Custom/EntityID streams below are patched onto it (see
        // BuildDrawMeshInstancedPacket's comment).
        bool overlayRoute = false;
        CommandPacket* packet = BuildDrawMeshInstancedPacket(mesh, transforms, material, isStatic, ownerKey, overlayRoute);
        if (!packet)
            return nullptr; // entirely culled, alloc failure, or the GPU-cull path already fully handled submission

        // Post-cull instance count from the produced packet — the transform-only
        // overload may have filtered out frustum-culled instances. We only
        // populate the auxiliary streams for the surviving subset to keep the
        // i-th color/custom/entityID aligned with the i-th transform.
        auto* cmd = packet->GetCommandData<DrawMeshInstancedCommand>();
        const u32 visibleCount = cmd->instanceCount;
        if (visibleCount == 0)
        {
            if (overlayRoute)
            {
                SubmitForwardOverlayPacket(packet);
                return nullptr;
            }
            return packet;
        }

        // The transform-only overload doesn't expose its post-cull index map,
        // so we re-run frustum culling with the same parameters to know which
        // source instances survived. If frustum culling is disabled, the
        // visible subset is 0..N-1 contiguous and this becomes a no-op pass.
        std::vector<u32> visibleIndices;
        if (s_Data.FrustumCullingEnabled && (isStatic || s_Data.DynamicCullingEnabled))
        {
            BoundingSphere localSphere = mesh->GetBoundingSphere();
            localSphere.Radius *= 1.3f;
            visibleIndices.reserve(visibleCount);
            for (sizet i = 0; i < instances.size() && visibleIndices.size() < visibleCount; ++i)
            {
                BoundingSphere worldSphere = localSphere.Transform(instances[i].Transform);
                if (s_Data.ViewFrustum.IsBoundingSphereVisible(worldSphere))
                    visibleIndices.push_back(static_cast<u32>(i));
            }
        }
        else
        {
            visibleIndices.resize(visibleCount);
            std::iota(visibleIndices.begin(), visibleIndices.end(), 0u);
        }

        FrameDataBuffer& frameBuffer = FrameDataBufferManager::Get();
        u32 colorOffset = frameBuffer.AllocateColors(visibleCount);
        u32 customOffset = frameBuffer.AllocateCustoms(visibleCount);
        u32 entityIDOffset = frameBuffer.AllocateEntityIDs(visibleCount);

        if (colorOffset != UINT32_MAX)
        {
            std::vector<glm::vec4> colors;
            colors.reserve(visibleCount);
            for (u32 idx : visibleIndices)
                colors.push_back(instances[idx].Color);
            frameBuffer.WriteColors(colorOffset, colors.data(), visibleCount);
            cmd->colorBufferOffset = colorOffset;
        }
        if (customOffset != UINT32_MAX)
        {
            std::vector<f32> customs;
            customs.reserve(visibleCount);
            for (u32 idx : visibleIndices)
                customs.push_back(instances[idx].Custom);
            frameBuffer.WriteCustoms(customOffset, customs.data(), visibleCount);
            cmd->customBufferOffset = customOffset;
        }
        if (entityIDOffset != UINT32_MAX)
        {
            std::vector<i32> ids;
            ids.reserve(visibleCount);
            for (u32 idx : visibleIndices)
                ids.push_back(instances[idx].EntityID);
            frameBuffer.WriteEntityIDs(entityIDOffset, ids.data(), visibleCount);
            cmd->entityIDBufferOffset = entityIDOffset;
        }

        // Per-instance baked lightmap regions (issue #867). The authored
        // InstanceData already carried a LightmapScaleOffset lane and the
        // dispatcher already reads one per instance
        // (CommandDispatch::DrawMeshInstancedCommand), but nothing ever
        // allocated the stream in between — so every instanced draw resolved to
        // the all-zero "no lightmap" sentinel however carefully the region was
        // authored. This is that missing hop.
        //
        // Allocated only when some surviving instance actually has a region:
        // the sentinel costs nothing downstream, and a foliage batch of 10k
        // unbaked instances should not pay a 160 KB frame allocation to say so.
        // Rides the generic vec4 (Colors) stream under its own offset, exactly
        // as CommandBucket::BatchCommands does for the batched single-draw case.
        const bool anyLightmapRegion =
            std::any_of(visibleIndices.begin(), visibleIndices.end(),
                        [&instances](u32 idx)
                        { return instances[idx].LightmapScaleOffset.x > 0.0f; });
        if (anyLightmapRegion)
        {
            if (u32 lightmapOffset = frameBuffer.AllocateColors(visibleCount); lightmapOffset != UINT32_MAX)
            {
                std::vector<glm::vec4> regions;
                regions.reserve(visibleCount);
                for (u32 idx : visibleIndices)
                    regions.push_back(instances[idx].LightmapScaleOffset);
                frameBuffer.WriteColors(lightmapOffset, regions.data(), visibleCount);
                cmd->lightmapRegionBufferOffset = lightmapOffset;
            }
            else
            {
                // Its own warning rather than sharing the tint stream's: a
                // dropped tint is a cosmetic regression, a dropped region is
                // "this batch silently lost its baked GI".
                OLO_CORE_WARN_TAG("Renderer3D",
                                  "instance vec4 stream exhausted — {} instances lose their baked lightmap "
                                  "regions this frame and fall back to probes/IBL",
                                  visibleCount);
            }
        }

        if (overlayRoute)
        {
            // Submit to the overlay bucket now that Color/Custom/EntityID
            // are patched onto the packet; return nullptr so the caller's
            // follow-up SubmitPacket(packet) becomes a safe no-op (same
            // pattern DrawMesh/DrawMeshInstanced(transforms) use).
            SubmitForwardOverlayPacket(packet);
            return nullptr;
        }

        return packet;
    }

    CommandPacket* Renderer3D::SubmitGPUCulledInstanced(const Ref<Mesh>& mesh,
                                                        std::span<const glm::mat4> transforms,
                                                        const Material& material, bool isStatic,
                                                        u64 ownerKey)
    {
        OLO_PROFILE_FUNCTION();

        // Pre-cull prev-transform history: look up the per-instance prev
        // array from the cache **before** the cull so survivors carry the
        // correct PrevTransform. The cache always sees the FULL pre-cull
        // list — the GPU cull projects it onto survivors automatically by
        // copying each slot's PrevTransform along with its Transform.
        const u64 meshKey = static_cast<u64>(mesh->GetHandle());
        bool usedFallback = false;
        auto prevTransforms = GetAndRecordPrevInstanceTransforms(meshKey, ownerKey, transforms, std::nullopt, &usedFallback);
        if (usedFallback || static_cast<sizet>(prevTransforms.Num()) != transforms.size())
        {
            prevTransforms.Reset();
            prevTransforms.Append(transforms.data(), static_cast<i64>(transforms.size())); // first frame / size mismatch -> zero velocity
        }

        // Build the InstanceData[] the cull compute reads. Color / Custom /
        // EntityID stay at their identity defaults — the transform-only
        // overload doesn't carry that per-instance data. The InstanceData
        // overload of DrawMeshInstanced overwrites them with the real values
        // once the GPU-cull path is wired into it as a future follow-up.
        TArray<InstanceData> packed;
        packed.Reserve(transforms.size());
        for (sizet i = 0; i < transforms.size(); ++i)
        {
            InstanceData inst;
            inst.Transform = transforms[i];
            inst.Normal = glm::transpose(glm::inverse(transforms[i]));
            inst.PrevTransform = prevTransforms[i];
            packed.Add(inst);
        }

        // Run the GPU cull. RadiusExpansion folds the CPU path's two safety
        // multipliers (1.3 × 1.05 = 1.365) into a single uniform so the two
        // paths produce identical visibility decisions (verified by
        // GPUFrustumCullParityTest in tests/Rendering/).
        BoundingSphere localSphere = mesh->GetBoundingSphere();
        constexpr f32 kRadiusExpansion = 1.3f * 1.05f;
        const glm::vec4 sphereUniform{ localSphere.Center, localSphere.Radius };

        // Deferred G-Buffer routing — mirrors DrawMeshInstanced()/DrawMesh().
        // See SelectInstancedShaderRouting for why PBRGBufferShader needs no
        // separate instanced shader variant.
        const InstancedShaderRouting routing = SelectInstancedShaderRouting(material);
        const Ref<Shader>& shaderToUse = routing.ShaderToUse;
        const bool overlayRoute = routing.OverlayRoute;
        if (!shaderToUse)
        {
            OLO_CORE_ERROR("Renderer3D::SubmitGPUCulledInstanced: No shader available!");
            return nullptr;
        }
        const RHI::ResourceHandle vertexArrayID = mesh->GetVertexArray()->GetRHIHandle();
        const RHI::ResourceHandle shaderRendererID = shaderToUse->GetRHIHandle();
        if (!ValidateDrawMeshResources("Renderer3D::SubmitGPUCulledInstanced", vertexArrayID, shaderRendererID))
            return nullptr;

        // Material / render state allocated once; both phase-1 and phase-2
        // packets reference the same indices.
        const u32 materialDataIndex = FrameDataBufferManager::Get().AllocateMaterialData(
            CreatePODMaterialDataForMaterial(material, shaderRendererID));
        const u32 renderStateIndex = FrameDataBufferManager::Get().AllocateRenderState(CreatePODRenderStateForMaterial(material));

        const u32 shaderID = shaderRendererID.Index & 0xFFFF;
        const u32 materialID = ComputeMaterialID(material);
        const u32 sortDepth = transforms.empty() ? 0 : ComputeDepthForSortKey(transforms[0]);

        // Build one DrawMeshInstancedCommand packet over a (survivors, indirect)
        // buffer pair. The dispatcher takes the indirect-draw branch because
        // `cullIndirectBufferID` is non-zero — skipping the FrameDataBuffer
        // scratch loop and binding the pre-culled survivors at SSBO_INSTANCE_DATA.
        const auto buildPacket = [&](RHI::ResourceHandle outputInstanceBuffer,
                                     RHI::ResourceHandle indirectBuffer,
                                     RHI::ResourceHandle rootDataBuffer = {},
                                     const u32 rootDataAddressOffsetBytes = 0u) -> CommandPacket*
        {
            CommandPacket* packet = overlayRoute
                                        ? CreateForwardOverlayDrawCall<DrawMeshInstancedCommand>()
                                        : CreateDrawCall<DrawMeshInstancedCommand>();
            if (!packet)
                return nullptr;
            auto* cmd = packet->GetCommandData<DrawMeshInstancedCommand>();
            cmd->header.type = CommandType::DrawMeshInstanced;
            cmd->meshHandle = mesh->GetHandle();
            cmd->vertexArrayID = vertexArrayID;
            cmd->indexCount = mesh->GetIndexCount();
            cmd->baseIndex = mesh->GetBaseIndex();
            // Pre-cull count for the profiler; the GPU determines the real
            // survivor count at draw time via the indirect command.
            cmd->transformCount = static_cast<u32>(transforms.size());
            cmd->instanceCount = static_cast<u32>(transforms.size());
            cmd->shaderHandle = shaderToUse->GetHandle();
            cmd->materialDataIndex = materialDataIndex;
            cmd->renderStateIndex = renderStateIndex;
            cmd->isAnimatedMesh = false;
            cmd->cullOutputInstanceBufferID = outputInstanceBuffer;
            cmd->cullIndirectBufferID = indirectBuffer;
            cmd->cullRootDataBufferID = rootDataBuffer;
            cmd->cullRootDataAddressOffsetBytes = rootDataAddressOffsetBytes;

            packet->SetCommandType(cmd->header.type);
            packet->SetDispatchFunction(CommandDispatch::GetDispatchFunction(cmd->header.type));

            PacketMetadata metadata = packet->GetMetadata();
            if (material.GetFlag(MaterialFlag::Blend))
                metadata.m_SortKey = DrawKey::CreateTransparent(0, ViewLayerType::ThreeD, shaderID, materialID, sortDepth);
            else
                metadata.m_SortKey = DrawKey::CreateOpaque(0, ViewLayerType::ThreeD, shaderID, materialID, sortDepth);
            metadata.m_IsStatic = isStatic;
            metadata.m_DebugName = GetMeshDebugName(mesh);
            packet->SetMetadata(metadata);
            return packet;
        };

        // Two-phase occlusion is switched OFF while the culling camera is frozen
        // (issue #726). Phase 2 rebuilds the pyramid from THIS frame's depth --
        // which is the observer's depth once frozen -- and that rebuild
        // overwrites the retained pyramid in place, destroying the frozen one
        // phase 1 tested against. The single-phase path below keeps culling
        // against the retained frozen pyramid, which is the honest answer: an
        // instance the frozen camera occluded stays occluded, which is the
        // property the whole feature rests on. See
        // docs/agent-rules/two-phase-occlusion-culling.md.
        const bool hzbOcclusion = IsHZBOcclusionCullingEnabled() && !IsCullingCameraFrozen();
        const bool deferred = (GetRendererSettings().Path == RenderingPath::Deferred);

        // Two-phase GPU-driven occlusion (#431): Forward / Forward+ with
        // HZB occlusion on. Phase 1 culls against the previous frame's HZB and
        // appends occluded survivors to a reject list; both the phase-1 draw and
        // a phase-2 draw are routed to GPUDrivenOcclusionPass, which re-tests the
        // reject list against this frame's depth after the phase-1 draws.
        if (GPUDrivenOcclusionPass* occlusionPass = (hzbOcclusion && !deferred) ? GetGPUOcclusionPass() : nullptr)
        {
            auto twoPhase = s_Data.GPUFrustumCuller->CullTwoPhasePhase1(
                std::span<const InstanceData>{ packed.GetData(), static_cast<sizet>(packed.Num()) }, mesh->GetIndexCount(), mesh->GetBaseIndex(), sphereUniform, kRadiusExpansion);

            CommandPacket* phase1Packet = buildPacket(twoPhase.Phase1Output->GetStorage()->GetRHIHandle(),
                                                      twoPhase.Phase1Indirect->GetRHIHandle());
            CommandPacket* phase2Packet = buildPacket(twoPhase.Phase2Output->GetStorage()->GetRHIHandle(),
                                                      twoPhase.Phase2Indirect->GetRHIHandle());
            if (phase1Packet)
                SubmitRenderStreamPacket(RenderStreamType::GPUOcclusion, phase1Packet);
            if (phase2Packet)
                occlusionPass->SubmitPhase2(phase2Packet, twoPhase);
            return nullptr; // both phases handled by the pass; caller's SubmitPacket is a no-op
        }

        // Two-phase GPU-driven occlusion on the Deferred path (#486). Phase 1
        // draws through the normal ScenePass G-Buffer bucket — the phase-1 packet
        // is returned to the caller exactly like the single-phase deferred packet,
        // so the disocclusion-corrected path preserves the current G-Buffer fill.
        // The phase-1 cull already ran (against the previous frame's HZB) and
        // appended occluded survivors to a reject list; the phase-2 packet is
        // registered with DeferredGPUOcclusionPass, which rebuilds the HZB from
        // this frame's G-Buffer depth (occluders + phase-1 survivors) and draws
        // the disoccluded instances before AO / lighting.
        //
        // Excluded when overlayRoute: a forward-only material override has no
        // G-Buffer depth to reconstruct an HZB from, so it skips deferred
        // occlusion entirely and falls through to the frustum-only path below.
        if (DeferredGPUOcclusionPass* deferredOcclusionPass = (hzbOcclusion && deferred && !overlayRoute) ? GetDeferredGPUOcclusionPass() : nullptr)
        {
            auto twoPhase = s_Data.GPUFrustumCuller->CullTwoPhasePhase1(
                std::span<const InstanceData>{ packed.GetData(), static_cast<sizet>(packed.Num()) }, mesh->GetIndexCount(), mesh->GetBaseIndex(), sphereUniform, kRadiusExpansion);

            if (CommandPacket* phase2Packet = buildPacket(twoPhase.Phase2Output->GetStorage()->GetRHIHandle(),
                                                          twoPhase.Phase2Indirect->GetRHIHandle()))
                deferredOcclusionPass->SubmitPhase2(phase2Packet, twoPhase);

            // Phase 1 → caller → ScenePass G-Buffer bucket.
            return buildPacket(twoPhase.Phase1Output->GetStorage()->GetRHIHandle(),
                               twoPhase.Phase1Indirect->GetRHIHandle());
        }

        // Single-phase path: frustum-only (occlusion off, or overlayRoute
        // bypassing deferred occlusion above) — drawn through the normal
        // ScenePass / G-Buffer bucket, or submitted directly to
        // ForwardOverlayPass when overlayRoute (mirrors DrawMeshInstanced()).
        GpuDrivenRootDataLayout reflectedRootLayout{};
        [[maybe_unused]] const bool hasGpuDrivenRootLayout = RenderCommand::QueryGpuDrivenRootDataLayout(
            shaderToUse->GetRHIHandle(), ShaderBindingLayout::SSBO_INSTANCE_DATA, reflectedRootLayout);
        // The neutral reflected layout is already exactly the culler's target
        // contract; a failed query leaves the value invalid and selects the
        // ordinary CPU-assembled root path.

        auto cullResult = s_Data.GPUFrustumCuller->Cull(
            std::span<const InstanceData>{ packed.GetData(), static_cast<sizet>(packed.Num()) }, mesh->GetIndexCount(), mesh->GetBaseIndex(), sphereUniform, kRadiusExpansion, reflectedRootLayout);
        CommandPacket* packet = buildPacket(cullResult.OutputBuffer->GetStorage()->GetRHIHandle(),
                                            cullResult.IndirectBuffer->GetRHIHandle(),
                                            cullResult.RootDataBuffer ? cullResult.RootDataBuffer->GetRHIHandle()
                                                                      : RHI::ResourceHandle{},
                                            cullResult.RootDataAddressOffsetBytes);
        if (overlayRoute)
        {
            if (packet)
                SubmitForwardOverlayPacket(packet);
            return nullptr; // caller's follow-up SubmitPacket(packet) becomes a safe no-op
        }
        return packet;
    }

    CommandPacket* Renderer3D::DrawAnimatedMesh(const Ref<Mesh>& mesh, const glm::mat4& modelMatrix, const Material& material, std::span<const glm::mat4> boneMatrices, bool isStatic, i32 entityID, u32 gpuSceneDrawLink)
    {
        // Delegate to the variant that accepts previous-frame bone matrices; pass an empty
        // vector so the callee treats prev as "same as current" (zero per-bone motion).
        constexpr std::span<const glm::mat4> s_EmptyPrev;
        return DrawAnimatedMesh(mesh, modelMatrix, material, boneMatrices, s_EmptyPrev, isStatic, entityID,
                                gpuSceneDrawLink);
    }

    CommandPacket* Renderer3D::DrawAnimatedMesh(const Ref<Mesh>& mesh, const glm::mat4& modelMatrix, const Material& material, std::span<const glm::mat4> boneMatrices, std::span<const glm::mat4> prevBoneMatrices, bool isStatic, i32 entityID, u32 gpuSceneDrawLink)
    {
        OLO_PROFILE_FUNCTION();

        if (!s_Data.Pipeline->FrameCorePasses.Scene)
        {
            OLO_CORE_ERROR("Renderer3D::DrawAnimatedMesh: ScenePass is null!");
            return nullptr;
        }

        ++s_Data.Stats.TotalMeshes;

        // For animated meshes, be more conservative with frustum culling
        // since bone transforms can move vertices significantly beyond rest pose bounds.
        if (s_Data.FrustumCullingEnabled && (isStatic || s_Data.DynamicCullingEnabled))
        {
            // For animated draws, expand the bounding sphere more aggressively to account for skinning deformation.
            if (!mesh || !mesh->GetMeshSource())
            {
                OLO_CORE_ERROR("Renderer3D::DrawAnimatedMesh: Invalid mesh or mesh source for frustum culling!");
                return nullptr;
            }

            BoundingSphere animatedSphere = mesh->GetTransformedBoundingSphere(modelMatrix);
            // Use a larger expansion factor for animated meshes to account for potential deformation.
            animatedSphere.Radius *= 2.0f; // More conservative than the standard 1.3f for static meshes.
            // ...plus the morph half of the same motion, which the 2x above does not
            // bound because it is a fudge for SKINNING (#1227).
            animatedSphere.Radius += MorphBoundsSlack(mesh, modelMatrix);

            if (!s_Data.ViewFrustum.IsBoundingSphereVisible(animatedSphere))
            {
                ++s_Data.Stats.CulledMeshes;
                return nullptr;
            }
        }

        if (!mesh || !mesh->GetMeshSource())
        {
            OLO_CORE_ERROR("Renderer3D::DrawAnimatedMesh: Invalid mesh or mesh source!");
            return nullptr;
        }

        auto meshSource = mesh->GetMeshSource();

        // Validate that the mesh supports skinning.
        OLO_CORE_ASSERT(meshSource->HasSkeleton(), "Animated mesh must have a skeleton!");
        OLO_CORE_ASSERT(!boneMatrices.empty(), "Bone matrices cannot be empty for animated mesh!");

        const auto* skeleton = meshSource->GetSkeleton();
        OLO_CORE_ASSERT(skeleton, "Mesh skeleton cannot be null!");

        // Validate bone matrix count matches skeleton bone count.
        if (boneMatrices.size() != skeleton->m_BoneNames.size())
        {
            OLO_CORE_ERROR("Bone matrices count ({}) must match skeleton bone count ({})",
                           boneMatrices.size(), skeleton->m_BoneNames.size());
            OLO_CORE_ASSERT(false, "Bone matrix count mismatch!");
        }

        static bool s_FirstRun = true;
        if (s_FirstRun)
        {
            OLO_CORE_INFO("Renderer3D::DrawAnimatedMesh: First animated mesh with {} bone influences", meshSource->GetBoneInfluences().Num());
            s_FirstRun = false;
        }

        if (!meshSource->HasBoneInfluences())
        {
            OLO_CORE_WARN("Renderer3D::DrawAnimatedMesh: Mesh has no bone influences (size: {}), falling back to regular mesh rendering",
                          meshSource->GetBoneInfluences().Num());
            return DrawMesh(mesh, modelMatrix, material, isStatic);
        }

        Ref<Shader> shaderToUse;
        // See DrawMesh() for the non-PBR-deferred → ForwardOverlayPass
        // rerouting rationale. The same reasoning applies here: non-PBR
        // skinned draws would otherwise alias their MRT outputs onto the
        // G-Buffer slots and corrupt every subsequent pixel.
        bool overlayRoute = false;
        if (material.GetShader())
        {
            shaderToUse = material.GetShader();
            // Same deferred-capability guard as DrawMesh — a forward-only
            // override on the Deferred path must be rerouted to
            // ForwardOverlayPass so it doesn't alias forward outputs onto
            // G-Buffer slots.
            if (s_Data.Settings.Path == RenderingPath::Deferred && s_Data.Pipeline->RenderStreamPasses.ForwardOverlay &&
                !IsDeferredCapableShader(shaderToUse))
            {
                overlayRoute = true;
            }
        }
        else if (material.GetType() == MaterialType::PBR)
        {
            // Transmission has no G-Buffer representation (issue #970). The
            // SKINNED forward shader has the same closure as the static one, so
            // a rigged glass mesh reroutes exactly like a static one — this path
            // was missed on the first cut and rendered skinned glass opaque.
            const bool deferred = s_Data.Settings.Path == RenderingPath::Deferred;
            if (ShouldRerouteTransmissiveToForwardOverlay(material, deferred,
                                                          s_Data.Pipeline->RenderStreamPasses.ForwardOverlay != nullptr,
                                                          s_Data.PBRSkinnedShader))
            {
                shaderToUse = s_Data.PBRSkinnedShader;
                overlayRoute = true;
            }
            else if (deferred && s_Data.PBRGBufferSkinnedShader)
                shaderToUse = s_Data.PBRGBufferSkinnedShader;
            else
                shaderToUse = s_Data.PBRSkinnedShader;
        }
        else
        {
            shaderToUse = s_Data.DefaultForwardSkinnedShader;
            if (s_Data.Settings.Path == RenderingPath::Deferred && s_Data.Pipeline->RenderStreamPasses.ForwardOverlay)
                overlayRoute = true;
        }

        if (!shaderToUse)
        {
            OLO_CORE_WARN("Renderer3D::DrawAnimatedMesh: Preferred shader not available, falling back to default forward shader");
            shaderToUse = s_Data.DefaultForwardShader;
            if (s_Data.Settings.Path == RenderingPath::Deferred && s_Data.Pipeline->RenderStreamPasses.ForwardOverlay)
                overlayRoute = true;
        }
        if (!shaderToUse)
        {
            OLO_CORE_ERROR("Renderer3D::DrawAnimatedMesh: No shader available!");
            return nullptr;
        }

        if (boneMatrices.empty())
        {
            OLO_CORE_WARN("Renderer3D::DrawAnimatedMesh: No bone matrices provided, using identity matrices");
        }

        // Check if VAO is valid before proceeding.
        auto vertexArray = mesh->GetVertexArray();
        if (!vertexArray)
        {
            OLO_CORE_ERROR("Renderer3D::DrawAnimatedMesh: Mesh has null VAO (Vertex Array Object)!");
            return nullptr;
        }

        // Validate the GPU resources BEFORE reserving anything for this draw.
        // Both the bone-matrix reservations below and the command packet come
        // out of the per-frame arena, and bailing after them strands that space
        // until the frame ends — every frame, for a mesh whose resources never
        // become valid.
        const RHI::ResourceHandle vertexArrayID = vertexArray->GetRHIHandle();
        const RHI::ResourceHandle shaderRendererID = shaderToUse->GetRHIHandle();
        if (!ValidateDrawMeshResources("Renderer3D::DrawAnimatedMesh", vertexArrayID, shaderRendererID))
            return nullptr;

        // Allocate space in FrameDataBuffer for bone matrices.
        FrameDataBuffer& frameBuffer = FrameDataBufferManager::Get();
        u32 boneCount = static_cast<u32>(boneMatrices.size());
        u32 boneBufferOffset = frameBuffer.AllocateBoneMatrices(boneCount);
        if (boneBufferOffset == UINT32_MAX)
        {
            OLO_CORE_ERROR("Renderer3D::DrawAnimatedMesh: Failed to allocate bone buffer space");
            return nullptr;
        }
        frameBuffer.WriteBoneMatrices(boneBufferOffset, boneMatrices.data(), boneCount);

        // Previous-frame bones: the skinned shader variants (PBR_GBuffer_Skinned
        // in Deferred, PBR_MultiLight_Skinned in Forward/Forward+) bind a
        // parallel PrevBoneMatrices UBO at binding 31 and use it to emit per-
        // bone velocity alongside u_PrevModel. When the caller doesn't provide
        // a prev pose, CommandDispatch::UploadBoneMatrices aliases the current
        // palette into the prev UBO so the shader always reads valid data and
        // the resulting bone-motion term is zero.
        u32 prevBoneBufferOffset = UINT32_MAX;
        const bool wantPrevStream = !prevBoneMatrices.empty() &&
                                    prevBoneMatrices.size() == boneMatrices.size();
        if (wantPrevStream)
        {
            u32 prevOffset = frameBuffer.AllocateBoneMatrices(boneCount);
            if (prevOffset != UINT32_MAX)
            {
                frameBuffer.WriteBoneMatrices(prevOffset, prevBoneMatrices.data(), boneCount);
                prevBoneBufferOffset = prevOffset;
            }
            // Else: fall back to aliasing current (no spare FDB space this frame).
        }

        // Create POD command.
        CommandPacket* packet = overlayRoute
                                    ? CreateForwardOverlayDrawCall<DrawMeshCommand>()
                                    : CreateDrawCall<DrawMeshCommand>();
        if (!packet)
            return nullptr;
        auto* cmd = packet->GetCommandData<DrawMeshCommand>();
        cmd->header.type = CommandType::DrawMesh;

        // Store asset handles and renderer IDs (POD).
        cmd->meshHandle = mesh->GetHandle();
        cmd->vertexArrayID = vertexArrayID;
        cmd->indexCount = mesh->GetIndexCount();
        cmd->baseIndex = mesh->GetBaseIndex();
        cmd->transform = modelMatrix;
        // Prev-transform applies to all paths — see DrawMesh() comment. Both
        // the forward PBR_MultiLight_Skinned and deferred PBR_GBuffer_Skinned
        // variants consume u_PrevModel + the prev-bone palette (binding 31)
        // to emit per-bone velocity into their respective velocity targets
        // (scene FB RT3 in Forward/Forward+, G-Buffer RT3 in Deferred).
        cmd->prevTransform = GetAndRecordPrevTransform(entityID, cmd->transform);
        cmd->shaderHandle = shaderToUse->GetHandle();

        // Material data via table.
        cmd->materialDataIndex = FrameDataBufferManager::Get().AllocateMaterialData(
            CreatePODMaterialDataForMaterial(material, shaderRendererID));

        // Render state via table.
        cmd->renderStateIndex = FrameDataBufferManager::Get().AllocateRenderState(CreatePODRenderStateForMaterial(material));

        // Animation support - store offset/count into FrameDataBuffer.
        cmd->isAnimatedMesh = true;
        cmd->boneBufferOffset = boneBufferOffset;
        cmd->prevBoneBufferOffset = prevBoneBufferOffset;
        cmd->boneCount = boneCount;

        // The canonical record this animated submesh was staged as (#1228).
        // GPUSceneDrawLinkNone keeps the pre-#1228 behaviour exactly.
        cmd->gpuSceneDrawLink = gpuSceneDrawLink;

        // Entity ID for picking.
        cmd->entityID = entityID;

        if (static bool s_LoggedBoneMatrices = false; !s_LoggedBoneMatrices && !boneMatrices.empty())
        {
            OLO_CORE_INFO("DrawAnimatedMesh: Storing {} bone matrices at offset {} in FrameDataBuffer", boneCount, boneBufferOffset);
            s_LoggedBoneMatrices = true;
        }

        packet->SetCommandType(cmd->header.type);
        packet->SetDispatchFunction(CommandDispatch::GetDispatchFunction(cmd->header.type));

        // Set sort key for animated mesh commands.
        PacketMetadata metadata = packet->GetMetadata();
        u32 shaderID = shaderRendererID.Index & 0xFFFF;
        u32 materialID = ComputeMaterialID(material);
        u32 depth = ComputeDepthForSortKey(modelMatrix);
        if (material.GetFlag(MaterialFlag::Blend))
            metadata.m_SortKey = DrawKey::CreateTransparent(0, ViewLayerType::ThreeD, shaderID, materialID, depth);
        else
            metadata.m_SortKey = DrawKey::CreateOpaque(0, ViewLayerType::ThreeD, shaderID, materialID, depth);
        metadata.m_IsStatic = isStatic;
        metadata.m_DebugName = GetMeshDebugName(mesh);
        packet->SetMetadata(metadata);

        if (overlayRoute)
        {
            // Route the packet to the overlay bucket so it renders after
            // DeferredLightingPass composites the G-Buffer. Return nullptr
            // so the caller's SubmitPacket(packet) is a no-op.
            SubmitForwardOverlayPacket(packet);
            return nullptr;
        }

        return packet;
    }

#if 0 // Retired by #1289: animated submission remains on Scene's serial GPUScene-aware path.
    void Renderer3D::RenderAnimatedMeshes(const Ref<Scene>& scene, const Material& defaultMaterial)
    {
        OLO_PROFILE_FUNCTION();

        if (static bool s_FirstRun = true; s_FirstRun)
        {
            OLO_CORE_INFO("Renderer3D::RenderAnimatedMeshes: Starting animated mesh rendering");
            s_FirstRun = false;
        }

        if (!scene)
        {
            OLO_CORE_WARN("Renderer3D::RenderAnimatedMeshes: Scene is null");
            return;
        }

        // NOT the live animated path. Scene's own animated-mesh loop is what renders
        // (see the note on the previous-pose gate there); this function has no
        // callers, and #1226 shipped a guard here that was therefore decorative.
        // It consequently does NOT resolve the shared animated surface's conventional
        // LOD level the way Scene::SelectAnimatedSurfaceLOD does (#1227): anything
        // that revives this path has to route through AnimatedSurfaceSource first,
        // or it will draw an undeformed LOD 0 while the morph pass writes elsewhere.
        auto view = scene->GetAllEntitiesWith<MeshComponent, SkeletonComponent, TransformComponent>();

        // Collect mesh descriptors for parallel submission.
        std::vector<MeshSubmitDesc> meshDescriptors;
        meshDescriptors.reserve(32); // Pre-allocate for typical case.

        sizet entityCount = 0;
        for (auto entityID : view)
        {
            Entity entity = { entityID, scene.get() };
            ++s_Data.Stats.TotalAnimatedMeshes;
            ++entityCount;

            // Validate components.
            if (!entity.HasComponent<MeshComponent>() ||
                !entity.HasComponent<SkeletonComponent>() ||
                !entity.HasComponent<TransformComponent>())
            {
                ++s_Data.Stats.SkippedAnimatedMeshes;
                continue;
            }

            auto& meshComp = entity.GetComponent<MeshComponent>();
            auto& skeletonComp = entity.GetComponent<SkeletonComponent>();
            const auto& transformComp = entity.GetComponent<TransformComponent>();

            if (!meshComp.m_MeshSource || !skeletonComp.m_Skeleton)
            {
                ++s_Data.Stats.SkippedAnimatedMeshes;
                continue;
            }

            glm::mat4 worldTransform = transformComp.GetTransform();
            const auto& boneMatrices = skeletonComp.m_Skeleton->m_FinalBoneMatrices;
            // Only offer a previous pose when the skeleton actually has one
            // (#1226). After a discontinuity -- a skeleton swap, a bone-count
            // change, entering Play -- the previous palette describes a pose
            // this skeleton was never in, and handing it to the shaders emits a
            // velocity across the seam that TAA and motion blur faithfully
            // smear. Passing none makes CommandDispatch alias the current
            // palette into the prev slot, i.e. exactly zero bone motion.
            static const std::vector<glm::mat4> s_NoBoneHistory;
            const auto& prevBoneMatrices = skeletonComp.m_Skeleton->HasBoneHistory()
                                               ? skeletonComp.m_Skeleton->m_PrevFinalBoneMatrices
                                               : s_NoBoneHistory;
            const i32 pickEntityID = static_cast<i32>(std::to_underlying(entityID));

            // Get material from entity or use default.
            Material material = defaultMaterial;
            if (entity.HasComponent<MaterialComponent>())
            {
                material = entity.GetComponent<MaterialComponent>().m_Material;
            }

            // Check for RelationshipComponent to find child submeshes.
            bool foundSubmeshes = false;
            if (entity.HasComponent<RelationshipComponent>())
            {
                const auto& relationshipComponent = entity.GetComponent<RelationshipComponent>();
                for (const UUID& childUUID : relationshipComponent.m_Children)
                {
                    auto submeshEntityOpt = scene->TryGetEntityWithUUID(childUUID);
                    if (submeshEntityOpt && submeshEntityOpt->HasComponent<SubmeshComponent>())
                    {
                        auto& submeshComponent = submeshEntityOpt->GetComponent<SubmeshComponent>();
                        if (submeshComponent.m_Mesh && submeshComponent.m_Visible)
                        {
                            // Get submesh material if available.
                            Material submeshMaterial = material;
                            if (submeshEntityOpt->HasComponent<MaterialComponent>())
                            {
                                submeshMaterial = submeshEntityOpt->GetComponent<MaterialComponent>().m_Material;
                            }

                            // Populate prev-world-transform from the shared
                            // per-entity cache on the main thread before we hand
                            // the desc to the parallel worker. Without this the
                            // parallel path drops object motion and TAA /
                            // MotionBlur see zero velocity for moving skinned
                            // meshes. The cache is maintained in Deferred paths
                            // via GetAndRecordPrevTransform; when no history
                            // exists yet it returns current, giving zero motion.
                            const glm::mat4 prevWorldTransform =
                                GetAndRecordPrevTransform(pickEntityID, worldTransform);
                            MeshSubmitDesc desc{};
                            desc.Mesh = submeshComponent.m_Mesh;
                            desc.Transform = worldTransform;
                            desc.MaterialData = submeshMaterial;
                            desc.IsStatic = false;
                            desc.EntityID = pickEntityID;
                            desc.IsAnimated = true;
                            desc.BoneMatrices = &boneMatrices;
                            desc.PrevBoneMatrices = &prevBoneMatrices;
                            desc.PrevTransform = prevWorldTransform;
                            desc.HasPrevTransform = true;
                            meshDescriptors.push_back(std::move(desc));
                            foundSubmeshes = true;
                        }
                    }
                }
            }

            // Fallback: if no submesh entities found, use first submesh from MeshSource.
            if (!foundSubmeshes && meshComp.m_MeshSource->GetSubmeshes().Num() > 0)
            {
                auto mesh = Ref<Mesh>::Create(meshComp.m_MeshSource, 0);
                const glm::mat4 prevWorldTransform =
                    GetAndRecordPrevTransform(pickEntityID, worldTransform);
                MeshSubmitDesc desc{};
                desc.Mesh = mesh;
                desc.Transform = worldTransform;
                desc.MaterialData = material;
                desc.IsStatic = false;
                desc.EntityID = pickEntityID;
                desc.IsAnimated = true;
                desc.BoneMatrices = &boneMatrices;
                desc.PrevBoneMatrices = &prevBoneMatrices;
                desc.PrevTransform = prevWorldTransform;
                desc.HasPrevTransform = true;
                meshDescriptors.push_back(std::move(desc));
            }

            ++s_Data.Stats.RenderedAnimatedMeshes;
        }

        // Submit all animated meshes in parallel.
        if (!meshDescriptors.empty())
        {
            SubmitMeshesParallel(meshDescriptors);
        }

        // Log stats when count changes.
        static sizet s_LastEntityCount = 0;
        if (entityCount != s_LastEntityCount)
        {
            OLO_CORE_INFO("RenderAnimatedMeshes: Found {} animated entities, {} submeshes",
                          entityCount, meshDescriptors.size());
            s_LastEntityCount = entityCount;
        }
    }

    void Renderer3D::RenderAnimatedMesh(const Ref<Scene>& scene, Entity entity, const Material& defaultMaterial)
    {
        OLO_PROFILE_FUNCTION();

        if (!entity.HasComponent<MeshComponent>() ||
            !entity.HasComponent<SkeletonComponent>() ||
            !entity.HasComponent<TransformComponent>())
        {
            ++s_Data.Stats.SkippedAnimatedMeshes;
            return;
        }

        auto& meshComp = entity.GetComponent<MeshComponent>();
        auto& skeletonComp = entity.GetComponent<SkeletonComponent>();
        const auto& transformComp = entity.GetComponent<TransformComponent>();

        if (!meshComp.m_MeshSource || !skeletonComp.m_Skeleton)
        {
            OLO_CORE_WARN("Renderer3D::RenderAnimatedMesh: Entity {} has invalid mesh or skeleton",
                          entity.GetComponent<TagComponent>().Tag);
            ++s_Data.Stats.SkippedAnimatedMeshes;
            return;
        }

        glm::mat4 worldTransform = transformComp.GetTransform();

        // Get current + previous bone matrices from the skeleton. The prev
        // pose feeds motion-vector computation in animated PBR shaders so
        // TAA / MotionBlur get correct per-bone velocity rather than a
        // stale-identity fallback -- but only when the skeleton HAS a previous
        // pose (#1226). After a discontinuity the previous palette describes a
        // pose this skeleton was never in, and offering none makes
        // CommandDispatch alias the current palette, i.e. zero bone motion.
        static const std::vector<glm::mat4> s_NoBoneHistory;
        const auto& boneMatrices = skeletonComp.m_Skeleton->m_FinalBoneMatrices;
        const auto& prevBoneMatrices = skeletonComp.m_Skeleton->HasBoneHistory()
                                           ? skeletonComp.m_Skeleton->m_PrevFinalBoneMatrices
                                           : s_NoBoneHistory;

        // Convert entt entity id to the i32 picking ID used by the editor.
        const i32 entityID = static_cast<i32>(static_cast<u32>(entity));

        // Use MaterialComponent if available, otherwise use default material.
        Material material = defaultMaterial;
        if (entity.HasComponent<MaterialComponent>())
        {
            material = entity.GetComponent<MaterialComponent>().m_Material;
        }


        // Find and render all child entities with SubmeshComponent.
        bool renderedAnySubmesh = false;

        // Check if entity has RelationshipComponent before accessing it.
        if (!entity.HasComponent<RelationshipComponent>())
        {
            OLO_CORE_WARN("DrawAnimatedMesh: Entity does not have RelationshipComponent, cannot render submeshes");
            return;
        }

        const auto& relationshipComponent = entity.GetComponent<RelationshipComponent>();
        for (const UUID& childUUID : relationshipComponent.m_Children)
        {
            auto submeshEntityOpt = scene->TryGetEntityWithUUID(childUUID);
            if (submeshEntityOpt && submeshEntityOpt->HasComponent<SubmeshComponent>())
            {
                auto& submeshComponent = submeshEntityOpt->GetComponent<SubmeshComponent>();
                if (submeshComponent.m_Mesh && submeshComponent.m_Visible)
                {
                    // Use MaterialComponent if available on submesh, otherwise use the parent's material.
                    Material submeshMaterial = material;
                    if (submeshEntityOpt->HasComponent<MaterialComponent>())
                    {
                        submeshMaterial = submeshEntityOpt->GetComponent<MaterialComponent>().m_Material;
                    }

                    auto* packet = DrawAnimatedMesh(
                        submeshComponent.m_Mesh,
                        worldTransform,
                        submeshMaterial,
                        boneMatrices,
                        prevBoneMatrices,
                        false,
                        entityID);

                    if (packet)
                    {
                        SubmitPacket(packet);
                        renderedAnySubmesh = true;
                    }
                }
            }
        }

        // Fallback: if no submesh entities found, create a mesh from the first submesh.
        if (!renderedAnySubmesh && meshComp.m_MeshSource->GetSubmeshes().Num() > 0)
        {
            auto mesh = Ref<Mesh>::Create(meshComp.m_MeshSource, 0);

            auto* packet = DrawAnimatedMesh(
                mesh,
                worldTransform,
                material,
                boneMatrices,
                prevBoneMatrices,
                false,
                entityID);

            if (packet)
            {
                SubmitPacket(packet);
                renderedAnySubmesh = true;
            }
        }

        if (renderedAnySubmesh)
        {
            ++s_Data.Stats.RenderedAnimatedMeshes;
        }
    }

#endif

    CommandPacket* Renderer3D::DrawMeshParallel(WorkerSubmitContext& ctx,
                                                const Ref<Mesh>& mesh,
                                                const glm::mat4& modelMatrix,
                                                const Material& material,
                                                bool isStatic,
                                                i32 entityID,
                                                const LODGroup* lodGroup,
                                                const glm::mat4* prevModelMatrix)
    {
        OLO_PROFILE_FUNCTION();

        if (!ctx.Allocator || !ctx.SceneContext)
        {
            OLO_CORE_ERROR("Renderer3D::DrawMeshParallel: Invalid worker context!");
            return nullptr;
        }

        // Frustum culling using parallel scene context.
        if (ctx.SceneContext->FrustumCullingEnabled &&
            (isStatic || ctx.SceneContext->DynamicCullingEnabled))
        {
            if (mesh)
            {
                BoundingSphere sphere = mesh->GetTransformedBoundingSphere(modelMatrix);
                sphere.Radius *= 1.3f;

                if (!ctx.SceneContext->ViewFrustum.IsBoundingSphereVisible(sphere))
                {
                    ++ctx.MeshesCulled;
                    return nullptr;
                }
            }
        }

        // LOD selection.
        Ref<Mesh> meshToUse;
        // Culling-camera LOD params, not the render camera's (issue #726) - see the
        // single-threaded twin in DrawMesh().
        if (const auto lodResult = SelectLODMesh(mesh, modelMatrix, ctx.SceneContext->LODView, lodGroup, meshToUse); lodResult.SelectedLODIndex >= 0)
        {
            if (lodResult.SelectedLODIndex >= ctx.ObjectsPerLODLevel.Num())
            {
                ctx.ObjectsPerLODLevel.SetNumZeroed(static_cast<i64>(lodResult.SelectedLODIndex + 1));
            }
            ++ctx.ObjectsPerLODLevel[lodResult.SelectedLODIndex];
            if (lodResult.Switched)
            {
                ++ctx.LODSwitches;
            }
        }

        if (!meshToUse || !meshToUse->GetVertexArray())
        {
            OLO_CORE_ERROR("Renderer3D::DrawMeshParallel: Invalid mesh or vertex array!");
            return nullptr;
        }

        // Select shader from parallel context.
        Ref<Shader> shaderToUse;
        if (material.GetShader())
        {
            shaderToUse = material.GetShader();
        }
        else if (material.GetType() == MaterialType::PBR)
        {
            // Transmission has no G-Buffer representation (issue #970).
            //
            // ctx.SceneContext->PBRShader is ALREADY swapped to PBRGBufferShader
            // on the Deferred path (RenderPipeline.cpp), so a transmissive
            // material would look deferred-capable here and the overlayReroute
            // gate below would stay false — the draw would land in the G-Buffer
            // and shade opaque. Selecting the genuine forward shader instead
            // makes that same gate route it to ForwardOverlayPass, with no
            // second copy of the rule.
            const bool deferred = s_Data.Settings.Path == RenderingPath::Deferred;
            if (ShouldRerouteTransmissiveToForwardOverlay(material, deferred,
                                                          s_Data.Pipeline->RenderStreamPasses.ForwardOverlay != nullptr,
                                                          s_Data.PBRShader))
            {
                shaderToUse = s_Data.PBRShader;
            }
            else
            {
                shaderToUse = ctx.SceneContext->PBRShader;
            }
        }
        else
        {
            shaderToUse = ctx.SceneContext->DefaultForwardShader;
        }

        if (!shaderToUse)
        {
            OLO_CORE_ERROR("Renderer3D::DrawMeshParallel: No shader available!");
            return nullptr;
        }

        // Deferred-path gating: workers submit exclusively into ScenePass's
        // per-thread bucket, which is the G-Buffer producer. Non-PBR /
        // forward-only override shaders on this path would alias forward
        // outputs onto G-Buffer slots (breaking lighting for every
        // subsequent pixel). Instead of dropping the draw we reroute the
        // fully-assembled packet into ForwardOverlayPass's global bucket,
        // matching the serial `DrawMesh` overlay-reroute behaviour so
        // worker-submitted forward-only materials render as overlays
        // after DeferredLightingPass composes the G-Buffer.
        //
        // Note: `ForwardOverlayPass::SubmitPacket` is mutex-protected
        // (CommandBucket's internal lock) so calling it from a worker is
        // safe — the serialisation cost is acceptable for rare forward-
        // only materials on the Deferred path. The packet memory is still
        // owned by the worker's allocator (both allocators reset at end-
        // of-frame; the overlay bucket stores pointers, not copies).
        //
        // In Deferred `ctx.SceneContext->PBRShader` is already swapped to
        // `PBRGBufferShader` (see ParallelContext init), so an un-overridden
        // PBR material never triggers this reroute.
        const bool overlayReroute = (s_Data.Settings.Path == RenderingPath::Deferred) &&
                                    !IsDeferredCapableShader(shaderToUse) &&
                                    s_Data.Pipeline->RenderStreamPasses.ForwardOverlay;

        const RHI::ResourceHandle vertexArrayID = meshToUse->GetVertexArray()->GetRHIHandle();
        const RHI::ResourceHandle shaderRendererID = shaderToUse->GetRHIHandle();
        if (!ValidateDrawMeshResources("Renderer3D::DrawMeshParallel", vertexArrayID, shaderRendererID))
            return nullptr;

        // Create POD command using worker's allocator.
        PacketMetadata initialMetadata;
        CommandPacket* packet = ctx.Allocator->AllocatePacketWithCommand<DrawMeshCommand>(initialMetadata);
        if (!packet)
        {
            OLO_CORE_ERROR("Renderer3D::DrawMeshParallel: Failed to allocate command packet!");
            return nullptr;
        }

        auto* cmd = packet->GetCommandData<DrawMeshCommand>();
        cmd->header.type = CommandType::DrawMesh;

        // Store asset handles and renderer IDs (POD).
        cmd->meshHandle = meshToUse->GetHandle();
        cmd->vertexArrayID = vertexArrayID;
        cmd->indexCount = meshToUse->GetIndexCount();
        cmd->baseIndex = meshToUse->GetBaseIndex();
        cmd->transform = glm::mat4(modelMatrix);
        // When the caller has prev-frame history, use it; otherwise alias
        // current so motion vectors are zero for this draw. Parallel workers
        // cannot touch the main-thread entity motion-history map, so the
        // caller (typically via MeshSubmitDesc::PrevTransform) must supply
        // the history for per-object velocity to be correct.
        cmd->prevTransform = prevModelMatrix ? *prevModelMatrix : cmd->transform;
        cmd->shaderHandle = shaderToUse->GetHandle();

        // Material data via table.
        cmd->materialDataIndex = FrameDataBufferManager::Get().AllocateMaterialData(
            CreatePODMaterialDataForMaterial(material, shaderRendererID));

        // Render state via table.
        cmd->renderStateIndex = FrameDataBufferManager::Get().AllocateRenderState(CreatePODRenderStateForMaterial(material));

        // Entity ID for picking.
        cmd->entityID = entityID;

        // No bone matrices for non-animated mesh.
        cmd->isAnimatedMesh = false;
        cmd->boneBufferOffset = 0;
        cmd->boneCount = 0;

        packet->SetCommandType(cmd->header.type);
        packet->SetDispatchFunction(CommandDispatch::GetDispatchFunction(cmd->header.type));

        // Set sort key using parallel context view matrix for depth.
        PacketMetadata metadata = packet->GetMetadata();
        const u32 shaderID = shaderRendererID.Index & 0xFFFF;
        const u32 materialID = ComputeMaterialID(material);
        const u32 depthKey = ComputeDepthForSortKeyWithView(modelMatrix, ctx.SceneContext->ViewMatrix);

        if (material.GetFlag(MaterialFlag::Blend))
            metadata.m_SortKey = DrawKey::CreateTransparent(0, ViewLayerType::ThreeD, shaderID, materialID, depthKey);
        else
            metadata.m_SortKey = DrawKey::CreateOpaque(0, ViewLayerType::ThreeD, shaderID, materialID, depthKey);
        metadata.m_IsStatic = isStatic;
        metadata.m_DebugName = GetMeshDebugName(meshToUse);
        packet->SetMetadata(metadata);

        if (overlayReroute)
        {
            // Hand the packet off to the overlay bucket directly and return
            // nullptr so the caller's follow-up SubmitPacketParallel becomes
            // a no-op (mirrors the serial DrawMesh overlay-reroute pattern).
            // The global bucket's mutex serialises worker submissions; the
            // volume of forward-only draws on Deferred is expected to be
            // small enough that this doesn't become a contention point.
            SubmitRenderStreamPacket(RenderStreamType::ForwardOverlay, packet);
            return nullptr;
        }

        return packet;
    }

#if 0 // Retired by #1289: benchmarked animated worker recording regressed CPU time and scene output.
    CommandPacket* Renderer3D::DrawAnimatedMeshParallel(WorkerSubmitContext& ctx,
                                                        const Ref<Mesh>& mesh,
                                                        const glm::mat4& modelMatrix,
                                                        const Material& material,
                                                        std::span<const glm::mat4> boneMatrices,
                                                        bool isStatic,
                                                        i32 entityID,
                                                        u32 gpuSceneDrawLink)
    {
        // Legacy entry point: no prev-pose information available. Alias current
        // bones and transform into the prev slot so motion-vector shaders see
        // zero per-bone and per-object motion for this draw.
        constexpr std::span<const glm::mat4> s_EmptyPrev;
        return DrawAnimatedMeshParallel(ctx, mesh, modelMatrix, material, boneMatrices,
                                        s_EmptyPrev, modelMatrix, /*hasPrevTransform*/ false, isStatic,
                                        entityID, gpuSceneDrawLink);
    }

    CommandPacket* Renderer3D::DrawAnimatedMeshParallel(WorkerSubmitContext& ctx,
                                                        const Ref<Mesh>& mesh,
                                                        const glm::mat4& modelMatrix,
                                                        const Material& material,
                                                        std::span<const glm::mat4> boneMatrices,
                                                        std::span<const glm::mat4> prevBoneMatrices,
                                                        const glm::mat4& prevModelMatrix,
                                                        bool hasPrevTransform,
                                                        bool isStatic,
                                                        i32 entityID,
                                                        u32 gpuSceneDrawLink)
    {
        OLO_PROFILE_FUNCTION();

        if (!ctx.Allocator || !ctx.SceneContext)
        {
            OLO_CORE_ERROR("Renderer3D::DrawAnimatedMeshParallel: Invalid worker context!");
            return nullptr;
        }

        // For animated meshes, be more conservative with frustum culling.
        if (ctx.SceneContext->FrustumCullingEnabled &&
            (isStatic || ctx.SceneContext->DynamicCullingEnabled))
        {
            if (mesh && mesh->GetMeshSource())
            {
                BoundingSphere animatedSphere = mesh->GetTransformedBoundingSphere(modelMatrix);
                animatedSphere.Radius *= 2.0f;
                animatedSphere.Radius += MorphBoundsSlack(mesh, modelMatrix); // #1227

                if (!ctx.SceneContext->ViewFrustum.IsBoundingSphereVisible(animatedSphere))
                {
                    ++ctx.MeshesCulled;
                    return nullptr;
                }
            }
        }

        if (!mesh || !mesh->GetMeshSource())
        {
            OLO_CORE_ERROR("Renderer3D::DrawAnimatedMeshParallel: Invalid mesh or mesh source!");
            return nullptr;
        }

        // Select skinned shader from parallel context.
        Ref<Shader> shaderToUse;
        if (material.GetShader())
        {
            shaderToUse = material.GetShader();
        }
        else if (material.GetType() == MaterialType::PBR)
        {
            // Transmission has no G-Buffer representation (issue #970), and
            // unlike DrawMeshParallel this path has NO overlay reroute — the
            // gate below DROPS a forward-only shader outright. Selecting the
            // forward skinned shader here would therefore make the mesh vanish,
            // which is worse than shading it opaque.
            //
            // So the G-Buffer shader is kept and the draw is COUNTED instead: a
            // rigged, transmissive mesh on the parallel path renders opaque, and
            // says so, until this path grows an overlay reroute of its own. The
            // serial DrawAnimatedMesh handles the same material correctly.
            if (s_Data.Settings.Path == RenderingPath::Deferred && material.IsTransmissive())
            {
                NoteTransmissiveDrawWithoutForwardOverlay();
            }
            shaderToUse = ctx.SceneContext->PBRSkinnedShader;
        }
        else
        {
            shaderToUse = ctx.SceneContext->DefaultForwardSkinnedShader;
        }

        if (!shaderToUse)
        {
            shaderToUse = ctx.SceneContext->DefaultForwardShader;
        }

        // Same Deferred gating as DrawMeshParallel — forward-only skinned
        // shaders submitted from a worker would corrupt the G-Buffer.
        if (s_Data.Settings.Path == RenderingPath::Deferred &&
            !IsDeferredCapableShader(shaderToUse))
        {
            if (static std::atomic<u64> s_WarnCount{ 0 }; s_WarnCount.fetch_add(1, std::memory_order_relaxed) < 8)
            {
                OLO_CORE_WARN("Renderer3D::DrawAnimatedMeshParallel: forward-only skinned shader on Deferred path — draw dropped (use serial DrawAnimatedMesh for overlay reroute)");
            }
            return nullptr;
        }

        if (!shaderToUse)
        {
            OLO_CORE_ERROR("Renderer3D::DrawAnimatedMeshParallel: No shader available!");
            return nullptr;
        }

        // Validate BEFORE reserving worker scratch or a packet — same reasoning
        // as the non-parallel path: both come out of per-frame storage that a
        // late bail-out strands for the rest of the frame.
        //
        // The null check mirrors DrawAnimatedMesh's. ValidateDrawMeshResources
        // takes handles, so it cannot catch an absent vertex array — by then
        // GetRHIHandle() has already been called on nothing.
        const auto vertexArray = mesh->GetVertexArray();
        if (!vertexArray)
        {
            OLO_CORE_ERROR("Renderer3D::DrawAnimatedMeshParallel: Mesh has null VAO (Vertex Array Object)!");
            return nullptr;
        }

        const RHI::ResourceHandle vertexArrayID = vertexArray->GetRHIHandle();
        const RHI::ResourceHandle shaderRendererID = shaderToUse->GetRHIHandle();
        if (!ValidateDrawMeshResources("Renderer3D::DrawAnimatedMeshParallel", vertexArrayID, shaderRendererID))
            return nullptr;

        // Allocate bone matrices in worker's scratch buffer.
        FrameDataBuffer& frameBuffer = FrameDataBufferManager::Get();
        const u32 boneCount = static_cast<u32>(boneMatrices.size());

        // Use parallel allocation API.
        const u32 localBoneOffset = frameBuffer.AllocateBoneMatricesParallel(ctx.WorkerIndex, boneCount);
        if (localBoneOffset == UINT32_MAX)
        {
            OLO_CORE_ERROR("Renderer3D::DrawAnimatedMeshParallel: Failed to allocate bone buffer space");
            return nullptr;
        }
        frameBuffer.WriteBoneMatricesParallel(ctx.WorkerIndex, localBoneOffset, boneMatrices.data(), boneCount);

        // Previous-frame pose: allocate a second palette in the same worker
        // scratch so skinned shaders (PBR_MultiLight_Skinned, PBR_GBuffer_Skinned)
        // can emit per-bone velocity via binding 31. Only do this when the
        // caller supplied a matching-size prev palette; otherwise we leave the
        // sentinel UINT32_MAX in the command so CommandDispatch::UploadBoneMatrices
        // aliases current into prev (zero per-bone motion). Allocation failure
        // silently falls back to alias-current for this draw only.
        u32 localPrevBoneOffset = UINT32_MAX;
        const bool wantPrevBones = !prevBoneMatrices.empty() &&
                                   prevBoneMatrices.size() == boneMatrices.size();
        if (wantPrevBones)
        {
            const u32 prevOffset = frameBuffer.AllocateBoneMatricesParallel(ctx.WorkerIndex, boneCount);
            if (prevOffset != UINT32_MAX)
            {
                frameBuffer.WriteBoneMatricesParallel(ctx.WorkerIndex, prevOffset, prevBoneMatrices.data(), boneCount);
                localPrevBoneOffset = prevOffset;
            }
        }

        // Create POD command using worker's allocator.
        PacketMetadata initialMetadata;
        CommandPacket* packet = ctx.Allocator->AllocatePacketWithCommand<DrawMeshCommand>(initialMetadata);
        if (!packet)
        {
            OLO_CORE_ERROR("Renderer3D::DrawAnimatedMeshParallel: Failed to allocate command packet!");
            return nullptr;
        }

        auto* cmd = packet->GetCommandData<DrawMeshCommand>();
        cmd->header.type = CommandType::DrawMesh;

        cmd->meshHandle = mesh->GetHandle();
        cmd->vertexArrayID = vertexArrayID;
        cmd->indexCount = mesh->GetIndexCount();
        cmd->baseIndex = mesh->GetBaseIndex();
        cmd->transform = modelMatrix;
        // Use caller-supplied prev transform when available; otherwise alias
        // current so u_PrevModel - u_Model = 0 and shader velocity is 0.
        cmd->prevTransform = hasPrevTransform ? prevModelMatrix : modelMatrix;
        cmd->shaderHandle = shaderToUse->GetHandle();

        // Material data via table.
        cmd->materialDataIndex = FrameDataBufferManager::Get().AllocateMaterialData(
            CreatePODMaterialDataForMaterial(material, shaderRendererID));

        cmd->renderStateIndex = FrameDataBufferManager::Get().AllocateRenderState(CreatePODRenderStateForMaterial(material));

        // Animation support - store worker-local offsets with remapping info.
        // Both current and (when present) prev offsets are worker-local and
        // must be remapped to global during EndParallelSubmission().
        cmd->isAnimatedMesh = true;
        cmd->boneBufferOffset = localBoneOffset;
        cmd->prevBoneBufferOffset = localPrevBoneOffset;
        cmd->boneCount = boneCount;
        cmd->workerIndex = static_cast<u8>(ctx.WorkerIndex);
        cmd->needsBoneOffsetRemap = true;

        // Picking ID, exactly as the serial DrawAnimatedMesh writes it. Omitting
        // it here left every animated draw on this route at the default -1, so
        // an animated mesh became unselectable in the editor as soon as its
        // batch was large enough for SubmitMeshesParallel to go parallel.
        cmd->entityID = entityID;

        // Carried, never minted here (#1228): see the declaration. The worker
        // copies an index the main thread produced; it never touches the link
        // table, which is what keeps concurrent recording free of a lock.
        cmd->gpuSceneDrawLink = gpuSceneDrawLink;

        packet->SetCommandType(cmd->header.type);
        packet->SetDispatchFunction(CommandDispatch::GetDispatchFunction(cmd->header.type));

        // Set sort key.
        PacketMetadata metadata = packet->GetMetadata();
        const u32 shaderID = shaderRendererID.Index & 0xFFFF;
        const u32 materialID = ComputeMaterialID(material);
        const u32 depthKey = ComputeDepthForSortKeyWithView(modelMatrix, ctx.SceneContext->ViewMatrix);

        if (material.GetFlag(MaterialFlag::Blend))
            metadata.m_SortKey = DrawKey::CreateTransparent(0, ViewLayerType::ThreeD, shaderID, materialID, depthKey);
        else
            metadata.m_SortKey = DrawKey::CreateOpaque(0, ViewLayerType::ThreeD, shaderID, materialID, depthKey);
        metadata.m_IsStatic = isStatic;
        metadata.m_DebugName = GetMeshDebugName(mesh);
        packet->SetMetadata(metadata);

        return packet;
    }

#endif

    u32 Renderer3D::SubmitMeshesParallel(std::span<const MeshSubmitDesc> meshes,
                                         i32 minBatchSize)
    {
        OLO_PROFILE_FUNCTION();

        if (meshes.empty())
        {
            return 0;
        }

        const i32 numMeshes = static_cast<i32>(meshes.size());

        // For small batches, use single-threaded path.
        if (numMeshes < minBatchSize * 2)
        {
            u32 totalSubmitted = 0;
            for (const auto& desc : meshes)
            {
                // DrawMesh records previous transforms in the shared cache.
                // Seed it when the caller already owns authoritative history.
                if (desc.HasPrevTransform && desc.EntityID >= 0)
                {
                    s_Data.PrevEntityTransforms.insert_or_assign(desc.EntityID, desc.PrevTransform);
                }
                CommandPacket* packet = DrawMesh(desc.Mesh, desc.Transform, desc.MaterialData, desc.IsStatic,
                                                 desc.EntityID, desc.LODGroupPtr);
                if (packet)
                {
                    // Baked lightmap region (issue #867), patched before
                    // submission — the same shape and the same `.x > 0` gate
                    // Scene.cpp's SubmitMeshSourceClassic uses.
                    if (desc.LightmapScaleOffset.x > 0.0f)
                    {
                        packet->GetCommandData<DrawMeshCommand>()->lightmapScaleOffset = desc.LightmapScaleOffset;

                        // A LIGHTMAPPED SKIN SURFACE LOSES ITS PER-PIXEL
                        // THICKNESS ON THE DEFERRED PATH (issue #1242), because
                        // G-Buffer RT5 is holding this pixel's baked irradiance
                        // and the thickness lane is the same channel. The
                        // irradiance wins — see oloSkinPackGBufferThickness for
                        // why that is the right way round — so the transmission
                        // term reads a thickness of 0 and does not fire.
                        //
                        // REPORTED HERE because this is the only site that knows
                        // BOTH facts: the material (through MaterialData) and
                        // whether this draw carries a lightmap region. The
                        // shader cannot log, and the material-fill function
                        // cannot see the lightmap.
                        //
                        // Counted even on the forward paths, where the term
                        // actually works, because the condition is a property of
                        // the ASSET rather than of the path: the same scene
                        // switched to Deferred will silently lose the effect, and
                        // that is worth knowing before the switch rather than
                        // after. The reason's name says which path it bites.
                        // `desc.MaterialData` is a Material, not the resolved
                        // POD, so the transport version has to come from the
                        // profile. Resolve() is the right call rather than a
                        // surprise: its own header says the slot is sticky and
                        // the cost is one asset-manager lookup per skin
                        // submission, and this site only reaches it for a
                        // lightmapped skin draw.
                        if (const Material& mat = desc.MaterialData;
                            mat.GetMaterialKind() == MaterialKind::Skin && mat.HasAuthoredThickness())
                        {
                            const SkinProfileResolution profile =
                                Renderer3D::GetSkinProfileTable().Resolve(mat.GetSkinProfileHandle());
                            // BOTH TRANSMITTING VERSIONS (issue #1243 appended
                            // the second). The versions are cumulative, so a
                            // version-3 profile transmits too — reporting only
                            // version 2 here would make the lightmap conflict
                            // stop being counted the moment an author moved a
                            // head forward, and this diagnostic exists precisely
                            // because the failure is otherwise invisible.
                            if (SkinEvaluatesThicknessTransmission(profile.Parameters.EvaluationModel))
                            {
                                Renderer3D::GetSkinProfileTable().ReportTransmissionFallback(
                                    SkinTransmissionFallbackReason::DeferredThicknessLaneUnavailable,
                                    mat.GetSkinProfileHandle());
                            }
                        }
                    }
                    SubmitPacket(packet);
                    ++totalSubmitted;
                }
            }
            return totalSubmitted;
        }

        // The scheduler may expose more tasks than the renderer's fixed worker
        // slots (including the calling thread). Existing contexts bound both
        // the worker tasks and the caller to valid allocator/bucket indices.
        BeginParallelSubmission();

        // Per-worker accumulator to track statistics.
        struct WorkerStats
        {
            WorkerSubmitContext Context;
            u32 Submitted = 0;
            u32 Culled = 0;
        };

        std::array<WorkerStats, MAX_RENDER_WORKERS> workerStats;
        for (u32 worker = 0; worker < MAX_RENDER_WORKERS; ++worker)
        {
            workerStats[worker].Context = GetWorkerContext(worker);
        }

        ParallelForWithExistingTaskContext(
            "SubmitMeshesParallel",
            TArrayView<WorkerStats>(workerStats.data(), MAX_RENDER_WORKERS),
            numMeshes,
            minBatchSize,
            // Body - process one mesh descriptor.
            [&meshes](WorkerStats& stats, i32 index)
            {
                const MeshSubmitDesc& desc = meshes[index];

                const glm::mat4* prevXform = desc.HasPrevTransform ? &desc.PrevTransform : nullptr;
                CommandPacket* packet = Renderer3D::DrawMeshParallel(
                    stats.Context, desc.Mesh, desc.Transform, desc.MaterialData, desc.IsStatic,
                    desc.EntityID, desc.LODGroupPtr, prevXform);

                if (packet)
                {
                    // Same patch as the serial path above; the two must agree,
                    // and a batch that merely crossed the parallel threshold
                    // silently losing its baked GI is exactly the kind of split
                    // this repo keeps paying for.
                    if (desc.LightmapScaleOffset.x > 0.0f)
                    {
                        packet->GetCommandData<DrawMeshCommand>()->lightmapScaleOffset = desc.LightmapScaleOffset;

                        // AND THE SAME DIAGNOSTIC (issue #1242), for exactly the
                        // reason the comment above gives. #1242 first added this
                        // report to the serial branch only, so a batch of
                        // lightmapped skin materials that merely crossed the
                        // parallel threshold stopped reporting a conflict that
                        // was still happening — the split this comment warns
                        // about, reintroduced by the change that quotes it.
                        //
                        // SkinProfileTable::Resolve and ReportTransmissionFallback
                        // are both documented thread-safe (mesh submission runs
                        // on more than one thread), which is what makes this
                        // callable from inside the parallel lambda.
                        if (const Material& mat = desc.MaterialData;
                            mat.GetMaterialKind() == MaterialKind::Skin && mat.HasAuthoredThickness())
                        {
                            const SkinProfileResolution profile =
                                Renderer3D::GetSkinProfileTable().Resolve(mat.GetSkinProfileHandle());
                            // BOTH TRANSMITTING VERSIONS (issue #1243 appended
                            // the second). The versions are cumulative, so a
                            // version-3 profile transmits too — reporting only
                            // version 2 here would make the lightmap conflict
                            // stop being counted the moment an author moved a
                            // head forward, and this diagnostic exists precisely
                            // because the failure is otherwise invisible.
                            if (SkinEvaluatesThicknessTransmission(profile.Parameters.EvaluationModel))
                            {
                                Renderer3D::GetSkinProfileTable().ReportTransmissionFallback(
                                    SkinTransmissionFallbackReason::DeferredThicknessLaneUnavailable,
                                    mat.GetSkinProfileHandle());
                            }
                        }
                    }
                    Renderer3D::SubmitPacketParallel(stats.Context, packet);
                    ++stats.Submitted;
                }
                else
                {
                    ++stats.Culled;
                }
            },
            EParallelForFlags::None);

        EndParallelSubmission();

        // Aggregate statistics.
        u32 totalSubmitted = 0;
        for (i32 i = 0; i < MAX_RENDER_WORKERS; ++i)
        {
            totalSubmitted += workerStats[i].Submitted;
            s_Data.Stats.LODSwitches += workerStats[i].Context.LODSwitches;
            for (sizet j = 0; j < static_cast<sizet>(workerStats[i].Context.ObjectsPerLODLevel.Num()); ++j)
            {
                if (j >= static_cast<sizet>(s_Data.Stats.ObjectsPerLODLevel.Num()))
                {
                    s_Data.Stats.ObjectsPerLODLevel.SetNumZeroed(static_cast<i64>(j + 1));
                }
                s_Data.Stats.ObjectsPerLODLevel[j] += workerStats[i].Context.ObjectsPerLODLevel[j];
            }
        }

        return totalSubmitted;
    }
} // namespace OloEngine
