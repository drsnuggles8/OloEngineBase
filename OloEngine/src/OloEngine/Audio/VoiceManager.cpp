#include "OloEnginePCH.h"
#include "VoiceManager.h"

#include "OloEngine/Debug/Profiler.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <iterator>

namespace OloEngine::Audio
{
    VoiceManager& VoiceManager::Get()
    {
        // Function-local static rather than an AudioEngine member: the budget must exist
        // even when no audio device came up (headless tests, a server build), because the
        // playback paths query it unconditionally.
        static VoiceManager s_Instance;
        return s_Instance;
    }

    f32 VoiceManager::ComputeScore(const VoiceParams& params, const glm::vec3& listenerPosition)
    {
        // A NaN anywhere in the inputs would make the ranking sort inconsistent (NaN
        // compares false against everything), which is undefined behavior for std::sort and
        // would corrupt the whole audible set — not just this voice. Fail such a voice to
        // the bottom of the ranking instead.
        if (!std::isfinite(params.Priority) || !std::isfinite(params.Volume))
        {
            return 0.0f;
        }

        if (params.Paused)
        {
            // A paused voice makes no sound, so it must be the first thing yielded when
            // something that CAN be heard wants the slot.
            return 0.0f;
        }

        const f32 priority = std::clamp(params.Priority, 0.0f, 1.0f);
        const f32 gain = std::clamp(params.Volume, 0.0f, 1.0f);

        f32 attenuation = 1.0f;
        if (params.Spatialized)
        {
            const glm::vec3 delta = params.Position - listenerPosition;
            const f32 distanceSquared = glm::dot(delta, delta);
            if (!std::isfinite(distanceSquared))
            {
                return 0.0f;
            }

            const f32 distance = std::sqrt(distanceSquared);
            const f32 minDistance = std::isfinite(params.MinDistance) ? std::max(params.MinDistance, 0.0f) : 0.0f;
            const f32 maxDistance = std::isfinite(params.MaxDistance) ? std::max(params.MaxDistance, minDistance) : minDistance;

            if (distance <= minDistance)
            {
                attenuation = 1.0f;
            }
            else if (distance >= maxDistance)
            {
                // Past MaxDistance the source is inaudible, so it must lose every contest
                // against a voice that can actually be heard. Everything out here ties at
                // zero, which is fine precisely because none of them would make a sound:
                // which one an empty slot happens to go to is inaudible either way.
                attenuation = 0.0f;
            }
            else
            {
                // Linear falloff across the authored window. Deliberately NOT the
                // per-source attenuation curve (inverse/exponential/…): the score only has
                // to rank voices consistently, and a linear ramp is monotone, cheap, and
                // identical for every attenuation model, so two sources at the same
                // distance never trade slots because of a curve difference.
                const f32 span = maxDistance - minDistance;
                attenuation = (span > 0.0f) ? (1.0f - (distance - minDistance) / span) : 0.0f;
            }
        }

        return priority * gain * attenuation;
    }

    bool VoiceManager::CanBecomeAudible(const VoiceRecord& record)
    {
        if (record.Params.Paused)
        {
            // Owner intent outranks budget policy. Promoting a paused voice would call
            // OnVoiceStart and resume playback the game explicitly stopped.
            return false;
        }
        if (record.WasAudible)
        {
            // The player heard this sound begin, so bringing it back continues something
            // already in their ear rather than dropping them into its middle. This is also
            // what makes pause/resume work for a one-shot.
            return true;
        }
        if (record.Params.Looping)
        {
            // The spec's virtualization case: a loop resumes in phase, so bringing it back
            // mid-stream is exactly right.
            return true;
        }
        if (record.Params.DurationSeconds <= 0.0)
        {
            // Unknown length — a stream, or a SoundGraph voice whose graph runs until told
            // to stop. We cannot schedule its end, so refusing it permanently would strand
            // it silent forever rather than merely skipping it.
            return true;
        }
        // A never-heard one-shot of known length may START (position 0), but must never be
        // seeked into: it has only ever advanced silently, so starting now would emit a
        // fragment of its tail. Issue #730: such a sound "is refused".
        return record.PlaybackPosition <= 0.0;
    }

