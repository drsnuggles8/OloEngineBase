#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Core/UUID.h"

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <vector>

namespace OloEngine
{
    class Material;
    class MeshSource;
    class Model;
    class Scene;

    // Which component a receiver came from. Only used for diagnostics and for
    // the editor's per-kind bake summary — the region identity is
    // (EntityUUID, SubKey) and nothing downstream branches on the kind.
    enum class LightmapReceiverKind : u8
    {
        Mesh = 0,      // MeshComponent (issue #439)
        Instance = 1,  // one instance of an InstancedMeshComponent (issue #867)
        ModelMesh = 2, // one distinct MeshSource of a ModelComponent (issue #867)
        Virtual = 3    // VirtualMeshComponent (issue #867)
    };

    // One lightmap-static receiver: the `(EntityUUID, SubKey)` region identity,
    // the MeshSource whose UV2 the bake unwraps and rasterizes, and the world
    // transform its indirect bounce is estimated at.
    struct LightmapReceiver
    {
        UUID EntityUUID{ 0 };
        u64 SubKey = 0;
        entt::entity Handle = entt::null;
        Ref<MeshSource> Mesh;
        glm::mat4 WorldTransform{ 1.0f };
        // Material override that wins over the mesh's imported per-submesh
        // materials, or null. Borrowed from the component that owns it, so it
        // is only valid for as long as the gather's scene is unmodified — every
        // caller consumes the vector on the game thread before returning.
        const Material* OverrideMaterial = nullptr;
        LightmapReceiverKind Kind = LightmapReceiverKind::Mesh;
    };

    // Every lightmap-static receiver in the scene, in a DETERMINISTIC order:
    // UUID ascending, then SubKey ascending.
    //
    // This is the single source of truth shared by all four walks that used to
    // be written out separately and had to agree: the editor's bake gather, the
    // reference-scene capture the bake traces against, the runtime's
    // self-healing re-unwrap, and SceneLightmapRuntime::ComputeBakeKey. Issue
    // #629's recurring failure in this repo is two loops that were supposed to
    // match and quietly did not; a lightmap gather that drifts from the bake key
    // does not render wrongly, it renders with NO baked GI and no error at all,
    // which is worse.
    //
    // Receivers whose mesh is missing or empty are skipped here rather than by
    // each caller, so "gathered" already means "bakeable as far as the ECS can
    // tell". The baker still rejects a singular world transform or a failed
    // unwrap of its own.
    [[nodiscard]] std::vector<LightmapReceiver> GatherLightmapReceivers(Scene& scene);

    // The sub-key the gather assigned to `model.GetMeshes()[meshIndex]` — the
    // index of the FIRST mesh sharing that mesh's MeshSource. The draw path
    // calls this per submesh so it recovers exactly the key the bake wrote;
    // computing it twice from the same list is what keeps the two in step
    // without threading a map through Model::DrawParallel.
    //
    // Returns 0 for an out-of-range index or a null mesh, which is the "whole
    // entity" key and simply misses in a table that has no entry for it.
    [[nodiscard]] u64 LightmapSubKeyForModelMesh(const Model& model, sizet meshIndex);

    // Unwrap one receiver's mesh for the bake, dealing with the cooked
    // virtual-geometry blob that would otherwise make the unwrap refuse.
    //
    // LightmapUnwrap::Generate rejects a MeshSource carrying a virtual-mesh blob
    // outright, because the unwrap SEAM-SPLITS vertices and the cooked cluster
    // DAG indexes the original vertex order — a DAG kept across an unwrap points
    // at the wrong vertices. That guard is right about the hazard and wrong as a
    // final answer for a lightmap-static virtual mesh: refusing means the mesh
    // can never carry UV2, so it can never receive baked GI, silently.
    //
    // The blob is a CACHE, not the truth: VirtualMeshRegistry falls back to
    // VirtualMeshBuilder::BuildSet when it is absent or fails validation. So the
    // resolution is to DROP the stale cook and let it be rebuilt from the
    // unwrapped geometry, which is the only ordering that produces a DAG whose
    // vertices carry UV2 at all.
    //
    // Returns true when the mesh has (or now has) a complete UV2 stream. Also
    // invalidates the registry's cached DAG so the next submission re-cooks —
    // without that, IsRegistered()'s fast path serves the pre-unwrap DAG for the
    // process lifetime.
    [[nodiscard]] bool PrepareReceiverForBake(const LightmapReceiver& receiver);
} // namespace OloEngine
