#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/VirtualGeometry/VirtualMesh.h"
#include "OloEngine/Renderer/VirtualGeometry/VirtualMeshGpuData.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace OloEngine
{
    // ── Where a skinned virtual mesh's GPU data lives (issue #1150) ──────────
    //
    // Neither of the two streams skinning needs has a binding of its own, for
    // exactly the reason VirtualLightmapUVPacking.h records for the uv2 stream:
    // the SSBO namespace is FULL below Mesa's hard ceiling of 80
    // (docs/agent-rules/ssbo-binding-cap-is-80-on-mesa.md — every one of 0..79
    // is named), and the one number that reads as reusable, SSBO_BONE_PULL (63),
    // is resolved from the DRAW'S VAO STREAMS on Vulkan and so cannot carry a
    // published buffer to the mesh-shader route at all.
    //
    // So both ride the cluster VERTEX ARENA (SSBO_VIRTUAL_VERTICES, 39) as
    // packed tails, after the uv2 tail:
    //
    //     [ vertices:     SlotCount * SlotVertexCapacity elements ]
    //     [ uv2 tail:     SlotCount * SlotVertexCapacity / 4      ]  (issue #867)
    //     [ skin tail:    SlotCount * SlotVertexCapacity / 2      ]
    //     [ cluster bones: one element per POOLED cluster         ]
    //
    // Both tails are allocated only when some registered mesh is skinned, so a
    // rigid scene's arena is byte-for-byte what it was before.
    //
    // WHY THE SKIN BINDING IS QUANTIZED TO 16 BYTES
    //
    // The obvious encoding — four u32 ids plus four f32 weights — is 32 bytes,
    // one whole arena element per vertex, i.e. it DOUBLES the largest buffer in
    // the renderer the moment one character is registered. Halving it costs
    // nothing real: bone ids are indices into a palette the engine caps at
    // AnimationConstants::MAX_BONES (100), so 16 bits is four hundred times the
    // range anything can use; and weights are unorm values the shader
    // RENORMALIZES by the weight that actually landed, so unorm16's 1.5e-5 step
    // is well under the precision at which a skinned vertex is meaningful.
    //
    // Quantization does NOT weaken any bound in VirtualSkinningBounds.h: every
    // one of them is derived from WHICH bones influence a cluster, never from
    // the weights. A weight can be rounded to a different value and the vertex
    // still lands inside the same convex hull.
    //
    // ── Why the addressing needs no per-page fixup ───────────────────────────
    //
    // The same two facts the uv2 tail rests on, and they are equally
    // load-bearing here:
    //  1. VirtualMeshRegistry::LoadPage copies a page's vertices to slot-local
    //     index 0, so a global index's offset within its slot is its local one.
    //  2. SlotVertexCapacity is rounded UP to a multiple of 4, which is also a
    //     multiple of the 2 this tail packs, so `slot * capacity` lands on a
    //     lane boundary.
    // Break either and this silently deforms vertices by another vertex's bones.
    //
    // The GLSL twin is include/VirtualSkinning.glsl. Change one, change both.

    // Skin bindings packed into one 32-byte arena element.
    inline constexpr u32 kVirtualSkinningPerElement = 2;

    // The engine's bone-palette ceiling expressed where the packing can see it.
    // A bone id at or above this cannot be addressed by any shader anyway
    // (every skinned shader tests `boneID < MAX_BONES`), so clamping to the
    // sentinel here turns "unreachable palette slot" into "no influence"
    // ONCE, at pack time, rather than into a wrong-bone read per frame.
    inline constexpr u32 kVirtualSkinningMaxBoneId = 0xFFFFu;

    // Elements needed to hold `vertexCount` skin bindings.
    [[nodiscard("returns the packed element count; it computes nothing else")]] inline constexpr u32
    VirtualSkinningElementCount(u32 vertexCount) noexcept
    {
        return (vertexCount + kVirtualSkinningPerElement - 1) / kVirtualSkinningPerElement;
    }

    // Element offset, RELATIVE to the tail's base element, holding this vertex.
    [[nodiscard("returns the element offset; it computes nothing else")]] inline constexpr u32
    VirtualSkinningElementOffset(u32 globalVertexIndex) noexcept
    {
        return globalVertexIndex >> 1;
    }

    // unorm16 quantization of one weight. Clamped rather than asserted: a
    // weight outside [0,1] has already been rejected by the cook and by the
    // blob reader, and clamping is the behaviour that keeps a hand-built test
    // fixture from tripping over the edge case.
    [[nodiscard]] inline u32 QuantizeVirtualSkinWeight(f32 weight) noexcept
    {
        if (!std::isfinite(weight))
        {
            return 0u;
        }
        f32 const clamped = std::clamp(weight, 0.0f, 1.0f);
        return static_cast<u32>(std::lround(static_cast<f64>(clamped) * 65535.0));
    }

    [[nodiscard]] inline f32 DequantizeVirtualSkinWeight(u32 quantized) noexcept
    {
        return static_cast<f32>(quantized & 0xFFFFu) / 65535.0f;
    }

    // Write one skin binding into its lane of `element`.
    //
    // Lane 0 is PositionU, lane 1 is NormalV. The field names belong to the
    // vertex layout this region borrows; here each lane is four raw 32-bit words:
    //     word0 = boneId0 | boneId1 << 16
    //     word1 = boneId2 | boneId3 << 16
    //     word2 = weight0 | weight1 << 16   (unorm16)
    //     word3 = weight2 | weight3 << 16
    // The words are built explicitly and memcpy'd into the float lanes, so the
    // bit layout is the same on both sides regardless of how either compiler
    // lays out a struct.
    inline void PackVirtualSkinning(VirtualGpuVertex& element, u32 globalVertexIndex,
                                    const VirtualVertexSkinning& binding) noexcept
    {
        auto boneWord = [&binding](u32 first) noexcept
        {
            auto clampId = [](u32 id, f32 weight) noexcept
            { return (weight > 0.0f && id <= kVirtualSkinningMaxBoneId) ? id : kVirtualSkinningMaxBoneId; };
            u32 const lo = clampId(binding.BoneIDs[first], binding.Weights[first]);
            u32 const hi = clampId(binding.BoneIDs[first + 1], binding.Weights[first + 1]);
            return lo | (hi << 16);
        };
        auto weightWord = [&binding](u32 first) noexcept
        {
            u32 const lo = QuantizeVirtualSkinWeight(binding.Weights[first]);
            u32 const hi = QuantizeVirtualSkinWeight(binding.Weights[first + 1]);
            return lo | (hi << 16);
        };

        u32 const words[4] = { boneWord(0), boneWord(2), weightWord(0), weightWord(2) };
        glm::vec4& lane = ((globalVertexIndex & 1u) == 0u) ? element.PositionU : element.NormalV;
        std::memcpy(&lane, words, sizeof(words));
    }

    // The C++ twin of the shader's read: used by the packing tests and by the
    // CPU side of the GPU/CPU skinning parity checks, which must compare against
    // what the GPU will actually see rather than against the unquantized cook.
    [[nodiscard]] inline VirtualVertexSkinning UnpackVirtualSkinning(const VirtualGpuVertex& element,
                                                                     u32 globalVertexIndex) noexcept
    {
        const glm::vec4& lane = ((globalVertexIndex & 1u) == 0u) ? element.PositionU : element.NormalV;
        u32 words[4] = {};
        std::memcpy(words, &lane, sizeof(words));

        VirtualVertexSkinning binding;
        for (u32 i = 0; i < 4; ++i)
        {
            u32 const packedId = (words[i / 2] >> ((i & 1u) * 16u)) & 0xFFFFu;
            u32 const packedWeight = (words[2 + i / 2] >> ((i & 1u) * 16u)) & 0xFFFFu;
            // The sentinel id is reported as weight 0 rather than as an id,
            // because that is how every consumer must treat it: a slot with no
            // bone contributes nothing, and an id of 0xFFFF is not a palette
            // slot anybody should reach for.
            bool const empty = packedId == kVirtualSkinningMaxBoneId;
            binding.BoneIDs[i] = empty ? 0u : packedId;
            binding.Weights[i] = empty ? 0.0f : DequantizeVirtualSkinWeight(packedWeight);
        }
        return binding;
    }
} // namespace OloEngine