    void VoiceManager::SetMaxVoices(u32 maxVoices)
    {
        const std::scoped_lock applyLock(m_TransitionMutex);
        std::vector<PendingTransition> transitions;
        {
            const std::scoped_lock lock(m_Mutex);
            // A cap of zero would silence everything with no way back; treat it as "at
            // least one voice" rather than a mute switch.
            m_MaxVoices = std::max(maxVoices, 1u);
            Rebalance(transitions);
        }
        ApplyTransitions(transitions);
    }

    u32 VoiceManager::GetMaxVoices() const
    {
        const std::scoped_lock lock(m_Mutex);
        return m_MaxVoices;
    }

    void VoiceManager::SetPromotionMargin(f32 margin)
    {
        const std::scoped_lock lock(m_Mutex);
        m_PromotionMargin = std::isfinite(margin) ? std::max(margin, 0.0f) : kDefaultPromotionMargin;
    }

    f32 VoiceManager::GetPromotionMargin() const
    {
        const std::scoped_lock lock(m_Mutex);
        return m_PromotionMargin;
    }

    void VoiceManager::SetListenerPosition(const glm::vec3& position)
    {
        const std::scoped_lock lock(m_Mutex);
        // The listener pose is read from a TransformComponent that a script or a corrupt
        // scene could have made non-finite; a NaN listener would zero every spatialized
        // score at once. Keep the previous pose instead.
        if (std::isfinite(position.x) && std::isfinite(position.y) && std::isfinite(position.z))
        {
            m_ListenerPosition = position;
        }
    }

    glm::vec3 VoiceManager::GetListenerPosition() const
    {
        const std::scoped_lock lock(m_Mutex);
        return m_ListenerPosition;
    }

    VoiceManager::VoiceRecord* VoiceManager::Find(VoiceHandle handle)
    {
        if (handle == kInvalidVoiceHandle)
        {
            return nullptr;
        }
        const auto it = std::ranges::find(m_Voices, handle, &VoiceRecord::Handle);
        return (it != m_Voices.end()) ? &(*it) : nullptr;
    }

    const VoiceManager::VoiceRecord* VoiceManager::Find(VoiceHandle handle) const
    {
        if (handle == kInvalidVoiceHandle)
        {
            return nullptr;
        }
        const auto it = std::ranges::find(m_Voices, handle, &VoiceRecord::Handle);
        return (it != m_Voices.end()) ? &(*it) : nullptr;
    }

    VoiceHandle VoiceManager::Acquire(const IVoiceHost* host, const VoiceParams& params)
    {
        OLO_PROFILE_FUNCTION();

        if (!host)
        {
            return kInvalidVoiceHandle;
        }

        const std::scoped_lock applyLock(m_TransitionMutex);
        VoiceHandle handle = kInvalidVoiceHandle;
        std::vector<PendingTransition> transitions;
        {
            const std::scoped_lock lock(m_Mutex);

            handle = m_NextHandle++;

            VoiceRecord record;
            record.Handle = handle;
            record.Host = host;
            record.Params = params;
            // Every voice enters virtual and is promoted by the rebalance below if it earns
            // a slot. Admitting first and trimming afterwards would let the audible count
            // exceed the cap for the width of this function — which is exactly the
            // invariant this class exists to hold.
            record.State = VoiceState::Virtual;
            record.PlaybackPosition = 0.0;
            record.Score = ComputeScore(params, m_ListenerPosition);
            m_Voices.push_back(record);

            Rebalance(transitions);
        }
        ApplyTransitions(transitions);

        return handle;
    }

