#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Math/Math.h"

#include <glm/glm.hpp>

#include <cmath>

namespace OloEngine::WaterDisturbance
{
    // =========================================================================
    // THE ENCODING CONTRACT (issue #967)
    //
    // This header is the ONE place the world-XZ -> foam-texel mapping is
    // written down. Three consumers mirror it and none of them re-derives it:
    //
    //   * WaterDisturbanceSystem.cpp             — sizes the texture, snaps the
    //                                              window, fills the params UBO;
    //   * compute/WaterDisturbance_Update.comp   — writes the field;
    //   * include/WaterDisturbanceCommon.glsl    — samples it from Water.glsl.
    //
    // Every defect in this scheme is a WRONG ADDRESS
    // (docs/agent-rules/terrain-virtual-texturing.md), so the addressing is
    // stated once, as pure functions, and pinned headlessly by
    // WaterDisturbanceFieldTest.cpp.
    //
    // ---- 1. The lattice -----------------------------------------------------
    //
    // The field is a square lattice of texels anchored at the WORLD ORIGIN —
    // not at the camera, and not at the water mesh. Absolute texel `a` (a
    // signed ivec2, freely negative) covers world XZ
    // [a * kTexelSizeMetres, (a+1) * kTexelSizeMetres) and has its centre at
    // (a + 0.5) * kTexelSizeMetres.
    //
    // Anchoring at the world origin is what makes the acceptance criterion
    // "the trail survives tessellation/LOD changes without sliding with mesh
    // vertices" hold STRUCTURALLY rather than by luck: the mapping never
    // mentions the mesh, its UVs, its tessellation level or the surface's own
    // transform, so no re-tessellation can move it.
    //
    // ---- 2. Toroidal storage ------------------------------------------------
    //
    // Only a kResolution x kResolution window is stored, toroidally: storage
    // coordinate `s` holds the lattice texel congruent to `s` modulo
    // kResolution that lies inside the current window. A one-texel window shift
    // therefore reassigns exactly one row/column and leaves every other texel's
    // content valid AT ITS EXISTING ADDRESS — which is the whole point: a field
    // that re-addressed on camera motion would drag the wake sideways every
    // time the camera moved.
    //
    // `%` truncates toward zero in BOTH C++ and GLSL, so a negative lattice
    // coordinate maps to a NEGATIVE storage index — which does not crash, it
    // silently samples elsewhere and mirrors the field about the origin. Both
    // sides use Euclidean modulo (Math::WrapIndex / waterDisturbanceWrapIndex).
    // See docs/agent-rules/ddgi-probe-cascades-and-sparsity.md §2.
    //
    // ---- 3. The half-texel convention ---------------------------------------
    //
    // Sampling is by ABSOLUTE world XZ with a REPEAT wrap mode:
    //
    //     uv = absoluteWorldXZ * kInvFieldExtentMetres
    //
    // This is exact and needs no correction term: storage texel `s` has its
    // centre at uv = (s + 0.5) / N, and world (a + 0.5) * texelSize maps to
    // uv = (a + 0.5) / N, which is congruent to (s + 0.5) / N modulo 1 for
    // every a == s (mod N). Because the storage IS a torus, hardware REPEAT
    // bilinear also interpolates correctly across the wrap seam.
    //
    // The one place it does not is the WINDOW boundary, where the neighbouring
    // stored texel belongs to the opposite edge of the window. `EdgeFade` below
    // fades the field out before that boundary is reached — which is needed
    // anyway so the field does not end in a visible 256 m square on the ocean.
    //
    // ---- 4. Why 16-bit float storage and not R8 -----------------------------
    //
    // R8 is a quarter of the memory and was the obvious first choice. It cannot
    // express this decay, and the failure is silent.
    //
    // The field decays multiplicatively (see DecayFactor). With a 6 s half-life
    // at 60 Hz the per-frame factor is 2^(-1/360) ~= 0.998076, so a texel at
    // 0.5 loses 0.00096 per frame. R8's quantum is 1/255 ~= 0.00392, and a
    // value rounds to the nearest representable level — so the decayed result
    // rounds straight back to 0.5 and THE WAKE NEVER FADES. It would sit there
    // permanently, looking like a decay bug in the shader or a stuck splat
    // queue, with every unit test green.
    //
    // A 16-bit float has a 10-bit mantissa, so its relative quantum near 0.5 is
    // about 2^-11 ~= 0.00049 — the same per-frame step is ~4 quanta, and the
    // decay proceeds. `WaterDisturbanceFieldTest.DecayStallsUnderR8Quantisation`
    // pins exactly this, negative-controlled against the R8 quantisation so it
    // cannot degenerate into asserting nothing.
    //
    // The actual storage is `ImageFormat::RG16F`, not an R16F: the engine's
    // ImageFormat enum has no single-channel 16-bit float, and adding one is a
    // cross-backend change with a silent failure mode — the enum's own comments
    // record R32UInt having had nothing to map to on Vulkan and returning the
    // NULL texture handle, so the whole resource quietly did not exist there.
    // RG16F is already plumbed on both backends (TEX_FLUID_THICKNESS uses it),
    // carries the identical mantissa, and costs 1 MB rather than 0.5 MB for the
    // whole field. Only .r is read; .g is written zero and reserved.
    // =========================================================================

