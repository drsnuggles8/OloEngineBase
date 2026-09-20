#pragma once

#include "OloEngine/Core/Base.h"

#include <type_traits>
#include <utility>

namespace OloEngine
{
    // ============================================================================
    // .ologroom Binary Groom Format — Version 3 (issues #1232, #1251, #1252)
    //
    // The engine-native cooked groom. Produced from an Alembic ICurves archive
    // (or any other curve source) by GroomCooker::CookToBytes; read back by
    // GroomSerializer with no Alembic dependency at all — the same runtime/cook
    // split .olovol uses for OpenVDB (see asset-import-usd-alembic.md).
    //
    // WHY THIS IS NOT MeshBinaryFormat. A groom is curves, not triangles: there
    // is no index buffer, no submesh, no material slot, and there IS a per-curve
    // offset table, a root parameterisation and a guide designation that a mesh
    // has no place for. Sharing .omesh's schema would mean carrying two disjoint
    // meanings in one versioned container — a format change for one silently
    // revs the other. Separate schema, separate version line.
    //
    // Layout:
    //   [FileHeader]
    //   [payload — zlib-compressed when FlagCompressed is set]
    //
    // The payload is a sequence of framed sections, each
    //   [SectionFrame { SectionId, ByteCount }] [ByteCount bytes of payload]
    // in this fixed order, each exactly once:
    //   Section 0 Info         — InfoSection (counts, bounds, basis)
    //   Section 1 CurveOffsets — u32 * (CurveCount + 1), prefix sums, [0] == 0
    //   Section 2 Points       — f32[3] * PointCount, object space
    //   Section 3 Widths       — f32   * PointCount, DIAMETERS
    //   Section 4 RootUVs      — f32[2] * CurveCount
    //   Section 5 CurveGroups  — u16 * CurveCount, index into the group table
    //   Section 6 CurveFlags   — u8  * CurveCount, GroomCurveFlag bits
    //   Section 7 GroupNames   — u32 count, then (u32 byteLength, bytes) * count
    //   Section 8 Provenance   — ProvenanceHeader, then SourcePath then
    //                            SourceFormat bytes (lengths in the header)
    //   Section 9 GroupCoats   — GroomCoatGroupDesc * GroupCount (issue #1251):
    //                            each group's coat role, density, length, width,
    //                            clump and tint. Exactly GroupCount entries — a
    //                            groom whose in-memory table is empty writes the
    //                            identity description for every group, so the
    //                            section is fixed-size and the reader never has
    //                            to decide what a short table means.
    //   Section 10 LodLevels   — u32 level count, then per level a
    //                            LodLevelHeader followed by that level's own
    //                            curve arrays in sections 1-6's order, then its
    //                            SourceCurves map (u32 * CurveCount). Issue
    //                            #1252. ZERO LEVELS IS THE COMMON CASE and is
    //                            written as a count of 0 with nothing after it,
    //                            so the section is always present and the
    //                            reader never has to decide whether a missing
    //                            section means "old file" or "no levels" — the
    //                            version already answers that.
    //
    // All multi-byte values are little-endian. FileHeader::Checksum is the
    // CRC32 of the payload bytes as stored on disk (i.e. of the COMPRESSED
    // bytes when FlagCompressed is set) — mirrors .olovol and .olmap.
    //
    // DETERMINISM. Everything here is a fixed-width binary field with explicit
    // padding: no text-formatted floats, no timestamps, no absolute paths, no
    // hash-map iteration order. Combined with GroomCooker's stable sort, the
    // same source produces the same bytes on every run and every machine. That
    // is the point of the format, and GroomCookDeterminismTest pins it.
    //
    // Versioning (docs/agent-rules/binary-format-versioning.md): a .ologroom is
    // a DERIVED artifact — fully regenerable by re-importing its source. So
    // MinSupportedVersion and CurrentVersion move together: any layout change
    // bumps both, old files are rejected outright by version rather than
    // mis-parsed, and the cost is one re-import instead of a migration path to
    // maintain forever.
    // ============================================================================

    namespace OloGroomFormat
    {
        constexpr u32 MagicNumber = 0x4D524750; // "PGRM" in little-endian
        // Version 2 added section 9 (per-group coat authoring, issue #1251);
        // version 3 added section 10 (cooked LOD levels, issue #1252).
        // An older file is REJECTED BY VERSION rather than read without the new
        // section: a .ologroom is a derived artifact, so the minimum moves with
        // the current one and the fix is one re-import — the policy
        // docs/agent-rules/binary-format-versioning.md sets for this class of
        // file, and the reason the reader has no migration branch.
        //
        // WHAT THAT MEANS FOR A PROJECT WITH COOKED GROOMS ON DISK: every
        // .ologroom written before this change fails to load with a named
        // version error and must be re-imported. That is the deliberate cost of
        // this policy, and GroomLodRoundTripTest pins the refusal so the
        // failure is the readable one rather than a mis-parse.
        constexpr u32 CurrentVersion = 3;
        constexpr u32 MinSupportedVersion = 3; // == CurrentVersion, on purpose — see header comment

