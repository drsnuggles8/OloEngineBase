#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/Frustum.h"
#include "OloEngine/Renderer/RHI/RHITypes.h"
#include "OloEngine/Terrain/Foliage/FoliageInstanceRegistry.h"

#include <glm/glm.hpp>
#include <span>
#include <vector>

namespace OloEngine
{
    class ComputeShader;
    class StorageBuffer;
    class VertexBuffer;

    // @brief GPU patch + instance culling and compaction for foliage, feeding an
    // indirect draw (issue #1235).
    //
    // ── The reuse decision, written down because the issue asks for it ────────
    //
    // The issue says "reuse existing GPU culling/indirect infrastructure where
    // possible". What is reused here is the indirect submission path
    // (`RendererAPI::DrawBoundElementsIndirect` and the `ComputeShader` /
    // `StorageBuffer` plumbing), the `GPUReadbackStats` counter channel (#721)
    // and the dispatch-local SSBO numbering convention `GPUFrustumCuller`
    // established. What is NOT reused is `GPUFrustumCuller` itself, for three
    // reasons that are all properties of the data rather than of taste:
    //
    //   1. **A different per-instance payload.** GPUFrustumCuller compacts
    //      128-byte `InstanceData` records into SSBO 15, which every mesh vertex
    //      stage reads through `InstanceBlock.glsl`. Foliage's per-instance
    //      record is the 48-byte `FoliageInstanceData` on VAO stream 1 — GL
    //      attributes, Vulkan's binding-63 pull — and it carries height, fade,
    //      tint and alpha cutoff that `InstanceData` has no lanes for. Routing
    //      foliage through the mesh culler would mean rewriting every foliage
    //      vertex stage AND tripling the cull's bandwidth to carry two 4x4
    //      matrices per blade of grass. The engine SSBO namespace is full
    //      (ShaderBindingLayout.h), so minting a third instance channel was not
    //      an option either; keeping foliage on stream 1 is what lets the
    //      compacted buffer be drawn with NO shader change at all.
    //
    //   2. **A per-instance bound, not one shared sphere.** GPUFrustumCuller
    //      takes a single local bounding sphere and one radius expansion for the
    //      whole batch. A foliage layer's plants differ in scale AND height, and
    //      their conservative bound has to include the wind and interaction
    //      displacement the registry already derived per layer (#1236 / #1238).
    //      The bound is therefore rebuilt per instance from the row, as the twin
    //      of `FoliageInstanceBounds()`.
    //
    //   3. **Two levels.** The issue's title is "patch AND instance culling".
    //      `FoliageInstanceRegistry` already buckets plants into 16 m spatial
    //      groups with correct bounds (#1230); testing those first is what makes
    //      the instance pass cheap. GPUFrustumCuller is flat and has no shape
    //      for a group.
    //
    // ── What runs ────────────────────────────────────────────────────────────
    //
    //   1. `FoliageGroupCull.comp`    — one thread per spatial group: frustum +
    //      distance against the group's terrain-local AABB, leaving a visibility
    //      bit.
    //   2. `FoliageInstanceCull.comp` — one thread per instance row: skips a
    //      rejected group outright, tests the survivors, and atomic-appends the
    //      row WHOLE into a compacted `VertexBuffer` while bumping every part's
    //      `DrawElementsIndirectCommand::instanceCount`.
    //   3. The draw binds the compacted buffer as VAO stream 1 and issues
    //      `DrawBoundElementsIndirect` — no CPU round trip, no readback.
    //
    // ── Identity ─────────────────────────────────────────────────────────────
    //
    // Rows are copied bit-for-bit and every per-instance history downstream is
    // keyed on the plant's POSITION rather than its slot, so a plant that leaves
    // and returns cannot inherit another's history. The compacted slot -> source
    // row map is written anyway so that claim is checkable against
    // `FoliageInstanceRegistry::GetIdForBufferRow`. The long form of this
    // argument is in FoliageInstanceCull.comp.
    class FoliageGPUCuller : public RefCounted
    {
      public:
        // Drawable parts a layer may emit into one indirect args block: the flat
        // card plus the authored mesh's submeshes (#1233). A layer with more
        // submeshes than this draws its extra parts non-indirectly, loudly —
        // see FoliageRenderer.
        static constexpr u32 kMaxParts = 16;

