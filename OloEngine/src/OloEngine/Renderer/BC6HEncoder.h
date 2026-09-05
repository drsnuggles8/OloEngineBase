#pragma once

#include "OloEngine/Core/Base.h"

// From-scratch BC6H block encoder (#624, replacing the mode-11-only encoder from #440).
//
// BC6H stores an HDR RGB 4x4 block in 16 bytes using one of 14 block modes: ten
// two-subset modes (0-9, a 5-bit partition index selecting one of 32 fixed shapes and
// 3-bit interpolation indices) and four one-subset modes (10-13, 4-bit indices). The
// modes trade endpoint precision against endpoint *range*: mode 13 stores a 16-bit base
// endpoint but only a 4-bit delta to the second, mode 9 stores four fully explicit but
// 6-bit endpoints, and so on. No single mode is best; a good encoder tries several and
// keeps the one that actually decodes closest.
//
// That is what this encoder does. For every block it fits endpoints by PCA (covariance
// power iteration), evaluates every mode it can represent the block in, scores each by
// the true error a conformant decoder would produce, and packs the winner. Because the
// old mode-10 (bit pattern 0b00011) path is still one of the candidates, per-block
// quality can never regress relative to #440's encoder given the same endpoint fit.
//
// The bit layouts are the exact inverse of the vendored bcdec reference decoder's
// per-mode read sequences, and BC6HModeCoverageTest proves that per mode by comparing
// the encoder's own predicted decode against bcdec, bit for bit.
//
// This header is engine-internal (only TextureCompression.cpp and the tests include it)
// and deliberately has no renderer / GL dependency.

namespace OloEngine::BC6H
{
    // Number of BC6H block modes. Indices are bcdec's internal numbering (0-13), which
    // is the DirectX "mode 1..14" table minus one.
    inline constexpr u32 kModeCount = 14;

    // Encode one 4x4 block of RGB floats (48 contiguous floats, row-major R,G,B per
    // texel) into a 16-byte BC6H block.
    //
    // `isSigned` selects the GL_COMPRESSED_RGB_BPTC_SIGNED_FLOAT variant, which can
    // represent negative components; the unsigned variant clamps them to 0. Non-finite
    // components are clamped to the largest finite half in both variants.
    void EncodeBlock(u8* dst, const f32* blockRGB, bool isSigned);

    // Test / diagnostic hook. Encodes `blockRGB` using ONLY `modeIndex` (0-13) and, in
    // `outPredictedHalf`, writes the 48 half-float bit patterns (16 texels x R,G,B) the
    // encoder predicts a conformant decoder will produce for the block it just packed.
    //
    // BC6HModeCoverageTest compares that prediction against bcdec's decode. A mistake in
    // one mode's field table shifts bits and the prediction stops matching, so a
    // transcription error cannot hide behind a matched encoder/decoder codepath the way
    // it would if the test decoded with our own assumptions.
    //
    // Returns false only for an out-of-range `modeIndex`; every mode can represent every
    // block (badly, if it must), so there is no "unrepresentable" outcome to report.
    [[nodiscard]] bool EncodeBlockForModeForTest(u8* dst, const f32* blockRGB, bool isSigned,
                                                 u32 modeIndex, u16* outPredictedHalf);
} // namespace OloEngine::BC6H