        constexpr u32 FlagCompressed = 1; // Bit 0: payload is zlib-compressed

        // ── Safety caps for deserialized values (defence against corrupt files) ──
        // Enforced symmetrically: the writer refuses to produce a file the
        // reader would reject. The count caps mirror GroomLimits in
        // OloEngine/Groom/GroomAsset.h — deliberately restated as format
        // constants so a reader TU need not include the asset header, with a
        // static_assert in GroomSerializer.cpp keeping the two in step.
        constexpr u32 MaxCurveCount = 8u * 1024u * 1024u;
        constexpr u64 MaxPointCount = 256ull * 1024ull * 1024ull;
        constexpr u32 MaxGroupCount = 65535;
        constexpr u32 MaxStringLength = 4096;
        // 256M points * 16 B/point (position + width) is 4 GiB, so the payload
        // cap is what actually bounds a groom in practice. 2 GiB is already far
        // past any single shipping groom.
        constexpr u64 MaxUncompressedPayloadSize = 2'000'000'000;
        constexpr u64 MaxCompressedPayloadSize = 2'000'000'000;

        enum class SectionType : u16
        {
            Info = 0,
            CurveOffsets = 1,
            Points = 2,
            Widths = 3,
            RootUVs = 4,
            CurveGroups = 5,
            CurveFlags = 6,
            GroupNames = 7,
            Provenance = 8,
            GroupCoats = 9,
            LodLevels = 10,
            Count = 11 // sentinel
        };

        // Cooked LOD levels are one per REPRESENTATION, and there are three of
        // those (GroomRepresentation), of which the finest is the base groom
        // itself. The cap is deliberately far above that: it bounds a corrupt
        // count before it sizes a loop, and nothing more.
        constexpr u32 MaxLodLevels = 8;

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

        // Section 0 payload. Bounds are the CURVE bounds already widened by the
        // largest strand radius, so a consumer culls what is drawn rather than
        // the centrelines (GroomAsset::RecomputeDerivedData does the widening).
        struct InfoSection
        {
            u32 CurveCount = 0;
            u32 PointCount = 0;
            u32 GroupCount = 0;
            u32 GuideCount = 0; // redundant with the flag array, kept so a
                                // header-only read can report it without
                                // decompressing the whole payload
            f32 BoundsMin[3] = { 0.0f, 0.0f, 0.0f };
            f32 BoundsMax[3] = { 0.0f, 0.0f, 0.0f };
            u8 Basis = 0; // GroomCurveBasis
            u8 Pad0 = 0;
            u16 Pad1 = 0;
            u32 Pad2 = 0;
        };

        // Section 10, one per level. The level's own curve arrays follow
        // immediately, in sections 1-6's order, then its u32 SourceCurves map.
        //
        // The level's counts are HERE rather than in the Info section because
        // they differ per level; the reader bounds each one against the payload
        // that actually remains before sizing anything, exactly as
        // ReadArraySection does for the base arrays.
        struct LodLevelHeader
        {
            u32 CurveCount = 0;
            u32 PointCount = 0;
            f32 SourcePixelSize = 0.0f;
            u8 Representation = 0; // GroomRepresentation
            u8 Pad0 = 0;
            u16 Pad1 = 0;
        };

        // Section 8 header. The two strings follow immediately, path first.
        struct ProvenanceHeader
        {
            u64 SourceContentHash = 0;
            u32 ImporterVersion = 0;
            u32 SourcePathLength = 0;
            u32 SourceFormatLength = 0;
            u32 Pad0 = 0;
        };
    } // namespace OloGroomFormat

    // ── Compile-time ABI guards for wire-format structs ──────────────
    // Any padding or field-order change will break binary compatibility.

    static_assert(std::is_trivially_copyable_v<OloGroomFormat::FileHeader>);
    static_assert(std::is_standard_layout_v<OloGroomFormat::FileHeader>);
    static_assert(sizeof(OloGroomFormat::FileHeader) == 24);

    static_assert(std::is_trivially_copyable_v<OloGroomFormat::SectionFrame>);
    static_assert(std::is_standard_layout_v<OloGroomFormat::SectionFrame>);
    static_assert(sizeof(OloGroomFormat::SectionFrame) == 16);

    static_assert(std::is_trivially_copyable_v<OloGroomFormat::InfoSection>);
    static_assert(std::is_standard_layout_v<OloGroomFormat::InfoSection>);
    static_assert(sizeof(OloGroomFormat::InfoSection) == 48);

    static_assert(std::is_trivially_copyable_v<OloGroomFormat::ProvenanceHeader>);
    static_assert(std::is_standard_layout_v<OloGroomFormat::ProvenanceHeader>);
    static_assert(sizeof(OloGroomFormat::ProvenanceHeader) == 24);

    static_assert(std::is_trivially_copyable_v<OloGroomFormat::LodLevelHeader>);
    static_assert(std::is_standard_layout_v<OloGroomFormat::LodLevelHeader>);
    static_assert(sizeof(OloGroomFormat::LodLevelHeader) == 16);
} // namespace OloEngine
