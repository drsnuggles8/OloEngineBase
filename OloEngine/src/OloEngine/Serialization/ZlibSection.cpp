#include "OloEnginePCH.h"

#include "OloEngine/Serialization/ZlibSection.h"

#include "OloEngine/Core/Log.h"

#include <zlib.h>

namespace OloEngine::ZlibSection
{
    namespace
    {
        // zlib's one-shot API addresses buffers with uLong, which is only
        // guaranteed 32 bits (and IS 32 bits on Windows). Refuse anything
        // larger than that floor rather than letting a static_cast truncate
        // silently — every engine format caps its payloads far below this
        // anyway. (Deliberately a literal, not numeric_limits<uLong>::max():
        // on LP64 platforms uLong is 64-bit and that comparison would be
        // tautologically false, tripping -Wtype-limits.)
        constexpr u64 kZlibAddressableMax = 0xFFFFFFFFull;
    } // anonymous namespace

    std::vector<u8> Compress(const void* data, sizet size, std::string_view context)
    {
        if (!data || size == 0)
        {
            return {};
        }
        if (size > kZlibAddressableMax)
        {
            OLO_CORE_ERROR("{}: payload size {} exceeds what zlib can address", context, size);
            return {};
        }

        auto bound = ::compressBound(static_cast<uLong>(size));
        std::vector<u8> compressed(bound);
        uLongf compressedSize = bound;

        if (auto ret = ::compress2(compressed.data(), &compressedSize,
                                   static_cast<const Bytef*>(data),
                                   static_cast<uLong>(size), 6);
            ret != Z_OK)
        {
            OLO_CORE_ERROR("{}: compress2 failed (error {})", context, ret);
            return {};
        }

        compressed.resize(compressedSize);
        return compressed;
    }

    std::vector<u8> Decompress(const void* compressedData, sizet compressedSize,
                               u64 expectedUncompressedSize, u64 maxUncompressedSize,
                               std::string_view context)
    {
        if (!compressedData || compressedSize == 0)
        {
            OLO_CORE_ERROR("{}: no compressed payload to decompress", context);
            return {};
        }
        if (expectedUncompressedSize == 0 || expectedUncompressedSize > maxUncompressedSize)
        {
            OLO_CORE_ERROR("{}: claimed uncompressed size {} is empty or exceeds the cap {}",
                           context, expectedUncompressedSize, maxUncompressedSize);
            return {};
        }
        if (compressedSize > kZlibAddressableMax || expectedUncompressedSize > kZlibAddressableMax)
        {
            OLO_CORE_ERROR("{}: payload sizes ({} compressed, {} claimed) exceed what zlib can address",
                           context, compressedSize, expectedUncompressedSize);
            return {};
        }
        // Reject a physically impossible claim BEFORE sizing the destination
        // buffer from it: deflate cannot expand beyond MaxInflateRatio, so a
        // small hostile file claiming a multi-GiB uncompressed size is
        // refused without allocating anything. (Integer division: rejects
        // only ratios strictly above the hard 1032:1 ceiling, so no valid
        // stream — not even a maximally compressible all-zero payload — can
        // be rejected here.)
        if (expectedUncompressedSize / MaxInflateRatio > compressedSize)
        {
            OLO_CORE_ERROR("{}: claimed uncompressed size {} is impossible for {} compressed bytes "
                           "(deflate max ratio {}:1) — corrupt or hostile header",
                           context, expectedUncompressedSize, compressedSize, MaxInflateRatio);
            return {};
        }

        std::vector<u8> decompressed(static_cast<sizet>(expectedUncompressedSize));
        auto destLen = static_cast<uLongf>(expectedUncompressedSize);

        if (auto ret = ::uncompress(decompressed.data(), &destLen,
                                    static_cast<const Bytef*>(compressedData),
                                    static_cast<uLong>(compressedSize));
            ret != Z_OK)
        {
            OLO_CORE_ERROR("{}: uncompress failed (error {})", context, ret);
            return {};
        }

        if (destLen != expectedUncompressedSize)
        {
            OLO_CORE_ERROR("{}: decompressed size {} does not match header claim {}",
                           context, static_cast<u64>(destLen), expectedUncompressedSize);
            return {};
        }

        return decompressed;
    }
} // namespace OloEngine::ZlibSection
