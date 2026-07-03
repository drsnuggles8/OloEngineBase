#include "OloEnginePCH.h"
#include "OloEngine/Animation/Skeleton.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/Renderer3DInternal.h"
#include "OloEngine/Renderer/Renderer3DDrawHelpers.h"
#include "OloEngine/Renderer/Instancing/GPUFrustumCuller.h"
#include "OloEngine/Renderer/MeshSource.h"
#include "OloEngine/Renderer/Occlusion/OcclusionCuller.h"
#include "OloEngine/Renderer/Occlusion/OcclusionQueryPool.h"
#include "OloEngine/Renderer/Occlusion/OcclusionState.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/Commands/CommandDispatch.h"
#include "OloEngine/Renderer/Commands/DrawKey.h"
#include "OloEngine/Renderer/Commands/FrameDataBuffer.h"
#include "OloEngine/Renderer/Commands/RenderCommand.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Containers/Array.h"
#include "OloEngine/Task/ParallelFor.h"

#include <numeric>

#include <atomic>

namespace OloEngine
{
    auto Renderer3D::ValidateDrawMeshRendererIDs(const char* context, const u32 vaoID, const u32 shaderID) -> bool
    {
        if (vaoID != 0 && shaderID != 0)
            return true;

        if (static std::atomic<u64> s_InvalidRendererIDWarnCount{ 0 }; s_InvalidRendererIDWarnCount.fetch_add(1, std::memory_order_relaxed) < 1)
        {
            OLO_CORE_WARN("{}: Dropping draw with invalid renderer IDs (VAO={}, Shader={})",
                          context, vaoID, shaderID);
        }

        return false;
    }

    bool Renderer3D::IsDeferredCapableShader(const Ref<Shader>& shader)
    {
        if (!shader)
            return false;
        const u64 handle = static_cast<u64>(shader->GetHandle());
        if (handle == 0)
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
        // Compare by AssetHandle rather than RendererID — the latter is
        // re-issued on hot-reload whereas the handle is stable across the
        // asset lifetime.
        const auto matches = [handle](const Ref<Shader>& candidate)
        {
            return candidate && static_cast<u64>(candidate->GetHandle()) == handle;
        };
        return matches(s_Data.PBRGBufferShader) || matches(s_Data.PBRGBufferSkinnedShader) ||
               matches(s_Data.SkyboxGBufferShader) || matches(s_Data.LightCubeGBufferShader) ||
               matches(s_Data.InfiniteGridGBufferShader) || matches(s_Data.TerrainGBufferShader) ||
               matches(s_Data.VoxelGBufferShader) || matches(s_Data.FoliageGBufferShader) ||
               matches(s_Data.DecalGBufferShader) || matches(s_Data.DecalGBufferNormalShader) ||
               matches(s_Data.DecalGBufferRMAShader) || matches(s_Data.DecalGBufferEmissiveShader);
    }

