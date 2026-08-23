#pragma once

#include "OloEngine/Core/Base.h"

#include <glm/glm.hpp>

namespace OloEngine
{
    // @brief Per-instance data uploaded to the instance SSBO (binding = SSBO_INSTANCE_DATA).
    //
    // Layout mirrors std430 packing rules. The instance buffer is an array of these
    // structs and is indexed in shaders via `gl_InstanceIndex`. Non-instanced draws
    // bind a single-element InstanceBuffer so the same shader code path works in
    // both cases.
    //
    // Field-by-field mapping for shaders migrating from the legacy ModelUBO (binding = 3):
    //   u_Model      -> instances[gl_InstanceIndex].Transform
    //   u_Normal     -> instances[gl_InstanceIndex].Normal
    //   u_PrevModel  -> instances[gl_InstanceIndex].PrevTransform
    //   u_EntityID   -> instances[gl_InstanceIndex].EntityID
    //
    // Color and Custom are new and used by the explicit instancing path
    // (InstancedMeshComponent): Color tints per-instance, Custom is a free float
    // (wind sway phase, health-bar fill, etc.).
    struct InstanceData
    {
        glm::mat4 Transform = glm::mat4(1.0f);     // world transform
        glm::mat4 Normal = glm::mat4(1.0f);        // transpose(inverse(Transform)), supplied CPU-side
        glm::mat4 PrevTransform = glm::mat4(1.0f); // previous-frame world transform for motion vectors
        glm::vec4 Color = glm::vec4(1.0f);         // per-instance tint
        i32 EntityID = -1;                         // editor picking; -1 = no entity
        f32 Custom = 0.0f;                         // free per-instance float
        i32 Pad0 = 0;
        i32 Pad1 = 0;
        // Lightmap atlas region (issue #439): the fragment stage addresses this
        // instance's charts in the scene lightmap atlas as uv2 * xy + zw. All
        // zeros (the default) means "no lightmap" — the shader's ambient ladder
        // falls through to probes/IBL, so non-lightmapped draws need no writes.
        glm::vec4 LightmapScaleOffset = glm::vec4(0.0f);
    };

    // std430 size assertion. Layout (offset, size):
    //   Transform           (  0, 64)
    //   Normal              ( 64, 64)
    //   PrevTransform       (128, 64)
    //   Color               (192, 16)
    //   EntityID            (208,  4)
    //   Custom              (212,  4)
    //   Pad0               (216,  4)
    //   Pad1               (220,  4)
    //   LightmapScaleOffset (224, 16)
    // Total: 240 bytes, divisible by 16 (mat4 alignment) so array stride is 240 with no end padding.
    // A drift here means the C++ struct and GLSL block disagree and the shader will read garbage.
    static_assert(sizeof(InstanceData) == 240, "InstanceData std430 size drifted from GLSL expectation (240 B)");
    static_assert(sizeof(InstanceData) % 16 == 0, "InstanceData size must be 16-byte aligned for std430 array stride");
} // namespace OloEngine