    void VoiceManager::Release(VoiceHandle handle)
    {
        if (handle == kInvalidVoiceHandle)
        {
            return;
        }

        const std::scoped_lock applyLock(m_TransitionMutex);
        std::vector<PendingTransition> transitions;
        {
            const std::scoped_lock lock(m_Mutex);
            const auto it = std::ranges::find(m_Voices, handle, &VoiceRecord::Handle);
            if (it == m_Voices.end())
            {
                return;
            }
            m_Voices.erase(it);
            Rebalance(transitions);
        }
        ApplyTransitions(transitions);
    }

    void VoiceManager::UpdateParams(VoiceHandle handle, const VoiceParams& params)
    {
        const std::scoped_lock lock(m_Mutex);
        if (auto* record = Find(handle))
        {
            record->Params = params;
            record->Score = ComputeScore(params, m_ListenerPosition);
        }
    }

    void VoiceManager::SetVoicePaused(VoiceHandle handle, bool paused)
    {
        const std::scoped_lock applyLock(m_TransitionMutex);
        std::vector<PendingTransition> transitions;
        {
            const std::scoped_lock lock(m_Mutex);
            auto* record = Find(handle);
            if (!record || record->Params.Paused == paused)
            {
                return;
            }
            record->Params.Paused = paused;
            record->Score = ComputeScore(record->Params, m_ListenerPosition);
            // Rebalance right here rather than waiting for the next Update: pausing should
            // hand the slot over immediately, and un-pausing should be audible on the same
            // frame the game asked for it, not a frame later.
            Rebalance(transitions);
        }
        ApplyTransitions(transitions);
    }

    void VoiceManager::SetPlaybackPosition(VoiceHandle handle, f64 seconds)
    {
        const std::scoped_lock lock(m_Mutex);
        if (auto* record = Find(handle))
        {
            record->PlaybackPosition = std::isfinite(seconds) ? std::max(seconds, 0.0) : 0.0;
        }
    }

    f64 VoiceManager::GetPlaybackPosition(VoiceHandle handle) const
    {
        const std::scoped_lock lock(m_Mutex);
        const auto* record = Find(handle);
        return record ? record->PlaybackPosition : 0.0;
    }

    bool VoiceManager::IsAudible(VoiceHandle handle) const
    {
        const std::scoped_lock lock(m_Mutex);
        const auto* record = Find(handle);
        return record && record->State == VoiceState::Playing;
    }

    bool VoiceManager::IsVirtual(VoiceHandle handle) const
    {
        const std::scoped_lock lock(m_Mutex);
        const auto* record = Find(handle);
        return record && record->State == VoiceState::Virtual;
    }

    bool VoiceManager::IsActive(VoiceHandle handle) const
    {
        const std::scoped_lock lock(m_Mutex);
        return Find(handle) != nullptr;
    }

    VoiceState VoiceManager::GetState(VoiceHandle handle) const
    {
        const std::scoped_lock lock(m_Mutex);
        const auto* record = Find(handle);
        return record ? record->State : VoiceState::Virtual;
    }