    auto Renderer3D::CreatePODMaterialDataForMaterial(const Material& material, RendererID shaderRendererID) -> PODMaterialData
    {
        PODMaterialData data{};
        data.shaderRendererID = shaderRendererID;

        // Legacy material properties.
        data.ambient = material.GetAmbient();
        data.diffuse = material.GetDiffuse();
        data.specular = material.GetSpecular();
        data.shininess = material.GetShininess();
        data.useTextureMaps = material.IsUsingTextureMaps();
        data.diffuseMapID = material.GetDiffuseMap() ? material.GetDiffuseMap()->GetRendererID() : 0;
        data.specularMapID = material.GetSpecularMap() ? material.GetSpecularMap()->GetRendererID() : 0;

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

        // PBR texture renderer IDs.
        data.albedoMapID = material.GetAlbedoMap() ? material.GetAlbedoMap()->GetRendererID() : 0;
        data.metallicRoughnessMapID = material.GetMetallicRoughnessMap() ? material.GetMetallicRoughnessMap()->GetRendererID() : 0;
        data.normalMapID = material.GetNormalMap() ? material.GetNormalMap()->GetRendererID() : 0;
        data.aoMapID = material.GetAOMap() ? material.GetAOMap()->GetRendererID() : 0;
        data.emissiveMapID = material.GetEmissiveMap() ? material.GetEmissiveMap()->GetRendererID() : 0;
        data.environmentMapID = material.GetEnvironmentMap() ? material.GetEnvironmentMap()->GetRendererID() : 0;
        data.irradianceMapID = material.GetIrradianceMap() ? material.GetIrradianceMap()->GetRendererID() : 0;
        data.prefilterMapID = material.GetPrefilterMap() ? material.GetPrefilterMap()->GetRendererID() : 0;
        data.brdfLutMapID = material.GetBRDFLutMap() ? material.GetBRDFLutMap()->GetRendererID() : 0;

        // Fall back to global IBL when the material has no IBL configured.
        if (data.enablePBR && data.irradianceMapID == 0 && Renderer3D::GetGlobalIrradianceMapID() != 0)
        {
            data.irradianceMapID = Renderer3D::GetGlobalIrradianceMapID();
            data.prefilterMapID = Renderer3D::GetGlobalPrefilterMapID();
            data.brdfLutMapID = Renderer3D::GetGlobalBRDFLutMapID();
            if (data.environmentMapID == 0)
                data.environmentMapID = Renderer3D::GetGlobalEnvironmentMapID();
            data.enableIBL = true;
            data.iblIntensity = Renderer3D::GetGlobalIBLIntensity();
        }

        return data;
    }

    CommandPacket* Renderer3D::DrawMesh(const Ref<Mesh>& mesh, const glm::mat4& modelMatrix, const Material& material, bool isStatic, i32 entityID, const LODGroup* lodGroup)
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
        if (auto lodResult = SelectLODMesh(mesh, modelMatrix, s_Data.ViewPos, lodGroup, meshToUse); lodResult.SelectedLODIndex >= 0)
        {
            if (lodResult.SelectedLODIndex >= static_cast<i32>(s_Data.Stats.ObjectsPerLODLevel.size()))
            {
                s_Data.Stats.ObjectsPerLODLevel.resize(lodResult.SelectedLODIndex + 1, 0);
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
        // Deferred mode demands that every ScenePass draw write the 4-RT
        // G-Buffer layout (Albedo/Metallic, Normal/Roughness/AO, Emissive/
        // Flags, Velocity). Non-PBR materials selecting s_Data.DefaultForwardShader
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
            // Route PBR default shader to the G-Buffer write variant when the
            // deferred path is active. Material overrides still win (so
            // custom shaders, e.g. terrain/foliage, keep their forward
            // pipeline until their own G-Buffer variants land in later phases).
            if (s_Data.Settings.Path == RenderingPath::Deferred && s_Data.PBRGBufferShader)
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

        const u32 vertexArrayID = meshToUse->GetVertexArray()->GetRendererID();
        const u32 shaderRendererID = shaderToUse->GetRendererID();
        if (!ValidateDrawMeshRendererIDs("Renderer3D::DrawMesh", vertexArrayID, shaderRendererID))
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
        u32 shaderID = shaderRendererID & 0xFFFF; // 16-bit shader ID.
        u32 materialID = ComputeMaterialID(material);
        u32 depth = ComputeDepthForSortKey(modelMatrix);
        if (material.GetFlag(MaterialFlag::Blend))
            metadata.m_SortKey = DrawKey::CreateTransparent(0, ViewLayerType::ThreeD, shaderID, materialID, depth);
        else
            metadata.m_SortKey = DrawKey::CreateOpaque(0, ViewLayerType::ThreeD, shaderID, materialID, depth);
        metadata.m_IsStatic = isStatic;
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

    CommandPacket* Renderer3D::DrawMeshInstanced(const Ref<Mesh>& mesh, const std::vector<glm::mat4>& transforms, const Material& material, bool isStatic, u64 ownerKey)
    {
        OLO_PROFILE_FUNCTION();
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
            return SubmitGPUCulledInstanced(mesh, transforms, material, isStatic, ownerKey);
        }

        const std::vector<glm::mat4>* activeTransforms = &transforms;
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
            activeTransforms = &filteredTransforms;
        }

        // Allocate space in FrameDataBuffer for instance transforms.
        FrameDataBuffer& frameBuffer = FrameDataBufferManager::Get();
        u32 transformCount = static_cast<u32>(activeTransforms->size());
        u32 transformOffset = frameBuffer.AllocateTransforms(transformCount);
        if (transformOffset == UINT32_MAX)
        {
            OLO_CORE_ERROR("Renderer3D::DrawMeshInstanced: Failed to allocate transform buffer space");
            return nullptr;
        }
        frameBuffer.WriteTransforms(transformOffset, activeTransforms->data(), transformCount);

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
            const std::vector<u32>* idxPtr = visibleIndices.empty() ? nullptr : &visibleIndices;
            std::vector<glm::mat4> prevTransforms = GetAndRecordPrevInstanceTransforms(meshKey, ownerKey, transforms, idxPtr, &usedFallback);
            // Use the explicit flag rather than pointer identity — the function
            // returns a projected vector by value in the fallback path, so
            // prevTransforms.data() is always a distinct buffer.
            if (!usedFallback && prevTransforms.size() == activeTransforms->size())
            {
                u32 prevOffset = frameBuffer.AllocateTransforms(transformCount);
                if (prevOffset != UINT32_MAX)
                {
                    frameBuffer.WriteTransforms(prevOffset, prevTransforms.data(), transformCount);
                    prevTransformOffset = prevOffset;
                }
            }
        }

