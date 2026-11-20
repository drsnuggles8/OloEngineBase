#pragma once

// =============================================================================
// GroomGpuDeformation.h — a bound coat moved by the GPU. Issue #1427.
//
// WHAT THIS REPLACES. Until #1427 a bound coat was rebuilt on the CPU every
// frame: every segment's four ribbon corners re-derived from the root
// transforms and the guide displacements, then the whole 64-byte-per-corner
// stream uploaded again. That was 125-219 MB per coat per frame — the cost the
// issue measured in hundreds of milliseconds.
//
// WHAT IT IS NOW. Two halves with two lifetimes:
//
//   * the REST stream (BuildGroomStrandRestMesh): the same sixteen floats per
//     corner, written once, with every point expressed in its root's BIND
//     frame. It depends on the asset, the budget, the coat and the binding, and
//     on nothing that moves, so it is cached like an unbound groom's stream.
//
//   * the FRAME buffer (this file): per drawn strand, the rigid transform its
//     root has this frame and had last frame, plus the guide displacements the
//     simulation produced. 64 bytes a strand rather than 256 bytes a segment,
//     so a coat of 60 000 strands and 900 000 segments sends 4 MB, not 230 MB.
//
// The vertex shader evaluates `Origin + Rotation * local`, then adds the
// interpolated guide displacement — exactly ApplyGroomRootTransform followed by
// SampleGroomGuideDisplacement, in that order, the order BuildGroomStrandMesh
// has always used. EvaluateGroomDeformedPoint below is that same arithmetic on
// the CPU, reading the SAME packed bytes, so the coat self-shadow bake (#1426)
// sees the pose the GPU draws and a test can compare the two paths without a
// GPU.
//
// THE BUFFER'S LAYOUT, in 16-byte units, one storage block (GLSL twin:
// include/GroomStrandDeform.glsl):
//
//   [0, 2R)                 guide weights, one GroomGuideWeights per strand —
//                           STATIC, written when the entry is built
//   [BindBase, +2R)         GroomDeformBindRecord per strand — STATIC, the
//                           root's bind frame (#1533), written by the first
//                           PackFrame after a Reset
//   [ClumpBase, +R)         the strand's clump growth in its bind frame —
//                           STATIC, what a wet coat gathers its tips toward
//                           (#1615), written with the bind frames
//   [WetBase, +4)           GroomDeformWetRecord, the coat's wetness  } per
//   [RootBase, +4R)         GroomDeformRootRecord per strand          } per
//   [SlotBase, +S)          GroomDeformSlotRecord per guide slot      } frame
//   [DisplacementBase, +2D) GroomDeformDisplacementRecord per point   }
//
// and, when the drawn roots are evaluated ON THE GPU (#1533 E1; see
// compute/GroomRootFrames.comp), three more that the vertex stage never reads:
//
//   [SkinBase, +2R)         GroomRootSkinRecord per strand   } STATIC
//   [VertexBase, +3V)       GroomSurfaceVertexRecord per     }
//                           vertex of the bound surface      }
//   [PaletteBase, +8B)      this frame's bone palette, then last frame's,
//                           one mat4 per bone -- per frame
//
// The root region is then WRITTEN BY THE COMPUTE PASS, not uploaded: skinning
// ~300k roots' triangles and packing and sending their 64-byte records was
// the CPU's largest cost in the showcase dog's frame.
//
// And when the guides are EXPANDED ON THE GPU (#1589; see
// compute/GroomExpandGuides.comp), three more, after everything above:
//
//   [StandInBase, +2S)      GroomGuideWeights per slot: its stand-ins } per
//   [GuideOffsetBase, ..)   the simulated guides' prefix, 4 to a unit } budget
//   [SourceBase, +2C)       GroomDeformDisplacementRecord per         } per
//                           SIMULATED point, C its capacity           } frame
//
// The slot and displacement regions are then laid out over EVERY slot -- the
// dense layout the vertex stage reads -- and the displacement region is
// WRITTEN BY THE KERNEL, not uploaded: a budget simulates a tenth of a coat's
// guide slots, and sending the other nine tenths' blends was ~170 MiB a frame
// for the acceptance herd, 52-56 ms of GroomPass on OpenGL. The slot and
// stand-in regions go up when the budget changes, the source every frame.
//
// ONE block because the storage-buffer namespace is full
// (ShaderBindingLayout.h, SSBO_GROOM_DEFORMATION), and because one block is one
// command-ordered upload per coat per frame on Vulkan rather than three.
// =============================================================================