    void VoiceManager::Update(f32 deltaSeconds)
    {
        OLO_PROFILE_FUNCTION();

        const f64 delta = std::isfinite(deltaSeconds) ? std::max(static_cast<f64>(deltaSeconds), 0.0) : 0.0;

        const std::scoped_lock applyLock(m_TransitionMutex);
        std::vector<PendingTransition> transitions;
        {
            const std::scoped_lock lock(m_Mutex);

            for (auto& record : m_Voices)
            {
                const f64 pitch = std::isfinite(record.Params.Pitch) ? std::max(static_cast<f64>(record.Params.Pitch), 0.0) : 1.0;

                if (record.State == VoiceState::Playing)
                {
                    // The backend is authoritative while the voice is really running — a
                    // logically integrated position would drift against the device clock
                    // over a long loop and resume at the wrong phase after a later steal.
                    const f64 devicePosition = record.Host ? record.Host->OnVoiceQueryPosition() : -1.0;
                    if (devicePosition >= 0.0 && std::isfinite(devicePosition))
                    {
                        record.PlaybackPosition = devicePosition;
                    }
                    else
                    {
                        record.PlaybackPosition += delta * pitch;
                    }
                }
                else
                {
                    // The whole point of virtualization: an inaudible voice keeps running
                    // on the clock, so bringing it back is a resume, not a restart.
                    record.PlaybackPosition += delta * pitch;
                }

                const f64 duration = record.Params.DurationSeconds;
                if (duration > 0.0 && record.PlaybackPosition >= duration)
                {
                    if (record.Params.Looping)
                    {
                        record.PlaybackPosition = std::fmod(record.PlaybackPosition, duration);
                    }
                    else
                    {
                        // A one-shot that ran out its length is finished, whether it did
                        // so audibly or while virtual. Retiring it here is what stops a
                        // dead voice from holding a slot forever: owners are not required
                        // to call Stop() on a sound that simply ended, so without this the
                        // budget would silt up with completed one-shots and every later
                        // trigger would be virtualized. A voice of unknown length
                        // (DurationSeconds == 0 — a stream) is never auto-retired and its
                        // owner MUST release it.
                        record.PlaybackPosition = duration;
                        record.Completed = true; // erased at the end of this pass
                        if (record.State == VoiceState::Playing)
                        {
                            // Stop the backend before the slot is handed on. The length
                            // came from the decoder, so the cursor can reach it a hair
                            // before the device has finished draining — freeing the slot
                            // without stopping would let the replacement voice start while
                            // this one is still audible, i.e. cap+1 sounds at once.
                            transitions.push_back(PendingTransition{ record.Host, record.Handle, /*Start=*/false, record.PlaybackPosition });
                            // The record is erased just below, so Rebalance recounts the
                            // audible set from scratch and sees the freed slot.
                            record.State = VoiceState::Virtual;
                            ++m_Virtualizations;
                        }
                    }
                }

                record.Score = ComputeScore(record.Params, m_ListenerPosition);
            }

            const auto completed = std::ranges::remove_if(m_Voices, &VoiceRecord::Completed);
            m_Completions += static_cast<u64>(std::distance(completed.begin(), completed.end()));
            m_Voices.erase(completed.begin(), completed.end());

            Rebalance(transitions);
        }
        ApplyTransitions(transitions);
    }

    void VoiceManager::Rebalance(std::vector<PendingTransition>& out)
    {
        // --- Step 1: never exceed the cap. -----------------------------------------
        // Virtualize the lowest-scoring audible voices until the audible count fits.
        // This runs first and unconditionally, so the invariant holds even if the cap was
        // just lowered under a full mix.
        // Counted once and then maintained by the two transition helpers, rather than
        // rescanned in every loop condition — the scans below are already O(n) each.
        u32 audible = static_cast<u32>(std::ranges::count(m_Voices, VoiceState::Playing, &VoiceRecord::State));

        auto worstAudible = [this]() -> VoiceRecord*
        {
            VoiceRecord* worst = nullptr;
            for (auto& record : m_Voices)
            {
                if (record.State == VoiceState::Playing && (!worst || record.Score < worst->Score))
                {
                    worst = &record;
                }
            }
            return worst;
        };

        // Only voices that CanBecomeAudible are candidates — a refused one-shot stays
        // silent for the rest of its life rather than being seeked into halfway.
        auto bestVirtual = [this]() -> VoiceRecord*
        {
            VoiceRecord* best = nullptr;
            for (auto& record : m_Voices)
            {
                if (record.State == VoiceState::Virtual && CanBecomeAudible(record) && (!best || record.Score > best->Score))
                {
                    best = &record;
                }
            }
            return best;
        };

        auto virtualize = [&out, &audible, this](VoiceRecord& record)
        {
            record.State = VoiceState::Virtual;
            --audible;
            ++m_Virtualizations;
            out.push_back(PendingTransition{ record.Host, record.Handle, /*Start=*/false, record.PlaybackPosition });
        };

        auto devirtualize = [&out, &audible, this](VoiceRecord& record)
        {
            record.State = VoiceState::Playing;
            record.WasAudible = true;
            ++audible;
            ++m_Devirtualizations;
            out.push_back(PendingTransition{ record.Host, record.Handle, /*Start=*/true, record.PlaybackPosition });
        };

        while (audible > m_MaxVoices)
        {
            VoiceRecord* worst = worstAudible();
            if (!worst)
            {
                break;
            }
            virtualize(*worst);
        }

        // --- Step 2: fill free slots with the best virtual voices. -----------------
        while (audible < m_MaxVoices)
        {
            VoiceRecord* best = bestVirtual();
            if (!best)
            {
                break;
            }
            devirtualize(*best);
        }

        // --- Step 3: steal. --------------------------------------------------------
        // With the mix full, a virtual voice takes a slot only by beating the worst
        // audible one by more than the promotion margin. Each swap strictly raises the
        // total audible score by at least the margin, so this terminates; the explicit
        // iteration bound is a belt-and-braces guard for a margin of exactly zero, where
        // a chain of equal scores could otherwise churn.
        const sizet maxSwaps = m_Voices.size();
        for (sizet swap = 0; swap < maxSwaps; ++swap)
        {
            VoiceRecord* worst = worstAudible();
            VoiceRecord* best = bestVirtual();
            if (!worst || !best || best->Score <= worst->Score + m_PromotionMargin)
            {
                break;
            }
            virtualize(*worst);
            devirtualize(*best);
            ++m_Steals;
        }
    }

