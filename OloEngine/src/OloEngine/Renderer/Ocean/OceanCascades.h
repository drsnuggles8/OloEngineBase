#pragma once

#include "OloEngine/Core/Base.h"

#include <glm/glm.hpp>

#include <array>

namespace OloEngine::Ocean
{
    // =========================================================================
    // The fixed three-band cascade preset (issue #969,
    // docs/design/WATER_FUTURE_IMPROVEMENTS.md §1.3).
    //
    // A single FFT tile has to be one thing at once: big enough for the swell
    // that carries the horizon, and fine enough for the chop under the bow. It
    // cannot be both, and the tile it settles on is also the distance at which
    // the whole sea visibly repeats. Three disjoint frequency bands, each on
    // its own tile, is the standard answer (Dupuy & Bruneton 2012).
    //
    // THIS IS A PRESET, NOT A CASCADE SYSTEM. The issue's non-goal is explicit:
    // no artist-facing cascade knobs until the fixed preset is visually proven.
    // Everything below derives from the two values a scene already authors —
    // m_FFTPatchSize and m_FFTResolution — plus the constants in this file.
    //
    // -- THE THREE THINGS THAT MAKE IT ONE SEA RATHER THAN THREE -------------
    //
    // 1. DISJOINT BANDS. Cascade i carries wave vectors in the half-open range
    //    [KMin, KMax). The boundary between cascade i and i+1 sits at the
    //    LOWEST |k| that cascade i+1 can represent at all — its own fundamental,
    //    2*pi/L_{i+1}. That single choice buys both acceptance criteria at once:
    //    no gap (cascade i carries everything up to where i+1 becomes able to)
    //    and no double-counted energy (the ranges are half-open and share an
    //    endpoint). The union over the three bands is exactly [0, inf).
    //
    // 2. NON-COMMENSURATE TILES. The tile ratios below are deliberately not
    //    small rationals, so the three lattices have no common period within any
    //    sea this engine draws. Ratios like 4 and 8 would put every fourth broad
    //    tile boundary on top of a mid one and re-introduce exactly the
    //    repetition the cascades exist to remove.
    //
    //    "Not a small rational" is a measurable property, and the first pair
    //    tried here FAILED it: 4.79 sits 0.05 from 24/5 and 4.31 sits 0.07 from
    //    13/3, so those lattices would have re-aligned every five and every
    //    three tiles. The shipped pair was searched for rather than guessed —
    //    see OceanCascadeTest.TileSizesAreNonCommensurate for the criterion and
    //    the range it holds over.
    //
    // 3. ONE ROTATED DOMAIN. Non-commensurate tiles still share an AXIS: every
    //    cascade's crest lattice runs along the same world x/z, so the eye finds
    //    the grid even when it cannot find the period. The mid cascade's
    //    sampling domain is therefore rotated (kMidCascadeRotationRadians), and
    //    its spectrum's wind direction is rotated INTO that frame by the same
    //    angle, so the waves still travel with the world wind while the lattice
    //    does not line up with anything. Getting only half of that pair right —
    //    or the sign of the second half wrong — is a sea whose mid waves cross
    //    the wind, which reads as a spectrum bug rather than a bookkeeping one.
    //    The derivation of the direction is in OceanFFTField::RegenerateSpectra,
    //    next to the line that applies it.
    //
    // -- RESOLUTION DERIVATION, AND WHY EVERY CHAIN STILL RUNS AT ONE SIZE ---
    //
    // Resolution below is derived per band from that band's SHORTEST wavelength
    // (kMinSamplesPerWavelength samples across it), not from a global maximum —
    // the broad and mid bands come out at 64 where the fine band needs the
    // authored resolution.
    //
    // The three chains nonetheless all run at ArrayResolution (the max of the
    // three), because the three fields are layers of ONE texture array and a
    // texture array's layers must share a size. That costs GPU time and NOTHING
    // ELSE: a band-limited spectrum evaluated on a larger grid is the same
    // spectrum with the extra bins zero, and an inverse FFT of a zero-padded
    // spectrum is the exact band-limited reconstruction of the smaller one. The
    // FIELD IS IDENTICAL either way, which is not an assertion — it is pinned by
    // OceanCascadeTest.DerivedResolutionReproducesTheArrayResolutionField.
    //
    // The alternative — one sampler pair per cascade — would spend four engine
    // texture slots and shift the shader-graph user base, to save a fraction of
    // a millisecond of compute. Resolution is kept because it is what SIZES the
    // array and what makes that trade visible rather than accidental.
    // =========================================================================