#include "OloEngine/Containers/Array.h"
#include "OloEngine/Core/Base.h"
#include "OloEngine/Groom/GroomBinding.h"
#include "OloEngine/Groom/GroomCoatShadow.h"
#include "OloEngine/Groom/GroomDeformation.h"
#include "OloEngine/Groom/GroomGuideInfluence.h"
#include "OloEngine/Groom/GroomStrandMesh.h"
#include "OloEngine/Groom/GroomWetness.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <span>
#include <vector>

namespace OloEngine
{
    /// One drawn strand's root, this frame and last. Four vec4s; the w lane of
    /// each origin is unused. A strand whose root had no deformed frame this
    /// frame carries its BIND frame here instead, which carries its bind-local
    /// points back to exactly where they rest — the held-at-rest answer
    /// ApplyGroomRootTransform gives, with no flag for the shader to test.
    struct GroomDeformRootRecord
    {
        glm::vec4 Origin{ 0.0f };
        /// Quaternion, (x, y, z, w).
        glm::vec4 Rotation{ 0.0f, 0.0f, 0.0f, 1.0f };
        glm::vec4 PrevOrigin{ 0.0f };
        glm::vec4 PrevRotation{ 0.0f, 0.0f, 0.0f, 1.0f };
    };
    static_assert(sizeof(GroomDeformRootRecord) == 64, "GroomStrandDeform.glsl reads a root as four uvec4s");

    /// One drawn strand's root at the BIND (#1533): where the rest stream's
    /// bind-local points rest, `Origin + Rotation * local`. STATIC -- nothing
    /// that moves changes it -- so it rides in the region uploaded once. A coat
    /// baked at rest is looked up through it: the vertex stage takes each point
    /// back to where it rests, and the turn since the bind, to the fragment.
    struct GroomDeformBindRecord
    {
        glm::vec4 Origin{ 0.0f };
        /// Quaternion, (x, y, z, w).
        glm::vec4 Rotation{ 0.0f, 0.0f, 0.0f, 1.0f };
    };
    static_assert(sizeof(GroomDeformBindRecord) == 32, "GroomStrandDeform.glsl reads a bind frame as two uvec4s");

    /// One guide slot of the influence table, as this frame's budget left it:
    /// where its displacement samples start and how many there are. Count 0
    /// means the slot was not simulated this frame, which is every reason
    /// SampleGroomGuideDisplacement skips a slot, folded into one number.
    struct GroomDeformSlotRecord
    {
        u32 First = 0;
        u32 Count = 0;
        u32 Pad0 = 0;
        u32 Pad1 = 0;
    };
    static_assert(sizeof(GroomDeformSlotRecord) == 16, "GroomStrandDeform.glsl reads a slot as one uvec4");

    /// One guide sample's displacement, this frame and last. `Previous` is a
    /// copy of `Current` when the simulation has no history, which is the rule
    /// SampleGroomGuideDisplacement applies at read time, applied at pack time.
    struct GroomDeformDisplacementRecord
    {
        glm::vec4 Current{ 0.0f };
        glm::vec4 Previous{ 0.0f };
    };
    static_assert(sizeof(GroomDeformDisplacementRecord) == 32, "GroomStrandDeform.glsl reads one as two uvec4s");

    static_assert(sizeof(GroomGuideWeights) == 32, "GroomStrandDeform.glsl reads the weights as two uvec4s");

    /// One drawn strand's root on the bound surface (#1533 E1), for the GPU
    /// evaluation: the rows of its triangle's three corners in the vertex region
    /// and where in the triangle it sits. STATIC. `Corners.w` is 1 when the
    /// binding reached this root, and 0 when it did not -- a root the compute
    /// pass then carries on its bind frame, the held-at-rest answer.
    struct GroomRootSkinRecord
    {
        glm::uvec4 Corners{ 0u };
        glm::vec4 Barycentric{ 0.0f };
    };
    static_assert(sizeof(GroomRootSkinRecord) == 32, "GroomRootFrames.comp reads a skin record as two uvec4s");

