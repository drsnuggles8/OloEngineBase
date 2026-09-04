#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/RHI/RHITypes.h"
#include "OloEngine/Renderer/Water/WaterDisturbanceField.h"

#include <glm/glm.hpp>

#include <cmath>

namespace OloEngine::WaterFoam
{
    // =========================================================================
    // THE FOAM-ADVECTION CONTRACT (issue #1034, §2.2)
    //
    // Open-ocean whitecaps today are a STATIC function of the surface: the
    // shader reads the FFT's folding signal (saturate(1 - J)) at the shading
    // point and whitens it. So a whitecap appears and vanishes exactly where
    // the crest folds, pulsing in place. Real foam is a THING FLOATING ON THE
    // WATER: it is laid down where the wave broke and then drifts with the
    // surface for seconds afterwards.
    //
    // This header is the ONE place that recurrence is written down. Three
    // consumers mirror it and none of them re-derives it:
    //
    //   * WaterDisturbanceSystem.cpp           — sizes and dispatches;
    //   * compute/WaterDisturbance_Update.comp — runs it;
    //   * include/WaterFoamCommon.glsl         — samples the result.
    //
    // ---- 1. Why this rides in the disturbance field --------------------------
    //
    // Advected foam needs a persistent, world-anchored, camera-followed,
    // toroidally-stored field at roughly half-metre resolution — which is,
    // exactly, WaterDisturbanceField.h. Rather than stand up a second field
    // with a second copy of that addressing contract, the foam is a CHANNEL of
    // the existing one and this header reuses those constants outright. The
    // texture went RG16F -> RGBA16F for it:
    //
    //   .r = wake / actor disturbance (issue #967, unchanged)
    //   .g = advected foam density    (this file)
    //   .ba = the previous frame's summed FFT horizontal displacement at this
    //         texel, in metres (see section 3)
    //
    // Two more reasons beyond tidiness, and the second is the load-bearing one:
    // one dispatch does both, and the engine has exactly ONE free UBO binding
    // and no free sampler slot below TEX_SHADER_GRAPH_0. A second field would
    // have spent both on a feature that shares a window with a field already
    // paid for.
    //
    // ---- 2. Why the pass is ping-ponged -------------------------------------
    //
    // The wake channel only ever reads its OWN texel, so #967 updated the field
    // in place. Advection does not: a semi-Lagrangian step reads at
    // `worldXZ - velocity * dt`, i.e. a NEIGHBOUR. A compute pass that reads
    // neighbours and writes in place is a data race between work groups — it
    // does not error, it produces a field that is partly this frame's and
    // partly last frame's in a pattern that follows the dispatch order, which
    // reads as the foam smearing in bands. So the field is two textures,
    // swapped each frame: read the previous, write the current.
    //
    // ---- 3. The advecting velocity ------------------------------------------
    //
    // Foam sits on water parcels, so it moves at the surface velocity, which
    // has two parts and BOTH are needed:
    //
    //   * ORBITAL — the wave's own motion. Deep-water orbits are closed, so
    //     this transports nothing on average, but it is what makes foam slide
    //     down the front of a breaking crest, which is the motion the eye
    //     actually reads. It is the time derivative of the FFT's horizontal
    //     displacement, and getting it is why .ba exists: the pass writes this
    //     frame's displacement and differences it against the one stored last
    //     frame. That is the EULERIAN derivative at a fixed texel rather than
    //     the material one, which differs at second order in the displacement
    //     amplitude — well under a texel over one frame.
    //
    //   * DRIFT — the wind-driven surface current (Stokes drift). Small, a few
    //     percent of the wind speed, but it is the only part with a non-zero
    //     mean, so it is the whole reason a foam patch ends up somewhere else
    //     rather than oscillating about where it started.
    //
    // Without the orbital term the foam translates rigidly and looks painted
    // on; without the drift it oscillates in place, which is the bug this issue
    // is about wearing a different hat.
    //
    // ---- 4. Why the deposit criterion lives here ----------------------------
    //
    // Spray (§2.3) emits from the same crests that deposit foam, and the issue
    // is explicit that they must not be two independent crest detectors. They
    // are not: DepositFromFold below is the ONE criterion, and it is evaluated
    // on the GPU by the compute pass and on the CPU by WaterSpraySystem, over
    // the same `saturate(1 - J)` fold signal that OceanFFTField produces for
    // both sides. What the spray emitter does NOT do is read the foam TEXTURE —
    // that would be a per-frame GPU readback, i.e. a full pipeline stall, to
    // learn something it can compute directly from the field it already has.
    // =========================================================================

