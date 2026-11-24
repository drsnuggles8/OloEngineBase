#pragma once

#include "OloEngine/Renderer/Crowd/CrowdClip.h"
#include "OloEngine/Renderer/MeshSource.h"

namespace OloEngine
{
    // Each tier keeps the source's submesh order, material slots and rig.
    // Each submesh draws once per LOD tier across all animation phases.
    [[nodiscard]] Ref<MeshSource> MakeCrowdMidGeometry(const Ref<MeshSource>& source);
    [[nodiscard]] Ref<MeshSource> MakeCrowdFarGeometry(const Ref<MeshSource>& source);

    // A rigid, posed copy shared by all members of a phase cohort. Existing
    // CSM/VSM instancing and canonical GPU Scene records can consume its VAO.
    [[nodiscard]] Ref<MeshSource> BakeCrowdProxy(const Ref<MeshSource>& geometry, const CrowdClip& clip, u32 frame);
} // namespace OloEngine
