#pragma once

// =============================================================================
// SkinProfileTable.h — AssetHandle -> small integer slot, so a skin profile can
// be named per pixel. Issue #1231.
//
// The deferred path identifies a profile by a THREE-BIT slot in the G-Buffer
// RT2 flags lane (include/PBRCommon.glsl); a 64-bit AssetHandle obviously does
// not fit. This table is the one place that decides which handle gets which
// slot, so the G-Buffer writer, the lighting pass and the editor's profile-id
// debug view cannot disagree about what "slot 2" means.
//
// SLOTS ARE STICKY, NOT PER-FRAME. A profile keeps the slot it was first given
// until Reset() (scene load / renderer teardown). Recycling slots per frame
// would make the identity of a pixel change between frames for no authored
// reason, which is exactly the kind of churn a temporal pass reads as motion.
//
// THE TABLE IS SMALL AND SAYS SO WHEN IT IS FULL. Running out of slots returns
// kSkinProfileSlotNone with SkinProfileFallbackReason::SlotBudgetFull and logs
// once per handle, rather than aliasing a seventh profile onto someone else's
// parameters — which would look like a correct frame with the wrong skin.
// =============================================================================

#include "OloEngine/Asset/Asset.h"
#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/SkinProfile.h"

#include <array>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace OloEngine
{

    // What a resolve produced: the slot to write into the G-Buffer, the
    // parameters to shade with, and — when they are not the ones that were asked
    // for — why.
    struct SkinProfileResolution
    {
        u32 Slot = kSkinProfileSlotNone;
        SkinProfileParameters Parameters{};
        SkinProfileFallbackReason Reason = SkinProfileFallbackReason::None;

        [[nodiscard]] bool IsFallback() const noexcept
        {
            return Reason != SkinProfileFallbackReason::None;
        }
    };

    class SkinProfileTable
    {
      public:
        SkinProfileTable() = default;

        // Resolve `handle` to a slot + parameters.
        //
        // `handle == 0` means the material is skin with nothing assigned, which
        // is an authoring error and reported as NoHandle — not silently the
        // default, because a head that lost its profile in a merge must look
        // wrong in the log before it looks wrong on screen.
        //
        // THE SLOT IS STICKY; THE PARAMETERS ARE NOT. Every resolve re-reads the
        // asset, so a `.oloskin` saved in the editor reaches the next frame on
        // both paths — the forward one through the material UBO and the deferred
        // one through the per-frame profile table. Caching the parameters with
        // the slot would have made the inspector show new values while the frame
        // shaded with the old ones, which is the shape of hot-reload bugs that
        // take an hour to believe. The cost is one asset-manager map lookup per
        // SKIN submission, and skin submissions are a handful per frame.
        //
        // Thread-safe: mesh submission runs on more than one thread here, and
        // two threads resolving the same new handle must agree on its slot.
        [[nodiscard]] SkinProfileResolution Resolve(AssetHandle handle);

        // The parameters behind a slot, for the passes that only carry the slot.
        // An unassigned or out-of-range slot yields the default parameters.
        [[nodiscard]] SkinProfileParameters GetParametersForSlot(u32 slot) const;

        // Number of slots handed out so far.
        [[nodiscard]] u32 GetAssignedSlotCount() const;

        // How many resolves fell back, by reason, since the last Reset(). The
        // "countably" half of the house rule — a test asserts on these rather
        // than scraping the log.
        [[nodiscard]] u64 GetFallbackCount(SkinProfileFallbackReason reason) const;

        // Drop every assignment. Called on scene load and renderer shutdown.
        void Reset();

        // Count and log a fallback the CALLER detected, for the cases that are
        // decided before a handle is worth resolving at all. Same contract as
        // the reasons Resolve() raises itself: counted every time, logged once
        // per handle.
        void ReportFallback(SkinProfileFallbackReason reason, AssetHandle handle);

        // Forget that `handle` failed, so the next resolve consults the asset
        // manager again. Called from Renderer3D::OnAssetReloaded: a profile the
        // author has just fixed and saved must stop shading with the fallback
        // on the next frame, and the negative cache below would otherwise hold
        // it until a scene load.
        void ForgetFailedHandle(AssetHandle handle);

      private:
        mutable std::mutex m_Mutex;
        std::unordered_map<u64, u32> m_SlotByHandle;
        std::array<SkinProfileParameters, kMaxSkinProfileSlots> m_Parameters{};
        std::array<u64, static_cast<sizet>(SkinProfileFallbackReason::Count)> m_FallbackCounts{};
        // Handles already logged, so a missing profile on 40 000 submissions
        // produces one line rather than 40 000.
        std::unordered_set<u64> m_LoggedHandles;
        // Handles whose asset could not be used, and WHY. Consulted BEFORE the
        // asset manager so a registered-but-deleted .oloskin is not re-opened
        // once per skin draw per frame — the log is deduplicated, and without
        // this the file IO would not be. The fallback is still COUNTED every
        // time, so "how often did this happen?" stays answerable.
        //
        // The reason is stored rather than just the handle: a cache that
        // remembered only "this failed" would report the SECOND occurrence of a
        // WrongAssetType as AssetMissing and increment the wrong counter, which
        // turns the countable half of the house rule into a lie.
        //
        // Cleared by Reset() (a scene load) and per-handle by
        // ForgetFailedHandle() (a hot reload), so a fixed profile recovers
        // without a restart either way.
        std::unordered_map<u64, SkinProfileFallbackReason> m_FailedHandles;
        u32 m_NextSlot = 0;
    };

} // namespace OloEngine