    /// The foam channel shares the disturbance field's lattice exactly. Named
    /// aliases rather than fresh constants so there is nothing to keep in step.
    inline constexpr i32 kResolution = WaterDisturbance::kResolution;
    inline constexpr f32 kTexelSizeMetres = WaterDisturbance::kTexelSizeMetres;
    inline constexpr f32 kInvFieldExtentMetres = WaterDisturbance::kInvFieldExtentMetres;

    /// Metres per second the advecting velocity is clamped to.
    ///
    /// A guard, not a look knob. The orbital term is a division by the frame's
    /// dt, so a single stalled frame (or the frame right after a window jump,
    /// where the stored displacement belongs to a different patch of sea)
    /// produces an arbitrarily large velocity, and a semi-Lagrangian backtrace
    /// of an arbitrarily large step samples somewhere unrelated. 8 m/s is well
    /// above any real surface orbital velocity on a sea this engine renders.
    inline constexpr f32 kMaxVelocityMetresPerSecond = 8.0f;

    /// Fold signal at which foam deposit SATURATES.
    ///
    /// MEASURED, not chosen: a 24 m/s storm sea (128^2, 90 m patch, choppiness
    /// 1.6) peaks at fold 0.345 with a mean of 0.056 over the patch —
    /// WaterSprayTest::ARealStormOceanFoldsHardEnoughToClearTheDefaultThresholds
    /// is the measurement, and it prints both numbers when it fails.
    ///
    /// This is here because the first version of DepositFromFold ramped to
    /// fold 1.0, which is `saturate(1 - J)` reaching 1, i.e. a Jacobian of 0 —
    /// a fully collapsed surface that a plausible sea never produces. Ramping
    /// to a value nothing reaches means the deposit tops out around a third of
    /// its range and the foam is permanently dim, which reads as the feature
    /// being too subtle rather than as the ramp being mis-scaled.
    inline constexpr f32 kFoamSaturationFold = 0.35f;

    /// Fraction of the wind speed taken as the mean surface drift.
    ///
    /// The textbook Stokes-drift figure for a fully developed sea is about 1.5%
    /// of the wind speed at the surface; 3% is a deliberate exaggeration,
    /// because the acceptance criterion is that the drift be VISIBLE over a
    /// handful of frames from a fixed camera and 1.5% of a 10 m/s wind moves a
    /// foam patch 2.5 cm in a frame — a twentieth of a texel.
    inline constexpr f32 kWindDriftFraction = 0.03f;

    // -------------------------------------------------------------------------
    // The recurrence
    // -------------------------------------------------------------------------

    /// Clamp a velocity to kMaxVelocityMetresPerSecond, preserving direction.
    /// Mirrors `waterFoamClampVelocity` in WaterFoamCommon.glsl.
    [[nodiscard("the clamped velocity is the only effect")]]
    inline glm::vec2 ClampVelocity(glm::vec2 velocity) noexcept
    {
        if (!std::isfinite(velocity.x) || !std::isfinite(velocity.y))
            return glm::vec2(0.0f);
        const f32 speedSq = glm::dot(velocity, velocity);
        constexpr f32 maxSq = kMaxVelocityMetresPerSecond * kMaxVelocityMetresPerSecond;
        if (speedSq <= maxSq || speedSq <= 0.0f)
            return velocity;
        return velocity * (kMaxVelocityMetresPerSecond / std::sqrt(speedSq));
    }

    /// Surface velocity at one texel, in metres per second.
    ///
    /// `currentDisplacement` / `previousDisplacement` are the summed FFT
    /// horizontal displacement at this texel's world position, this frame and
    /// last. `windDrift` is the mean current (see kWindDriftFraction).
    ///
    /// `deltaSeconds <= 0` drops the orbital term rather than dividing by it —
    /// a paused editor ticks at dt 0, and a NaN velocity would poison the whole
    /// field permanently through the max() in the deposit step.
    [[nodiscard("the velocity is the only effect")]]
    inline glm::vec2 SurfaceVelocity(glm::vec2 currentDisplacement, glm::vec2 previousDisplacement,
                                     glm::vec2 windDrift, f32 deltaSeconds) noexcept
    {
        glm::vec2 orbital(0.0f);
        if (std::isfinite(deltaSeconds) && deltaSeconds > 0.0f)
            orbital = (currentDisplacement - previousDisplacement) / deltaSeconds;
        return ClampVelocity(orbital + windDrift);
    }

