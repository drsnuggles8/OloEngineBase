#include "OloEnginePCH.h"
#include "OloEngine/Terrain/Foliage/FoliageInteraction.h"

#include "OloEngine/Debug/Instrumentor.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace OloEngine
{
    FoliageInteractionField::FieldData FoliageInteractionField::s_Data;

    namespace
    {
        // Every number that reaches this field comes from a component an author
        // typed into, a YAML file or a save game, so none of them is trusted —
        // cpp-coding-quality §2b. A rejected value takes the struct's default
        // rather than propagating a NaN into a spring that would then never
        // recover: NaN compares false against the retire epsilon, so a poisoned
        // slot would be a PERMANENT bend, which is exactly the artifact the
        // fourth acceptance criterion forbids.
        [[nodiscard]] f32 FiniteOr(f32 value, f32 fallback)
        {
            return std::isfinite(value) ? value : fallback;
        }

        [[nodiscard]] bool FiniteVec3(const glm::vec3& v)
        {
            return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
        }

        // How much a slot is still "worth" — used both to decide which slot a
        // new source may evict and to decide when a decayed one retires.
        [[nodiscard]] f32 SlotWeight(const FoliageSpringState& spring)
        {
            return std::max({ std::abs(spring.Value.x), std::abs(spring.Value.y), std::abs(spring.Value.z) });
        }
    } // namespace

    void FoliageInteractionField::Reset()
    {
        for (auto& slot : s_Data.Slots)
            slot = Slot{};
    }

    u32 FoliageInteractionField::GetActiveCount()
    {
        u32 count = 0;
        for (const auto& slot : s_Data.Slots)
            if (slot.Active)
                ++count;
        return count;
    }

    f32 FoliageInteractionField::GetMaximumBendRate()
    {
        f32 rate = 0.0f;
        for (const auto& slot : s_Data.Slots)
        {
            if (!slot.Active)
                continue;
            const f32 moving = glm::length(slot.Spring.Velocity);
            const f32 characteristic = slot.Omega * glm::length(slot.Spring.Value);
            if (const f32 candidate = std::max(moving, characteristic); std::isfinite(candidate))
                rate = std::max(rate, candidate);
        }
        return rate;
    }

    FoliageInteractionGPUData FoliageInteractionField::GetGPUData()
    {
        FoliageInteractionGPUData data{};
        u32 written = 0;
        for (const auto& slot : s_Data.Slots)
        {
            if (!slot.Active)
                continue;

            auto& out = data.Slots[written];
            out.Center = glm::vec4(slot.Center, slot.Radius);
            out.Push = glm::vec4(slot.Spring.Value.x, slot.Spring.Value.y, slot.Spring.Value.z, slot.Falloff);
            // A slot with no history reports its CURRENT centre and bend as the
            // previous ones, which makes its screen-space velocity exactly zero
            // for one frame. The alternative — a previous state of zero —
            // reprojects a fully bent plant from its rest position and paints a
            // one-frame smear through the temporal history the moment an actor
            // appears or teleports. Same reasoning, same shape, as
            // FoliageUBO::WindHistoryValid.
            out.PrevCenter = glm::vec4(slot.HistoryValid ? slot.PrevCenter : slot.Center, slot.Height);
            const glm::vec3 previous = slot.HistoryValid ? slot.PrevValue : slot.Spring.Value;
            out.PrevPush = glm::vec4(previous.x, previous.y, previous.z, 0.0f);
            ++written;
        }
        data.Params.x = static_cast<f32>(written);
        return data;
    }

    void FoliageInteractionField::Update(std::span<const FoliageInteractionSource> sources, f32 dt)
    {
        OLO_PROFILE_FUNCTION();

        const f32 step = std::clamp(FiniteOr(dt, 0.0f), 0.0f, kFoliageInteractionMaxStep);

        // Snapshot history BEFORE anything moves. Every slot's previous centre
        // and bend must describe the frame that was actually drawn, so this
        // cannot be folded into the per-slot update below — a slot the loop
        // reached later would otherwise record a centre from this frame.
        //
        // HistoryValid is set here and nowhere else: a slot created during this
        // Update was not drawn last frame and must not claim a past, which is
        // what keeps a spawning or teleporting actor from streaking.
        for (auto& slot : s_Data.Slots)
        {
            if (!slot.Active)
                continue;
            slot.PrevCenter = slot.Center;
            slot.PrevValue = slot.Spring.Value;
            slot.PrevActor = slot.Actor;
            slot.HistoryValid = true;
            slot.Stamped = false;
        }

        for (const auto& source : sources)
        {
            if (!FiniteVec3(source.Position))
                continue;
            const f32 radius = std::clamp(FiniteOr(source.Radius, 0.0f), 0.0f, 256.0f);
            const f32 strength = std::clamp(FiniteOr(source.Strength, 0.0f), 0.0f, kFoliageInteractionMaxStrength);
            if (radius <= 0.0f || strength <= 0.0f)
                continue;

            const f32 height = std::clamp(FiniteOr(source.Height, 1.0f), 0.0f, 256.0f);
            const f32 falloff = std::clamp(FiniteOr(source.Falloff, 2.0f), 0.25f, 16.0f);
            const f32 recovery = std::clamp(FiniteOr(source.RecoverySeconds, 0.6f),
                                            kFoliageInteractionMinRecovery, kFoliageInteractionMaxRecovery);
            const f32 spacing = std::clamp(FiniteOr(source.TrailSpacing, 0.0f), 0.0f, 64.0f);

            Slot* slot = Find(source.Id);

            // What the actor did since the frame that was drawn. Read from the
            // ACTOR history, not from the influence centre: with a trail the
            // centre is planted and deliberately lags, and deriving the lean
            // from it would make a runner's plants point at their own footprint
            // instead of down the path.
            glm::vec3 motion(0.0f);
            if (slot && slot->HistoryValid)
                motion = source.Position - slot->PrevActor;

            // A TELEPORT is any jump too large to be motion. Both halves
            // matter: the old location sheds a residual so the grass the actor
            // was standing in recovers rather than snapping upright, and the
            // replacement starts with no history so the velocity buffer does
            // not draw a streak across the whole jump.
            bool teleported = false;
            bool wantsPlant = slot == nullptr;
            if (slot)
            {
                const f32 travel = glm::length(source.Position - slot->Center);
                teleported = travel > std::max(radius * 4.0f, spacing * 4.0f);
                wantsPlant = teleported || (spacing > 0.0f && travel > spacing);
            }

            if (wantsPlant)
            {
                // Reserve the replacement BEFORE shedding. A saturated field
                // must degrade to "this influence stops leaving footprints",
                // never to "this influence disappears and re-appears": shedding
                // first and failing to allocate would drop a live actor for a
                // frame, and which actor lost would depend on iteration order.
                if (Slot* fresh = Allocate(strength, slot))
                {
                    const FoliageSpringState carried = slot && !teleported ? slot->Spring : FoliageSpringState{};
                    if (slot)
                        Shed(*slot);
                    if (teleported)
                        motion = glm::vec3(0.0f);
                    const glm::vec3 previousActor = source.Position - motion;
                    *fresh = Slot{};
                    fresh->Spring = carried;
                    fresh->Center = source.Position;
                    fresh->PrevCenter = source.Position;
                    fresh->PrevValue = carried.Value;
                    fresh->Actor = previousActor;
                    fresh->PrevActor = previousActor;
                    fresh->HistoryValid = false;
                    slot = fresh;
                }
                else if (!slot)
                {
                    continue; // the field is full of stronger influences — dropped for this frame only
                }
                else
                {
                    // Nothing free to plant into: the existing influence simply
                    // follows the actor this frame. A teleport additionally
                    // drops its history, or the velocity lane draws the jump.
                    slot->Center = source.Position;
                    if (teleported)
                    {
                        slot->HistoryValid = false;
                        motion = glm::vec3(0.0f);
                    }
                }
            }

            slot->SourceId = source.Id;
            slot->Radius = radius;
            slot->Height = height;
            slot->Falloff = falloff;
            slot->Active = true;
            slot->Stamped = true;
            slot->Actor = source.Position;
            // Without a trail the influence FOLLOWS the actor exactly, which is
            // the simplest behaviour and what a hovering or sliding source
            // wants. With one it stays planted until the block above sheds it,
            // so a trail is a sequence of stationary, independently relaxing
            // footprints rather than one smeared blob.
            if (spacing <= 0.0f)
                slot->Center = source.Position;

            // The bend the actor is ASKING for. The radial term presses foliage
            // away from the centre wherever the actor is — what a standing
            // character does to the grass it is in — and the directional term
            // leans it the way the actor is moving, faded in by speed so a slow
            // walk does not read like a sprint.
            glm::vec3 target(0.0f, 0.0f, strength);
            if (step > 0.0f)
            {
                const glm::vec2 horizontal(motion.x, motion.z);
                if (const f32 distance = glm::length(horizontal); distance > 1e-6f)
                {
                    const f32 weight = std::clamp(distance / step / kFoliageInteractionReferenceSpeed, 0.0f, 1.0f);
                    const glm::vec2 push = horizontal / distance * (strength * weight);
                    target.x = push.x;
                    target.y = push.y;
                }
            }

            // Bending is fast, standing back up is slow. Chosen per step from
            // whether the bend is growing or relaxing, so the authored
            // RecoverySeconds always means the RECOVERY and never the attack.
            slot->Omega = 1.0f / recovery;
            const f32 rate = glm::length(target) >= glm::length(slot->Spring.Value)
                                 ? slot->Omega / kFoliageInteractionAttackRatio
                                 : slot->Omega;
            FoliageSpringStep(slot->Spring, target, rate, step);
        }

        // Unstamped slots — a source removed, an actor that left, or a residual
        // a trail shed — relax toward zero and RETIRE. The hard floor is what
        // makes "influence removal leaves no permanent deformation" a
        // finite-time guarantee rather than an asymptotic one.
        for (auto& slot : s_Data.Slots)
        {
            if (!slot.Active || slot.Stamped)
                continue;

            slot.SourceId = 0; // a residual; the source that shed it must not reclaim it
            FoliageSpringStep(slot.Spring, glm::vec3(0.0f), slot.Omega, step);
            if (!FiniteVec3(slot.Spring.Value) || !FiniteVec3(slot.Spring.Velocity) ||
                SlotWeight(slot.Spring) < kFoliageInteractionRetireEpsilon)
                slot = Slot{};
        }
    }

    FoliageInteractionField::Slot* FoliageInteractionField::Find(u64 sourceId)
    {
        if (sourceId == 0)
            return nullptr;
        for (auto& slot : s_Data.Slots)
            if (slot.Active && slot.SourceId == sourceId)
                return &slot;
        return nullptr;
    }

    FoliageInteractionField::Slot* FoliageInteractionField::Allocate(f32 strength, const Slot* exclude)
    {
        for (auto& slot : s_Data.Slots)
            if (!slot.Active)
                return &slot;

        // Full. Evict the faintest RESIDUAL, and only if this source would
        // out-push it — otherwise a crowd of weak influences churns the field
        // and nothing ever holds still.
        //
        // A LIVE SLOT IS NEVER A CANDIDATE, and `Stamped` alone does not say
        // that: it is cleared for every slot at the top of the frame and set
        // again only as each source is reached, so a source still waiting its
        // turn later in `sources` looks unstamped here. Evicting it would wipe
        // a live actor's spring state and its identity, and WHICH actor lost
        // would depend on iteration order. `SourceId == 0` is the residual
        // marker (Shed and the relax pass both set it), so it is the test that
        // actually means "nobody owns this".
        Slot* weakest = nullptr;
        f32 weakestWeight = std::numeric_limits<f32>::max();
        for (auto& slot : s_Data.Slots)
        {
            if (slot.Stamped || slot.SourceId != 0 || &slot == exclude)
                continue;
            if (const f32 weight = SlotWeight(slot.Spring); weight < weakestWeight)
            {
                weakestWeight = weight;
                weakest = &slot;
            }
        }
        if (!weakest || weakestWeight >= strength)
            return nullptr;
        return weakest;
    }

    void FoliageInteractionField::Shed(Slot& slot)
    {
        // The influence the actor is LEAVING stays where it was, keeps the bend
        // it had accumulated, and decays from there. This is the whole trail:
        // no extra buffer, no per-plant state, just the old slot orphaned from
        // its source so the relax pass treats it as unstamped.
        slot.SourceId = 0;
        slot.Stamped = false;
    }
} // namespace OloEngine
