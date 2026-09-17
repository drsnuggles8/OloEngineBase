#pragma once

#include "OloEngine/Core/Base.h"

#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>
#include <span>

namespace OloEngine
{
    // ── Local foliage interaction (issue #1238) ──────────────────────────────
    //
    // A moving actor presses a bounded local INFLUENCE into the world; foliage
    // inside it bends away from its root and springs back when the actor
    // leaves. The influence set is global per frame, capped, and travels to
    // every raster consumer in the SAME FoliageParams block the wind field
    // rides, so colour, depth, shadow and velocity read one description.
    //
    // WHERE THE STATE LIVES, AND WHY IT IS NOT PER PLANT. A per-plant bend
    // buffer would be O(instances) of GPU state that every pass must agree on;
    // the state here is per INFLUENCE instead — a few dozen floats — and the
    // per-plant bend is a pure function of it evaluated in the vertex stage.
    // The cost of that choice is that "recovery" has to be a property of the
    // influence rather than of the blade, which is what the shed residuals
    // below are for: a walking actor leaves a trail of influences that stay
    // where the foot was and relax to zero, so the grass behind it recovers
    // instead of snapping back the instant the actor's own influence moves on.
    //
    // THE OFF SWITCH IS THE ABSENCE OF A SOURCE. With no active influence the
    // shader's contribution is exactly 0.0 — not a small number — so every
    // scene authored before this feature renders the same pixels. That is the
    // property FoliageInteractionShader's zero-count column pins.
    //
    // ONE THING DOES CHANGE FOR SUCH A SCENE, AND IT IS NOT PIXELS: because the
    // per-layer response defaults to 1, every foliage instance's AABB is padded
    // by kFoliageInteractionMaxStrength whether or not the scene can produce an
    // influence. A frustum bound that is too LARGE only ever draws something
    // already off screen, so the image is unaffected — the cost is looser
    // culling, and a one-time instance-id retirement when the bounds profile
    // hash first changes. See FoliageInteractionMaximumDisplacement for why the
    // cap is 1 and not larger.

    // How many influences reach the GPU in one frame. Mirrored by
    // OLO_FOLIAGE_INTERACTION_SLOTS in include/FoliageInteraction.glsl;
    // ShaderUBOSizeConsistencyTest fails if the two ever disagree, because the
    // reflected FoliageParams block would outgrow its C++ twin.
    inline constexpr u32 kFoliageInteractionSlots = 16u;

    // The per-influence push magnitude ceiling, in world units, and therefore
    // the term every bound is padded by. An authored strength is clamped to it
    // on the way in and the SUM over slots is clamped to the layer's own
    // response times it in the shader — so no number of overlapping actors can
    // move a plant further than FoliageInteractionMaximumDisplacement says.
    //
    // WHY ONE AND NOT MORE. This number is paid by EVERY foliage layer in EVERY
    // scene, whether or not anything in it emits an influence: the response
    // defaults to 1, so the instance AABB is padded by this much always, and a
    // typical grass layer's wind padding is about 0.34 world units for
    // comparison. Two units would be six times the wind term of permanently
    // looser culling bought for a bend nobody in that scene can produce. One
    // unit of lateral tip displacement is already a pronounced lean on the
    // 2-3 unit grass these layers author, and a layer that genuinely wants more
    // raises its own InteractionResponse — which pays for the wider bound
    // exactly where the wider bend was asked for.
    inline constexpr f32 kFoliageInteractionMaxStrength = 1.0f;

    // Recovery slower than this is indistinguishable from a plant that never
    // recovers, which is the artifact the fourth acceptance criterion rules
    // out; faster than this is a snap rather than a bend.
    inline constexpr f32 kFoliageInteractionMinRecovery = 0.02f;
    inline constexpr f32 kFoliageInteractionMaxRecovery = 8.0f;

    // A slot whose bend has decayed below this is RETIRED — set to exactly
    // zero and freed. Without a hard floor an exponential decay never reaches
    // zero and a scene keeps paying for an actor that left an hour ago.
    inline constexpr f32 kFoliageInteractionRetireEpsilon = 1e-3f;

    // dt is clamped to this before it reaches the spring. A frame spike or a
    // debugger pause must not teleport the bend; the spring is exact for any
    // dt, but "exactly where it would be after four seconds" is still a visual
    // discontinuity, and the clamp is what keeps one long frame from looking
    // like a cut.
    inline constexpr f32 kFoliageInteractionMaxStep = 0.25f;

    // Speed at which an actor's DIRECTIONAL push is at full strength. Below it
    // the push is mostly radial (a standing actor presses grass outward); above
    // it the plants lean the way the actor is running.
    inline constexpr f32 kFoliageInteractionReferenceSpeed = 2.0f;

    // Attack is this multiple of the authored recovery time. Bending under a
    // foot is fast; standing back up is not. One spring, two rates, chosen by
    // whether the bend is growing or relaxing.
    inline constexpr f32 kFoliageInteractionAttackRatio = 0.18f;

