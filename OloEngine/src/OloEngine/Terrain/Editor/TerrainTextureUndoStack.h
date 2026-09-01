#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/Texture.h"

#include <vector>

namespace OloEngine
{
    // Bounded ring of GPU texture region snapshots, restored by blit (issue #716).
    //
    // This is the piece that normally blocks moving terrain authoring onto the
    // GPU: with the heightmap and splatmaps GPU-resident there is no CPU array to
    // copy for undo, and snapshotting the whole texture per stroke is neither
    // affordable nor necessary. A stroke touches a rect, so a snapshot is a rect.
    //
    // BOUNDED IN TWO DIMENSIONS on purpose — an entry count and a byte budget.
    // Neither alone is enough: a hundred 8x8 dabs and three 4096x4096 full-map
    // strokes are the same VRAM problem from opposite ends. Evicting the oldest
    // is what keeps a long editing session from being an unbounded VRAM leak, and
    // it is why every read is fallible: a command whose snapshot has aged out
    // reports failure rather than restoring garbage.
    //
    // On the handle-ownership trap in
    // docs/agent-rules/shared-atlas-allocator.md — a non-RAII GPU handle embedded
    // in a POD-looking struct that vector::resize() silently leaks — every entry
    // holds a Ref<Texture2D>, not a raw RHI handle, so eviction, reallocation and
    // stack teardown all release through the same refcount. Do not "optimise" that
    // into a raw handle plus a manual free.
    class TerrainTextureUndoStack : public RefCounted
    {
      public:
        using SnapshotId = u64;
        static constexpr SnapshotId kInvalidSnapshot = 0;

        // Defaults sized for an interactive editing session rather than a bake:
        // 64 entries is roughly 32 undo steps across two textures, and 192 MiB is
        // ~12 full-map 1025x1025 R32F snapshots or ~700 typical brush rects.
        explicit TerrainTextureUndoStack(u32 maxEntries = 64, sizet maxBytes = 192ull * 1024 * 1024);

        // Copy `source`'s (x, y, w, h) rect into a freshly allocated snapshot
        // texture. GPU-to-GPU: no readback. Returns kInvalidSnapshot if the region
        // is empty, the snapshot texture could not be created, OR the new entry did
        // not survive its own budget check — a single capture larger than the whole
        // byte budget is evicted immediately, and handing back an id that names
        // nothing would let a caller build an undo command that silently does
        // nothing on the first press.
        SnapshotId Capture(const Ref<Texture2D>& source, u32 x, u32 y, u32 w, u32 h);

        // Blit a snapshot back over the rect it was taken from. False if the id is
        // unknown (evicted, released, or never valid) or `dest` is null — the
        // caller is expected to treat that as "this undo step is no longer
        // available", not as a reason to write something else.
        // dest BY VALUE: Ref<T> propagates constness, so a const Ref& would hand out
        // a const Texture2D and the RegenerateMips() this performs could not be
        // called on it. Third occurrence of that trap in this change — see
        // TerrainEditorPanel::SettleErosionEdit and OnVoxelUpdate.
        bool Restore(SnapshotId id, Ref<Texture2D> dest) const;

        // Full-image working copy, held by a caller for the life of one stroke.
        //
        // A stroke does not know its final rect until the mouse comes up, but its
        // BEFORE state only exists at the moment the mouse goes down. Capturing
        // the whole texture into the ring at press would make every stroke cost a
        // full-map entry; instead the caller keeps one reusable full copy here and
        // takes both rect-sized snapshots at settle — before from this copy, after
        // from the live texture. `slot` is reallocated only when the source size
        // or format changes, so a drag session allocates once.
        //
        // Static because it touches no ring state: it is the same GPU-to-GPU copy,
        // just to a caller-owned destination rather than a budgeted entry.
        static bool EnsureFullCopy(Ref<Texture2D>& slot, const Ref<Texture2D>& source);

        // Drop a snapshot the owner no longer needs (an undo command being
        // destroyed, a redo branch being discarded).
        void Release(SnapshotId id);

        [[nodiscard]] u32 GetEntryCount() const
        {
            return static_cast<u32>(m_Entries.size());
        }
        [[nodiscard]] sizet GetBytesUsed() const
        {
            return m_BytesUsed;
        }
        [[nodiscard]] u32 GetMaxEntries() const
        {
            return m_MaxEntries;
        }
        [[nodiscard]] sizet GetMaxBytes() const
        {
            return m_MaxBytes;
        }
        // Snapshots dropped to stay inside the budget since construction. Exposed
        // so a test can prove eviction actually happened rather than inferring it
        // from a count that a leak would also produce.
        [[nodiscard]] u32 GetEvictionCount() const
        {
            return m_EvictionCount;
        }

        [[nodiscard]] bool Contains(SnapshotId id) const;

      private:
        struct Entry
        {
            SnapshotId Id = kInvalidSnapshot;
            Ref<Texture2D> Snapshot; // RAII — see the class comment
            u32 X = 0;
            u32 Y = 0;
            u32 Width = 0;
            u32 Height = 0;
            sizet Bytes = 0;
        };

        void EvictUntilWithinBudget();

        std::vector<Entry> m_Entries; // oldest first
        SnapshotId m_NextId = 1;      // 0 is reserved for kInvalidSnapshot
        u32 m_MaxEntries;
        sizet m_MaxBytes;
        sizet m_BytesUsed = 0;
        u32 m_EvictionCount = 0;
    };
} // namespace OloEngine