    /// One vertex of the bound surface as SkinGroomSurfaceVertex reads it: the
    /// rest position and four influences. STATIC.
    struct GroomSurfaceVertexRecord
    {
        glm::vec4 Position{ 0.0f };
        glm::uvec4 BoneIds{ 0u };
        glm::vec4 Weights{ 0.0f };
    };
    static_assert(sizeof(GroomSurfaceVertexRecord) == 48, "GroomRootFrames.comp reads a vertex as three uvec4s");

    /// Where each region of one coat's buffer starts, in 16-byte units.
    struct GroomDeformBufferLayout
    {
        u32 RootCount = 0;
        u32 SlotCount = 0;
        u32 DisplacementCapacity = 0;

        /// Always RootCount * 2: the shader derives it from the root count, so it
        /// needs no lane of its own (see oloGroomDeformLayout).
        u32 BindBase = 0;
        /// RootCount * 4 and RootCount * 5, derived the same way (#1615).
        u32 ClumpBase = 0;
        u32 WetBase = 0;
        u32 RootBase = 0;
        u32 SlotBase = 0;
        u32 DisplacementBase = 0;
        /// The GPU root evaluation's regions (#1533 E1), all zero-sized when the
        /// roots are packed on the CPU.
        u32 VertexCount = 0;
        u32 BoneCount = 0;
        u32 SkinBase = 0;
        u32 VertexBase = 0;
        u32 PaletteBase = 0;
        /// The GPU guide expansion's regions (#1589), all zero-sized when the
        /// expansion is uploaded dense from the CPU. SourceCapacity is the
        /// simulated points the source region holds.
        u32 SourceCapacity = 0;
        u32 StandInBase = 0;
        u32 GuideOffsetBase = 0;
        u32 SourceBase = 0;
        u32 TotalUnits = 0;

        [[nodiscard]] static GroomDeformBufferLayout Make(u32 rootCount, u32 slotCount,
                                                          u32 displacementCapacity) noexcept;
        /// The layout with the GPU root evaluation's regions, for a bound surface
        /// of `vertexCount` vertices skinned by `boneCount` bones. Either count
        /// zero gives the CPU-packed layout.
        [[nodiscard]] static GroomDeformBufferLayout Make(u32 rootCount, u32 slotCount, u32 displacementCapacity,
                                                          u32 vertexCount, u32 boneCount) noexcept;

        /// This layout with the GPU guide expansion's regions appended (#1589),
        /// for up to `sourceCapacity` simulated points. Unchanged when there are
        /// no slots or no capacity: the expansion is then the CPU's.
        [[nodiscard]] GroomDeformBufferLayout WithGuideExpansion(u32 sourceCapacity) const noexcept;

        /// Whether the root region is written by the compute pass.
        [[nodiscard]] bool RootsOnGpu() const noexcept
        {
            return BoneCount > 0u && VertexCount > 0u;
        }
        /// Whether a coat's stand-ins are expanded by the compute pass into the
        /// displacement region (GroomExpandGuides.comp).
        [[nodiscard]] bool ExpandsGuides() const noexcept
        {
            return SourceCapacity > 0u && SlotCount > 0u;
        }
        [[nodiscard]] u64 PaletteOffsetBytes() const noexcept
        {
            return static_cast<u64>(PaletteBase) * 16u;
        }
        /// The palette's end: this frame's palette and last frame's.
        [[nodiscard]] u64 PaletteEndBytes() const noexcept
        {
            return PaletteOffsetBytes() + static_cast<u64>(BoneCount) * 2u * sizeof(glm::mat4);
        }
        [[nodiscard]] u64 SlotOffsetBytes() const noexcept
        {
            return static_cast<u64>(SlotBase) * 16u;
        }
        /// The slot region's end, which is where the displacement region starts.
        [[nodiscard]] u64 SlotEndBytes() const noexcept
        {
            return static_cast<u64>(DisplacementBase) * 16u;
        }
        /// The stand-in region, sent when the budget's stand-ins change.
        [[nodiscard]] u64 StandInOffsetBytes() const noexcept
        {
            return static_cast<u64>(StandInBase) * 16u;
        }
        [[nodiscard]] u64 StandInEndBytes() const noexcept
        {
            return static_cast<u64>(GuideOffsetBase) * 16u;
        }
        /// The guide offsets and then `sourceCount` simulated points: what a
        /// frame whose guides the GPU expands sends of its guides.
        [[nodiscard]] u64 GuideOffsetBytes() const noexcept
        {
            return static_cast<u64>(GuideOffsetBase) * 16u;
        }
        [[nodiscard]] u64 SourceEndBytes(u32 sourceCount) const noexcept
        {
            return (static_cast<u64>(SourceBase) + static_cast<u64>(std::min(sourceCount, SourceCapacity)) * 2u) * 16u;
        }
        /// The bound surface's vertex region, for re-sending it alone when a
        /// morph moves the body's rest positions; empty on the CPU-packed layout.
        [[nodiscard]] u64 VertexOffsetBytes() const noexcept
        {
            return static_cast<u64>(VertexBase) * 16u;
        }
        [[nodiscard]] u64 VertexEndBytes() const noexcept
        {
            return VertexOffsetBytes() + static_cast<u64>(VertexCount) * sizeof(GroomSurfaceVertexRecord);
        }