    /// The world position this texel's water was at one step ago — the
    /// semi-Lagrangian backtrace. Mirrors `waterFoamBacktrace`.
    [[nodiscard("the source position is the only effect")]]
    inline glm::vec2 Backtrace(glm::vec2 worldXZ, glm::vec2 velocity, f32 deltaSeconds) noexcept
    {
        if (!std::isfinite(deltaSeconds) || deltaSeconds <= 0.0f)
            return worldXZ;
        return worldXZ - velocity * deltaSeconds;
    }

    /// Foam deposited by a fold signal `fold` (the FFT's saturate(1 - J)).
    ///
    /// A ramp from `threshold` to kFoamSaturationFold rather than a step: a
    /// step makes the foam boundary follow an iso-contour of the Jacobian
    /// exactly, which on a smooth field is a smooth curve and reads as a drawn
    /// outline. The ramp also means raising the threshold thins the foam out
    /// instead of switching it off all at once.
    ///
    /// The upper end is a MEASURED fold rather than 1.0 — see
    /// kFoamSaturationFold for why ramping to a value no sea reaches leaves the
    /// foam permanently dim.
    ///
    /// THE one crest criterion — the spray emitter (§2.3) calls this same
    /// function on the CPU rather than inventing a second one. See section 4.
    [[nodiscard("the deposit is the only effect")]]
    inline f32 DepositFromFold(f32 fold, f32 threshold) noexcept
    {
        if (!std::isfinite(fold) || !std::isfinite(threshold))
            return 0.0f;
        const f32 lo = glm::clamp(threshold, 0.0f, 0.99f);
        // A threshold at or above saturation degenerates to a step, which is
        // the honest behaviour for "nothing this sea does clears the bar".
        const f32 span = glm::max(kFoamSaturationFold - lo, 1.0e-3f);
        return glm::clamp((fold - lo) / span, 0.0f, 1.0f);
    }

    /// Combine advected foam with this frame's deposit.
    ///
    /// `max`, exactly as WaterDisturbance::CombineSplat, and for the same
    /// reason: an accumulate makes the result depend on how many frames the
    /// crest was rendered for, so the same sea would foam twice as heavily at
    /// 120 Hz as at 60.
    [[nodiscard("the combined density is the only effect")]]
    inline f32 CombineDeposit(f32 advected, f32 deposit) noexcept
    {
        return glm::clamp(glm::max(advected, deposit), 0.0f, 1.0f);
    }

    /// The four storage texels and weights a bilinear read of the previous
    /// field at one world position touches.
    ///
    /// ADDRESSING IS SEPARATE FROM FETCHING here, and that split is not
    /// stylistic. Under OLO_BINDLESS the compute shader's images are declared
    /// as LOCALS inside main() (BindlessHeap.glsl's OLO_HEAP_IMAGE), so a
    /// file-scope helper cannot see them and cannot take one as a parameter
    /// without a format qualifier GLSL will not accept on a parameter. Handing
    /// back taps lets the shared code own the part that can be wrong — the
    /// toroidal addressing — while the imageLoad stays where the image is.
    struct BilinearTaps
    {
        glm::ivec2 m_Storage[4]{};
        f32 m_Weight[4]{};
    };

