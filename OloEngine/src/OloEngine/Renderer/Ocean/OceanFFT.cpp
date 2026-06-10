#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Ocean/OceanFFT.h"

#include <cmath>

// =============================================================================
// Iterative radix-2 Cooley-Tukey FFT. Bit-reversal permutation followed by
// log2(N) butterfly stages. The same butterfly structure the GPU compute port
// implements as ping-pong passes — keeping the CPU reference and the GPU shader
// on the same algorithm makes a direct numerical comparison meaningful.
// =============================================================================

namespace OloEngine::Ocean
{
    namespace
    {
        constexpr f32 kTwoPi = 6.28318530717958647692f;

        // In-place bit-reversal reordering of an N-element (power-of-two) array.
        void BitReverseReorder(std::vector<Complex>& data)
        {
            const u32 n = static_cast<u32>(data.size());
            for (u32 i = 1u, j = 0u; i < n; ++i)
            {
                u32 bit = n >> 1u;
                for (; (j & bit) != 0u; bit >>= 1u)
                    j ^= bit;
                j ^= bit;
                if (i < j)
                    std::swap(data[i], data[j]);
            }
        }
    } // namespace

    void FFT1D(std::vector<Complex>& data, bool inverse)
    {
        const u32 n = static_cast<u32>(data.size());
        OLO_CORE_ASSERT(IsPowerOfTwo(n), "FFT1D size must be a power of two");
        if (n <= 1u)
            return;

        BitReverseReorder(data);

        // Butterfly stages: length doubles each stage (2, 4, ..., N).
        const f32 sign = inverse ? 1.0f : -1.0f;
        for (u32 len = 2u; len <= n; len <<= 1u)
        {
            const f32 theta = sign * kTwoPi / static_cast<f32>(len);
            const Complex wLen(std::cos(theta), std::sin(theta));
            for (u32 start = 0u; start < n; start += len)
            {
                Complex w(1.0f, 0.0f);
                const u32 half = len >> 1u;
                for (u32 k = 0u; k < half; ++k)
                {
                    const Complex u = data[start + k];
                    const Complex v = data[start + k + half] * w;
                    data[start + k] = u + v;
                    data[start + k + half] = u - v;
                    w *= wLen;
                }
            }
        }

        if (inverse)
        {
            const f32 invN = 1.0f / static_cast<f32>(n);
            for (Complex& c : data)
                c *= invN;
        }
    }

    void FFT2D(std::vector<Complex>& grid, u32 width, u32 height, bool inverse)
    {
        OLO_CORE_ASSERT(IsPowerOfTwo(width) && IsPowerOfTwo(height), "FFT2D dims must be powers of two");
        OLO_CORE_ASSERT(grid.size() == static_cast<sizet>(width) * height, "FFT2D grid size mismatch");

        // Transform each row in place.
        std::vector<Complex> row(width);
        for (u32 y = 0u; y < height; ++y)
        {
            const sizet base = static_cast<sizet>(y) * width;
            for (u32 x = 0u; x < width; ++x)
                row[x] = grid[base + x];
            FFT1D(row, inverse);
            for (u32 x = 0u; x < width; ++x)
                grid[base + x] = row[x];
        }

        // Transform each column in place.
        std::vector<Complex> col(height);
        for (u32 x = 0u; x < width; ++x)
        {
            for (u32 y = 0u; y < height; ++y)
                col[y] = grid[static_cast<sizet>(y) * width + x];
            FFT1D(col, inverse);
            for (u32 y = 0u; y < height; ++y)
                grid[static_cast<sizet>(y) * width + x] = col[y];
        }
    }
} // namespace OloEngine::Ocean