        [[nodiscard]] u64 TotalBytes() const noexcept
        {
            return static_cast<u64>(TotalUnits) * 16u;
        }
        /// The per-frame part: everything from the wet record on.
        [[nodiscard]] u64 DynamicOffsetBytes() const noexcept
        {
            return static_cast<u64>(WetBase) * 16u;
        }
        /// The wet record's end, for a layout whose roots the GPU writes: the
        /// record is sent on its own, the root region not at all.
        [[nodiscard]] u64 WetEndBytes() const noexcept
        {
            return static_cast<u64>(RootBase) * 16u;
        }
        [[nodiscard]] u64 DynamicBytes() const noexcept
        {
            return TotalBytes() - DynamicOffsetBytes();
        }

        [[nodiscard]] bool operator==(const GroomDeformBufferLayout&) const = default;
    };

    /// What one frame's pack did. The per-strand counters the CPU build used to
    /// report as a by-product of building, now reported by the thing that
    /// replaced it.
    struct GroomDeformFrameStats
    {
        /// Guide displacements travel this frame; false leaves every strand on
        /// its bound rest shape, which is what an un-simulated coat is.
        bool Simulated = false;
        /// The displacement records the vertex stage reads: the simulated
        /// guides' points, or every slot's when the stand-ins are expanded.
        u32 DisplacementCount = 0;
        u32 StrandsHeldAtRest = 0;
        u32 StrandsSimulated = 0;
        u32 StrandsUnguided = 0;

        /// The stand-ins are expanded by the compute pass this frame (#1589):
        /// the displacement region is the kernel's to write, and what was
        /// packed for it is the source -- SourceCount simulated points of
        /// SourceGuides guides -- and, when StandInsWritten, the slot and
        /// stand-in regions, rewritten for a new budget.
        bool GuidesExpandedOnGpu = false;
        bool StandInsWritten = false;
        bool SourceHasPrevious = false;
        u32 SourceCount = 0;
        u32 SourceGuides = 0;
    };

    /// GroomExpandGuides.comp's parameters for one coat's frame, in the
    /// GroomExpandGuideParams block's order (ShaderBindingLayout.h). Groups is
    /// the dispatch's x; zero means there is nothing to expand.
    struct GroomGuideExpansionDispatch
    {
        /// x = slots, y = simulated guides, z = 1 with a previous frame, w =
        /// simulated points.
        glm::ivec4 Counts{ 0 };
        /// The regions it reads, in 16-byte units: x = stand-ins, y = slot
        /// records, z = guide offsets, w = source records.
        glm::ivec4 Bases{ 0 };
        /// x = the displacement records it writes; yzw unused.
        glm::ivec4 Target{ 0 };
        u32 Groups = 0;
    };

    /// The GUIDE-DISPLACEMENT capacity a coat's buffer needs: every point of
    /// every guide in the table, so no budget the scheduler picks can outgrow it.
    [[nodiscard]] u32 GroomDeformDisplacementCapacity(const GroomAsset& groom,
                                                      const GroomGuideInfluenceTable* influence) noexcept;