        // Stride of one DrawElementsIndirectCommand record. 32 rather than the
        // 20 bytes GL reads, so each part's draw offset is 4-byte aligned on
        // both backends and the std430 struct needs no packing rules of its own.
        static constexpr u32 kDrawArgsStride = 32;

        // Shadow views that get their own cull. FOUR, matching
        // ShaderBindingLayout::MAX_CSM_CASCADES, so the whole cascade set is
        // covered; a local-light atlas region with more entries than this culls
        // the first four and draws the rest uncompacted, loudly.
        //
        // ONE SET OF BUFFERS PER VIEW, and that is not a nicety. Vulkan records
        // the shadow cascades in PARALLEL (ShadowRenderPass::RecordShadowRegion
        // -> RenderCommand::RecordParallel), and an object written by two items
        // of one region is a hard error there -- amendment (92) rule 6, the same
        // rule the per-item InstanceBuffer next to it exists for. A first cut
        // shared one "shadow scratch" set across cascades, on the argument that
        // cascades execute in GPU command order; the Vulkan backend rejected it
        // with "storage buffer written by RecordParallel items 1 and 3 in one
        // region -- the second write is dropped", plus three "VulkanOneShot::
        // Submit from a RecordParallel item" for the CPU-side header uploads.
        // Both are now structurally impossible: every cull runs BEFORE the
        // parallel region, and an item only ever READS its own slot.
        static constexpr u32 kMaxShadowViews = 4;

        // Which cull a set of buffers belongs to. Slot 0 is the main view; slots
        // 1..kMaxShadowViews are shadow views, in the region's item order.
        enum class ViewSlot : u32
        {
            Main = 0,
            // The FIRST shadow view. Later ones are ShadowSlot(i).
            Shadow = 1,
        };
        static constexpr u32 kViewSlotCount = 1u + kMaxShadowViews;

        [[nodiscard]] static constexpr u32 ShadowSlot(u32 shadowViewIndex)
        {
            return 1u + shadowViewIndex;
        }

        // One drawable index range of a layer, as the args kernel needs it.
        struct Part
        {
            u32 IndexCount = 0;
            u32 BaseIndex = 0;
        };

        // The view being culled for. Everything TERRAIN-LOCAL: the instance rows
        // and group bounds live there, so evaluating the test there needs no
        // per-instance transform and keeps the math precise far from the world
        // origin (#429). Build with MakeFoliageCullInputs.
        struct ViewInputs
        {
            Frustum ViewFrustum;
            // The MAIN view's position, never the light's — see the
            // s_DistanceOrigin comment in FoliageCullCommon.glsl.
            glm::vec3 DistanceOrigin{ 0.0f };
            f32 MaxDistance = 0.0f;
        };

        // Per-LAYER GPU data: group bounds + the row -> group table + the bounds
        // profile. Rebuilt only when the registry generation moves, which is
        // what makes the per-frame cost two dispatches and no uploads.
        struct LayerResources
        {
            Ref<StorageBuffer> LayerBuffer;
            u32 GroupCount = 0;
            u32 InstanceCount = 0;
            // Registry generation + terrain transform the buffer was built from.
            // The transform matters because group bounds are cached in world
            // space by the registry while these are terrain-local — a move does
            // not invalidate them, so only the generation is tracked.
            u64 BuiltGeneration = ~0ull;
            // The registry and the instance buffer disagreed at BuiltGeneration,
            // so this layer is deliberately NOT culled. Latched with the
            // generation rather than recomputed, because the refusal costs a
            // full record scan and an error line, and without the latch both
            // repeat once per layer per view per frame.
            bool RefusedForBuiltGeneration = false;

            [[nodiscard]] bool IsValid() const
            {
                return LayerBuffer != nullptr && GroupCount > 0 && InstanceCount > 0;
            }
        };