    void VoiceManager::ApplyTransitions(std::vector<PendingTransition>& transitions)
    {
        if (transitions.empty())
        {
            return;
        }

        for (const auto& transition : transitions)
        {
            if (!transition.Host)
            {
                continue;
            }

            if (transition.Start)
            {
                if (!transition.Host->OnVoiceStart(transition.Position))
                {
                    // The backend refused to (re)start — put the voice back to virtual so
                    // the slot it was given is handed to the next-best candidate rather
                    // than being held by a voice producing no sound. Deliberately does NOT
                    // re-run Rebalance: the recovery happens on the next Update, which
                    // avoids recursing through ApplyTransitions from inside itself.
                    const std::scoped_lock lock(m_Mutex);
                    if (auto* record = Find(transition.Handle))
                    {
                        record->State = VoiceState::Virtual;
                    }
                }
            }
            else
            {
                const f64 stoppedAt = transition.Host->OnVoiceStop();
                if (stoppedAt >= 0.0 && std::isfinite(stoppedAt))
                {
                    // Prefer the backend's own idea of where it stopped: for a clip-backed
                    // voice that is the exact sample cursor, which is what makes a resumed
                    // loop come back in phase rather than merely come back.
                    const std::scoped_lock lock(m_Mutex);
                    if (auto* record = Find(transition.Handle))
                    {
                        record->PlaybackPosition = stoppedAt;
                    }
                }
            }
        }

        transitions.clear();
    }

    void VoiceManager::Reset()
    {
        // Takes the transition lock too, even though it queues none: dropping the table
        // out from under an in-flight ApplyTransitions is exactly the interleaving that
        // lock exists to prevent.
        const std::scoped_lock applyLock(m_TransitionMutex);
        const std::scoped_lock lock(m_Mutex);
        m_Voices.clear();
        m_Steals = 0;
        m_Virtualizations = 0;
        m_Devirtualizations = 0;
        m_Completions = 0;
        m_ListenerPosition = glm::vec3(0.0f);
    }

    VoiceStats VoiceManager::GetStats() const
    {
        const std::scoped_lock lock(m_Mutex);
        VoiceStats stats;
        stats.MaxVoices = m_MaxVoices;
        for (const auto& record : m_Voices)
        {
            if (record.State == VoiceState::Playing)
            {
                ++stats.Playing;
            }
            else
            {
                ++stats.Virtual;
            }
        }
        stats.Steals = m_Steals;
        stats.Virtualizations = m_Virtualizations;
        stats.Devirtualizations = m_Devirtualizations;
        stats.Completions = m_Completions;
        return stats;
    }
} // namespace OloEngine::Audio