    /**
     * @brief One coat's frame buffer, CPU side.
     *
     * The bytes the GPU reads, held here as well because the coat bake and the
     * parity tests read them too — a CPU evaluation of a DIFFERENT copy of the
     * pose would be a second implementation of it.
     */
    class GroomDeformBuffer
    {
      public:
        /// Sizes the buffer and writes its STATIC region: the guide weights of
        /// every root slot, read from `influence` (all-zero when null).
        ///
        /// `cpuOnly` (#1533): the records alone, no byte image. For a reader
        /// on the CPU (EvaluateGroomDeformedPoint reads the records), such as
        /// the ray-traced proxy, which copied every region into an image no
        /// GPU ever received.
        void Reset(const GroomDeformBufferLayout& layout, std::span<const u32> rootCurves,
                   const GroomGuideInfluenceTable* influence, bool cpuOnly = false);

        /**
         * @brief Writes the per-frame region.
         *
         * @param rootCurves the rest stream's slot -> base curve map.
         * @param binding the binding the rest stream was built from.
         * @param rootTransforms this frame's transforms, indexed by base curve
         *        and spanning the whole groom (GroomStrandRequest::RootTransforms).
         * @param simulation the frame's guide displacements, or null. An
         *        unusable one is treated as absent, as BuildGroomStrandMesh does.
         *        One that expands stand-ins (#1589) is drawn from every slot's
         *        expansion: on a layout that ExpandsGuides() with room for its
         *        points, the simulated points are packed for
         *        GroomExpandGuides.comp and the GPU expands them
         *        (GuidesExpandedOnGpu); otherwise the expansion is made here, on
         *        the CPU, and packed dense, as before #1589.
         * @param baseCurveCount the base groom's curve count.
         * @param gpuExpandsGuides false packs the expansion dense even on a
         *        layout with the kernel's regions: the caller will not dispatch
         *        it this frame (OLO_GROOM_GUIDES_EXPANDED_ON_CPU).
         */
        GroomDeformFrameStats PackFrame(std::span<const u32> rootCurves, const GroomBindingAsset& binding,
                                        std::span<const GroomRootTransform> rootTransforms,
                                        const GroomStrandSimulation* simulation, u32 baseCurveCount,
                                        bool gpuExpandsGuides = true);

        /// GroomExpandGuides.comp's parameters for the frame PackFrame packed
        /// (#1589); Groups is zero unless that frame's GuidesExpandedOnGpu.
        [[nodiscard]] GroomGuideExpansionDispatch GuideExpansionDispatch() const noexcept;

        /**
         * @brief The dense simulation this frame's pack is drawn from, for a CPU reader (#1589).
         *
         * And the CPU mirror's displacement records resolved to it, so
         * EvaluateGroomDeformedPoint and the functions built on it read what the
         * GPU draws. `simulation` must be the one PackFrame was handed this
         * frame. When the GPU expands the guides, the CPU never needs the
         * expansion unless a reader asks, so it is made here, once per pack;
         * when PackFrame expanded them on the CPU, it is that expansion. Without
         * stand-ins it is `simulation` itself. Valid until the next PackFrame.
         */
        [[nodiscard]] GroomStrandSimulation ResolveCpuGuides(const GroomStrandSimulation& simulation);

        /**
         * @brief Writes the GPU root evaluation's STATIC regions (#1533 E1).
         *
         * Each root slot's triangle corners and barycentric, from `binding`, and
         * every vertex of the bound surface with its influences. A root the
         * binding does not reach, or whose triangle is out of the surface's
         * range, is written unreached (Corners.w = 0). Call after Reset with a
         * layout whose RootsOnGpu() is true; a no-op otherwise.
         *
         * @return the root slots it reached: the ones the kernel evaluates. The
         *         rest are held at rest on every frame of this layout.
         */
        u32 WriteSurfaceSkin(std::span<const u32> rootCurves, const GroomBindingAsset& binding,
                             const GroomSurfaceView& surface, const GroomSkinningView& skinning,
                             u32 baseCurveCount);

        /// The vertex region alone, from `surface`: for a body whose morph moved
        /// its rest positions under a live layout (a blink, an expression). Each
        /// root's triangle is unchanged, so nothing per root is redone.
        void WriteSurfaceVertices(const GroomSurfaceView& surface, const GroomSkinningView& skinning);