    /// Texels per axis of the stored window. 512 x 0.5 m = a 256 m field, which
    /// covers the near/mid water around a Drift boat out to the distance the
    /// shader's own wake fade ends at.
    inline constexpr i32 kResolution = 512;

    /// Metres per texel. A Drift hull is ~2-3 m in the beam, so a wake gets
    /// ~5-6 texels across — enough to read as a trail rather than a line, and
    /// cheap enough that the whole field is one 512x512 R16F (512 KB).
    inline constexpr f32 kTexelSizeMetres = 0.5f;

    /// Side length of the stored window in metres.
    inline constexpr f32 kFieldExtentMetres = static_cast<f32>(kResolution) * kTexelSizeMetres;

    inline constexpr f32 kInvFieldExtentMetres = 1.0f / kFieldExtentMetres;

    /// Normalised half-extent at which the edge fade begins (0.5 == the window
    /// boundary). 0.42 leaves a ~20 m fade band on a 256 m field.
    inline constexpr f32 kEdgeFadeStart = 0.42f;

    /// Maximum splats consumed by one compute dispatch. This is a HARD,
    /// structural bound: the splat array lives in a std140 UBO sized at compile
    /// time, so the queue cannot silently grow past it. What happens when the
    /// queue is full is documented on WaterDisturbanceSystem::SubmitSplat.
    inline constexpr u32 kMaxSplatsPerFrame = 96;

    /// Compute work-group side. Must match the .comp's local_size_x/y.
    inline constexpr u32 kWorkGroupSize = 16;

    // -------------------------------------------------------------------------
    // Addressing
    // -------------------------------------------------------------------------

    /// Absolute lattice texel containing world XZ `worldXZ`.
    ///
    /// `floor`, not a truncating cast: world XZ goes negative, and truncation
    /// folds [-texelSize, 0) onto texel 0 — doubling that one row's footprint
    /// and shifting every negative texel by one.
    [[nodiscard("the lattice coordinate is the only effect")]]
    inline glm::ivec2 LatticeForWorld(glm::vec2 worldXZ) noexcept
    {
        return { static_cast<i32>(std::floor(worldXZ.x / kTexelSizeMetres)),
                 static_cast<i32>(std::floor(worldXZ.y / kTexelSizeMetres)) };
    }

    /// World XZ of the CENTRE of absolute lattice texel `lattice`.
    [[nodiscard("the world position is the only effect")]]
    inline glm::vec2 WorldForLatticeCentre(glm::ivec2 lattice) noexcept
    {
        return { (static_cast<f32>(lattice.x) + 0.5f) * kTexelSizeMetres,
                 (static_cast<f32>(lattice.y) + 0.5f) * kTexelSizeMetres };
    }

    /// Storage coordinate holding absolute lattice texel `lattice`.
    /// Euclidean modulo — see §2 above.
    [[nodiscard("the storage coordinate is the only effect")]]
    inline glm::ivec2 StorageForLattice(glm::ivec2 lattice) noexcept
    {
        return { Math::WrapIndex(lattice.x, kResolution), Math::WrapIndex(lattice.y, kResolution) };
    }