        // Per (layer, view slot).
        struct ViewResources
        {
            // The compacted survivors. A VertexBuffer rather than a
            // StorageBuffer: it is written by compute AND consumed as VAO stream
            // 1 by the draw, and both backends already allow that (GL: a buffer
            // is a buffer; Vulkan: VulkanVertexBuffer carries
            // VK_BUFFER_USAGE_STORAGE_BUFFER_BIT and BindStorageBuffer resolves
            // its device address).
            Ref<VertexBuffer> Compacted;
            Ref<StorageBuffer> State;    // binding 19 — params, counters, tail
            Ref<StorageBuffer> DrawArgs; // binding 17 — kMaxParts commands
            u32 Capacity = 0;            // instances the compacted buffer holds
            u32 GroupCapacity = 0;       // groups the state tail holds
            u32 PartCount = 0;           // parts the last cull wrote args for

            [[nodiscard]] bool IsValid() const
            {
                return Compacted != nullptr && State != nullptr && DrawArgs != nullptr;
            }
        };

        FoliageGPUCuller() = default;

        // Load the two kernels. Idempotent, and a failure is latched so a broken
        // shader costs one error line rather than one per frame.
        void EnsureInitialised();

        // False when the kernels could not be loaded. The caller then submits
        // the layer's FULL instance stream — a correct frame, just an uncompacted
        // one — and says so once. Never a silent fallback.
        [[nodiscard]] bool IsAvailable() const
        {
            return m_Initialised;
        }

        [[nodiscard]] bool LoadFailed() const
        {
            return m_LoadFailed;
        }

        // (Re)build `out` from the registry if the generation moved. Returns
        // false when the layer has nothing to cull (no instances, or no groups),
        // which is not an error.
        bool BuildLayer(LayerResources& out, const FoliageInstanceRegistry& registry, u32 layerIndex,
                        u32 instanceCount, const FoliageBoundsProfile& profile);

        // `emitStats` publishes the Generated/Tested/Visible/Submitted counters
        // into the per-frame GPUReadbackStats block. True for the main view
        // only; every other view would sum into the same block.

        // Size `view` for `capacity` instances and `groupCount` groups. Returns
        // true when the COMPACTED BUFFER was (re)created, which is the caller's
        // signal to rebuild the vertex arrays that stream it.
        bool EnsureViewCapacity(ViewResources& view, u32 capacity, u32 groupCount) const;

        // Dispatch the two kernels. `sourceInstances` is the layer's full
        // instance stream. Returns false without dispatching when anything is
        // missing, so the caller can take the uncompacted path.
        bool Cull(const LayerResources& layer, ViewResources& view, RHI::ResourceHandle sourceInstances,
                  std::span<const Part> parts, const ViewInputs& inputs, bool emitStats);

        // Artificially shrink the capacity the append bound-checks against, so
        // the overflow path can be exercised for real (issue #1235's fourth
        // criterion), exactly as GPUFrustumCuller::SetDebugOutputCapacity does
        // for the mesh cull. 0 = off, the only value production uses.
        //
        // It has to be a real truncation rather than a "pretend the flag fired"
        // switch: the criterion is that an overflowed buffer is BOUNDED and
        // EXPLICIT, and a faked flag would test the wiring between a bool and
        // the overlay instead of the thing that actually goes wrong. With this
        // knob the draw really renders fewer plants, FoliageCullDropped really
        // counts the ones refused, and FoliageCullOutput reports a condition
        // that really happened.
        void SetDebugOutputCapacity(u32 entries)
        {
            m_DebugOutputCapacity = entries;
        }

        [[nodiscard]] u32 GetDebugOutputCapacity() const
        {
            return m_DebugOutputCapacity;
        }

