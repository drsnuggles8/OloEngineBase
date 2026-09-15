#include "OloEnginePCH.h"
#include "OloEngine/Renderer/SkinProfileTable.h"

#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Project/Project.h"

namespace OloEngine
{
    SkinProfileResolution SkinProfileTable::Resolve(AssetHandle handle)
    {
        SkinProfileResolution result;
        const u64 key = static_cast<u64>(handle);

        // A skin material with no profile assigned. Deliberately reported and
        // counted: this is what a merge that dropped the handle looks like.
        if (key == 0)
        {
            std::scoped_lock lock(m_Mutex);
            result.Reason = SkinProfileFallbackReason::NoHandle;
            ++m_FallbackCounts[static_cast<sizet>(result.Reason)];
            if (m_LoggedHandles.insert(key).second)
                OLO_CORE_WARN("SkinProfileTable - a MaterialKind::Skin material has no SkinProfile assigned; shading with the default profile.");
            result.Parameters = SkinProfile::DefaultParameters();
            return result;
        }

        // Deliberately NOT an early return on a slot hit: the slot is sticky but
        // the parameters are re-read every time, so an edited profile reaches the
        // next frame (see the header). Only a handle already known to be
        // unusable short-circuits, and only to keep it off the file system.
        {
            std::scoped_lock lock(m_Mutex);
            if (auto it = m_FailedHandles.find(key); it != m_FailedHandles.end())
            {
                // The ORIGINAL reason, not a generic one — see the member's
                // comment for why re-reporting it as AssetMissing would be worse
                // than not caching at all.
                result.Reason = it->second;
                ++m_FallbackCounts[static_cast<sizet>(result.Reason)];
                result.Parameters = SkinProfile::DefaultParameters();
                return result;
            }
        }

        // Asset resolution happens OUTSIDE the lock: AssetManager::GetAsset can
        // re-enter the importer, whose own registry mutex is not recursive, and
        // holding a renderer lock across that is how the editor deadlocks on the
        // first frame that draws the asset (the trap TilesetSerializer documents).
        SkinProfileParameters parameters;
        SkinProfileFallbackReason reason = SkinProfileFallbackReason::None;
        if (!Project::GetAssetManager())
        {
            // No project mounted — a headless test, a cooked runtime before its
            // pack is open, a tool. Reported as AssetMissing rather than
            // asserted: AssetManager's accessors assert on a null manager, and a
            // renderer path is not the right place to take a process down.
            reason = SkinProfileFallbackReason::AssetMissing;
        }
        else
        {
            // The REGISTERED type, consulted first so a handle pointing at a
            // texture is reported as such instead of as a missing profile. It
            // reads None for a memory-only asset (those are not in the registry),
            // which is why this only rejects a type that is present and wrong.
            const AssetType registered = AssetManager::GetAssetType(handle);
            if (registered != AssetType::None && registered != AssetType::SkinProfile)
            {
                reason = SkinProfileFallbackReason::WrongAssetType;
            }
            else if (Ref<SkinProfile> profile = AssetManager::GetAsset<SkinProfile>(handle))
            {
                parameters = profile->GetParameters();
            }
            else
            {
                reason = SkinProfileFallbackReason::AssetMissing;
            }
        }

        std::scoped_lock lock(m_Mutex);

        if (reason != SkinProfileFallbackReason::None)
        {
            ++m_FallbackCounts[static_cast<sizet>(reason)];
            // Remembered WITH its reason so the next draw does not re-open a
            // file that is not there and does not misreport why; still counted
            // above, and cleared by Reset() or ForgetFailedHandle().
            m_FailedHandles.emplace(key, reason);
            if (m_LoggedHandles.insert(key).second)
            {
                OLO_CORE_ERROR("SkinProfileTable - SkinProfile {} could not be used ({}); shading with the default profile.",
                               key, ToString(reason));
            }
            result.Reason = reason;
            result.Parameters = SkinProfile::DefaultParameters();
            return result;
        }

        // A slot this handle already owns — including one another thread
        // assigned while the asset above was being fetched, so a slot is never
        // handed out twice. The parameters are the ones just read, not the ones
        // stored last frame.
        if (auto it = m_SlotByHandle.find(key); it != m_SlotByHandle.end())
        {
            m_Parameters[it->second] = parameters;
            result.Slot = it->second;
            result.Parameters = parameters;
            return result;
        }
        if (m_NextSlot >= kMaxSkinProfileSlots)
        {
            ++m_FallbackCounts[static_cast<sizet>(SkinProfileFallbackReason::SlotBudgetFull)];
            if (m_LoggedHandles.insert(key).second)
            {
                OLO_CORE_ERROR("SkinProfileTable - out of skin-profile slots ({} in use) while resolving {}; "
                               "the deferred path cannot name it per pixel, so it shades with the default profile.",
                               kMaxSkinProfileSlots, key);
            }
            result.Reason = SkinProfileFallbackReason::SlotBudgetFull;
            // The AUTHORED parameters are still used on the forward paths, which
            // carry the profile per material and need no slot. Only the
            // per-pixel identity is lost, and only on deferred.
            result.Parameters = parameters;
            return result;
        }

        const u32 slot = m_NextSlot++;
        m_SlotByHandle.emplace(key, slot);
        m_Parameters[slot] = parameters;
        result.Slot = slot;
        result.Parameters = parameters;
        return result;
    }

    SkinProfileParameters SkinProfileTable::GetParametersForSlot(u32 slot) const
    {
        std::scoped_lock lock(m_Mutex);
        if (slot >= kMaxSkinProfileSlots || slot >= m_NextSlot)
            return SkinProfile::DefaultParameters();
        return m_Parameters[slot];
    }

    u32 SkinProfileTable::GetAssignedSlotCount() const
    {
        std::scoped_lock lock(m_Mutex);
        return m_NextSlot;
    }

    u64 SkinProfileTable::GetFallbackCount(SkinProfileFallbackReason reason) const
    {
        std::scoped_lock lock(m_Mutex);
        const auto index = static_cast<sizet>(reason);
        if (index >= m_FallbackCounts.size())
            return 0;
        return m_FallbackCounts[index];
    }

    void SkinProfileTable::ReportFallback(SkinProfileFallbackReason reason, AssetHandle handle)
    {
        const u64 key = static_cast<u64>(handle);
        std::scoped_lock lock(m_Mutex);
        ++m_FallbackCounts[static_cast<sizet>(reason)];
        if (m_LoggedHandles.insert(key).second)
        {
            OLO_CORE_ERROR("SkinProfileTable - skin material with profile {} falls back ({}); "
                           "shading it as a generic material.",
                           key, ToString(reason));
        }
    }

    void SkinProfileTable::ForgetFailedHandle(AssetHandle handle)
    {
        const u64 key = static_cast<u64>(handle);
        std::scoped_lock lock(m_Mutex);
        // The log dedup key goes with it, so the failure is reported again if
        // the reloaded asset is still broken. A silent second failure would be
        // worse than a repeated line.
        m_FailedHandles.erase(key);
        m_LoggedHandles.erase(key);
    }

    void SkinProfileTable::Reset()
    {
        std::scoped_lock lock(m_Mutex);
        m_SlotByHandle.clear();
        m_Parameters.fill(SkinProfileParameters{});
        m_FallbackCounts.fill(0);
        m_LoggedHandles.clear();
        m_FailedHandles.clear();
        m_NextSlot = 0;
    }

} // namespace OloEngine
