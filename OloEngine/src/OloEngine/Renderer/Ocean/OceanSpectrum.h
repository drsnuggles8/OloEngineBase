#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Ocean/OceanFFT.h"

#include <glm/glm.hpp>

#include <vector>

namespace OloEngine::Ocean
{
    // =========================================================================
    // CPU Tessendorf ocean spectrum + field evaluation
    // (docs/WATER_FUTURE_IMPROVEMENTS.md §1.1 / §1.4).
    //
    // Pipeline (all on the CPU, the reference for the GPU compute port and the
    // physics/buoyancy sampler):
    //   1. GenerateH0()      — Phillips spectrum → frequency-domain h0(k).
    //   2. EvaluateField()   — time-evolve h0 with the deep-water dispersion
    //                          relation, then inverse-FFT to a spatial tile of
    //                          height + horizontal displacement + normal +
    //                          Jacobian (folding, for foam).
    //
    // The produced tile is periodic with period == PatchSize, so it can be
    // wrapped seamlessly across the world. Frequencies use the FFT ordering
    // defined in OceanFFT.h so the layout matches the GPU butterfly output.
    // =========================================================================

    /// Which statistical energy spectrum drives the base heightfield h0(k).
    /// Phillips is Tessendorf's classic model; JONSWAP (§1.4) adds a sharper,
    /// fetch-limited peak for a more realistic Atlantic/Pacific swell. The
    /// underlying value is serialized — keep the numbering stable.
    enum class SpectrumType : u32
    {
        Phillips = 0,
        JONSWAP = 1,
    };

    struct SpectrumParams
    {
        u32 m_Resolution = 256;                     ///< N, FFT grid size (power of two)
        f32 m_PatchSize = 250.0f;                   ///< L, world-space tile size (metres)
        f32 m_WindSpeed = 20.0f;                    ///< V, wind speed (m/s) — sets the dominant wavelength
        glm::vec2 m_WindDirection = { 1.0f, 0.0f }; ///< wind heading (normalised internally)
        f32 m_Amplitude = 4.0f;                     ///< A, global energy scale (both spectra)
        f32 m_Gravity = 9.81f;                      ///< g, gravitational acceleration
        f32 m_SmallWaveSuppression = 1.0f;          ///< l, cutoff length for exp(-k²l²) high-freq damping (0 disables)
        f32 m_DirectionalExponent = 2.0f;           ///< exponent on |k̂·ŵ| (2 = classic Phillips cos²)
        f32 m_Choppiness = 1.0f;                    ///< λ, horizontal-displacement (choppy crests) scale
        u32 m_Seed = 1337u;                         ///< RNG seed for the Gaussian spectrum (deterministic)

        // JONSWAP-only shape parameters (ignored when m_SpectrumType == Phillips).
        SpectrumType m_SpectrumType = SpectrumType::Phillips; ///< which energy spectrum to evaluate
        f32 m_JonswapGamma = 3.3f;                            ///< γ peak-enhancement (1 ≈ Pierson-Moskowitz, 3.3 = mean JONSWAP)
        f32 m_JonswapFetch = 100000.0f;                       ///< F, fetch (m) the wind blows over — sets the peak frequency
    };

    /// Phillips spectrum energy at wave vector `k` (units m²). Returns 0 at k=0
    /// and applies the wind-direction term and high-frequency suppression.
    [[nodiscard]] f32 PhillipsSpectrum(const SpectrumParams& params, glm::vec2 k);

    /// JONSWAP (Joint North Sea Wave Project, §1.4) spectral energy at wave
    /// vector `k`. The 1-D fetch-limited frequency spectrum S(ω) — α·g²/ω⁵
    /// envelope, exp(-5/4·(ωₚ/ω)⁴) low-frequency rolloff, and γ^r peak
    /// enhancement — is mapped to a 2-D wave-vector density via the deep-water
    /// dispersion ω=√(g·k) and the polar Jacobian, then given the same
    /// directional term + small-wave suppression as Phillips so the wind
    /// controls behave identically. The peak frequency ωₚ derives from
    /// m_WindSpeed and m_JonswapFetch. Returns 0 at k=0.
    [[nodiscard]] f32 JonswapSpectrum(const SpectrumParams& params, glm::vec2 k);

    /// Energy at wave vector `k` for the spectrum selected by
    /// m_SpectrumType — dispatches to PhillipsSpectrum / JonswapSpectrum.
    /// GenerateH0() builds the base heightfield through this.
    [[nodiscard]] f32 SpectrumEnergy(const SpectrumParams& params, glm::vec2 k);

    /// Deep-water dispersion relation ω(k) = sqrt(g·|k|).
    [[nodiscard]] f32 Dispersion(f32 kMagnitude, f32 gravity);

    /// Initial frequency-domain heightfield h0(k) — FFT-ordered, row-major
    /// N×N. h0(k) = (1/√2)(ξr + iξi)·√(Phillips(k)) with ξ ~ N(0,1).
    /// Deterministic for a fixed seed/params.
    [[nodiscard]] std::vector<Complex> GenerateH0(const SpectrumParams& params);

    /// Extract the low-|k| band of a full-resolution h0 into a
    /// targetResolution² spectrum (same patch size ⇒ same wave vectors per
    /// signed frequency), so the small-grid IFFT reproduces the band-limited
    /// full surface exactly — same phases, same metres — minus only the
    /// high-frequency detail the small grid cannot represent. Copied bins are
    /// scaled by (target/source)² to cancel the grid-size-dependent 1/N²
    /// factor FFT2D's inverse applies, and the small grid's Nyquist
    /// row/column is zeroed (the full grid keeps ±N'/2 as distinct bins; the
    /// small grid cannot, and Phillips energy there is negligible). Used for
    /// the low-res CPU physics proxy that keeps SampleHeight() working while
    /// the GPU owns the rendered field.
    /// Returns a copy when targetResolution >= the source resolution.
    [[nodiscard]] std::vector<Complex> ExtractBandLimitedH0(const std::vector<Complex>& h0, u32 resolution,
                                                            u32 targetResolution);

    /// One fully-evaluated ocean tile at a given time. All grids are row-major
    /// N×N (index = y*N + x); x/y step PatchSize/N metres in world XZ.
    struct DisplacementField
    {
        u32 m_Resolution = 0u;
        std::vector<f32> m_Height;                       ///< vertical displacement (metres)
        std::vector<glm::vec2> m_HorizontalDisplacement; ///< (dx, dz) choppy displacement (metres)
        std::vector<glm::vec3> m_Normal;                 ///< unit surface normal
        std::vector<f32> m_Jacobian;                     ///< folding determinant (<1 ⇒ compression/foam)

        [[nodiscard]] bool IsValid() const noexcept
        {
            return m_Resolution > 0u && m_Height.size() == static_cast<sizet>(m_Resolution) * m_Resolution;
        }
    };

    /// Time-evolve `h0` to `time` seconds and inverse-FFT to a spatial tile.
    /// `h0` must be the GenerateH0() output for the same params.
    [[nodiscard]] DisplacementField EvaluateField(const SpectrumParams& params, const std::vector<Complex>& h0, f32 time);

    /// Bilinearly sample a field's height at world XZ, wrapping by PatchSize.
    /// Convenience for CPU consumers (e.g. buoyancy) — returns 0 for an empty
    /// field.
    [[nodiscard]] f32 SampleHeightBilinear(const DisplacementField& field, f32 patchSize, glm::vec2 worldXZ);
} // namespace OloEngine::Ocean
