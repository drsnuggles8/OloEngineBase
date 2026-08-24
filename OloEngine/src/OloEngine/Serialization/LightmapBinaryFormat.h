#pragma once

#include "OloEngine/Core/Base.h"

#include <type_traits>
#include <utility>

namespace OloEngine
{
    // ============================================================================
    // .olmap Binary Lightmap Format — Version 1 (issue #439)
    //
    // Layout:
    //   [FileHeader]
    //   [payload — zlib-compressed when FlagCompressed is set]
    //
    // The payload is a sequence of framed sections, each
    //   [SectionFrame { SectionId, ByteCount }] [ByteCount bytes of payload]
    // in this fixed order, each exactly once:
    //   Section 0 Info        — InfoSection (atlas dimensions, page count, bake key)
    //   Section 1 Texels      — raw f32 RGBA data, ByteCount = PageCount*W*H*16
    //   Section 2 EntityTable — EntityTableHeader + EntryCount × LightmapEntityEntry
    //                           (LightmapEntityEntry: Renderer/LightmapAsset.h, 32 bytes,
    //                           layout pinned by static_asserts there)
    //
    // All multi-byte values are little-endian. FileHeader::Checksum is the
    // CRC32 of the payload bytes as stored on disk (i.e. of the COMPRESSED
    // bytes when FlagCompressed is set).
    //
    // Versioning (docs/agent-rules/binary-format-versioning.md): unlike a
    // save-game, a .olmap is a DERIVED artifact — fully regenerable by
    // re-running the bake. So instead of per-field version gates and a
    // migration chain, MinSupportedVersion and CurrentVersion move TOGETHER:
    // any layout change bumps both, old files are rejected outright, and the
    // cost is one re-bake rather than a migration path to maintain forever.
    // A version ABOVE CurrentVersion is always rejected — this build cannot
    // guess a future layout.
    // ============================================================================

    namespace OLmapFormat
    {
        constexpr u32 MagicNumber = 0x504D4C4F; // "OLMP" in little-endian
        constexpr u32 CurrentVersion = 1;
        constexpr u32 MinSupportedVersion = 1; // == CurrentVersion, on purpose — see header comment

        constexpr u32 FlagCompressed = 1; // Bit 0: payload is zlib-compressed

        // ── Safety caps for deserialized values (defence against corrupt files) ──
        // Enforced symmetrically: the writer refuses to produce a file the
        // reader would reject.
        constexpr u32 MaxDimension = 16384;
        constexpr u32 MaxPageCount = 8;
        constexpr u32 MaxEntryCount = 1'000'000;
        // Allocation bound for the decompressed payload, checked before any
        // buffer is sized from a header field. Bounds practical v1 atlases
        // (an 8192² f32 RGBA page is 1 GiB); a hostile header cannot ask for
        // more. Raising it is a reader-side change, not a format change.
        //
        // NOTE for the reader: the per-value caps above do NOT bound the
        // texel-buffer product on their own — MaxDimension² x MaxPageCount
        // x 16 bytes is 32 GiB, far beyond MaxUncompressedPayloadSize. The
        // reader must therefore also bound every section-sized allocation by
        // the payload bytes ACTUALLY remaining (and the decompression helper
        // rejects an uncompressed-size claim exceeding zlib's 1032:1 inflate
        // ceiling) before allocating — see LightmapSerializer::DecodeFromBytes
        // and Serialization/ZlibSection.h.
        constexpr u64 MaxUncompressedPayloadSize = 2'000'000'000; // 2 GB
        constexpr u64 MaxCompressedPayloadSize = 2'000'000'000;   // 2 GB

        // Section identifiers. The v1 reader requires exactly these three, in
        // order — an unknown id is corruption, not an extension point (strict
        // versioning above: new sections come with a version bump).
        enum class SectionType : u16
        {
            Info = 0,
            Texels = 1,
            EntityTable = 2,
            Count = 3 // sentinel
        };

        constexpr auto kSectionCount = std::to_underlying(SectionType::Count);

        struct FileHeader
        {
            u32 Magic = MagicNumber;
            u32 Version = CurrentVersion;
            u32 Flags = 0;                   // Bit 0: zlib-compressed payload
            u32 Checksum = 0;                // CRC32 of the stored (compressed) payload after the header
            u64 UncompressedPayloadSize = 0; // Payload size before compression
        };

        // Frame preceding each section's payload bytes.
        struct SectionFrame
        {
            u16 SectionId = 0; // SectionType
            u16 Pad0 = 0;
            u32 Pad1 = 0;      // explicit — the u64 below would otherwise leave 4 bytes of
                               // uninitialised implicit padding in the written stream
            u64 ByteCount = 0; // payload bytes following this frame
        };

        // Section 0 payload.
        struct InfoSection
        {
            u32 Width = 0;
            u32 Height = 0;
            u32 PageCount = 0;
            u32 Pad0 = 0;
            u64 BakeKey = 0; // FNV-1a hash of the baked scene state; 0 = unset
        };

        // Section 2 payload header; followed by EntryCount × LightmapEntityEntry.
        struct EntityTableHeader
        {
            u32 EntryCount = 0;
            u32 Pad0 = 0;
        };
    } // namespace OLmapFormat

    // ── Compile-time ABI guards for wire-format structs ──────────────
    // Any padding or field-order change will break binary compatibility.

    static_assert(std::is_trivially_copyable_v<OLmapFormat::FileHeader>);
    static_assert(std::is_standard_layout_v<OLmapFormat::FileHeader>);
    static_assert(sizeof(OLmapFormat::FileHeader) == 24);

    static_assert(std::is_trivially_copyable_v<OLmapFormat::SectionFrame>);
    static_assert(std::is_standard_layout_v<OLmapFormat::SectionFrame>);
    static_assert(sizeof(OLmapFormat::SectionFrame) == 16);

    static_assert(std::is_trivially_copyable_v<OLmapFormat::InfoSection>);
    static_assert(std::is_standard_layout_v<OLmapFormat::InfoSection>);
    static_assert(sizeof(OLmapFormat::InfoSection) == 24);

    static_assert(std::is_trivially_copyable_v<OLmapFormat::EntityTableHeader>);
    static_assert(std::is_standard_layout_v<OLmapFormat::EntityTableHeader>);
    static_assert(sizeof(OLmapFormat::EntityTableHeader) == 8);

} // namespace OloEngine
