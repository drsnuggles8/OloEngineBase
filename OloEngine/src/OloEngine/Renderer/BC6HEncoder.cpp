#include "OloEnginePCH.h"
#include "OloEngine/Renderer/BC6HEncoder.h"

#include "OloEngine/Core/Assert.h"

// glm::packHalf1x16 — float -> IEEE half bit pattern, the space BC6H ultimately stores.
#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iterator>
#include <limits>

namespace OloEngine::BC6H
{
    namespace
    {
        // ---- Decoder-side constants, transcribed from the vendored bcdec ------------
        //
        // Everything in this block mirrors bcdec_bc6h_half(). The encoder inverts it, so
        // any divergence here is a silent quality/corruption bug — BC6HModeCoverageTest
        // pins the whole chain against bcdec per mode.

        // Interpolation weights (/64) for 3-bit (two-subset modes) and 4-bit (one-subset
        // modes) indices. Neither table is symmetric — aWeight4[13] is 55, not 56 — which
        // is why an endpoint swap has to re-select indices rather than invert them.
        constexpr std::array<i32, 8> kWeights3 = { 0, 9, 18, 27, 37, 46, 55, 64 };
        constexpr std::array<i32, 16> kWeights4 = { 0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64 };

        // The 32 two-subset partition shapes, in raster order. Bit 0 is the subset a
        // texel belongs to; bit 7 marks the two *anchor* texels (texel 0, always subset
        // 0's anchor, and the subset-1 fixup texel), whose index is stored with one bit
        // fewer because its high bit is implicitly zero.
        constexpr std::array<std::array<u8, 16>, 32> kPartitions = { {
            { 0x80, 0x00, 0x01, 0x01, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00, 0x01, 0x81 },
            { 0x80, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x81 },
            { 0x80, 0x01, 0x01, 0x01, 0x00, 0x01, 0x01, 0x01, 0x00, 0x01, 0x01, 0x01, 0x00, 0x01, 0x01, 0x81 },
            { 0x80, 0x00, 0x00, 0x01, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00, 0x01, 0x01, 0x00, 0x01, 0x01, 0x81 },
            { 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x01, 0x81 },
            { 0x80, 0x00, 0x01, 0x01, 0x00, 0x01, 0x01, 0x01, 0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x81 },
            { 0x80, 0x00, 0x00, 0x01, 0x00, 0x00, 0x01, 0x01, 0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x81 },
            { 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x01, 0x01, 0x00, 0x01, 0x01, 0x81 },
            { 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x01, 0x81 },
            { 0x80, 0x00, 0x01, 0x01, 0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x81 },
            { 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x81 },
            { 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x01, 0x81 },
            { 0x80, 0x00, 0x00, 0x01, 0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x81 },
            { 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x81 },
            { 0x80, 0x00, 0x00, 0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x81 },
            { 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01, 0x01, 0x81 },
            { 0x80, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x01, 0x01, 0x00, 0x01, 0x01, 0x01, 0x81 },
            { 0x80, 0x01, 0x81, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
            { 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x81, 0x00, 0x00, 0x00, 0x01, 0x01, 0x01, 0x00 },
            { 0x80, 0x01, 0x81, 0x01, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00 },
            { 0x80, 0x00, 0x81, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
            { 0x80, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x81, 0x01, 0x00, 0x00, 0x01, 0x01, 0x01, 0x00 },
            { 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x81, 0x00, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00 },
            { 0x80, 0x01, 0x01, 0x01, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00, 0x81 },
            { 0x80, 0x00, 0x81, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00 },
            { 0x80, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x81, 0x00, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00 },
            { 0x80, 0x01, 0x81, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00, 0x01, 0x01, 0x00 },
            { 0x80, 0x00, 0x81, 0x01, 0x00, 0x01, 0x01, 0x00, 0x00, 0x01, 0x01, 0x00, 0x01, 0x01, 0x00, 0x00 },
            { 0x80, 0x00, 0x00, 0x01, 0x00, 0x01, 0x01, 0x01, 0x81, 0x01, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00 },
            { 0x80, 0x00, 0x00, 0x00, 0x01, 0x01, 0x01, 0x01, 0x81, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00 },
            { 0x80, 0x01, 0x81, 0x01, 0x00, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00, 0x01, 0x01, 0x01, 0x00 },
            { 0x80, 0x00, 0x81, 0x01, 0x01, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00, 0x01, 0x01, 0x01, 0x00, 0x00 },
        } };

        // ---- Mode descriptions -------------------------------------------------------

        // One field of a packed block, in bitstream order. `Slot` addresses the endpoint
        // component the field carries: endpoints are numbered w,x,y,z (0-3) and slot =
        // endpoint * 3 + channel, with slot 12 meaning the 5-bit partition index.
        // `Reversed` mirrors bcdec's read_bits_r — modes 12 and 13 store the high bits of
        // their base endpoint in reverse bit order.
        struct FieldSpec
        {
            u8 Slot;
            u8 Shift;
            u8 Bits;
            u8 Reversed;
        };

        constexpr u8 kPartitionSlot = 12;
        constexpr u32 kMaxFields = 24;

        struct ModeSpec
        {
            u8 ModeBits;             // width of the leading mode field: 2 or 5
            u8 ModeValue;            // its value, read LSB-first
            u8 BaseBits;             // precision every endpoint is unquantized at
            std::array<u8, 3> Delta; // per-channel delta width; equals BaseBits when HasDelta is false
            u8 HasDelta;             // 0 for the two explicit-endpoint modes (9 and 10)
            u8 Subsets;              // 1 or 2
            u8 FieldCount;
            std::array<FieldSpec, kMaxFields> Fields;
        };

        // Field tables generated from bcdec_bc6h_half()'s per-mode read sequence, so
        // packing is that sequence run backwards. Endpoint precisions match bcdec's
        // actual_bits_count[][].
        constexpr std::array<ModeSpec, kModeCount> kModes = { {
            // Mode 0 — 2 subsets, 10-bit base with 5,5,5 deltas.
            { 2, 0b00, 10, { 5, 5, 5 }, 1, 2, 20, { { { 7, 4, 1, 0 }, { 8, 4, 1, 0 }, { 11, 4, 1, 0 }, { 0, 0, 10, 0 }, { 1, 0, 10, 0 }, { 2, 0, 10, 0 }, { 3, 0, 5, 0 }, { 10, 4, 1, 0 }, { 7, 0, 4, 0 }, { 4, 0, 5, 0 }, { 11, 0, 1, 0 }, { 10, 0, 4, 0 }, { 5, 0, 5, 0 }, { 11, 1, 1, 0 }, { 8, 0, 4, 0 }, { 6, 0, 5, 0 }, { 11, 2, 1, 0 }, { 9, 0, 5, 0 }, { 11, 3, 1, 0 }, { kPartitionSlot, 0, 5, 0 } } } },
            // Mode 1 — 2 subsets, 7-bit base with 6,6,6 deltas (widest two-subset range).
            { 2, 0b01, 7, { 6, 6, 6 }, 1, 2, 24, { { { 7, 5, 1, 0 }, { 10, 4, 1, 0 }, { 10, 5, 1, 0 }, { 0, 0, 7, 0 }, { 11, 0, 1, 0 }, { 11, 1, 1, 0 }, { 8, 4, 1, 0 }, { 1, 0, 7, 0 }, { 8, 5, 1, 0 }, { 11, 2, 1, 0 }, { 7, 4, 1, 0 }, { 2, 0, 7, 0 }, { 11, 3, 1, 0 }, { 11, 5, 1, 0 }, { 11, 4, 1, 0 }, { 3, 0, 6, 0 }, { 7, 0, 4, 0 }, { 4, 0, 6, 0 }, { 10, 0, 4, 0 }, { 5, 0, 6, 0 }, { 8, 0, 4, 0 }, { 6, 0, 6, 0 }, { 9, 0, 6, 0 }, { kPartitionSlot, 0, 5, 0 } } } },
            // Mode 2 — 2 subsets, 11-bit base with 5,4,4 deltas.
            { 5, 0b00010, 11, { 5, 4, 4 }, 1, 2, 19, { { { 0, 0, 10, 0 }, { 1, 0, 10, 0 }, { 2, 0, 10, 0 }, { 3, 0, 5, 0 }, { 0, 10, 1, 0 }, { 7, 0, 4, 0 }, { 4, 0, 4, 0 }, { 1, 10, 1, 0 }, { 11, 0, 1, 0 }, { 10, 0, 4, 0 }, { 5, 0, 4, 0 }, { 2, 10, 1, 0 }, { 11, 1, 1, 0 }, { 8, 0, 4, 0 }, { 6, 0, 5, 0 }, { 11, 2, 1, 0 }, { 9, 0, 5, 0 }, { 11, 3, 1, 0 }, { kPartitionSlot, 0, 5, 0 } } } },
            // Mode 3 — 2 subsets, 11-bit base with 4,5,4 deltas.
            { 5, 0b00110, 11, { 4, 5, 4 }, 1, 2, 21, { { { 0, 0, 10, 0 }, { 1, 0, 10, 0 }, { 2, 0, 10, 0 }, { 3, 0, 4, 0 }, { 0, 10, 1, 0 }, { 10, 4, 1, 0 }, { 7, 0, 4, 0 }, { 4, 0, 5, 0 }, { 1, 10, 1, 0 }, { 10, 0, 4, 0 }, { 5, 0, 4, 0 }, { 2, 10, 1, 0 }, { 11, 1, 1, 0 }, { 8, 0, 4, 0 }, { 6, 0, 4, 0 }, { 11, 0, 1, 0 }, { 11, 2, 1, 0 }, { 9, 0, 4, 0 }, { 7, 4, 1, 0 }, { 11, 3, 1, 0 }, { kPartitionSlot, 0, 5, 0 } } } },
            // Mode 4 — 2 subsets, 11-bit base with 4,4,5 deltas.
            { 5, 0b01010, 11, { 4, 4, 5 }, 1, 2, 21, { { { 0, 0, 10, 0 }, { 1, 0, 10, 0 }, { 2, 0, 10, 0 }, { 3, 0, 4, 0 }, { 0, 10, 1, 0 }, { 8, 4, 1, 0 }, { 7, 0, 4, 0 }, { 4, 0, 4, 0 }, { 1, 10, 1, 0 }, { 11, 0, 1, 0 }, { 10, 0, 4, 0 }, { 5, 0, 5, 0 }, { 2, 10, 1, 0 }, { 8, 0, 4, 0 }, { 6, 0, 4, 0 }, { 11, 1, 1, 0 }, { 11, 2, 1, 0 }, { 9, 0, 4, 0 }, { 11, 4, 1, 0 }, { 11, 3, 1, 0 }, { kPartitionSlot, 0, 5, 0 } } } },
            // Mode 5 — 2 subsets, 9-bit base with 5,5,5 deltas.
            { 5, 0b01110, 9, { 5, 5, 5 }, 1, 2, 20, { { { 0, 0, 9, 0 }, { 8, 4, 1, 0 }, { 1, 0, 9, 0 }, { 7, 4, 1, 0 }, { 2, 0, 9, 0 }, { 11, 4, 1, 0 }, { 3, 0, 5, 0 }, { 10, 4, 1, 0 }, { 7, 0, 4, 0 }, { 4, 0, 5, 0 }, { 11, 0, 1, 0 }, { 10, 0, 4, 0 }, { 5, 0, 5, 0 }, { 11, 1, 1, 0 }, { 8, 0, 4, 0 }, { 6, 0, 5, 0 }, { 11, 2, 1, 0 }, { 9, 0, 5, 0 }, { 11, 3, 1, 0 }, { kPartitionSlot, 0, 5, 0 } } } },
            // Mode 6 — 2 subsets, 8-bit base with 6,5,5 deltas.
            { 5, 0b10010, 8, { 6, 5, 5 }, 1, 2, 20, { { { 0, 0, 8, 0 }, { 10, 4, 1, 0 }, { 8, 4, 1, 0 }, { 1, 0, 8, 0 }, { 11, 2, 1, 0 }, { 7, 4, 1, 0 }, { 2, 0, 8, 0 }, { 11, 3, 1, 0 }, { 11, 4, 1, 0 }, { 3, 0, 6, 0 }, { 7, 0, 4, 0 }, { 4, 0, 5, 0 }, { 11, 0, 1, 0 }, { 10, 0, 4, 0 }, { 5, 0, 5, 0 }, { 11, 1, 1, 0 }, { 8, 0, 4, 0 }, { 6, 0, 6, 0 }, { 9, 0, 6, 0 }, { kPartitionSlot, 0, 5, 0 } } } },
            // Mode 7 — 2 subsets, 8-bit base with 5,6,5 deltas.
            { 5, 0b10110, 8, { 5, 6, 5 }, 1, 2, 22, { { { 0, 0, 8, 0 }, { 11, 0, 1, 0 }, { 8, 4, 1, 0 }, { 1, 0, 8, 0 }, { 7, 5, 1, 0 }, { 7, 4, 1, 0 }, { 2, 0, 8, 0 }, { 10, 5, 1, 0 }, { 11, 4, 1, 0 }, { 3, 0, 5, 0 }, { 10, 4, 1, 0 }, { 7, 0, 4, 0 }, { 4, 0, 6, 0 }, { 10, 0, 4, 0 }, { 5, 0, 5, 0 }, { 11, 1, 1, 0 }, { 8, 0, 4, 0 }, { 6, 0, 5, 0 }, { 11, 2, 1, 0 }, { 9, 0, 5, 0 }, { 11, 3, 1, 0 }, { kPartitionSlot, 0, 5, 0 } } } },
            // Mode 8 — 2 subsets, 8-bit base with 5,5,6 deltas.
            { 5, 0b11010, 8, { 5, 5, 6 }, 1, 2, 22, { { { 0, 0, 8, 0 }, { 11, 1, 1, 0 }, { 8, 4, 1, 0 }, { 1, 0, 8, 0 }, { 8, 5, 1, 0 }, { 7, 4, 1, 0 }, { 2, 0, 8, 0 }, { 11, 5, 1, 0 }, { 11, 4, 1, 0 }, { 3, 0, 5, 0 }, { 10, 4, 1, 0 }, { 7, 0, 4, 0 }, { 4, 0, 5, 0 }, { 11, 0, 1, 0 }, { 10, 0, 4, 0 }, { 5, 0, 6, 0 }, { 8, 0, 4, 0 }, { 6, 0, 5, 0 }, { 11, 2, 1, 0 }, { 9, 0, 5, 0 }, { 11, 3, 1, 0 }, { kPartitionSlot, 0, 5, 0 } } } },
            // Mode 9 — 2 subsets, four fully explicit 6-bit endpoints. Coarse, but the
            // only two-subset mode that can represent ANY pair of endpoint clusters, so
            // it is the two-subset fallback for a block with a huge dynamic range.
            { 5, 0b11110, 6, { 6, 6, 6 }, 0, 2, 24, { { { 0, 0, 6, 0 }, { 10, 4, 1, 0 }, { 11, 0, 1, 0 }, { 11, 1, 1, 0 }, { 8, 4, 1, 0 }, { 1, 0, 6, 0 }, { 7, 5, 1, 0 }, { 8, 5, 1, 0 }, { 11, 2, 1, 0 }, { 7, 4, 1, 0 }, { 2, 0, 6, 0 }, { 10, 5, 1, 0 }, { 11, 3, 1, 0 }, { 11, 5, 1, 0 }, { 11, 4, 1, 0 }, { 3, 0, 6, 0 }, { 7, 0, 4, 0 }, { 4, 0, 6, 0 }, { 10, 0, 4, 0 }, { 5, 0, 6, 0 }, { 8, 0, 4, 0 }, { 6, 0, 6, 0 }, { 9, 0, 6, 0 }, { kPartitionSlot, 0, 5, 0 } } } },
            // Mode 10 — 1 subset, two explicit 10-bit endpoints. #440's only mode; kept
            // as a candidate so this encoder can never lose to it on a block.
            { 5, 0b00011, 10, { 10, 10, 10 }, 0, 1, 6, { { { 0, 0, 10, 0 }, { 1, 0, 10, 0 }, { 2, 0, 10, 0 }, { 3, 0, 10, 0 }, { 4, 0, 10, 0 }, { 5, 0, 10, 0 } } } },
            // Mode 11 — 1 subset, 11-bit base with 9-bit deltas.
            { 5, 0b00111, 11, { 9, 9, 9 }, 1, 1, 9, { { { 0, 0, 10, 0 }, { 1, 0, 10, 0 }, { 2, 0, 10, 0 }, { 3, 0, 9, 0 }, { 0, 10, 1, 0 }, { 4, 0, 9, 0 }, { 1, 10, 1, 0 }, { 5, 0, 9, 0 }, { 2, 10, 1, 0 } } } },
            // Mode 12 — 1 subset, 12-bit base with 8-bit deltas.
            { 5, 0b01011, 12, { 8, 8, 8 }, 1, 1, 9, { { { 0, 0, 10, 0 }, { 1, 0, 10, 0 }, { 2, 0, 10, 0 }, { 3, 0, 8, 0 }, { 0, 10, 2, 1 }, { 4, 0, 8, 0 }, { 1, 10, 2, 1 }, { 5, 0, 8, 0 }, { 2, 10, 2, 1 } } } },
            // Mode 13 — 1 subset, full 16-bit base with 4-bit deltas. Endpoints are
            // stored without loss, so a flat or very low-contrast block is exact.
            { 5, 0b01111, 16, { 4, 4, 4 }, 1, 1, 9, { { { 0, 0, 10, 0 }, { 1, 0, 10, 0 }, { 2, 0, 10, 0 }, { 3, 0, 4, 0 }, { 0, 10, 6, 1 }, { 4, 0, 4, 0 }, { 1, 10, 6, 1 }, { 5, 0, 4, 0 }, { 2, 10, 6, 1 } } } },
        } };

        constexpr f32 kMaxFiniteHalf = 65504.0f;
        constexpr i32 kInterpMaxUnsigned = 0xFFFF;
        constexpr i32 kInterpMaxSigned = 0x7FFF;

        // ---- Value-space conversions -------------------------------------------------
        //
        // BC6H interpolates in a 16-bit "interpolation space" and then scales the result
        // into a half-float bit pattern (finish_unquantize). Encoding therefore works in
        // interpolation space throughout, and the source float is mapped into it by
        // taking its half bit pattern and inverting that final scale.

        i32 InterpMax(bool isSigned)
        {
            return isSigned ? kInterpMaxSigned : kInterpMaxUnsigned;
        }

        i32 InterpMin(bool isSigned)
        {
            return isSigned ? -kInterpMaxSigned : 0;
        }

        // Source float -> interpolation space. Unsigned BC6H cannot store negatives, so
        // they clamp to 0; non-finite values clamp to the largest finite half.
        i32 FloatToInterp(f32 value, bool isSigned)
        {
            if (!std::isfinite(value))
                value = value > 0.0f ? kMaxFiniteHalf : (isSigned && value < 0.0f ? -kMaxFiniteHalf : 0.0f);
            value = std::clamp(value, isSigned ? -kMaxFiniteHalf : 0.0f, kMaxFiniteHalf);

            const u32 halfBits = static_cast<u32>(::glm::packHalf1x16(value)) & 0xFFFFu;
            if (!isSigned)
            {
                // finish_unquantize (unsigned): half = (interp * 31) >> 6.
                const i32 interp = static_cast<i32>((static_cast<i64>(halfBits) * 64 + 15) / 31);
                return std::clamp(interp, 0, kInterpMaxUnsigned);
            }
            // finish_unquantize (signed): magnitude = (|interp| * 31) >> 5, sign kept apart.
            const i32 magnitude = static_cast<i32>(halfBits & 0x7FFFu);
            const i32 interp = static_cast<i32>((static_cast<i64>(magnitude) * 32 + 15) / 31);
            const i32 clamped = std::clamp(interp, 0, kInterpMaxSigned);
            return (halfBits & 0x8000u) != 0 ? -clamped : clamped;
        }

        // bcdec__finish_unquantize: interpolation space -> half-float bit pattern. Only
        // the test hook needs this, to state what the encoder expects a decoder to emit.
        u16 FinishUnquantize(i32 value, bool isSigned)
        {
            if (!isSigned)
                return static_cast<u16>((value * 31) >> 6);
            const i32 scaled = value < 0 ? -(((-value) * 31) >> 5) : ((value * 31) >> 5);
            return scaled < 0 ? static_cast<u16>(0x8000 | (-scaled)) : static_cast<u16>(scaled);
        }

        // bcdec__unquantize: a `bits`-precision endpoint -> interpolation space.
        i32 Unquantize(i32 value, u32 bits, bool isSigned)
        {
            if (!isSigned)
            {
                if (bits >= 15)
                    return value;
                if (value == 0)
                    return 0;
                if (value == ((1 << bits) - 1))
                    return 0xFFFF;
                return ((value << 16) + 0x8000) >> bits;
            }
            if (bits >= 16)
                return value;

            const bool negative = value < 0;
            const i32 magnitude = negative ? -value : value;
            i32 unq = 0;
            if (magnitude == 0)
                unq = 0;
            else if (magnitude >= ((1 << (bits - 1)) - 1))
                unq = 0x7FFF;
            else
                unq = ((magnitude << 15) + 0x4000) >> (bits - 1);
            return negative ? -unq : unq;
        }

        // Inverse of Unquantize: the `bits`-precision endpoint whose unquantized value is
        // nearest `target`. Unquantize is monotone and near-linear, so a division-derived
        // guess plus a 5-wide sweep finds the exact nearest without a full search.
        i32 Quantize(i32 target, u32 bits, bool isSigned)
        {
            const i32 maxValue = isSigned ? ((1 << (bits - 1)) - 1) : ((1 << bits) - 1);
            const i32 magnitude = std::min(std::abs(target), isSigned ? kInterpMaxSigned : kInterpMaxUnsigned);
            const i32 guess = std::clamp(bits >= 16 ? magnitude : (magnitude >> (16 - bits)), 0, maxValue);

            i32 best = guess;
            i32 bestError = std::numeric_limits<i32>::max();
            for (i32 candidate = std::max(0, guess - 2); candidate <= std::min(maxValue, guess + 2); ++candidate)
            {
                const i32 error = std::abs(Unquantize(candidate, bits, isSigned) - magnitude);
                if (error < bestError)
                {
                    bestError = error;
                    best = candidate;
                }
            }
            return (isSigned && target < 0) ? -best : best;
        }

        // bcdec__interpolate.
        i32 Interpolate(i32 a, i32 b, i32 weight)
        {
            return (a * (64 - weight) + b * weight + 32) >> 6;
        }

        // A delta-mode endpoint is stored as a signed `deltaBits` offset from endpoint 0,
        // wrapped modulo 2^baseBits. Pull `value` into the window that can actually be
        // stored, rather than rejecting the mode — a clamped endpoint still competes, and
        // the candidate is scored on its true decoded error either way.
        i32 ClampToDeltaRange(i32 value, i32 base, u32 deltaBits, u32 baseBits, bool isSigned)
        {
            const i32 low = base - (1 << (deltaBits - 1));
            const i32 high = base + (1 << (deltaBits - 1)) - 1;
            const i32 valueMin = isSigned ? -((1 << (baseBits - 1)) - 1) : 0;
            const i32 valueMax = isSigned ? ((1 << (baseBits - 1)) - 1) : ((1 << baseBits) - 1);
            return std::clamp(value, std::max(low, valueMin), std::min(high, valueMax));
        }

        // ---- Endpoint fitting --------------------------------------------------------

        struct Endpoints
        {
            std::array<i32, 3> E0{};
            std::array<i32, 3> E1{};
        };

        using BlockTargets = std::array<std::array<i32, 3>, 16>;

        // Symmetric 3x3 covariance of the texels in `mask`, plus their mean. Returns the
        // texel count so callers can skip empty subsets (no partition produces one, but
        // the arithmetic below would divide by zero if one ever did).
        u32 Moments(const BlockTargets& target, u32 mask, std::array<f64, 3>& outMean, std::array<f64, 6>& outCov)
        {
            outMean = { 0.0, 0.0, 0.0 };
            outCov = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
            u32 count = 0;
            for (u32 t = 0; t < 16; ++t)
            {
                if ((mask & (1u << t)) == 0)
                    continue;
                ++count;
                for (u32 c = 0; c < 3; ++c)
                    outMean[c] += static_cast<f64>(target[t][c]);
            }
            if (count == 0)
                return 0;
            for (u32 c = 0; c < 3; ++c)
                outMean[c] /= static_cast<f64>(count);

            for (u32 t = 0; t < 16; ++t)
            {
                if ((mask & (1u << t)) == 0)
                    continue;
                const f64 dr = static_cast<f64>(target[t][0]) - outMean[0];
                const f64 dg = static_cast<f64>(target[t][1]) - outMean[1];
                const f64 db = static_cast<f64>(target[t][2]) - outMean[2];
                outCov[0] += dr * dr;
                outCov[1] += dr * dg;
                outCov[2] += dr * db;
                outCov[3] += dg * dg;
                outCov[4] += dg * db;
                outCov[5] += db * db;
            }
            return count;
        }

        // Dominant eigenvector of the symmetric covariance (packed rr,rg,rb,gg,gb,bb) by
        // power iteration, seeded from the axis with the largest variance so the
        // iteration starts inside the dominant subspace rather than orthogonal to it.
        // Returns the eigenvalue; `outAxis` is unit length, or zero for a degenerate
        // (single-point) cluster.
        f64 PrincipalAxis(const std::array<f64, 6>& cov, std::array<f64, 3>& outAxis)
        {
            const std::array<f64, 3> variance = { cov[0], cov[3], cov[5] };
            const auto seed = static_cast<u32>(std::distance(variance.begin(), std::ranges::max_element(variance)));
            if (variance[seed] <= 0.0)
            {
                outAxis = { 0.0, 0.0, 0.0 };
                return 0.0;
            }

            std::array<f64, 3> v = { 0.0, 0.0, 0.0 };
            v[seed] = 1.0;
            f64 eigenvalue = 0.0;
            for (u32 iteration = 0; iteration < 12; ++iteration)
            {
                const std::array<f64, 3> next = {
                    cov[0] * v[0] + cov[1] * v[1] + cov[2] * v[2],
                    cov[1] * v[0] + cov[3] * v[1] + cov[4] * v[2],
                    cov[2] * v[0] + cov[4] * v[1] + cov[5] * v[2],
                };
                const f64 length = std::sqrt(next[0] * next[0] + next[1] * next[1] + next[2] * next[2]);
                if (length <= 0.0)
                {
                    outAxis = { 0.0, 0.0, 0.0 };
                    return 0.0;
                }
                eigenvalue = length;
                for (u32 c = 0; c < 3; ++c)
                    v[c] = next[c] / length;
            }
            outAxis = v;
            return eigenvalue;
        }

        // PCA endpoint fit for one subset: project every texel onto the principal axis
        // through the mean and take the two extremes. `anchorTexel` is the texel whose
        // stored index loses its high bit, so the pair is oriented to put it nearest E0 —
        // that satisfies the anchor constraint by construction instead of by a later
        // endpoint swap, which would invalidate the delta encoding against endpoint 0.
        Endpoints FitEndpoints(const BlockTargets& target, u32 mask, u32 anchorTexel, bool isSigned)
        {
            std::array<f64, 3> mean{};
            std::array<f64, 6> cov{};
            Endpoints result;
            const u32 count = Moments(target, mask, mean, cov);
            const i32 lo = InterpMin(isSigned);
            const i32 hi = InterpMax(isSigned);
            const auto toEndpoint = [lo, hi](const std::array<f64, 3>& point, std::array<i32, 3>& out)
            {
                for (u32 c = 0; c < 3; ++c)
                    out[c] = std::clamp(static_cast<i32>(std::lround(point[c])), lo, hi);
            };

            std::array<f64, 3> axis{};
            if (count == 0 || PrincipalAxis(cov, axis) <= 0.0)
            {
                toEndpoint(mean, result.E0);
                result.E1 = result.E0;
                return result;
            }

            f64 tMin = std::numeric_limits<f64>::max();
            f64 tMax = std::numeric_limits<f64>::lowest();
            f64 tAnchor = 0.0;
            for (u32 t = 0; t < 16; ++t)
            {
                if ((mask & (1u << t)) == 0)
                    continue;
                f64 projection = 0.0;
                for (u32 c = 0; c < 3; ++c)
                    projection += (static_cast<f64>(target[t][c]) - mean[c]) * axis[c];
                tMin = std::min(tMin, projection);
                tMax = std::max(tMax, projection);
                if (t == anchorTexel)
                    tAnchor = projection;
            }

            // Put the anchor at the E0 end.
            if ((tAnchor - tMin) > (tMax - tAnchor))
                std::swap(tMin, tMax);

            std::array<f64, 3> p0{};
            std::array<f64, 3> p1{};
            for (u32 c = 0; c < 3; ++c)
            {
                p0[c] = mean[c] + tMin * axis[c];
                p1[c] = mean[c] + tMax * axis[c];
            }
            toEndpoint(p0, result.E0);
            toEndpoint(p1, result.E1);
            return result;
        }

        // ---- Candidate evaluation ----------------------------------------------------

        struct Candidate
        {
            i64 Error = std::numeric_limits<i64>::max();
            u32 ModeIndex = 10;
            u32 Partition = 0;
            std::array<std::array<i32, 3>, 4> Q{}; // quantized endpoints, base precision
            std::array<i32, 16> Indices{};
        };

        // Per-texel index selection for one subset. The anchor texel is restricted to the
        // legal (high-bit-clear) half of the index range instead of being clamped
        // afterwards, so it still gets the best index it is allowed to have.
        i64 SelectIndices(const BlockTargets& target, u32 mask, u32 anchorTexel,
                          const std::array<i32, 3>& u0, const std::array<i32, 3>& u1,
                          const i32* weights, u32 weightCount, std::array<i32, 16>& indices)
        {
            i64 total = 0;
            for (u32 t = 0; t < 16; ++t)
            {
                if ((mask & (1u << t)) == 0)
                    continue;
                const u32 limit = (t == anchorTexel) ? (weightCount / 2) : weightCount;
                i64 bestError = std::numeric_limits<i64>::max();
                i32 bestIndex = 0;
                for (u32 w = 0; w < limit; ++w)
                {
                    i64 error = 0;
                    for (u32 c = 0; c < 3; ++c)
                    {
                        const i64 d = static_cast<i64>(Interpolate(u0[c], u1[c], weights[w])) - target[t][c];
                        error += d * d;
                    }
                    if (error < bestError)
                    {
                        bestError = error;
                        bestIndex = static_cast<i32>(w);
                    }
                }
                indices[t] = bestIndex;
                total += bestError;
            }
            return total;
        }

        // Least-squares endpoint refit for one subset, holding the current indices fixed.
        // interp = E0*(1 - w/64) + E1*(w/64) is linear in the endpoints, so each channel
        // is a 2x2 normal-equation solve.
        Endpoints RefitEndpoints(const BlockTargets& target, u32 mask, const std::array<i32, 16>& indices,
                                 const i32* weights, const Endpoints& fallback, bool isSigned)
        {
            Endpoints result = fallback;
            const i32 lo = InterpMin(isSigned);
            const i32 hi = InterpMax(isSigned);
            for (u32 c = 0; c < 3; ++c)
            {
                f64 a00 = 0.0;
                f64 a01 = 0.0;
                f64 a11 = 0.0;
                f64 rhs0 = 0.0;
                f64 rhs1 = 0.0;
                for (u32 t = 0; t < 16; ++t)
                {
                    if ((mask & (1u << t)) == 0)
                        continue;
                    const f64 w = static_cast<f64>(weights[indices[t]]) / 64.0;
                    const f64 s = 1.0 - w;
                    a00 += s * s;
                    a01 += s * w;
                    a11 += w * w;
                    rhs0 += s * static_cast<f64>(target[t][c]);
                    rhs1 += w * static_cast<f64>(target[t][c]);
                }
                const f64 determinant = a00 * a11 - a01 * a01;
                if (std::abs(determinant) < 1e-6)
                    continue; // every index identical — the fit is underdetermined; keep what we had
                result.E0[c] = std::clamp(static_cast<i32>(std::lround((rhs0 * a11 - rhs1 * a01) / determinant)), lo, hi);
                result.E1[c] = std::clamp(static_cast<i32>(std::lround((rhs1 * a00 - rhs0 * a01) / determinant)), lo, hi);
            }
            return result;
        }

        // Quantize a whole endpoint set to the mode's precision and pull the delta-coded
        // endpoints into their storable window. Endpoint 0 is the delta base, so it is
        // quantized first and never clamped.
        void QuantizeEndpoints(const ModeSpec& mode, const std::array<Endpoints, 2>& fits, bool isSigned,
                               std::array<std::array<i32, 3>, 4>& q)
        {
            for (u32 s = 0; s < mode.Subsets; ++s)
                for (u32 c = 0; c < 3; ++c)
                {
                    q[s * 2 + 0][c] = Quantize(fits[s].E0[c], mode.BaseBits, isSigned);
                    q[s * 2 + 1][c] = Quantize(fits[s].E1[c], mode.BaseBits, isSigned);
                }
            if (mode.HasDelta == 0)
                return;
            const u32 endpointCount = mode.Subsets * 2u;
            for (u32 e = 1; e < endpointCount; ++e)
                for (u32 c = 0; c < 3; ++c)
                    q[e][c] = ClampToDeltaRange(q[e][c], q[0][c], mode.Delta[c], mode.BaseBits, isSigned);
        }

        // Score one (mode, partition, endpoint fit) triple: quantize, pick indices,
        // refine, and return the total squared error a conformant decoder would produce
        // in interpolation space. `q` and `indices` receive the winning encoding.
        i64 EvaluateCandidate(const ModeSpec& mode, const BlockTargets& target, u32 partition, bool isSigned,
                              std::array<Endpoints, 2> fits, std::array<std::array<i32, 3>, 4>& q,
                              std::array<i32, 16>& indices)
        {
            const bool oneSubset = mode.Subsets == 1;
            const i32* weights = oneSubset ? kWeights4.data() : kWeights3.data();
            const u32 weightCount = oneSubset ? 16u : 8u;

            std::array<u32, 2> masks{};
            std::array<u32, 2> anchors{};
            if (oneSubset)
            {
                masks[0] = 0xFFFFu;
                anchors[0] = 0;
            }
            else
            {
                const auto& shape = kPartitions[partition];
                for (u32 t = 0; t < 16; ++t)
                {
                    const u32 subset = shape[t] & 0x01u;
                    masks[subset] |= (1u << t);
                    if ((shape[t] & 0x80u) != 0)
                        anchors[subset] = t;
                }
            }

            const auto score = [&](const std::array<Endpoints, 2>& candidateFits,
                                   std::array<std::array<i32, 3>, 4>& outQ,
                                   std::array<i32, 16>& outIndices) -> i64
            {
                QuantizeEndpoints(mode, candidateFits, isSigned, outQ);
                i64 total = 0;
                for (u32 s = 0; s < mode.Subsets; ++s)
                {
                    std::array<i32, 3> u0{};
                    std::array<i32, 3> u1{};
                    for (u32 c = 0; c < 3; ++c)
                    {
                        u0[c] = Unquantize(outQ[s * 2 + 0][c], mode.BaseBits, isSigned);
                        u1[c] = Unquantize(outQ[s * 2 + 1][c], mode.BaseBits, isSigned);
                    }
                    total += SelectIndices(target, masks[s], anchors[s], u0, u1, weights, weightCount, outIndices);
                }
                return total;
            };

            i64 bestError = score(fits, q, indices);

            // Alternate a least-squares endpoint refit with index re-selection, keeping a
            // step only when it lowers the error — so refinement can never regress.
            for (u32 iteration = 0; iteration < 2; ++iteration)
            {
                std::array<Endpoints, 2> refits = fits;
                for (u32 s = 0; s < mode.Subsets; ++s)
                    refits[s] = RefitEndpoints(target, masks[s], indices, weights, fits[s], isSigned);

                std::array<std::array<i32, 3>, 4> candidateQ{};
                std::array<i32, 16> candidateIndices{};
                const i64 error = score(refits, candidateQ, candidateIndices);
                if (error >= bestError)
                    break;
                bestError = error;
                fits = refits;
                q = candidateQ;
                indices = candidateIndices;
            }
            return bestError;
        }

        // ---- Bit packing -------------------------------------------------------------

        // Little-endian, LSB-first bit writer over a 16-byte block — the exact order
        // bcdec's bitstream reader consumes.
        struct BitWriter
        {
            std::array<u8, 16>& Data;
            u32 Pos = 0;

            void Put(u32 value, u32 bits)
            {
                for (u32 i = 0; i < bits; ++i)
                {
                    if (((value >> i) & 1u) != 0)
                        Data[Pos >> 3] |= static_cast<u8>(1u << (Pos & 7u));
                    ++Pos;
                }
            }

            // bcdec's read_bits_r reverses the bits it read, so writing them means
            // reversing first.
            void PutReversed(u32 value, u32 bits)
            {
                u32 reversed = 0;
                for (u32 i = 0; i < bits; ++i)
                    reversed |= ((value >> i) & 1u) << (bits - 1 - i);
                Put(reversed, bits);
            }
        };

        void PackBlock(u8* dst, const Candidate& candidate)
        {
            const ModeSpec& mode = kModes[candidate.ModeIndex];
            std::array<u8, 16> block{};
            BitWriter writer{ block };
            writer.Put(mode.ModeValue, mode.ModeBits);

            // Endpoint slots hold the raw stored field values: endpoint 0 is the base and
            // the rest are deltas for a delta mode. Everything is masked to its field
            // width, which is also how a negative signed value becomes its two's
            // complement bit pattern.
            std::array<u32, 13> slots{};
            const u32 endpointCount = mode.Subsets * 2u;
            for (u32 c = 0; c < 3; ++c)
            {
                const u32 baseMask = (1u << mode.BaseBits) - 1u;
                slots[c] = static_cast<u32>(candidate.Q[0][c]) & baseMask;
                for (u32 e = 1; e < endpointCount; ++e)
                {
                    const u32 fieldBits = mode.HasDelta != 0 ? mode.Delta[c] : mode.BaseBits;
                    const u32 fieldMask = (1u << fieldBits) - 1u;
                    const i32 stored = mode.HasDelta != 0 ? (candidate.Q[e][c] - candidate.Q[0][c]) : candidate.Q[e][c];
                    slots[e * 3 + c] = static_cast<u32>(stored) & fieldMask;
                }
            }
            slots[kPartitionSlot] = candidate.Partition;

            for (u32 f = 0; f < mode.FieldCount; ++f)
            {
                const FieldSpec& field = mode.Fields[f];
                const u32 value = (slots[field.Slot] >> field.Shift) & ((1u << field.Bits) - 1u);
                if (field.Reversed != 0)
                    writer.PutReversed(value, field.Bits);
                else
                    writer.Put(value, field.Bits);
            }

            // Indices, in raster order; the two anchors (one for a single-subset block)
            // store one bit fewer.
            if (mode.Subsets == 1)
            {
                writer.Put(static_cast<u32>(candidate.Indices[0]), 3);
                for (u32 t = 1; t < 16; ++t)
                    writer.Put(static_cast<u32>(candidate.Indices[t]), 4);
            }
            else
            {
                const auto& shape = kPartitions[candidate.Partition];
                for (u32 t = 0; t < 16; ++t)
                    writer.Put(static_cast<u32>(candidate.Indices[t]), (shape[t] & 0x80u) != 0 ? 2u : 3u);
            }

            OLO_CORE_ASSERT(writer.Pos == 128, "BC6H: packed block is not exactly 128 bits");
            std::memcpy(dst, block.data(), 16);
        }

        // What a conformant decoder produces for `candidate`: the 48 half bit patterns.
        void PredictDecode(const Candidate& candidate, bool isSigned, u16* outHalf)
        {
            const ModeSpec& mode = kModes[candidate.ModeIndex];
            const i32* weights = mode.Subsets == 1 ? kWeights4.data() : kWeights3.data();
            std::array<std::array<i32, 3>, 4> u{};
            for (u32 e = 0; e < mode.Subsets * 2u; ++e)
                for (u32 c = 0; c < 3; ++c)
                    u[e][c] = Unquantize(candidate.Q[e][c], mode.BaseBits, isSigned);

            for (u32 t = 0; t < 16; ++t)
            {
                const u32 subset = mode.Subsets == 1 ? 0u : (kPartitions[candidate.Partition][t] & 0x01u);
                for (u32 c = 0; c < 3; ++c)
                {
                    const i32 value = Interpolate(u[subset * 2][c], u[subset * 2 + 1][c], weights[candidate.Indices[t]]);
                    outHalf[t * 3 + c] = FinishUnquantize(value, isSigned);
                }
            }
        }

        // ---- Block encode ------------------------------------------------------------

        BlockTargets ToTargets(const f32* blockRGB, bool isSigned)
        {
            BlockTargets target{};
            for (u32 t = 0; t < 16; ++t)
                for (u32 c = 0; c < 3; ++c)
                    target[t][c] = FloatToInterp(blockRGB[t * 3 + c], isSigned);
            return target;
        }

        // Residual of the best-fit line through a subset: total variance minus the
        // variance captured by the principal axis. Summed over both subsets this ranks
        // partitions without quantizing anything, which is what keeps the two-subset
        // search affordable — only the best few shapes go on to a full evaluation.
        f64 PartitionResidual(const BlockTargets& target, u32 partition)
        {
            const auto& shape = kPartitions[partition];
            std::array<u32, 2> masks{};
            for (u32 t = 0; t < 16; ++t)
                masks[shape[t] & 0x01u] |= (1u << t);

            f64 residual = 0.0;
            for (const u32 mask : masks)
            {
                std::array<f64, 3> mean{};
                std::array<f64, 6> cov{};
                if (Moments(target, mask, mean, cov) == 0)
                    continue;
                std::array<f64, 3> axis{};
                residual += std::max(0.0, (cov[0] + cov[3] + cov[5]) - PrincipalAxis(cov, axis));
            }
            return residual;
        }

        // How many partition shapes get a full per-mode evaluation. Two is enough in
        // practice: the residual ranking above is a good proxy, and every extra shape
        // costs another ten mode evaluations per block.
        constexpr u32 kPartitionsEvaluated = 2;

        Candidate EncodeBlockInternal(const f32* blockRGB, bool isSigned, u32 modeMask)
        {
            const BlockTargets target = ToTargets(blockRGB, isSigned);
            Candidate best;

            const auto consider = [&](u32 modeIndex, u32 partition, const std::array<Endpoints, 2>& fits)
            {
                std::array<std::array<i32, 3>, 4> q{};
                std::array<i32, 16> indices{};
                const i64 error = EvaluateCandidate(kModes[modeIndex], target, partition, isSigned, fits, q, indices);
                if (error < best.Error)
                {
                    best.Error = error;
                    best.ModeIndex = modeIndex;
                    best.Partition = partition;
                    best.Q = q;
                    best.Indices = indices;
                }
            };

            // One-subset modes share a single fit over the whole block.
            {
                std::array<Endpoints, 2> fits{};
                fits[0] = FitEndpoints(target, 0xFFFFu, 0, isSigned);
                for (u32 modeIndex = 0; modeIndex < kModeCount; ++modeIndex)
                {
                    if (kModes[modeIndex].Subsets != 1 || (modeMask & (1u << modeIndex)) == 0)
                        continue;
                    consider(modeIndex, 0, fits);
                }
            }

            // Two-subset modes: rank the 32 shapes cheaply, then fully evaluate the best.
            bool wantTwoSubset = false;
            for (u32 modeIndex = 0; modeIndex < kModeCount; ++modeIndex)
                wantTwoSubset = wantTwoSubset || (kModes[modeIndex].Subsets == 2 && (modeMask & (1u << modeIndex)) != 0);
            if (wantTwoSubset)
            {
                std::array<u32, kPartitionsEvaluated> bestPartitions{};
                std::array<f64, kPartitionsEvaluated> bestResiduals{};
                bestResiduals.fill(std::numeric_limits<f64>::max());
                for (u32 partition = 0; partition < static_cast<u32>(kPartitions.size()); ++partition)
                {
                    const f64 residual = PartitionResidual(target, partition);
                    for (u32 slot = 0; slot < kPartitionsEvaluated; ++slot)
                    {
                        if (residual >= bestResiduals[slot])
                            continue;
                        for (u32 shift = kPartitionsEvaluated - 1; shift > slot; --shift)
                        {
                            bestResiduals[shift] = bestResiduals[shift - 1];
                            bestPartitions[shift] = bestPartitions[shift - 1];
                        }
                        bestResiduals[slot] = residual;
                        bestPartitions[slot] = partition;
                        break;
                    }
                }

                for (const u32 partition : bestPartitions)
                {
                    const auto& shape = kPartitions[partition];
                    std::array<u32, 2> masks{};
                    std::array<u32, 2> anchors{};
                    for (u32 t = 0; t < 16; ++t)
                    {
                        const u32 subset = shape[t] & 0x01u;
                        masks[subset] |= (1u << t);
                        if ((shape[t] & 0x80u) != 0)
                            anchors[subset] = t;
                    }
                    std::array<Endpoints, 2> fits{};
                    for (u32 s = 0; s < 2; ++s)
                        fits[s] = FitEndpoints(target, masks[s], anchors[s], isSigned);

                    for (u32 modeIndex = 0; modeIndex < kModeCount; ++modeIndex)
                    {
                        if (kModes[modeIndex].Subsets != 2 || (modeMask & (1u << modeIndex)) == 0)
                            continue;
                        consider(modeIndex, partition, fits);
                    }
                }
            }
            return best;
        }
    } // namespace

    void EncodeBlock(u8* dst, const f32* blockRGB, bool isSigned)
    {
        constexpr u32 kAllModes = (1u << kModeCount) - 1u;
        PackBlock(dst, EncodeBlockInternal(blockRGB, isSigned, kAllModes));
    }

    bool EncodeBlockForModeForTest(u8* dst, const f32* blockRGB, bool isSigned, u32 modeIndex, u16* outPredictedHalf)
    {
        if (modeIndex >= kModeCount)
            return false;
        const Candidate candidate = EncodeBlockInternal(blockRGB, isSigned, 1u << modeIndex);
        PackBlock(dst, candidate);
        PredictDecode(candidate, isSigned, outPredictedHalf);
        return true;
    }
} // namespace OloEngine::BC6H