        /// Each base curve's clump growth in groom object space at rest
        /// (ComputeGroomClumpGrowth, indexed by curve), handed out to the drawn
        /// strands `rootCurves` names, in root-slot order (#1615). Taken into
        /// each root's bind frame and written to the static region with the bind
        /// frames; after the bind frames went up, written into the region here,
        /// and the caller sends [ClumpBase, DynamicOffsetBytes). Empty leaves
        /// every record zero: no clump to gather toward.
        void SetClumpGrowth(std::span<const glm::vec3> curveGrowth, std::span<const u32> rootCurves);

        /// This frame's wetness (#1615), into its record.
        void SetWetness(const GroomDeformWetRecord& wet);

        /// Writes this frame's palette and last frame's into the palette region;
        /// `previous` empty or short writes the current palette in its place.
        void WritePalette(std::span<const glm::mat4> current, std::span<const glm::mat4> previous);

        /// The CPU twin's root records on the GPU layout (#1533 E1). The region
        /// the kernel writes is never read back, so a CPU reader of the drawn
        /// pose -- the coat bake -- packs the same records from CPU transforms
        /// here first. The byte image is untouched. Returns the roots held at
        /// rest.
        u32 PackCpuRoots(std::span<const u32> rootCurves, const GroomBindingAsset& binding,
                         std::span<const GroomRootTransform> rootTransforms, u32 baseCurveCount);

        [[nodiscard]] const GroomDeformBufferLayout& GetLayout() const noexcept
        {
            return m_Layout;
        }
        [[nodiscard]] const GroomDeformFrameStats& GetFrameStats() const noexcept
        {
            return m_Frame;
        }
        /// The whole buffer, static region included.
        [[nodiscard]] std::span<const u8> GetBytes() const noexcept
        {
            return { m_Bytes.GetData(), static_cast<sizet>(m_Bytes.Num()) };
        }
        /// CPU bytes this mirror holds by allocated CAPACITY -- the typed records,
        /// the byte image and the count cache -- for the memory report (#1342)
        /// and the pass's memory breakdown (#1533).
        [[nodiscard]] u64 GetCpuBytes() const noexcept
        {
            return static_cast<u64>(m_Weights.GetAllocatedSize() + m_BindFrames.GetAllocatedSize() +
                                    m_Clump.GetAllocatedSize() +
                                    m_Roots.GetAllocatedSize() + m_Slots.GetAllocatedSize() +
                                    m_Displacements.GetAllocatedSize() + m_Bytes.GetAllocatedSize() +
                                    m_Source.GetAllocatedSize() + m_GuideOffsets.GetAllocatedSize() +
                                    m_IdentitySlots.GetAllocatedSize()) +
                   static_cast<u64>(m_CountedGuideOfSlot.capacity()) * sizeof(u32) + m_Expansion.GetCpuBytes();
        }
        /// The per-frame region only — what a frame uploads.
        [[nodiscard]] std::span<const u8> GetDynamicBytes() const noexcept
        {
            return GetBytes().subspan(static_cast<sizet>(m_Layout.DynamicOffsetBytes()));
        }

        [[nodiscard]] std::span<const GroomGuideWeights> Weights() const noexcept;
        [[nodiscard]] std::span<const GroomDeformBindRecord> BindFrames() const noexcept;
        /// The clump records, bind-local (w unused).
        [[nodiscard]] std::span<const glm::vec4> ClumpGrowth() const noexcept;
        [[nodiscard]] const GroomDeformWetRecord& Wet() const noexcept
        {
            return m_Wet;
        }
        /// The CPU-packed root records. Identity frames when the layout's
        /// RootsOnGpu(): the GPU writes that region, and nothing reads it back.
        [[nodiscard]] std::span<const GroomDeformRootRecord> Roots() const noexcept;
        [[nodiscard]] std::span<const GroomDeformSlotRecord> Slots() const noexcept;
        [[nodiscard]] std::span<const GroomDeformDisplacementRecord> Displacements() const noexcept;

      private:
        /// Each root slot's record from `rootTransforms` into m_Roots, the bind
        /// frame where a root has no deformed one. Returns the roots held.
        u32 PackRootRecords(std::span<const u32> rootCurves, const GroomBindingAsset& binding,
                            std::span<const GroomRootTransform> rootTransforms, u32 baseCurveCount);