    /// The preset is fixed at three bands; kSingleCascadeCount is the legacy
    /// path every existing scene stays on until it opts in.
    inline constexpr u32 kMaxOceanCascades = 3u;
    inline constexpr u32 kSingleCascadeCount = 1u;
    inline constexpr u32 kThreeBandCascadeCount = 3u;

    /// Tile-size ratios against the authored patch size L, which the preset
    /// treats as the MID band. The broad band extends L outward (the long swell
    /// that gives the horizon its coherence — the symptom #969 opens with) and
    /// the fine band inward (the close chop). An author who has tuned L keeps
    /// the wave scale they tuned; the preset adds an octave either side of it.
    ///
    /// Chosen to maximise how far apart the three lattices stay: no q <= 6
    /// multiple of any ratio lands within 0.13 of a whole number, which is close
    /// to the best any real number can do over that range. Six periods of the
    /// broad tile is roughly 5 km — past any sea this engine draws (Drift's is
    /// 1.6 km, and #878 names a "few km, not 50" ceiling for floating-origin
    /// reasons), so a re-alignment beyond it cannot be seen.
    inline constexpr f32 kBroadTileRatio = 6.29f;       ///< L0 = L * this
    inline constexpr f32 kFineTileRatio = 1.0f / 4.43f; ///< L2 = L * this

    /// Rotation applied to the MID cascade's sampling domain (radians, ~37.5°).
    /// Not a multiple of 45°/90°, or the rotated lattice would re-align with the
    /// unrotated ones on the diagonal.
    inline constexpr f32 kMidCascadeRotationRadians = 0.65449847f;

    /// Grid samples across the shortest wavelength a band carries. Two is the
    /// Nyquist floor and looks like it: the field is bilinearly interpolated by
    /// the sampler, and bilinear error on a sinusoid at S samples per period is
    /// roughly (pi/S)^2/2 — 11% at S=6, under 3% at S=13. Eight is the smallest
    /// value that keeps the interpolated crest smooth, which matters more here
    /// than anywhere: this is the band-limited part of the sea, so any error it
    /// has is error nothing else masks.
    inline constexpr f32 kMinSamplesPerWavelength = 8.0f;

    /// Floor on a derived cascade resolution. Below 32 the grid has too few
    /// distinct wave vectors to look like a spectrum rather than a pattern.
    inline constexpr u32 kMinCascadeResolution = 32u;

    /// One band of the preset. KMin/KMax are wave-vector magnitudes in rad/m
    /// and bound the band as the half-open range [KMin, KMax); the top band's
    /// KMax is infinite (its own grid Nyquist is the real limit).
    struct CascadeBand
    {
        f32 PatchSize = 0.0f;      ///< L_i, world tile size (metres)
        u32 Resolution = 0u;       ///< N_i derived from the band's shortest wavelength
        f32 KMin = 0.0f;           ///< inclusive lower wave-vector magnitude (rad/m)
        f32 KMax = 0.0f;           ///< exclusive upper wave-vector magnitude (rad/m)
        f32 DomainRotation = 0.0f; ///< theta_i, sampling-domain rotation (radians)
    };

    /// The whole preset: Count active bands plus the shared grid size every
    /// chain actually runs at (see the header comment).
    struct CascadePreset
    {
        u32 Count = 0u;
        u32 ArrayResolution = 0u;
        std::array<CascadeBand, kMaxOceanCascades> Bands{};

        [[nodiscard]] bool IsValid() const noexcept
        {
            return Count > 0u && Count <= kMaxOceanCascades && ArrayResolution > 0u;
        }
    };

    /// Round `v` up to a power of two, clamped into [kMinCascadeResolution, maxResolution].
    [[nodiscard]] u32 RoundUpCascadeResolution(f32 v, u32 maxResolution) noexcept;

