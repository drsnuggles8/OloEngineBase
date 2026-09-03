#include "OloEnginePCH.h"
#include "OloEngine/Scene/SceneLightmapGather.h"

#include "OloEngine/Animation/AnimatedMeshComponents.h"
#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Asset/InstancePlacementAsset.h"
#include "OloEngine/Renderer/Instancing/InstancedMeshComponent.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshSource.h"
#include "OloEngine/Renderer/Model.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Scene.h"

#include <algorithm>
#include <tuple>

namespace OloEngine
{
    namespace
    {
        [[nodiscard]] bool IsBakeableMesh(const Ref<MeshSource>& mesh)
        {
            return mesh && !mesh->GetVertices().IsEmpty();
        }

        // The entity's material override, in the same precedence the draw path
        // uses. Returned as a borrowed pointer — see LightmapReceiver's note.
        [[nodiscard]] const Material* EntityMaterialOverride(Scene& scene, entt::entity handle)
        {
            Entity entity{ handle, &scene };
            return entity.HasComponent<MaterialComponent>() ? &entity.GetComponent<MaterialComponent>().m_Material
                                                            : nullptr;
        }
    } // namespace

    std::vector<LightmapReceiver> GatherLightmapReceivers(Scene& scene)
    {
        OLO_PROFILE_FUNCTION();

        std::vector<LightmapReceiver> receivers;

        // -- MeshComponent (issue #439) - one entity, one region, sub-key 0 --
        //
        // Skinned entities are excluded, mirroring both the classic draw loop
        // and ReferenceSceneBuilder::AddScene: GI capture is receive-only for
        // dynamics (ADR 0007), and a skinned VAO keeps bones in the stream slot
        // the lightmap UV2 would need.
        {
            auto view = scene.GetAllEntitiesWith<IDComponent, MeshComponent>();
            for (auto handle : view)
            {
                const auto& mesh = view.get<MeshComponent>(handle);
                if (!mesh.m_LightmapStatic || !IsBakeableMesh(mesh.m_MeshSource))
                {
                    continue;
                }
                if (Entity entity{ handle, &scene }; entity.HasComponent<SkeletonComponent>())
                {
                    continue;
                }
                receivers.push_back(LightmapReceiver{ view.get<IDComponent>(handle).ID,
                                                      0,
                                                      handle,
                                                      mesh.m_MeshSource,
                                                      scene.GetWorldTransform(handle),
                                                      EntityMaterialOverride(scene, handle),
                                                      LightmapReceiverKind::Mesh });
            }
        }

        // -- InstancedMeshComponent (issue #867) - one region PER INSTANCE --
        //
        // The instance list is inline placements followed by the placement
        // asset's, with stable IDs ensured first. That is exactly the order and
        // the identity Scene.cpp's merged cache builds for the draw, and the two
        // must agree: the draw looks a region up by StableID, so a gather that
        // enumerated a different set would bake regions nothing ever samples.
        // The transform is the instance's own - instances live in world space
        // and the entity's TransformComponent is deliberately NOT applied.
        {
            auto view = scene.GetAllEntitiesWith<IDComponent, InstancedMeshComponent>();
            for (auto handle : view)
            {
                auto& imc = view.get<InstancedMeshComponent>(handle);
                if (!imc.LightmapStatic || !IsBakeableMesh(imc.MeshSource))
                {
                    continue;
                }

                const UUID uuid = view.get<IDComponent>(handle).ID;
                const Material* overrideMaterial =
                    imc.OverrideMaterial ? imc.OverrideMaterial.Raw() : EntityMaterialOverride(scene, handle);

                // `sourceNamespace` is NOT optional. EnsureStableIDs runs
                // independently over the inline list and the placement asset's,
                // so both hand out 1, 2, 3, ... — AssetStableIDNamespace (bit
                // 63) is the only thing telling the two apart once they are
                // merged, which is exactly what Scene.cpp's draw ORs in when it
                // looks the region up. Drop it here and one inline instance and
                // one asset instance share a sub-key: the bake writes one region
                // for two surfaces sitting in different places, and the second
                // shades from the first's charts.
                const auto addInstances = [&](std::vector<InstanceData>& instances, u64 sourceNamespace)
                {
                    (void)InstancedMeshComponent::EnsureStableIDs(instances);
                    for (const InstanceData& instance : instances)
                    {
                        // EnsureStableIDs guarantees non-zero, but a zero here
                        // would silently collide with the "whole entity" sub-key
                        // and alias every such instance onto one region.
                        if (instance.StableID == 0)
                        {
                            continue;
                        }
                        receivers.push_back(LightmapReceiver{ uuid, instance.StableID | sourceNamespace, handle,
                                                              imc.MeshSource, instance.Transform, overrideMaterial,
                                                              LightmapReceiverKind::Instance });
                    }
                };

                addInstances(imc.Instances, 0);
                if (imc.PlacementAssetHandle != 0)
                {
                    if (auto placement = AssetManager::GetAsset<InstancePlacementAsset>(imc.PlacementAssetHandle))
                    {
                        addInstances(placement->GetInstances(), InstancedMeshComponent::AssetStableIDNamespace);
                    }
                }
            }
        }

        // -- ModelComponent (issue #867) - one region per DISTINCT MeshSource --
        //
        // Deduplication is the whole point. On the warm .omesh path every Model
        // mesh is a submesh VIEW into one combined MeshSource, so the model is a
        // single unwrap and a single region (sub-key 0); a cold Assimp import
        // gives each mesh its own source and its own region. Emitting one input
        // per MESH instead would rasterize the same whole source into N
        // identical regions and burn N times the atlas.
        {
            auto view = scene.GetAllEntitiesWith<IDComponent, ModelComponent>();
            for (auto handle : view)
            {
                const auto& model = view.get<ModelComponent>(handle);
                if (!model.m_LightmapStatic || !model.m_Model)
                {
                    continue;
                }

                const UUID uuid = view.get<IDComponent>(handle).ID;
                const glm::mat4 worldTransform = scene.GetWorldTransform(handle);
                const Material* overrideMaterial = EntityMaterialOverride(scene, handle);

                const auto& meshes = model.m_Model->GetMeshes();
                for (sizet i = 0; i < meshes.size(); ++i)
                {
                    if (!meshes[i])
                    {
                        continue;
                    }
                    Ref<MeshSource> source = meshes[i]->GetMeshSource();
                    if (!IsBakeableMesh(source))
                    {
                        continue;
                    }
                    // Only the FIRST mesh using this source emits a receiver, and
                    // its index is the sub-key - so the draw side recovers the
                    // same key with the same scan (LightmapSubKeyForModelMesh).
                    bool seenEarlier = false;
                    for (sizet j = 0; j < i; ++j)
                    {
                        if (meshes[j] && meshes[j]->GetMeshSource() == source)
                        {
                            seenEarlier = true;
                            break;
                        }
                    }
                    if (seenEarlier)
                    {
                        continue;
                    }
                    receivers.push_back(LightmapReceiver{ uuid, static_cast<u64>(i), handle, source, worldTransform,
                                                          overrideMaterial, LightmapReceiverKind::ModelMesh });
                }
            }
        }

        // -- VirtualMeshComponent: deliberately NOT gathered (issue #867) --
        //
        // A virtual mesh cannot sample the lightmap yet. Its clusters rasterize
        // through their own 32-byte vertex format, so the UV2 needs a second
        // per-vertex buffer, and the SSBO binding namespace is FULL — every
        // number below the hard Mesa ceiling of 80 is claimed, and the one
        // reusable slot (SSBO_BONE_PULL 63) is the Vulkan vertex-pull stream.
        // ShaderBindingLayout.h asks for that to be an issue-level decision.
        //
        // Gathering these entities anyway would BAKE regions nothing samples:
        // atlas space spent, page pressure raised, and a scene that looks
        // identical. And wiring only the classic fallback (which would sample
        // one happily) would make baked GI appear and disappear with
        // RendererSettings::VirtualGeometryEnabled — the trap #867 calls out by
        // name. So neither side samples, and nothing is baked for it.
        //
        // The cook-side half of the receiver IS done and tested: MeshSource UV2
        // now survives cluster building and the LOD simplifier's boundary locks
        // (VirtualMeshBuilder's 7-attribute set and widened protect window), so
        // a virtual mesh sharing its MeshSource with a lightmap-static
        // MeshComponent already carries correct UV2 through the DAG.

        // Deterministic order. Registry iteration order is not a contract and
        // the bake key, the atlas layout and every parity test are - so the sort
        // is load-bearing, not cosmetic. (UUID, SubKey) is a total order because
        // the gather never emits the same pair twice.
        std::sort(receivers.begin(), receivers.end(),
                  [](const LightmapReceiver& a, const LightmapReceiver& b)
                  { return std::tie(a.EntityUUID, a.SubKey) < std::tie(b.EntityUUID, b.SubKey); });
        return receivers;
    }

    u64 LightmapSubKeyForModelMesh(const Model& model, sizet meshIndex)
    {
        const auto& meshes = model.GetMeshes();
        if (meshIndex >= meshes.size() || !meshes[meshIndex])
        {
            return 0;
        }
        const Ref<MeshSource> source = meshes[meshIndex]->GetMeshSource();
        for (sizet j = 0; j < meshIndex; ++j)
        {
            if (meshes[j] && meshes[j]->GetMeshSource() == source)
            {
                return static_cast<u64>(j);
            }
        }
        return static_cast<u64>(meshIndex);
    }
} // namespace OloEngine
