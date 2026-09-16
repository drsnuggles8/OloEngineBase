#pragma once

// =============================================================================
// FoliageLeafProfileTable.h — FoliageLeafProfile -> the three-bit slot the
// G-Buffer names it by. Issue #1234, modelled on SkinProfileTable (#1231).
//
// Read FoliageLeafProfile.h first for WHY the slot exists. This file is the one
// place that decides which profile gets which slot, so the G-Buffer writer, the
// deferred lighting pass and the debug views cannot disagree about what "slot
// 2" means.
//
// THE ASSIGNMENT IS REBUILT EVERY FRAME, AND THIS IS WHERE IT DIFFERS FROM
// SkinProfileTable. Skin keys on an ASSET HANDLE, which is a stable identity, so
// its slots can be sticky. A leaf profile has no handle — it is interned by
// VALUE, because the material is authored inline on the layer — and a value is
// not a stable identity while somebody is dragging a slider. Sticky interning by
// value would mint a slot for every intermediate value the drag passes through;
// seven drags and the table is full for the rest of the session, after which the
// DEFERRED path silently loses transmission while forward keeps it. That is
// precisely the forward/deferred divergence this material exists to avoid,
// reached by ordinary authoring.
//
// So BeginFrame() clears the assignment and the frame's submissions refill it.
// That is MORE temporally stable than stickiness, not less: slots are handed out
// in submission order, submission order is layer order, so an unchanged scene
// gets the same slot for the same layer every frame — and editing a value
// changes what slot N CONTAINS rather than which slot a layer uses.
//
// THE TABLE IS SMALL AND SAYS SO WHEN IT IS FULL. An eighth distinct leaf
// material in ONE FRAME gets kFoliageLeafSlotNone and one logged line, and the
// layer then renders with MaterialKind::Generic on the DEFERRED path only — no
// transmission rather than somebody else's transmission. The forward paths carry
// the parameters per draw in the foliage UBO and need no slot, so they keep
// shading it correctly; that asymmetry is stated here because it is exactly the
// kind of thing that otherwise shows up as "deferred looks different" with no
// explanation.
//
// Unlike SkinProfileTable there is no asset behind a profile, so there is no
// negative cache, no ForgetFailedHandle and no fallback taxonomy — the only way
// to fail is to need more than seven DISTINCT leaf materials in one frame.
// =============================================================================

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/FoliageLeafProfile.h"

#include <array>
#include <mutex>

namespace OloEngine
{
    class FoliageLeafProfileTable
    {
      public:
        FoliageLeafProfileTable() = default;

        // Drop the frame's assignment. Called once per frame from
        // Renderer3D::BeginScene, BEFORE any foliage submits — see the header
        // for why a value-interned table cannot be sticky the way skin's is.
        void BeginFrame();

        // Intern `profile` into this frame's assignment and return its slot, or
        // kFoliageLeafSlotNone when the profile is not a leaf material
        // (strength 0) or the frame already holds seven distinct materials.
        //
        // Thread-safe: foliage submission is not guaranteed single-threaded, and
        // two threads interning the same new profile must agree on its slot.
        [[nodiscard]] u32 Resolve(const FoliageLeafProfile& profile);

        // The parameters behind a slot, for the pass that only carries the slot.
        // An unassigned or out-of-range slot yields a profile with strength 0,
        // which shades as no transmission rather than as garbage.
        [[nodiscard]] FoliageLeafProfile GetProfileForSlot(u32 slot) const;

        [[nodiscard]] u32 GetAssignedSlotCount() const;

        // How many resolves were refused for want of a slot since the last
        // Reset(). Counted rather than only logged so a test can assert on it.
        // NOT cleared by BeginFrame: a budget that is blown every frame should
        // read as a large number, not as one.
        [[nodiscard]] u64 GetSlotBudgetFullCount() const;

        // Drop every assignment AND the counters. Called on scene load and
        // renderer shutdown; BeginFrame is the per-frame sibling that clears
        // only the assignment.
        void Reset();

      private:
        mutable std::mutex m_Mutex;
        std::array<FoliageLeafProfile, kMaxFoliageLeafSlots> m_Profiles{};
        u32 m_NextSlot = 0;
        u64 m_SlotBudgetFullCount = 0;
        bool m_LoggedSlotBudgetFull = false;
    };

} // namespace OloEngine
