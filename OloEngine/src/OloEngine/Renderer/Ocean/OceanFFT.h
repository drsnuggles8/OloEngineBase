#pragma once

#include "OloEngine/Core/Base.h"

#include <complex>
#include <vector>

namespace OloEngine::Ocean
{
    // =========================================================================
    // CPU radix-2 Cooley-Tukey FFT — the reference implementation for the
    // Tessendorf FFT ocean (docs/design/WATER_FUTURE_IMPROVEMENTS.md §1).
    //
    // This is a *reference*: it pins the spectrum + inverse-FFT math on the CPU
    // so the GPU butterfly compute port (Phase 2) can be validated against a
    // known-correct result, and so gameplay/physics (buoyancy) can sample the
    // exact ocean the GPU renders without a readback. Correctness and clarity
    // matter more than raw speed here — the per-frame GPU path does the heavy
    // lifting in production.
    //
    // Conventions (so callers and tests agree):
    //   * Forward transform uses the e^{-i 2π kn/N} kernel, inverse e^{+i ...},
    //     and the inverse divides by N (so Inverse(Forward(x)) == x).
    //   * Frequencies are in natural FFT order: index 0 is DC, indices
    //     1..N/2-1 are positive frequencies, N/2 is Nyquist, N/2+1..N-1 are the
    //     negative frequencies. The signed frequency of index n is
    //     SignedFrequency(n, N) below. This is the ordering the GPU butterfly
    //     passes produce, so the CPU and GPU layouts match bit-for-bit.
    //   * N must be a power of two (IsPowerOfTwo()).
    // =========================================================================

    using Complex = std::complex<f32>;

    /// True when v is a power of two and non-zero.
    [[nodiscard]] constexpr bool IsPowerOfTwo(u32 v) noexcept
    {
        return v != 0u && (v & (v - 1u)) == 0u;
    }

    /// Signed frequency index for FFT bin `n` of an `N`-point transform.
    /// Returns n for the lower half [0, N/2] and n-N for the upper half, so the
    /// wave vector of bin n is 2π * SignedFrequency(n, N) / L.
    [[nodiscard]] constexpr i32 SignedFrequency(u32 n, u32 N) noexcept
    {
        return (n <= N / 2u) ? static_cast<i32>(n) : static_cast<i32>(n) - static_cast<i32>(N);
    }

    /// In-place 1D FFT. `inverse == true` selects the e^{+i...} kernel and
    /// divides by N. `data.size()` must be a power of two.
    void FFT1D(std::vector<Complex>& data, bool inverse);

    /// In-place 2D FFT of a row-major width*height grid (transform rows then
    /// columns). `width` and `height` must both be powers of two. `inverse`
    /// selects the inverse transform (e^{+i...}, divided by width*height).
    void FFT2D(std::vector<Complex>& grid, u32 width, u32 height, bool inverse);
} // namespace OloEngine::Ocean