    /// Build the preset for `cascadeCount` bands over an authored patch size and
    /// resolution. `cascadeCount == 1` reproduces the single-cascade field
    /// exactly: one band, the authored L and N, no rotation, no band limit — so
    /// a scene that has not opted in is on its old path unchanged. Any other
    /// count is treated as the fixed three-band preset.
    [[nodiscard]] CascadePreset MakeCascadePreset(u32 cascadeCount, f32 patchSize, u32 resolution);

    /// Rotate `v` by the angle whose cosine/sine are given. The sampling
    /// contract below is written in terms of this and its inverse (negate
    /// `sinA`), so both halves of the CPU/GPU mirror quote one definition
    /// rather than each writing out a matrix.
    [[nodiscard]] constexpr glm::vec2 RotateVec2(glm::vec2 v, f32 cosA, f32 sinA) noexcept
    {
        return glm::vec2(cosA * v.x - sinA * v.y, sinA * v.x + cosA * v.y);
    }

    // =========================================================================
    // THE SAMPLING CONTRACT — the one paragraph both halves must obey.
    //
    // Mirrored by OceanFFTField::SampleCascades (CPU, buoyancy) and by
    // include/OceanCascadeCommon.glsl::sampleOceanCascades (GPU, vertex /
    // tess-eval / fragment). docs/agent-rules/cpu-gpu-surface-parity.md §2 is
    // about the half of a mirror a same-point parity test cannot check — the
    // SPACE the arguments are in — so it is stated first and in capitals:
    //
    //   THE ARGUMENT IS ABSOLUTE WORLD XZ OF THE COLUMN BEING ASKED ABOUT.
    //   Not camera-relative (add u_RenderOrigin.xz back first), and not the
    //   authored base position of a vertex the displacement has since moved.
    //
    // For each active cascade i, with theta_i = Bands[i].DomainRotation:
    //
    //   uv_i    = RotateVec2(worldXZ, cos t_i,  sin t_i) / L_i
    //   disp_i  = displacement[layer i] sampled at uv_i   (dx, h, dz, foam)
    //   deriv_i = derivatives [layer i] sampled at uv_i   (nx, ny, nz, J)
    //
    //   height     += disp_i.y
    //   horizontal += RotateVec2(disp_i.xz, cos t_i, -sin t_i)
    //   slope      += RotateVec2(-deriv_i.xz / deriv_i.y, cos t_i, -sin t_i)
    //   foam        = saturate(foam + disp_i.w)
    //
    // Both the displacement vector and the height gradient live in the
    // cascade's own frame and are rotated BACK by R(-theta) — the gradient
    // because grad_x h(Rx) = R^T grad h, the displacement because it is a vector
    // in those axes. Rotating one and not the other is a sea whose crests lean
    // the wrong way, and it looks like a shading bug.
    //
    // Slopes are summed, never normals: normals do not add. The derivatives
    // texture stores the unit normal (unchanged encoding, so the fallback path
    // is untouched) and the slope is recovered as -n.xz / n.y, which is exact
    // against Ocean_Assemble.comp's normalize(vec3(-sx, 1, -sz)).
    //
    // Foam is the saturated SUM of per-cascade folding rather than the folding
    // of the summed displacement. The exact quantity is det(I + sum ddisp_i),
    // which needs the gradient tensors rather than the per-cascade determinants
    // the texture carries; the sum is its first-order term and agrees exactly
    // when one cascade is active. Documented rather than silently approximated.
    // =========================================================================

    /// Per-cascade values the water shader needs, packed for the UBO exactly as
    /// UBOStructures::WaterUBO::FFTCascadeParams carries them:
    ///   x = 1 / L1, y = 1 / L2, z = cos theta_mid, w = sin theta_mid.
    /// Cascade 0's 1/L0 stays in FFTParams.y, where the single-cascade path
    /// already had it, so nothing that reads the old field has to change.
    /// A one-cascade preset packs zeroes into x/y and the identity rotation.
    [[nodiscard]] glm::vec4 PackCascadeShaderParams(const CascadePreset& preset);
} // namespace OloEngine::Ocean