        /// A dense simulation's displacement records into m_Displacements, and
        /// into the byte image unless `cpuMirrorOnly`. Returns the count.
        u32 PackDisplacementRecords(const GroomGuideDisplacements& guides, bool cpuMirrorOnly);

        /// The frame's simulated points, guide offsets and -- for a new set --
        /// the dense slot windows and the stand-ins, for GroomExpandGuides.comp.
        void PackGuideSource(const GroomStrandSimulation& simulation);

        /// The strand counters, recounted only when the slots a guide moves
        /// are not the ones last counted. `guideOfSlot` is the frame's slot
        /// map; every slot counts as moved when the stand-ins are expanded.
        void CountStrands(std::span<const u32> rootCurves, const GroomStrandSimulation& simulation,
                          std::span<const u32> guideOfSlot);

        GroomDeformBufferLayout m_Layout;
        GroomDeformFrameStats m_Frame;
        // The records as TYPED arrays, which is what the CPU twin reads, and
        // the byte image the GPU is sent, assembled from them by memcpy. Two
        // copies rather than typed views over one byte buffer: a record read
        // through a pointer to a type that was never constructed there is
        // undefined behaviour a strict-aliasing compiler (the Linux CI's) is
        // entitled to act on, and the image costs one memcpy of the frame's
        // few megabytes.
        TArray<GroomGuideWeights> m_Weights;
        TArray<GroomDeformBindRecord> m_BindFrames;
        // The bind frames need the binding, which Reset is not handed; the first
        // PackFrame after it writes them, into the region the relayout upload
        // sends whole.
        bool m_BindFramesWritten = false;
        // The clump growth as it was handed over, in object space, taken into
        // the bind frames when those are written (#1615).
        std::vector<glm::vec3> m_ClumpObject;
        TArray<glm::vec4> m_Clump;
        GroomDeformWetRecord m_Wet;
        bool m_CpuOnly = false;
        bool m_HasLayout = false;
        TArray<GroomDeformRootRecord> m_Roots;
        TArray<GroomDeformSlotRecord> m_Slots;
        TArray<GroomDeformDisplacementRecord> m_Displacements;
        TArray64<u8> m_Bytes;

        // THE STRAND COUNTS, cached against what they were counted from (#1533
        // E1). Counting is a lookup per drawn strand -- a quarter of a million a
        // frame on the showcase dog -- for a number that moves only when the
        // simulation budget changes which guides it moved. So PackFrame counts
        // when the table or the slot-to-guide map differs from the last count,
        // and Reset (a new root set) forgets it.
        std::vector<u32> m_CountedGuideOfSlot;
        const GroomGuideInfluenceTable* m_CountedTable = nullptr;
        u32 m_CountedSimulated = 0;
        u32 m_CountedUnguided = 0;
        bool m_CountsValid = false;

        // THE GUIDE EXPANSION (#1589). The source the kernel expands -- the
        // simulated points, this frame and last, and the guides' offsets padded
        // to whole units -- and the serial of the stand-in set whose slot
        // windows and stand-ins the image holds, so a set is written (and sent)
        // once, not every frame. 0 is no set: serials start at 1.
        TArray<GroomDeformDisplacementRecord> m_Source;
        TArray<u32> m_GuideOffsets;
        u64 m_PackedStandInSerial = 0;
        // The CPU's expansion of the frame's stand-ins, made by PackFrame when
        // it packs dense, or by ResolveCpuGuides for a CPU reader when the GPU
        // expands. Its view is m_Expanded's, valid while m_ExpandedValid.
        GroomGuideExpansion m_Expansion;
        GroomStrandSimulation m_Expanded;
        bool m_ExpandedValid = false;
        // False while the GPU expands the frame's guides and no CPU reader has
        // asked for them: the CPU mirror's displacement records are stale.
        bool m_CpuGuidesResolved = true;
        // 0..slots-1: the slot map of an expanded frame, for the counters.
        TArray<u32> m_IdentitySlots;
    };

