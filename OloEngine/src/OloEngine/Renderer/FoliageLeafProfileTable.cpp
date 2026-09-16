#include "OloEnginePCH.h"
#include "OloEngine/Renderer/FoliageLeafProfileTable.h"

namespace OloEngine
{
    void FoliageLeafProfileTable::BeginFrame()
    {
        std::scoped_lock lock(m_Mutex);
        // The ASSIGNMENT only. The counters and the one-shot log flag survive,
        // so a scene that blows the budget every frame reports a large number
        // once rather than reporting one, repeatedly, forever.
        m_NextSlot = 0;
    }

    u32 FoliageLeafProfileTable::Resolve(const FoliageLeafProfile& profile)
    {
        // Not a leaf material. Deliberately NOT an error and deliberately not
        // counted: strength 0 is the documented off switch every layer that
        // predates #1234 deserializes to, so the overwhelmingly common case
        // must be free and silent.
        if (!profile.IsLeaf())
            return kFoliageLeafSlotNone;

        std::scoped_lock lock(m_Mutex);

        for (u32 slot = 0; slot < m_NextSlot; ++slot)
        {
            if (m_Profiles[slot] == profile)
                return slot;
        }

        if (m_NextSlot >= kMaxFoliageLeafSlots)
        {
            ++m_SlotBudgetFullCount;
            if (!m_LoggedSlotBudgetFull)
            {
                m_LoggedSlotBudgetFull = true;
                OLO_CORE_ERROR("FoliageLeafProfileTable - more than {} DISTINCT leaf materials in one frame. The "
                               "deferred path cannot name this layer's transmission per pixel, so it shades as "
                               "MaterialKind::Generic (no transmission) there; the forward paths carry the parameters "
                               "per draw and are unaffected. Merge layers that share a leaf material, or widen the "
                               "G-Buffer slot field.",
                               kMaxFoliageLeafSlots);
            }
            return kFoliageLeafSlotNone;
        }

        const u32 slot = m_NextSlot++;
        m_Profiles[slot] = profile;
        return slot;
    }

    FoliageLeafProfile FoliageLeafProfileTable::GetProfileForSlot(u32 slot) const
    {
        std::scoped_lock lock(m_Mutex);
        if (slot >= kMaxFoliageLeafSlots || slot >= m_NextSlot)
        {
            // Strength 0 — shades as no transmission. A slot the G-Buffer names
            // but the table has since forgotten (a scene switch mid-frame) must
            // lose the effect, not acquire someone else's.
            return FoliageLeafProfile{};
        }
        return m_Profiles[slot];
    }

    u32 FoliageLeafProfileTable::GetAssignedSlotCount() const
    {
        std::scoped_lock lock(m_Mutex);
        return m_NextSlot;
    }

    u64 FoliageLeafProfileTable::GetSlotBudgetFullCount() const
    {
        std::scoped_lock lock(m_Mutex);
        return m_SlotBudgetFullCount;
    }

    void FoliageLeafProfileTable::Reset()
    {
        std::scoped_lock lock(m_Mutex);
        m_Profiles.fill(FoliageLeafProfile{});
        m_NextSlot = 0;
        m_SlotBudgetFullCount = 0;
        m_LoggedSlotBudgetFull = false;
    }

} // namespace OloEngine