    /// Taps for a bilinear read at ABSOLUTE world XZ of a field stored as a
    /// kResolution^2 toroidal window whose lower corner is `latticeMin`.
    ///
    /// A tap whose lattice texel lies OUTSIDE the window gets weight 0 (and a
    /// storage coordinate of (0,0), which is in range and therefore safe to
    /// fetch and multiply by zero). That is the whole point: the storage is a
    /// torus, so a tap one texel past the window edge holds content from the
    /// OPPOSITE edge, hundreds of metres away. Advecting that in is the classic
    /// toroidal defect — it renders, and it renders as a ghost of the foam
    /// trailing the camera.
    ///
    /// Mirrors `waterFoamPrevTaps` in WaterFoamCommon.glsl, tap for tap.
    [[nodiscard("the taps are the only effect")]]
    inline BilinearTaps PrevTaps(glm::vec2 absoluteWorldXZ, glm::ivec2 latticeMin) noexcept
    {
        BilinearTaps taps;
        if (!std::isfinite(absoluteWorldXZ.x) || !std::isfinite(absoluteWorldXZ.y))
            return taps;

        // Continuous coordinates in TEXEL-CENTRE space: lattice texel `a` has
        // its centre at (a + 0.5) * texelSize, so subtracting the half texel
        // puts integer values on texel centres and makes floor() the correct
        // lower tap. Dropping the -0.5 shifts every advection step by half a
        // texel, which accumulates into a drift indistinguishable from a wrong
        // wind direction.
        const glm::vec2 g = absoluteWorldXZ / kTexelSizeMetres - 0.5f;
        const glm::ivec2 base{ static_cast<i32>(std::floor(g.x)),
                               static_cast<i32>(std::floor(g.y)) };
        const glm::vec2 frac = g - glm::vec2(base);

        i32 index = 0;
        for (i32 dz = 0; dz <= 1; ++dz)
        {
            for (i32 dx = 0; dx <= 1; ++dx, ++index)
            {
                const glm::ivec2 lattice = base + glm::ivec2(dx, dz);
                if (!WaterDisturbance::WindowContains(lattice, latticeMin))
                    continue;
                const f32 wx = (dx == 0) ? (1.0f - frac.x) : frac.x;
                const f32 wz = (dz == 0) ? (1.0f - frac.y) : frac.y;
                taps.m_Storage[index] = WaterDisturbance::StorageForLattice(lattice);
                taps.m_Weight[index] = wx * wz;
            }
        }
        return taps;
    }

    /// Bilinear read of a foam field, built on PrevTaps. `fetch` reads storage
    /// texel (x, y).
    template<typename Fetch>
    [[nodiscard("the sampled density is the only effect")]]
    f32 SampleBilinear(glm::vec2 absoluteWorldXZ, glm::ivec2 latticeMin, Fetch&& fetch)
    {
        const BilinearTaps taps = PrevTaps(absoluteWorldXZ, latticeMin);
        f32 sum = 0.0f;
        for (i32 i = 0; i < 4; ++i)
        {
            if (taps.m_Weight[i] != 0.0f)
                sum += taps.m_Weight[i] * fetch(taps.m_Storage[i].x, taps.m_Storage[i].y);
        }
        return sum;
    }

    /// Scene-level foam-advection controls. Published each frame by
    /// Scene::ProcessScene3DSharedLogic from the dominant WaterComponent,
    /// alongside WaterDisturbanceSettings, and consumed by the same dispatch.
    ///
    /// The ocean handles ride here rather than on the water draw command
    /// because the dispatch happens in RenderPipeline's pre-graph block, which
    /// never sees a draw command — and because the field is ONE global
    /// resource, so it has to be fed by the one dominant surface anyway.
    struct WaterFoamSettings
    {
        /// Whether to advect at all. FALSE also means the compute writes zero
        /// into the foam channel, so a scene that switches advection off cannot
        /// leave a frozen foam field behind it.
        bool m_Enabled = false;
        /// Shader multiplier on the sampled field; 0 disables it at the
        /// sampling end too.
        f32 m_Intensity = 1.0f;
        /// Seconds for deposited foam to halve. Shorter than the wake's by
        /// default: a whitecap is gone in a few seconds, a boat's churn is not.
        f32 m_HalfLifeSeconds = 3.5f;
        /// Fold signal (saturate(1 - J)) at which foam starts being deposited.
        /// 0.10 against a measured storm peak of 0.345 and a mean of 0.056, so
        /// whitecaps land on the folding crests and not on the whole sea.
        f32 m_DepositThreshold = 0.10f;
        /// Mean surface current, metres per second, world XZ. Derived from the
        /// water tile's OWN FFT wind — the wind that made the waves — rather
        /// than from the global WindSystem, so the drift cannot point across
        /// the swell it is supposed to be carrying foam along.
        glm::vec2 m_DriftMetresPerSecond{ 0.0f };
        /// The FFT cascade displacement array and its sampling params, as
        /// Ocean::PackCascadeShaderParams produced them for the field the
        /// dominant surface is actually rendering — never re-derived, or the
        /// deposit ends up sampling tiles the spectra were not generated on.
        ///
        /// Advection is a no-op without them: the fold signal IS the FFT, so a
        /// Gerstner sea has nothing to deposit from. WaterDisturbanceSystem
        /// forces m_Enabled false in that case rather than dispatching a pass
        /// that reads an unbound array.
        RHI::ResourceHandle m_FFTDisplacement{};
        glm::vec4 m_FFTParams{ 0.0f };
        glm::vec4 m_FFTCascadeParams{ 0.0f };
    };
} // namespace OloEngine::WaterFoam
