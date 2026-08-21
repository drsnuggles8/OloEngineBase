#pragma once

#include "OloEngine/Core/Base.h"

#include <string_view>
#include <vector>

namespace OloEngine
{
    // ============================================================================
    // Shared zlib payload helper (issue #439 review follow-up)
    //
    // The engine's binary containers (.olmap, .omesh, .oanim) all follow the
    // same pattern: a format-specific FileHeader carrying a CRC32 of the
    // stored (compressed) bytes plus the exact uncompressed payload size,
    // followed by one zlib-deflated payload. The deflate/inflate halves were
    // duplicated per TU; this is the single shared implementation.
    //
    // Deliberately NOT part of this helper, because the formats differ there:
    //   * CRC32 verification — .olmap checks the stored payload whether or not
    //     it is compressed, while .omesh/.oanim only check the compressed
    //     branch, and each format keeps the checksum in its own header layout.
    //   * SaveGame's zlib usage (SaveGameFile) stays self-contained by
    //     decision — do not migrate it here.
    //
    // Compress() is byte-identical to the per-TU copies it replaced (deflate
    // level 6 via compress2), so every existing .olmap / .omesh / .oanim file
    // and golden remains bit-exact.
    // ============================================================================
    namespace ZlibSection
    {
        // Hard ceiling on deflate's expansion: a length-258 match can be
        // encoded in as few as 2 bits, so no valid zlib stream inflates to
        // more than 1032x its compressed size. Any header claiming a larger
        // ratio is corrupt or hostile and is rejected BEFORE the destination
        // buffer is allocated.
        constexpr u64 MaxInflateRatio = 1032;

        // Compress a byte buffer with zlib deflate (level 6 = good balance).
        // Returns an empty vector on failure (logged with `context`).
        [[nodiscard]] std::vector<u8> Compress(const void* data, sizet size, std::string_view context);

        // Decompress a zlib stream whose exact uncompressed size is known from
        // a file header. Allocation-hardened: `expectedUncompressedSize` is an
        // untrusted on-disk value, so it is validated against BOTH the caller's
        // `maxUncompressedSize` cap and the MaxInflateRatio bound implied by
        // `compressedSize` before any buffer is sized from it. Inflate must
        // complete to exactly `expectedUncompressedSize` bytes — a short
        // result is rejected, never returned truncated. Returns an empty
        // vector on any failure (logged with `context`).
        [[nodiscard]] std::vector<u8> Decompress(const void* compressedData, sizet compressedSize,
                                                 u64 expectedUncompressedSize, u64 maxUncompressedSize,
                                                 std::string_view context);
    } // namespace ZlibSection
} // namespace OloEngine
