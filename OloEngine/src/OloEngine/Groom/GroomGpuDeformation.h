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
//   [RootBase, +4R)         GroomDeformRootRecord per strand          } per
//   [SlotBase, +S)          GroomDeformSlotRecord per guide slot      } frame
//   [DisplacementBase, +2D) GroomDeformDisplacementRecord per point   }
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

#include <glm/glm.hpp>

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

    /// Where each region of one coat's buffer starts, in 16-byte units.
    struct GroomDeformBufferLayout
    {
        u32 RootCount = 0;
        u32 SlotCount = 0;
        u32 DisplacementCapacity = 0;

        u32 RootBase = 0;
        u32 SlotBase = 0;
        u32 DisplacementBase = 0;
        u32 TotalUnits = 0;

        [[nodiscard]] static GroomDeformBufferLayout Make(u32 rootCount, u32 slotCount,
                                                          u32 displacementCapacity) noexcept;

        [[nodiscard]] u64 TotalBytes() const noexcept
        {
            return static_cast<u64>(TotalUnits) * 16u;
        }
        /// The per-frame part: everything from the first root record on.
        [[nodiscard]] u64 DynamicOffsetBytes() const noexcept
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
        u32 DisplacementCount = 0;
        u32 StrandsHeldAtRest = 0;
        u32 StrandsSimulated = 0;
        u32 StrandsUnguided = 0;
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
        void Reset(const GroomDeformBufferLayout& layout, std::span<const u32> rootCurves,
                   const GroomGuideInfluenceTable* influence);

        /**
         * @brief Writes the per-frame region.
         *
         * @param rootCurves the rest stream's slot -> base curve map.
         * @param binding the binding the rest stream was built from.
         * @param rootTransforms this frame's transforms, indexed by base curve
         *        and spanning the whole groom (GroomStrandRequest::RootTransforms).
         * @param simulation the frame's guide displacements, or null. An
         *        unusable one is treated as absent, as BuildGroomStrandMesh does.
         * @param baseCurveCount the base groom's curve count.
         */
        GroomDeformFrameStats PackFrame(std::span<const u32> rootCurves, const GroomBindingAsset& binding,
                                        std::span<const GroomRootTransform> rootTransforms,
                                        const GroomStrandSimulation* simulation, u32 baseCurveCount);

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
        /// The per-frame region only — what a frame uploads.
        [[nodiscard]] std::span<const u8> GetDynamicBytes() const noexcept
        {
            return GetBytes().subspan(static_cast<sizet>(m_Layout.DynamicOffsetBytes()));
        }

        [[nodiscard]] std::span<const GroomGuideWeights> Weights() const noexcept;
        [[nodiscard]] std::span<const GroomDeformRootRecord> Roots() const noexcept;
        [[nodiscard]] std::span<const GroomDeformSlotRecord> Slots() const noexcept;
        [[nodiscard]] std::span<const GroomDeformDisplacementRecord> Displacements() const noexcept;

      private:
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
        TArray<GroomDeformRootRecord> m_Roots;
        TArray<GroomDeformSlotRecord> m_Slots;
        TArray<GroomDeformDisplacementRecord> m_Displacements;
        TArray64<u8> m_Bytes;
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

    /// The drawn centrelines of a GPU-deformed coat, for the coat bake (#1426):
    /// one segment per entry of `pose`, in stream order, with the radii the
    /// stream holds (no per-groom width scale — GroomCoatShadow applies that).
    /// `outSegments` is cleared first.
    void EvaluateGroomDeformedPose(const GroomDeformBuffer& buffer, std::span<const GroomRestPoseSegment> pose,
                                   std::vector<GroomCoatShadow::CoatSegment>& outSegments);
} // namespace OloEngine
