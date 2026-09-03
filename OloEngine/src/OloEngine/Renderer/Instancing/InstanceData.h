#pragma once

#include "OloEngine/Core/Base.h"

#include <glm/glm.hpp>

#include <cstddef>

namespace OloEngine
{
    // The "no canonical link" value of InstanceData::GPUSceneRef. Zero in the
    // two generation lanes is what a consumer tests: GPUSceneTypes.h reserves
    // generation zero as invalid, so a link can never be mistaken for one.
    inline constexpr u32 GPUSceneDrawRefUnlinked = 0u;

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
        // Persistent CPU identity used by GPU Scene temporal matching. It
        // occupies the former 8-byte padding lane, so the SSBO stride and all
        // following field offsets remain unchanged. Zero means unassigned;
        // InstancedMeshComponent assigns a unique non-zero value before use.
        u64 StableID = 0;
        // Lightmap atlas region (issue #439): the fragment stage addresses this
        // instance's charts in the scene lightmap atlas as uv2 * xy + zw. All
        // zeros (the default) means "no lightmap" — the shader's ambient ladder
        // falls through to probes/IBL, so non-lightmapped draws need no writes.
        glm::vec4 LightmapScaleOffset = glm::vec4(0.0f);
        // Canonical GPU Scene reference (issue #994): the slot and generation
        // of the GPUSceneInstance this draw was extracted as, and of the
        // GPUSceneMaterial that instance resolved to. This is the LINK, not a
        // copy: the record stays the authority, and a consumer re-validates the
        // generation before it reads the slot, exactly as GPUSceneTypes.h
        // requires of any handle held across a frame boundary.
        //
        // A zero generation means "this draw carries no canonical link" — a
        // legacy adapter submitted it (GPUSceneLegacyAdapters.h) or its record
        // was retired between extraction and dispatch. Every consumer must then
        // fall back to the per-draw UBO it read before; unsupported geometry
        // must never disappear because its link is absent.
        glm::uvec4 GPUSceneRef = glm::uvec4(GPUSceneDrawRefUnlinked);
    };

    // std430 size assertion. Layout (offset, size):
    //   Transform           (  0, 64)
    //   Normal              ( 64, 64)
    //   PrevTransform       (128, 64)
    //   Color               (192, 16)
    //   EntityID            (208,  4)
    //   Custom              (212,  4)
    //   StableID           (216,  8)
    //   LightmapScaleOffset (224, 16)
    //   GPUSceneRef         (240, 16)
    // Total: 256 bytes, divisible by 16 (mat4 alignment) so array stride is 256 with no end padding.
    // A drift here means the C++ struct and GLSL block disagree and the shader will read garbage.
    static_assert(sizeof(InstanceData) == 256, "InstanceData std430 size drifted from GLSL expectation (256 B)");
    static_assert(offsetof(InstanceData, LightmapScaleOffset) == 224);
    static_assert(offsetof(InstanceData, GPUSceneRef) == 240);
    static_assert(sizeof(InstanceData) % 16 == 0, "InstanceData size must be 16-byte aligned for std430 array stride");
} // namespace OloEngine