    /// @brief One critically damped spring, integrated ANALYTICALLY.
    ///
    /// This is the whole answer to "recover without frame-rate-dependent
    /// oscillation". A per-frame lerp (`x += (target - x) * k`) converges at a
    /// rate that depends on how often it is called, so the same motion recovers
    /// at a different speed at 30 and 144 fps, and a large `k` with a long
    /// frame overshoots and rings. The closed form below is the exact solution
    /// of `x'' = -2w x' - w^2 (x - T)` and therefore
    ///
    ///   * composes: N steps of dt equal one step of N*dt for a constant
    ///     target, to float rounding (FoliageInteractionContract pins this at
    ///     four cadences plus a spike);
    ///   * never oscillates: the critically damped root is real and repeated,
    ///     so there is no imaginary part to ring with;
    ///   * never overshoots from rest: (1 + u) e^-u <= 1 for every u >= 0.
    struct FoliageSpringState
    {
        glm::vec3 Value{ 0.0f };
        glm::vec3 Velocity{ 0.0f };
    };

    inline void FoliageSpringStep(FoliageSpringState& state, const glm::vec3& target, f32 omega, f32 dt)
    {
        if (!std::isfinite(omega) || omega <= 0.0f || !std::isfinite(dt) || dt <= 0.0f)
            return;

        const glm::vec3 offset = state.Value - target;
        const f32 decay = std::exp(-omega * dt);
        const glm::vec3 blend = state.Velocity + offset * omega;
        state.Value = target + (offset + blend * dt) * decay;
        state.Velocity = (state.Velocity - blend * (omega * dt)) * decay;
    }

    /// @brief The bound every consumer pads by, in world units.
    ///
    /// `response` is the layer's own scale on the shared influence set, and the
    /// shader clamps the SUMMED push to exactly this, so the bound holds for
    /// any influence count. Pure, so FoliageInstanceBounds and the shader can
    /// be checked against one another without a GPU.
    [[nodiscard]] inline f32 FoliageInteractionMaximumDisplacement(f32 response)
    {
        if (!std::isfinite(response))
            return 0.0f;
        return std::clamp(std::abs(response), 0.0f, 8.0f) * kFoliageInteractionMaxStrength;
    }

    /// @brief What an actor hands the field each frame.
    ///
    /// Position is ABSOLUTE world space, matching the root the shader
    /// reconstructs from `u_Model * pivot + u_WindFlags.xyz` — the same
    /// convention foliageWindOffset already uses for its gust phase, so the two
    /// producers cannot disagree about where a plant is.
    struct FoliageInteractionSource
    {
        // Stable across frames; this is what lets the field recognise the same
        // actor and keep its spring state. An entity UUID in practice.
        u64 Id = 0;
        glm::vec3 Position{ 0.0f };
        // Horizontal radius of the influence cylinder.
        f32 Radius = 1.0f;
        // How far ABOVE the centre a plant's root may sit and still be bent. A
        // cylinder rather than a sphere because an actor's origin is at its
        // feet and grass roots are on the ground: a sphere centred on the
        // actor would either miss the grass it stands in or reach up a cliff.
        f32 Height = 1.0f;
        f32 Strength = 1.0f;
        // Exponent on the normalized radial falloff. 1 is linear, higher
        // concentrates the bend at the centre.
        f32 Falloff = 2.0f;
        // Seconds for the bend to relax once the actor is gone. See
        // FoliageSpringStep — this is a time constant, never a per-frame rate.
        f32 RecoverySeconds = 0.6f;
        // Distance the actor may travel before the field sheds the influence it
        // is leaving as a decaying residual. 0 disables the trail entirely, and
        // the plants then recover the moment the actor's own influence moves
        // off them. Kept authorable because a heavy animal should flatten a
        // path and a bird should not.
        f32 TrailSpacing = 0.0f;
    };

    /// @brief One influence as the GPU sees it. std140, 64 bytes.
    ///
    /// Current and previous are BOTH carried because the vertex stage writes
    /// velocity (G-Buffer RT3 / the forward velocity attachment): a bend
    /// evaluated only at the current frame reprojects to the plant's REST
    /// position, and the difference smears or ghosts across exactly the pixels
    /// the actor is bending. Shape (radius, falloff, height) has no previous
    /// twin on purpose — those change only when someone edits the component,
    /// where one frame of velocity error is not observable.
    struct FoliageInteractionSlot
    {
        glm::vec4 Center{ 0.0f };     // xyz = absolute world centre, w = radius
        glm::vec4 Push{ 0.0f };       // xy = directional push (world XZ), z = radial push, w = falloff
        glm::vec4 PrevCenter{ 0.0f }; // xyz = previous centre, w = vertical extent above the centre
        glm::vec4 PrevPush{ 0.0f };   // xy = previous directional push, z = previous radial push, w = unused
    };

    struct FoliageInteractionGPUData
    {
        // x = active slot count. y/z/w reserved; the per-draw response and
        // bend cap are filled by the UBO writer, which is the only place that
        // knows which layer is being drawn.
        glm::vec4 Params{ 0.0f };
        FoliageInteractionSlot Slots[kFoliageInteractionSlots]{};
    };