        Ref<Shader> shaderToUse = material.GetShader() ? material.GetShader() : s_Data.DefaultForwardShader;

        // Create POD command.
        CommandPacket* packet = CreateDrawCall<DrawMeshInstancedCommand>();
        auto* cmd = packet->GetCommandData<DrawMeshInstancedCommand>();
        cmd->header.type = CommandType::DrawMeshInstanced;

        const u32 vertexArrayID = mesh->GetVertexArray()->GetRendererID();
        const u32 shaderRendererID = shaderToUse->GetRendererID();
        if (!ValidateDrawMeshRendererIDs("Renderer3D::DrawMeshInstanced", vertexArrayID, shaderRendererID))
            return nullptr;

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
        u32 shaderID = shaderRendererID & 0xFFFF;
        u32 materialID = ComputeMaterialID(material);
        u32 depth = activeTransforms->empty() ? 0 : ComputeDepthForSortKey((*activeTransforms)[0]);
        if (material.GetFlag(MaterialFlag::Blend))
            metadata.m_SortKey = DrawKey::CreateTransparent(0, ViewLayerType::ThreeD, shaderID, materialID, depth);
        else
            metadata.m_SortKey = DrawKey::CreateOpaque(0, ViewLayerType::ThreeD, shaderID, materialID, depth);
        metadata.m_IsStatic = isStatic;
        packet->SetMetadata(metadata);