    /// Lower corner, in lattice texels, of the window centred on `centreXZ`.
    /// The window covers [LatticeMin, LatticeMin + kResolution) on both axes.
    ///
    /// Snapping to the lattice (rather than centring exactly on the camera) is
    /// what keeps the mapping stable: an unsnapped centre would move the
    /// world->texel mapping by a sub-texel amount every frame and the whole
    /// field would crawl under the surface.
    [[nodiscard("the window origin is the only effect")]]
    inline glm::ivec2 LatticeMinForCentre(glm::vec2 centreXZ) noexcept
    {
        return LatticeForWorld(centreXZ) - glm::ivec2(kResolution / 2);
    }

    /// Inverse of StorageForLattice within a window: the unique lattice
    /// coordinate congruent to `storage` (mod kResolution) that lies inside
    /// [latticeMin, latticeMin + kResolution).
    [[nodiscard("the lattice coordinate is the only effect")]]
    inline glm::ivec2 LatticeForStorage(glm::ivec2 storage, glm::ivec2 latticeMin) noexcept
    {
        return { latticeMin.x + Math::WrapIndex(storage.x - latticeMin.x, kResolution),
                 latticeMin.y + Math::WrapIndex(storage.y - latticeMin.y, kResolution) };
    }

    /// True when `lattice` lies inside the window whose lower corner is
    /// `latticeMin`.
    ///
    /// A texel inside the NEW window but outside the PREVIOUS one is newly
    /// exposed: whatever sits at its storage address is content from the far
    /// side of the field, and must be RESET rather than decayed. Skipping this
    /// is the classic toroidal defect — the field renders, looks plausible, and
    /// carries a mirror image of itself trailing the camera.
    [[nodiscard("the containment result is the only effect")]]
    inline bool WindowContains(glm::ivec2 lattice, glm::ivec2 latticeMin) noexcept
    {
        return lattice.x >= latticeMin.x && lattice.x < latticeMin.x + kResolution &&
               lattice.y >= latticeMin.y && lattice.y < latticeMin.y + kResolution;
    }

    /// World centre of the window whose lower corner is `latticeMin`.
    [[nodiscard("the window centre is the only effect")]]
    inline glm::vec2 WindowCentreWorld(glm::ivec2 latticeMin) noexcept
    {
        return { (static_cast<f32>(latticeMin.x) + static_cast<f32>(kResolution) * 0.5f) * kTexelSizeMetres,
                 (static_cast<f32>(latticeMin.y) + static_cast<f32>(kResolution) * 0.5f) * kTexelSizeMetres };
    }

    // -------------------------------------------------------------------------
    // Field evolution — the exact recurrence the compute shader runs
    // -------------------------------------------------------------------------

    /// Per-frame multiplicative decay factor for a given half-life.
    ///
    /// Exponential rather than a per-frame constant subtraction so the result
    /// is FRAME-RATE INDEPENDENT: the same wall-clock elapsed time decays the
    /// field by the same factor whether it arrived as one 33 ms step or two
    /// 16 ms ones. A wake that faded faster on a fast machine would be a
    /// gameplay difference, not merely a visual one.
    [[nodiscard("the decay factor is the only effect")]]
    inline f32 DecayFactor(f32 halfLifeSeconds, f32 deltaSeconds) noexcept
    {
        if (!std::isfinite(halfLifeSeconds) || halfLifeSeconds <= 0.0f)
            return 0.0f; // degenerate half-life == clear immediately
        if (!std::isfinite(deltaSeconds) || deltaSeconds <= 0.0f)
            return 1.0f;
        return std::exp2(-deltaSeconds / halfLifeSeconds);
    }

