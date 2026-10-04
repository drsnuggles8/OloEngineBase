#pragma once

// =============================================================================
// GroomEvaluationScratch.h -- the retained CPU working storage of the per-frame
// groom evaluations, owned and counted (#1533 review).
//
// The root evaluation's skinned surface (EvaluateGroomRootTransforms) and the
// caster pose's frames and sums (PoseGroomCasterRunsBy*) are sized by the
// largest body and coat evaluated, and kept across frames so a steady frame
// allocates nothing. Held in function-local thread_locals, that storage
// outlived every groom and pass that used it, until its thread exited, and no
// memory report saw it. So instead:
//
//   * IT IS OWNED. The caller holds the scratch and passes it in: the groom
//     pass one of each for the frames it evaluates, the scene one for the
//     roots it evaluates, the RT proxy cache one for its proxies' roots. Each
//     owner gives its scratch back by its own rule (Release), and with itself.
//   * IT IS COUNTED ONCE. Every instance publishes its retained capacity to one
//     process-wide ledger after each use and on release
//     (GroomEvaluationScratchRetainedBytes). The memory report reads the
//     ledger, and no owner adds a scratch to its own rows: a scratch shared by
//     every groom an owner evaluates counts once, and none is missed.
//   * ONE CALL AT A TIME. An instance serves one evaluation at a time; the
//     evaluation's workers read the calling thread's instance.
// =============================================================================

#include "OloEngine/Core/Base.h"

#include <glm/glm.hpp>

#include <vector>

namespace OloEngine
{
    /// The capacity every live scratch instance retains, in bytes, as each last
    /// published it.
    [[nodiscard]] u64 GroomEvaluationScratchRetainedBytes() noexcept;

    /// How many live instances retain any capacity.
    [[nodiscard]] u32 GroomEvaluationScratchInstancesRetaining() noexcept;

    /// One instance's entry in the ledger. Not copyable: a copy would either
    /// count the same storage twice or count none of its own.
    class GroomEvaluationScratchLedgerEntry
    {
      public:
        GroomEvaluationScratchLedgerEntry() = default;
        ~GroomEvaluationScratchLedgerEntry();
        GroomEvaluationScratchLedgerEntry(const GroomEvaluationScratchLedgerEntry&) = delete;
        GroomEvaluationScratchLedgerEntry& operator=(const GroomEvaluationScratchLedgerEntry&) = delete;
        GroomEvaluationScratchLedgerEntry(GroomEvaluationScratchLedgerEntry&&) = delete;
        GroomEvaluationScratchLedgerEntry& operator=(GroomEvaluationScratchLedgerEntry&&) = delete;

        /// What this instance retains, as it last published.
        [[nodiscard]] u64 RetainedBytes() const noexcept
        {
            return m_Published;
        }
        /// How many uses grew the retained capacity. A steady frame -- the same
        /// coat on the same body as the frame before -- adds none.
        [[nodiscard]] u64 Growths() const noexcept
        {
            return m_Growths;
        }

      protected:
        /// Replaces this instance's figure in the ledger with `bytes`.
        void Publish(u64 bytes) noexcept;

      private:
        u64 m_Published = 0;
        u64 m_Growths = 0;
    };

    /// The root evaluation's surface, every vertex skinned once for this frame
    /// and the last (EvaluateGroomRootTransforms).
    struct GroomSurfaceSkinScratch : GroomEvaluationScratchLedgerEntry
    {
        std::vector<glm::vec3> Current;
        std::vector<glm::vec3> Previous;
        std::vector<u8> Weighted;

        ~GroomSurfaceSkinScratch();
        /// Publishes the capacity it holds now. The evaluation calls it.
        void Account() noexcept;
        /// Gives every byte back.
        void Release() noexcept;
    };
} // namespace OloEngine