        return packet;
    }

    // InstanceData overload — extracts transforms for the existing pipeline
    // (frustum cull, FrameDataBuffer allocation, prev-transform history,
    // command construction) then patches the resulting packet with per-instance
    // EntityID / Color / Custom streams pulled straight from the InstanceData
    // span. Keeps the single source of truth for instancing logic in the
    // transform-only overload.
    CommandPacket* Renderer3D::DrawMeshInstanced(const Ref<Mesh>& mesh, std::span<const InstanceData> instances, const Material& material, bool isStatic, u64 ownerKey)
    {
        OLO_PROFILE_FUNCTION();
        if (instances.empty())
            return nullptr;

        std::vector<glm::mat4> transforms;
        transforms.reserve(instances.size());
        for (const auto& inst : instances)
            transforms.push_back(inst.Transform);

        CommandPacket* packet = DrawMeshInstanced(mesh, transforms, material, isStatic, ownerKey);
        if (!packet)
            return nullptr; // entirely culled or alloc failure

        // Post-cull instance count from the produced packet — the transform-only
        // overload may have filtered out frustum-culled instances. We only
        // populate the auxiliary streams for the surviving subset to keep the
        // i-th color/custom/entityID aligned with the i-th transform.
        auto* cmd = packet->GetCommandData<DrawMeshInstancedCommand>();
        const u32 visibleCount = cmd->instanceCount;
        if (visibleCount == 0)
            return packet;

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

        return packet;
    }

    CommandPacket* Renderer3D::SubmitGPUCulledInstanced(const Ref<Mesh>& mesh,
                                                        const std::vector<glm::mat4>& transforms,
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
        std::vector<glm::mat4> prevTransforms = GetAndRecordPrevInstanceTransforms(meshKey, ownerKey, transforms, nullptr, &usedFallback);
        if (usedFallback || prevTransforms.size() != transforms.size())
            prevTransforms = transforms; // first frame / size mismatch -> zero velocity

        // Build the InstanceData[] the cull compute reads. Color / Custom /
        // EntityID stay at their identity defaults — the transform-only
        // overload doesn't carry that per-instance data. The InstanceData
        // overload of DrawMeshInstanced overwrites them with the real values
        // once the GPU-cull path is wired into it as a future follow-up.
        std::vector<InstanceData> packed;
        packed.reserve(transforms.size());
        for (sizet i = 0; i < transforms.size(); ++i)
        {
            InstanceData inst;
            inst.Transform = transforms[i];
            inst.Normal = glm::transpose(glm::inverse(transforms[i]));
            inst.PrevTransform = prevTransforms[i];
            packed.push_back(inst);
        }

        // Run the GPU cull. RadiusExpansion folds the CPU path's two safety
        // multipliers (1.3 × 1.05 = 1.365) into a single uniform so the two
        // paths produce identical visibility decisions (verified by
        // GPUFrustumCullParityTest in tests/Rendering/).
        BoundingSphere localSphere = mesh->GetBoundingSphere();
        constexpr f32 kRadiusExpansion = 1.3f * 1.05f;
        const glm::vec4 sphereUniform{ localSphere.Center, localSphere.Radius };

        Ref<Shader> shaderToUse = material.GetShader() ? material.GetShader() : s_Data.DefaultForwardShader;
        const u32 vertexArrayID = mesh->GetVertexArray()->GetRendererID();
        const u32 shaderRendererID = shaderToUse->GetRendererID();
        if (!ValidateDrawMeshRendererIDs("Renderer3D::SubmitGPUCulledInstanced", vertexArrayID, shaderRendererID))
            return nullptr;

        // Material / render state allocated once; both phase-1 and phase-2
        // packets reference the same indices.
        const u32 materialDataIndex = FrameDataBufferManager::Get().AllocateMaterialData(
            CreatePODMaterialDataForMaterial(material, shaderRendererID));
        const u32 renderStateIndex = FrameDataBufferManager::Get().AllocateRenderState(CreatePODRenderStateForMaterial(material));

        const u32 shaderID = shaderRendererID & 0xFFFF;
        const u32 materialID = ComputeMaterialID(material);
        const u32 sortDepth = transforms.empty() ? 0 : ComputeDepthForSortKey(transforms[0]);

        // Build one DrawMeshInstancedCommand packet over a (survivors, indirect)
        // buffer pair. The dispatcher takes the indirect-draw branch because
        // `cullIndirectBufferID` is non-zero — skipping the FrameDataBuffer
        // scratch loop and binding the pre-culled survivors at SSBO_INSTANCE_DATA.
        const auto buildPacket = [&](u32 outputInstanceBufferID, u32 indirectBufferID) -> CommandPacket*
        {
            CommandPacket* packet = CreateDrawCall<DrawMeshInstancedCommand>();
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
            cmd->cullOutputInstanceBufferID = outputInstanceBufferID;
            cmd->cullIndirectBufferID = indirectBufferID;

            packet->SetCommandType(cmd->header.type);
            packet->SetDispatchFunction(CommandDispatch::GetDispatchFunction(cmd->header.type));

            PacketMetadata metadata = packet->GetMetadata();
            if (material.GetFlag(MaterialFlag::Blend))
                metadata.m_SortKey = DrawKey::CreateTransparent(0, ViewLayerType::ThreeD, shaderID, materialID, sortDepth);
            else
                metadata.m_SortKey = DrawKey::CreateOpaque(0, ViewLayerType::ThreeD, shaderID, materialID, sortDepth);
            metadata.m_IsStatic = isStatic;
            packet->SetMetadata(metadata);
            return packet;
        };

        const bool hzbOcclusion = IsHZBOcclusionCullingEnabled();
        const bool deferred = (GetRendererSettings().Path == RenderingPath::Deferred);

        // Two-phase GPU-driven occlusion (#431 Stage 2): Forward / Forward+ with
        // HZB occlusion on. Phase 1 culls against the previous frame's HZB and
        // appends occluded survivors to a reject list; both the phase-1 draw and
        // a phase-2 draw are routed to GPUDrivenOcclusionPass, which re-tests the
        // reject list against this frame's depth after the phase-1 draws.
        if (GPUDrivenOcclusionPass* occlusionPass = (hzbOcclusion && !deferred) ? GetGPUOcclusionPass() : nullptr)
        {
            auto twoPhase = s_Data.GPUFrustumCuller->CullTwoPhasePhase1(
                packed, mesh->GetIndexCount(), mesh->GetBaseIndex(), sphereUniform, kRadiusExpansion);

            CommandPacket* phase1Packet = buildPacket(twoPhase.Phase1Output->GetStorage()->GetRendererID(),
                                                      twoPhase.Phase1Indirect->GetRendererID());
            CommandPacket* phase2Packet = buildPacket(twoPhase.Phase2Output->GetStorage()->GetRendererID(),
                                                      twoPhase.Phase2Indirect->GetRendererID());
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
        if (DeferredGPUOcclusionPass* deferredOcclusionPass = (hzbOcclusion && deferred) ? GetDeferredGPUOcclusionPass() : nullptr)
        {
            auto twoPhase = s_Data.GPUFrustumCuller->CullTwoPhasePhase1(
                packed, mesh->GetIndexCount(), mesh->GetBaseIndex(), sphereUniform, kRadiusExpansion);

            if (CommandPacket* phase2Packet = buildPacket(twoPhase.Phase2Output->GetStorage()->GetRendererID(),
                                                          twoPhase.Phase2Indirect->GetRendererID()))
                deferredOcclusionPass->SubmitPhase2(phase2Packet, twoPhase);

            // Phase 1 → caller → ScenePass G-Buffer bucket.
            return buildPacket(twoPhase.Phase1Output->GetStorage()->GetRendererID(),
                               twoPhase.Phase1Indirect->GetRendererID());
        }

        // Single-phase path: frustum-only (occlusion off) — drawn through the
        // normal ScenePass / G-Buffer bucket.
        auto cullResult = s_Data.GPUFrustumCuller->Cull(
            packed, mesh->GetIndexCount(), mesh->GetBaseIndex(), sphereUniform, kRadiusExpansion);
        return buildPacket(cullResult.OutputBuffer->GetStorage()->GetRendererID(),
                           cullResult.IndirectBuffer->GetRendererID());
    }

    CommandPacket* Renderer3D::DrawAnimatedMesh(const Ref<Mesh>& mesh, const glm::mat4& modelMatrix, const Material& material, const std::vector<glm::mat4>& boneMatrices, bool isStatic, i32 entityID)
    {
        // Delegate to the variant that accepts previous-frame bone matrices; pass an empty
        // vector so the callee treats prev as "same as current" (zero per-bone motion).
        static const std::vector<glm::mat4> s_EmptyPrev;
        return DrawAnimatedMesh(mesh, modelMatrix, material, boneMatrices, s_EmptyPrev, isStatic, entityID);
    }

    CommandPacket* Renderer3D::DrawAnimatedMesh(const Ref<Mesh>& mesh, const glm::mat4& modelMatrix, const Material& material, const std::vector<glm::mat4>& boneMatrices, const std::vector<glm::mat4>& prevBoneMatrices, bool isStatic, i32 entityID)
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
            if (s_Data.Settings.Path == RenderingPath::Deferred && s_Data.PBRGBufferSkinnedShader)
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

        const u32 vertexArrayID = vertexArray->GetRendererID();
        const u32 shaderRendererID = shaderToUse->GetRendererID();
        if (!ValidateDrawMeshRendererIDs("Renderer3D::DrawAnimatedMesh", vertexArrayID, shaderRendererID))
            return nullptr;

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
        u32 shaderID = shaderRendererID & 0xFFFF;
        u32 materialID = ComputeMaterialID(material);
        u32 depth = ComputeDepthForSortKey(modelMatrix);
        if (material.GetFlag(MaterialFlag::Blend))
            metadata.m_SortKey = DrawKey::CreateTransparent(0, ViewLayerType::ThreeD, shaderID, materialID, depth);
        else
            metadata.m_SortKey = DrawKey::CreateOpaque(0, ViewLayerType::ThreeD, shaderID, materialID, depth);
        metadata.m_IsStatic = isStatic;
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

        // Get all entities with required components.
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
            const auto& prevBoneMatrices = skeletonComp.m_Skeleton->m_PrevFinalBoneMatrices;
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
        // stale-identity fallback.
        const auto& boneMatrices = skeletonComp.m_Skeleton->m_FinalBoneMatrices;
        const auto& prevBoneMatrices = skeletonComp.m_Skeleton->m_PrevFinalBoneMatrices;

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
        if (const auto lodResult = SelectLODMesh(mesh, modelMatrix, ctx.SceneContext->ViewPosition, lodGroup, meshToUse); lodResult.SelectedLODIndex >= 0)
        {
            if (lodResult.SelectedLODIndex >= static_cast<i32>(ctx.ObjectsPerLODLevel.size()))
            {
                ctx.ObjectsPerLODLevel.resize(lodResult.SelectedLODIndex + 1, 0);
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
            shaderToUse = ctx.SceneContext->PBRShader;
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

        const u32 vertexArrayID = meshToUse->GetVertexArray()->GetRendererID();
        const u32 shaderRendererID = shaderToUse->GetRendererID();
        if (!ValidateDrawMeshRendererIDs("Renderer3D::DrawMeshParallel", vertexArrayID, shaderRendererID))
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
        const u32 shaderID = shaderRendererID & 0xFFFF;
        const u32 materialID = ComputeMaterialID(material);
        const u32 depthKey = ComputeDepthForSortKeyWithView(modelMatrix, ctx.SceneContext->ViewMatrix);

        if (material.GetFlag(MaterialFlag::Blend))
            metadata.m_SortKey = DrawKey::CreateTransparent(0, ViewLayerType::ThreeD, shaderID, materialID, depthKey);
        else
            metadata.m_SortKey = DrawKey::CreateOpaque(0, ViewLayerType::ThreeD, shaderID, materialID, depthKey);
        metadata.m_IsStatic = isStatic;
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

    CommandPacket* Renderer3D::DrawAnimatedMeshParallel(WorkerSubmitContext& ctx,
                                                        const Ref<Mesh>& mesh,
                                                        const glm::mat4& modelMatrix,
                                                        const Material& material,
                                                        const std::vector<glm::mat4>& boneMatrices,
                                                        bool isStatic)
    {
        // Legacy entry point: no prev-pose information available. Alias current
        // bones and transform into the prev slot so motion-vector shaders see
        // zero per-bone and per-object motion for this draw.
        static const std::vector<glm::mat4> s_EmptyPrev;
        return DrawAnimatedMeshParallel(ctx, mesh, modelMatrix, material, boneMatrices,
                                        s_EmptyPrev, modelMatrix, /*hasPrevTransform*/ false, isStatic);
    }

    CommandPacket* Renderer3D::DrawAnimatedMeshParallel(WorkerSubmitContext& ctx,
                                                        const Ref<Mesh>& mesh,
                                                        const glm::mat4& modelMatrix,
                                                        const Material& material,
                                                        const std::vector<glm::mat4>& boneMatrices,
                                                        const std::vector<glm::mat4>& prevBoneMatrices,
                                                        const glm::mat4& prevModelMatrix,
                                                        bool hasPrevTransform,
                                                        bool isStatic)
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

        const u32 vertexArrayID = mesh->GetVertexArray()->GetRendererID();
        const u32 shaderRendererID = shaderToUse->GetRendererID();
        if (!ValidateDrawMeshRendererIDs("Renderer3D::DrawAnimatedMeshParallel", vertexArrayID, shaderRendererID))
            return nullptr;

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

        packet->SetCommandType(cmd->header.type);
        packet->SetDispatchFunction(CommandDispatch::GetDispatchFunction(cmd->header.type));

        // Set sort key.
        PacketMetadata metadata = packet->GetMetadata();
        const u32 shaderID = shaderRendererID & 0xFFFF;
        const u32 materialID = ComputeMaterialID(material);
        const u32 depthKey = ComputeDepthForSortKeyWithView(modelMatrix, ctx.SceneContext->ViewMatrix);

        if (material.GetFlag(MaterialFlag::Blend))
            metadata.m_SortKey = DrawKey::CreateTransparent(0, ViewLayerType::ThreeD, shaderID, materialID, depthKey);
        else
            metadata.m_SortKey = DrawKey::CreateOpaque(0, ViewLayerType::ThreeD, shaderID, materialID, depthKey);
        metadata.m_IsStatic = isStatic;
        packet->SetMetadata(metadata);

        return packet;
    }

    u32 Renderer3D::SubmitMeshesParallel(const std::vector<MeshSubmitDesc>& meshes,
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
                CommandPacket* packet = nullptr;
                if (desc.IsAnimated && desc.BoneMatrices)
                {
                    // Route through the prev-aware DrawAnimatedMesh overload
                    // when the caller supplied prev-pose data; otherwise the
                    // legacy entry aliases current->prev (zero motion).
                    if (desc.PrevBoneMatrices)
                    {
                        packet = DrawAnimatedMesh(desc.Mesh, desc.Transform, desc.MaterialData,
                                                  *desc.BoneMatrices, *desc.PrevBoneMatrices,
                                                  desc.IsStatic, desc.EntityID);
                    }
                    else
                    {
                        packet = DrawAnimatedMesh(desc.Mesh, desc.Transform, desc.MaterialData,
                                                  *desc.BoneMatrices, desc.IsStatic, desc.EntityID);
                    }
                }
                else
                {
                    // DrawMesh internally records prev-transform via the shared
                    // per-entity cache (GetAndRecordPrevTransform) keyed on
                    // entityID, so object motion is preserved even without an
                    // explicit prev-aware overload on this path. When the
                    // caller has already computed a prev-transform (e.g. for
                    // animated-mesh fallback paths that store it in desc), seed
                    // the cache so DrawMesh's internal lookup returns the
                    // caller-authoritative value instead of potentially stale
                    // prior-frame history.
                    if (desc.HasPrevTransform && desc.EntityID >= 0)
                    {
                        s_Data.PrevEntityTransforms.insert_or_assign(desc.EntityID, desc.PrevTransform);
                    }
                    packet = DrawMesh(desc.Mesh, desc.Transform, desc.MaterialData, desc.IsStatic, desc.EntityID, desc.LODGroupPtr);
                }
                if (packet)
                {
                    SubmitPacket(packet);
                    ++totalSubmitted;
                }
            }
            return totalSubmitted;
        }

        // Parallel path using ParallelForWithTaskContext.
        BeginParallelSubmission();

        // Per-worker accumulator to track statistics.
        struct WorkerStats
        {
            WorkerSubmitContext Context;
            u32 Submitted = 0;
            u32 Culled = 0;
        };

        TArray<WorkerStats> workerStats;

        ParallelForWithTaskContext(
            "SubmitMeshesParallel",
            workerStats,
            numMeshes,
            minBatchSize,
            // Context constructor - initialize worker context for each task slot.
            // Use explicit contextIndex to avoid std::thread::id lookup.
            [](i32 contextIndex, i32 /*numContexts*/) -> WorkerStats
            {
                WorkerStats stats;
                // Use the optimized path with explicit worker index.
                stats.Context = Renderer3D::GetWorkerContext(static_cast<u32>(contextIndex));
                return stats;
            },
            // Body - process one mesh descriptor.
            [&meshes](WorkerStats& stats, i32 index)
            {
                const MeshSubmitDesc& desc = meshes[index];

                CommandPacket* packet = nullptr;
                if (desc.IsAnimated && desc.BoneMatrices)
                {
                    // Route through the prev-aware overload when the caller
                    // supplied prev-pose data; otherwise fall back to the
                    // legacy entry which aliases current->prev (zero motion).
                    if (desc.PrevBoneMatrices || desc.HasPrevTransform)
                    {
                        static const std::vector<glm::mat4> s_EmptyPrev;
                        const std::vector<glm::mat4>& prevBones =
                            desc.PrevBoneMatrices ? *desc.PrevBoneMatrices : s_EmptyPrev;
                        packet = Renderer3D::DrawAnimatedMeshParallel(
                            stats.Context,
                            desc.Mesh,
                            desc.Transform,
                            desc.MaterialData,
                            *desc.BoneMatrices,
                            prevBones,
                            desc.PrevTransform,
                            desc.HasPrevTransform,
                            desc.IsStatic);
                    }
                    else
                    {
                        packet = Renderer3D::DrawAnimatedMeshParallel(
                            stats.Context,
                            desc.Mesh,
                            desc.Transform,
                            desc.MaterialData,
                            *desc.BoneMatrices,
                            desc.IsStatic);
                    }
                }
                else
                {
                    const glm::mat4* prevXform = desc.HasPrevTransform ? &desc.PrevTransform : nullptr;
                    packet = Renderer3D::DrawMeshParallel(
                        stats.Context,
                        desc.Mesh,
                        desc.Transform,
                        desc.MaterialData,
                        desc.IsStatic,
                        desc.EntityID,
                        desc.LODGroupPtr,
                        prevXform);
                }

                if (packet)
                {
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
        for (i32 i = 0; i < workerStats.Num(); ++i)
        {
            totalSubmitted += workerStats[i].Submitted;
            s_Data.Stats.LODSwitches += workerStats[i].Context.LODSwitches;
            for (sizet j = 0; j < workerStats[i].Context.ObjectsPerLODLevel.size(); ++j)
            {
                if (j >= s_Data.Stats.ObjectsPerLODLevel.size())
                {
                    s_Data.Stats.ObjectsPerLODLevel.resize(j + 1, 0);
                }
                s_Data.Stats.ObjectsPerLODLevel[j] += workerStats[i].Context.ObjectsPerLODLevel[j];
            }
        }

        return totalSubmitted;
    }
} // namespace OloEngine