    /// Falloff of one splat at `worldXZ`, for a capsule running from `p0` to
    /// `p1` with radius `radius`. A point splat is p0 == p1.
    ///
    /// The capsule — rather than a disc per frame — is what stops a fast boat
    /// leaving a DOTTED trail. At 15 m/s and 60 Hz the hull moves 0.25 m per
    /// frame, which a 1.5 m disc covers; but one dropped frame, or an editor
    /// tick at 10 Hz, opens a gap that then persists for the whole decay tail.
    /// Stamping the swept segment closes it by construction rather than by
    /// hoping the frame rate holds.
    ///
    /// Deliberately expression-for-expression identical to
    /// `waterDisturbanceSplatWeight` in WaterDisturbanceCommon.glsl, down to
    /// the `max(x, 1e-4)` guards rather than the `isfinite ? : ` form the rest
    /// of the engine uses for deserialized floats. Non-finite inputs are
    /// rejected one layer up, at WaterDisturbanceSystem::SubmitSplat, precisely
    /// so this function can stay a literal mirror — a CPU/GPU pair that
    /// sanitises differently agrees on every value anyone tests and disagrees
    /// on the ones nobody does.
    [[nodiscard("the falloff weight is the only effect")]]
    inline f32 SplatWeight(glm::vec2 worldXZ, glm::vec2 p0, glm::vec2 p1, f32 radius, f32 softness) noexcept
    {
        const f32 safeRadius = glm::max(radius, 1.0e-4f);
        const glm::vec2 seg = p1 - p0;
        const f32 segLenSq = glm::dot(seg, seg);
        // Projection of worldXZ onto the segment, clamped to its ends.
        const f32 t = (segLenSq > 1.0e-12f)
                          ? glm::clamp(glm::dot(worldXZ - p0, seg) / segLenSq, 0.0f, 1.0f)
                          : 0.0f;
        const glm::vec2 d = worldXZ - (p0 + seg * t);
        const f32 normalized = std::sqrt(glm::dot(d, d)) / safeRadius;
        if (normalized >= 1.0f)
            return 0.0f;
        return std::pow(1.0f - normalized, glm::max(softness, 1.0e-4f));
    }

    /// Combine an existing field value with one splat's contribution.
    ///
    /// `max`, deliberately, NOT an accumulate. Both reasons are about
    /// determinism rather than looks:
    ///   * accumulation scales with FRAME RATE (twice the frames over the same
    ///     water = twice the foam), which is exactly what the exponential decay
    ///     above exists to avoid;
    ///   * a boat holding station would ramp to full white and stay there,
    ///     because nothing bounds a sum of overlapping splats.
    /// With `max`, a stationary boat holds its churn at the splat's own
    /// strength and the decay tail starts the moment it leaves.
    [[nodiscard("the combined intensity is the only effect")]]
    inline f32 CombineSplat(f32 existing, f32 splatIntensity) noexcept
    {
        return glm::clamp(glm::max(existing, splatIntensity), 0.0f, 1.0f);
    }

    // -------------------------------------------------------------------------
    // Sampling
    // -------------------------------------------------------------------------

    /// UV for a REPEAT-wrapped sample of the field at absolute world XZ.
    /// Mirrors `waterDisturbanceFieldUV` in WaterDisturbanceCommon.glsl.
    [[nodiscard("the uv is the only effect")]]
    inline glm::vec2 FieldUV(glm::vec2 absoluteWorldXZ) noexcept
    {
        return absoluteWorldXZ * kInvFieldExtentMetres;
    }

    /// Fade applied to the sampled field near the window boundary, where the
    /// torus seam puts unrelated content one texel away. `windowCentreXZ` is
    /// the world centre of the current window (WindowCentreWorld).
    ///
    /// Also the only thing stopping the field ending in a hard square edge, so
    /// it is not merely a seam patch.
    [[nodiscard("the fade weight is the only effect")]]
    inline f32 EdgeFade(glm::vec2 absoluteWorldXZ, glm::vec2 windowCentreXZ) noexcept
    {
        const glm::vec2 d = glm::abs(absoluteWorldXZ - windowCentreXZ) * kInvFieldExtentMetres;
        const f32 m = glm::max(d.x, d.y); // 0 at the centre, 0.5 at the boundary
        return 1.0f - glm::clamp((m - kEdgeFadeStart) / (0.5f - kEdgeFadeStart), 0.0f, 1.0f);
    }
} // namespace OloEngine::WaterDisturbance