    /// @brief Engine-wide interaction field. One instance, like WindSystem.
    ///
    /// Update() is the only mutator and it is called once per frame from the
    /// scene's shared 3D path, so the editor viewport and the runtime tick the
    /// same field. Every UBO writer then reads GetGPUData(), exactly as they
    /// read WindSystem::GetGPUData().
    class FoliageInteractionField
    {
      public:
        /// Advance every slot by dt and stamp `sources` into it.
        /// `dt` is sanitized here — a caller may pass a raw frame delta.
        static void Update(std::span<const FoliageInteractionSource> sources, f32 dt);

        /// Drop every influence, immediately and completely.
        ///
        /// The teardown half of the fourth acceptance criterion: a scene
        /// change, a region unload or a world-origin rebase invalidates every
        /// absolute centre in the field at once, and a residual left behind
        /// would be a bend sitting in mid-air over the new content.
        static void Reset();

        [[nodiscard]] static FoliageInteractionGPUData GetGPUData();

        /// Slots that currently contribute. Zero means the shader contribution
        /// is exactly zero — the property every "no permanent artifact" test
        /// asserts on.
        [[nodiscard]] static u32 GetActiveCount();

        /// @brief Fastest rate any influence's bend is currently changing, in
        /// world units per second, before the per-layer response scales it.
        ///
        /// Exists for the ray-traced vegetation cache (issue #1240), whose
        /// proxy age limit is `error / velocity`: a snapshot taken while an
        /// actor is running through the grass goes stale far sooner than one
        /// taken under wind alone, and a bound that ignored interaction would
        /// keep serving it. Reported from the spring's own state rather than
        /// estimated — |v| for the motion already under way, plus the
        /// characteristic rate w*|x| for the relaxation that is about to be.
        [[nodiscard]] static f32 GetMaximumBendRate();

      private:
        struct Slot
        {
            u64 SourceId = 0; // 0 == a shed residual, which no source will reclaim
            bool Active = false;
            bool Stamped = false;
            bool HistoryValid = false;
            // Where the influence IS. With TrailSpacing > 0 this is a PLANTED
            // point that does not follow the actor: it stays put until the
            // actor is a spacing away, then the slot is shed as a residual and
            // a fresh one is planted underfoot. That is the whole footprint
            // trail, and it is why the grass behind a runner recovers on its
            // own clock instead of snapping up the moment the actor passes.
            glm::vec3 Center{ 0.0f };
            glm::vec3 PrevCenter{ 0.0f };
            glm::vec3 PrevValue{ 0.0f };
            // Where the ACTOR is, which is not the same thing once the
            // influence is planted. Kept so the lean direction still comes from
            // real motion rather than from a centre that deliberately lags.
            glm::vec3 Actor{ 0.0f };
            glm::vec3 PrevActor{ 0.0f };
            f32 Radius = 1.0f;
            f32 Height = 1.0f;
            f32 Falloff = 2.0f;
            f32 Omega = 1.0f; // 1 / RecoverySeconds
            FoliageSpringState Spring{};
        };

        struct FieldData
        {
            Slot Slots[kFoliageInteractionSlots]{};
        };

        [[nodiscard]] static Slot* Find(u64 sourceId);
        // `exclude` is the slot the caller is about to shed: a replacement must
        // never be the very slot being turned into a residual, or the trail
        // would consume itself and a saturated field would flicker.
        [[nodiscard]] static Slot* Allocate(f32 strength, const Slot* exclude);
        static void Shed(Slot& slot);

        static FieldData s_Data;
    };

    /// @brief Copy this frame's influence set into a foliage UBO.
    ///
    /// A template purely to keep this header free of ShaderBindingLayout.h
    /// (which includes this one). Every site that fills a FoliageUBO calls it —
    /// the colour draw, the shadow draw and the command dispatch — so the three
    /// cannot drift into carrying different influence sets, which is the
    /// failure mode where a plant's shadow stays upright while the plant bends.
    template<typename FoliageUBOType>
    inline void ApplyFoliageInteraction(FoliageUBOType& ubo, f32 response)
    {
        const f32 clamped = std::isfinite(response) ? std::clamp(response, 0.0f, 8.0f) : 1.0f;
        ubo.InteractionParams = glm::vec4(0.0f, clamped,
                                          FoliageInteractionMaximumDisplacement(clamped), 0.0f);
        // The common case is an empty field, and it is on the path of EVERY
        // foliage draw in every scene — colour, depth and one per shadow
        // cascade. GetGPUData returns the whole slot array by value, so taking
        // the count first keeps a scene with no influence source from paying a
        // kilobyte of copying per draw for sixteen zeroed slots. The caller's
        // FoliageUBO is value-initialised, so the slots are already zero, and a
        // count of 0 makes the shader return before it reads them.
        if (FoliageInteractionField::GetActiveCount() == 0u)
            return;

        const auto field = FoliageInteractionField::GetGPUData();
        ubo.InteractionParams.x = field.Params.x;
        for (u32 i = 0; i < kFoliageInteractionSlots; ++i)
            ubo.Interactions[i] = field.Slots[i];
    }
} // namespace OloEngine
