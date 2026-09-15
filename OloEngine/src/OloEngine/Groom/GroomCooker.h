#pragma once

// =============================================================================
// GroomCooker.h — canonicalisation, and the determinism contract. Issue #1232.
//
// "Deterministic cooking" is in the issue's title, so it is treated here as a
// CONTRACT, not an aspiration: cooking the same source twice, in either order,
// in either process, on either platform, must produce byte-identical
// .ologroom bytes. GroomCookDeterminismTest pins it by cooking twice and
// comparing the buffers.
//
// What that forbids, and what this file does about each:
//
//   * UNORDERED ITERATION. Group ids come from first-appearance order in
//     GroomBuilder, never from hash-map iteration; the cook's sort is a STABLE
//     sort keyed only on the group id, so curves that share a group keep their
//     source order. Nothing here iterates an unordered_map to produce output.
//   * POINTER-VALUE HASHING / ADDRESS-DEPENDENT ORDER. No key is derived from
//     an address; the source content hash is over file BYTES.
//   * UNINITIALISED PADDING. Every struct written to the stream is explicitly
//     padded and static_assert'ed on its size — see GroomBinaryFormat.h.
//   * TIMESTAMPS AND ABSOLUTE PATHS. GroomProvenance carries neither; see the
//     comment there.
//   * FLOAT FORMATTING. Floats go to disk as IEEE-754 bit patterns, never as
//     text, so no locale or precision setting can move them.
//
// The cook itself is a CANONICALISATION, not a transform: no resampling, no
// tessellation, no width remapping. Every value the importer read survives it
// byte-for-byte — which is what makes acceptance criterion 2's round trip a
// meaningful test rather than a test of the cook's own rounding.
// =============================================================================

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Groom/GroomAsset.h"

#include <string>
#include <vector>

namespace OloEngine
{
    namespace GroomCooker
    {
        // Reorders `groom`'s curves so that every group occupies one contiguous
        // range, and refreshes the derived data. Stable within a group, so the
        // result is a pure function of the input.
        //
        // Why the cook reorders at all: a DCC exports strands in whatever order
        // it built them, so a groom's group membership is typically interleaved.
        // Making a group a RANGE is what lets the preview, and later the
        // per-group shading in #1246, address a group without a per-curve scan
        // — and it is the only structural change the cook makes.
        //
        // Returns false with a named reason if the result does not validate.
        [[nodiscard]] bool Canonicalize(GroomAsset& groom, std::string& outReason);

        // Canonicalises a COPY of `groom` and encodes it to the cooked
        // .ologroom byte stream. Returns false with a named reason on failure.
        // This is the whole cook: one call, one deterministic buffer.
        [[nodiscard]] bool CookToBytes(const GroomAsset& groom, std::vector<u8>& outBytes, std::string& outReason);

        // 64-bit FNV-1a over a file's bytes — the provenance source hash. Named
        // here (rather than inlined in the importer) because the determinism
        // test and the importer must agree on it exactly.
        [[nodiscard]] u64 HashSourceBytes(const void* data, sizet size) noexcept;
    } // namespace GroomCooker
} // namespace OloEngine
