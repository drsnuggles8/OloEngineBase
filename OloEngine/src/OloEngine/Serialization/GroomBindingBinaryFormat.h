#pragma once

#include "OloEngine/Core/Base.h"

#include <type_traits>
#include <utility>

namespace OloEngine
{
    // ============================================================================
    // .ologroombinding Binary Format — Version 1 (issue #1249)
    //
    // The cooked groom binding: which triangle of a body each curve grows out
    // of, and the frame that triangle had at bind time. Produced by
    // GroomBindingCooker::CookToBytes from a GroomBindingBuilder result; read
    // back by GroomBindingSerializer.
    //
    // WHY THIS IS NOT A SECTION INSIDE .ologroom. A binding is a relationship
    // between TWO assets, and one groom can be bound to several bodies (a coat
    // shared by a herd of variants) while one body can carry several grooms
    // (scalp, brows, lashes). Folding the binding into either asset would make
    // the pair a one-to-one that the content is not, and would rev the groom's
    // format every time the binder changed. Separate asset, separate schema,
    // separate version line — the same argument GroomBinaryFormat.h makes for
    // not sharing .omesh's.
    //
    // Layout:
    //   [FileHeader]
    //   [payload — zlib-compressed when FlagCompressed is set]
    //
    // The payload is a sequence of framed sections, each
    //   [SectionFrame { SectionId, ByteCount }] [ByteCount bytes of payload]
    // in this fixed order, each exactly once:
    //   Section 0 Info  — InfoSection (counts, both signatures, binder version)
    //   Section 1 Roots — GroomRootBinding * RootCount, a flat array
    //   Section 2 Paths — PathHeader, then the groom path then the target path
    //
    // All multi-byte values are little-endian. FileHeader::Checksum is the CRC32
    // of the payload bytes as stored on disk (i.e. of the COMPRESSED bytes when
    // FlagCompressed is set) — mirrors .ologroom, .olovol and .olmap.
    //
    // DETERMINISM. Everything here is a fixed-width binary field with explicit
    // padding: no text-formatted floats, no timestamps, no absolute paths, no
    // hash-map iteration order. Combined with GroomBindingBuilder's index-based
    // tie-break, the same pair produces the same bytes on every run and every
    // machine, and GroomBindingCookDeterminismTest pins it.
    //
    // Versioning (docs/agent-rules/binary-format-versioning.md): a binding is a
    // DERIVED artifact — fully regenerable from the groom and the body it was
    // built against. So MinSupportedVersion and CurrentVersion move together:
    // any layout change bumps both, old files are rejected outright BY VERSION
    // rather than mis-parsed, and the cost is one rebuild instead of a migration
    // path to maintain forever.
    //
    // THERE ARE TWO VERSIONS HERE AND THEY ARE NOT THE SAME THING. This one is
    // the container's: it moves when the BYTES move. kGroomBinderVersion (see
    // Groom/GroomBinding.h) moves when the binder's OUTPUT moves for unchanged
    // input — a different projection or frame convention — which can happen
    // without a single field changing shape. A file can therefore be perfectly
    // readable and still be refused at attach, and the two refusals say
    // different things on purpose.
    // ============================================================================

    namespace OloGroomBindingFormat
    {
        constexpr u32 MagicNumber = 0x44424750; // "PGBD" in little-endian
        constexpr u32 CurrentVersion = 1;
        constexpr u32 MinSupportedVersion = 1; // == CurrentVersion, on purpose — see header comment

        constexpr u32 FlagCompressed = 1; // Bit 0: payload is zlib-compressed

        // ── Safety caps for deserialized values (defence against corrupt files) ──
        // Enforced symmetrically: the writer refuses to produce a file the
        // reader would reject. These mirror GroomBindingLimits in
        // OloEngine/Groom/GroomBinding.h — deliberately restated as format
        // constants so a reader TU need not include the asset header, with a
        // static_assert in GroomBindingSerializer.cpp keeping the two in step.
        constexpr u32 MaxRootCount = 8u * 1024u * 1024u;
        constexpr u32 MaxTargetTriangleCount = 64u * 1024u * 1024u;
        constexpr u32 MaxStringLength = 4096;
        // 8M roots * 56 B/root is 448 MB, so the payload cap is far above any
        // real binding and exists only to bound a hostile header.
        constexpr u64 MaxUncompressedPayloadSize = 2'000'000'000;
        constexpr u64 MaxCompressedPayloadSize = 2'000'000'000;

        enum class SectionType : u16
        {
            Info = 0,
            Roots = 1,
            Paths = 2,
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

        struct SectionFrame
        {
            u16 SectionId = 0; // SectionType
            u16 Pad0 = 0;
            u32 Pad1 = 0;      // explicit — the u64 below would otherwise leave 4 bytes of
                               // uninitialised implicit padding in the written stream
            u64 ByteCount = 0; // payload bytes following this frame
        };

        // Section 0 payload. Both signatures live here rather than in sections of
        // their own: they are what a reader must check BEFORE it trusts anything
        // else in the file, and a header-only read should be able to answer "may
        // this binding attach to that body?" without decompressing the roots.
        struct InfoSection
        {
            u64 SourceRootHash = 0;
            u64 TargetIndexHash = 0;
            u64 TargetRestPositionHash = 0;
            u64 TargetSkeletonNameHash = 0;

            u32 RootCount = 0;
            u32 BinderVersion = 0;
            u32 SourceCurveCount = 0;
            u32 SourceGuideCount = 0;
            u32 TargetVertexCount = 0;
            u32 TargetIndexCount = 0;
            u32 TargetBoneCount = 0;
            u32 Pad0 = 0;
        };

        // Section 2 header. The two strings follow immediately, groom first.
        struct PathHeader
        {
            u32 GroomPathLength = 0;
            u32 TargetPathLength = 0;
        };
    } // namespace OloGroomBindingFormat

    // ── Compile-time ABI guards for wire-format structs ──────────────
    // Any padding or field-order change will break binary compatibility.

    static_assert(std::is_trivially_copyable_v<OloGroomBindingFormat::FileHeader>);
    static_assert(std::is_standard_layout_v<OloGroomBindingFormat::FileHeader>);
    static_assert(sizeof(OloGroomBindingFormat::FileHeader) == 24);

    static_assert(std::is_trivially_copyable_v<OloGroomBindingFormat::SectionFrame>);
    static_assert(std::is_standard_layout_v<OloGroomBindingFormat::SectionFrame>);
    static_assert(sizeof(OloGroomBindingFormat::SectionFrame) == 16);

    static_assert(std::is_trivially_copyable_v<OloGroomBindingFormat::InfoSection>);
    static_assert(std::is_standard_layout_v<OloGroomBindingFormat::InfoSection>);
    static_assert(sizeof(OloGroomBindingFormat::InfoSection) == 64);

    static_assert(std::is_trivially_copyable_v<OloGroomBindingFormat::PathHeader>);
    static_assert(std::is_standard_layout_v<OloGroomBindingFormat::PathHeader>);
    static_assert(sizeof(OloGroomBindingFormat::PathHeader) == 8);
} // namespace OloEngine
