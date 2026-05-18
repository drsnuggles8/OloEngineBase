#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Asset/Asset.h" // AssetHandle
#include "OloEngine/Animation/AnimatedMeshComponents.h" // MeshPrimitive
#include "OloEngine/Renderer/Instancing/InstanceData.h"
#include "OloEngine/Renderer/Material.h"
#include "OloEngine/Renderer/Mesh.h"

#include <vector>

namespace OloEngine
{
    // @brief Entity-owned dense instance batch.
    //
    // A single entity holding many copies of the same mesh+material — the
    // "10,000 trees in a clearing" case that auto-batching of separate
    // MeshComponent entities can't cover efficiently. Each entry in `Instances`
    // is a fully-resolved world-space InstanceData (Transform, Color, ...),
    // uploaded into the renderer's InstanceBuffer SSBO and drawn through a
    // single DrawMeshInstanced packet (chunked at MaxMeshInstances when the
    // count exceeds the per-batch cap).
    //
    // Lifetime / ownership:
    //   - `MeshSource` is shared between all instances; null entries are
    //     skipped by the scene renderer rather than crashing.
    //   - `OverrideMaterial`, if set, takes precedence over the entity's
    //     MaterialComponent. Leave null to fall back to MaterialComponent or
    //     the engine default.
    //   - `Instances` is the authoritative transform list. The entity's own
    //     TransformComponent is NOT applied on top — instances live in world
    //     space. (Parented authoring tools can pre-multiply at write time.)
    //
    // Serialization, editor inspector, and scripting bindings are added in
    // Phase 5 of the GPU instancing rollout — the component is C++-only for
    // now.
    struct InstancedMeshComponent
    {
        Ref<MeshSource> MeshSource;

        // Engine primitive to use when MeshSource is not explicitly assigned —
        // mirrors MeshComponent's pattern. SceneSerializer creates the
        // matching MeshSource from this primitive at deserialize time when
        // MeshSource is null. Useful for demo scenes that want a cube/plane/
        // cone instance batch without authoring a separate mesh asset.
        MeshPrimitive Primitive = MeshPrimitive::None;

        Ref<class Material> OverrideMaterial;

        // Inline placement list authored at runtime / via scripting. Always
        // rendered. Combined with PlacementAssetHandle's instances at draw
        // time (asset instances are appended to this list).
        std::vector<InstanceData> Instances;

        // Optional reference to an authored .oloinstances placement asset.
        // Scene render loop loads the asset (if handle valid) and includes
        // its instances in the per-frame draw. Useful for sharing dense
        // placements (foliage / debris / crowds) across scenes.
        AssetHandle PlacementAssetHandle = 0;

        bool FrustumCullPerInstance = true; // Cull instances individually before upload
        bool CastShadows            = true; // Submit shadow-caster entries for visible instances
        f32  CullDistance           = 0.0f; // World-space radius; 0 disables distance culling
    };
} // namespace OloEngine