        // What one cull produced, read back to the CPU.
        //
        // A GPU -> CPU read, so it STALLS: this is for tests, the MCP inspector
        // and a one-off diagnostic, never for a per-frame consumer. The
        // per-frame numbers go through the GPUReadbackStats channel instead,
        // which is fenced and never blocks (Renderer/Debug/GPUReadbackStats.h).
        //
        // SourceRows is the half that makes identity checkable: entry i is the
        // instance-buffer ROW that landed in compacted slot i, and
        // FoliageInstanceRegistry::GetIdForBufferRow turns that into the
        // canonical FoliageInstanceId.
        struct Readback
        {
            u32 GroupsVisible = 0;
            u32 Visible = 0;   // passed the per-instance test
            u32 Reserved = 0;  // append slots handed out; > Visible never happens
            u32 Submitted = 0; // what the indirect draw draws
            std::vector<u32> SourceRows;
            std::vector<FoliageInstanceData> Compacted;
        };

        [[nodiscard]] bool ReadbackResult(const LayerResources& layer, const ViewResources& view,
                                          Readback& out) const;

        // Byte offset of part `index`'s command inside a ViewResources::DrawArgs
        // buffer, for DrawBoundElementsIndirect.
        [[nodiscard]] static constexpr u32 DrawArgsOffset(u32 index)
        {
            return index * kDrawArgsStride;
        }

      private:
        Ref<ComputeShader> m_GroupCullShader;
        Ref<ComputeShader> m_InstanceCullShader;
        bool m_Initialised = false;
        bool m_LoadFailed = false;
        u32 m_DebugOutputCapacity = 0;
    };

    // ── The two GPU-side headers, mirrored ────────────────────────────────────
    //
    // GLSL twin: OloEditor/assets/shaders/include/FoliageCullCommon.glsl. These
    // are std430 headers followed by a `uint` tail, so member ORDER is the
    // contract; FoliageGPUCullParityTest runs the real dispatch and would fail
    // on any drift, but the static_asserts below catch the cheap half at build
    // time.

    struct FoliageCullLayerHeader
    {
        u32 GroupCount = 0;
        u32 InstanceCount = 0;
        u32 RowGroupOffset = 0; // in uints, into the tail
        u32 Pad0 = 0;
        // FoliageBoundsProfile, field for field.
        f32 HalfExtentXZ = 0.5f;
        f32 HalfExtentXZHeightScaled = 0.0f;
        f32 MinY = 0.0f;
        f32 MaxY = 1.0f;
        f32 WindDisplacement = 0.0f;
        f32 InteractionDisplacement = 0.0f;
        f32 Pad1 = 0.0f;
        f32 Pad2 = 0.0f;
    };
    static_assert(sizeof(FoliageCullLayerHeader) == 48, "FoliageCullLayerHeader must match the std430 block");

    struct FoliageCullStateHeader
    {
        glm::vec4 Planes[6]{};
        glm::vec4 DistanceOrigin{ 0.0f }; // xyz = main view position, w = max distance
        u32 InstanceCount = 0;
        u32 GroupCount = 0;
        u32 OutputCapacity = 0;
        u32 PartCount = 0;
        u32 VisibleCount = 0;
        u32 ReserveCursor = 0;
        u32 GroupsVisible = 0;
        u32 SourceRowOffset = 0; // in uints, into the tail
        // 1 on the main view only -- see the s_EmitStats comment in
        // FoliageCullCommon.glsl for why the ratio counters cannot be summed
        // over the frame's five culls.
        u32 EmitStats = 0;
        u32 StatsPad0 = 0;
        u32 StatsPad1 = 0;
        u32 StatsPad2 = 0;
    };
    static_assert(sizeof(FoliageCullStateHeader) == 160, "FoliageCullStateHeader must match the std430 block");

    // One DrawElementsIndirectCommand as the kernels and both backends read it.
    struct FoliageCullDrawArgs
    {
        u32 Count = 0;
        u32 InstanceCount = 0;
        u32 FirstIndex = 0;
        u32 BaseVertex = 0;
        u32 BaseInstance = 0;
        u32 Pad0 = 0;
        u32 Pad1 = 0;
        u32 Pad2 = 0;
    };
    static_assert(sizeof(FoliageCullDrawArgs) == FoliageGPUCuller::kDrawArgsStride,
                  "the draw-args stride and the record must agree, or part N draws part N-1's range");
} // namespace OloEngine