    /**
     * @brief The CPU twin of GroomStrandDeform.glsl's oloGroomDeformPoint.
     *
     * One point of one strand, deformed and displaced, read from the packed
     * buffer. The arithmetic is ApplyGroomRootTransform's and
     * SampleGroomGuideDisplacement's, term for term: for a strand whose root was
     * deformed this frame it reproduces BuildGroomStrandMesh's point exactly.
     */
    [[nodiscard]] glm::vec3 EvaluateGroomDeformedPoint(const GroomDeformBuffer& buffer, u32 rootSlot,
                                                       const glm::vec3& local, f32 t, bool previous) noexcept;

    /// A rest stream (BuildGroomStrandRestMesh's root-local corners) deformed and
    /// displaced through `buffer`, as GroomStrand.glsl's vertex stage deforms it:
    /// each corner's Position, Other and PrevPosition, every other lane copied.
    /// For a strand whose root deformed this frame, Position and Other are
    /// BuildGroomStrandMesh's bit for bit. The ray-traced proxy refits from this
    /// each frame (#1533): rebuilding a coat from its groom walks every curve
    /// several times, ~90 ms a frame on the showcase dog, where this touches the
    /// drawn corners only. `outVertices` is resized to `rest`.
    void DeformGroomRestStream(const GroomDeformBuffer& buffer, std::span<const GroomStrandVertex> rest,
                               std::vector<GroomStrandVertex>& outVertices);

    /// The drawn centrelines of a GPU-deformed coat, for the coat bake (#1426):
    /// one segment per entry of `pose`, in stream order, with the radii the
    /// stream holds (no per-groom width scale — GroomCoatShadow applies that).
    /// `outSegments` is cleared first.
    void EvaluateGroomDeformedPose(const GroomDeformBuffer& buffer, std::span<const GroomRestPoseSegment> pose,
                                   std::vector<GroomCoatShadow::CoatSegment>& outSegments);

    /**
     * @brief Where a GPU-rooted coat's roots can be, without evaluating one (#1533 E1).
     *
     * When GroomRootFrames.comp evaluates the drawn roots, the CPU holds no
     * posed root to bound the coat by -- and a coat with no box casts no shadow
     * (the shadow pass culls casters by it). So the box is bounded from the
     * skeleton instead: each bone's box of the rest-pose surface corners the
     * drawn roots sit on, posed by that bone's matrix. Linear-blend skinning
     * puts a vertex inside the convex hull of its bones' images of it, and a
     * root is a barycentric blend of three such vertices, so the union of the
     * posed boxes contains every root the kernel writes -- a conservative box
     * for a cull, at a few dozen matrix transforms a frame.
     *
     * Corners no valid bone moves stay at rest (the kernel's answer), and a root
     * the kernel holds at rest sits at its bind origin: both are bounded too.
     * A triangle that collapses in one POSE only is held at its bind origin by
     * the kernel and is not covered -- it is a broken rig, counted as held.
     */
    struct GroomRootBoneBounds
    {
        /// Per bone, the rest-space box of the corners it moves; empty where
        /// Min > Max.
        std::vector<glm::vec3> Min;
        std::vector<glm::vec3> Max;
        /// Corners no valid bone moves, in surface space.
        glm::vec3 RestMin{ 0.0f };
        glm::vec3 RestMax{ 0.0f };
        bool HasRest = false;
        /// Bind origins of roots the kernel holds at rest, in groom space.
        glm::vec3 HeldMin{ 0.0f };
        glm::vec3 HeldMax{ 0.0f };
        bool HasHeld = false;
    };

    /// Built from the same inputs, and under the same reach rules, as
    /// GroomDeformBuffer::WriteSurfaceSkin writes the kernel's surface from.
    [[nodiscard]] GroomRootBoneBounds BuildGroomRootBoneBounds(std::span<const u32> rootCurves,
                                                               const GroomBindingAsset& binding,
                                                               const GroomSurfaceView& surface,
                                                               const GroomSkinningView& skinning, u32 boneCount,
                                                               u32 baseCurveCount);

    /// The box every drawn root's origin lies in for this palette, in groom
    /// space. False when nothing was bounded (no drawn root).
    [[nodiscard]] bool PoseGroomRootBoneBounds(const GroomRootBoneBounds& bounds, std::span<const glm::mat4> palette,
                                               const glm::mat4& surfaceToGroom, glm::vec3& outMin,
                                               glm::vec3& outMax) noexcept;
} // namespace OloEngine
