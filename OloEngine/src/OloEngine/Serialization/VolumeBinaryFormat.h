#pragma once

#include "OloEngine/Core/Base.h"

#include <type_traits>
#include <utility>

namespace OloEngine
{
    // ============================================================================
    // .olovol Binary Volume Format — Version 1 (issue #724)
    //
    // The engine-native, cook-time-produced dense volumetric density format.
    // OpenVDB import (OloEngine-VolumeCook, editor/cook-only) resamples a sparse
    // .vdb grid into this dense layout once; the runtime reader in
    // OloEngine/src/OloEngine/Asset/Serializers/VolumeSerializer.cpp is pure CPU
    // (no OpenVDB dependency — see docs/agent-rules/asset-import-usd-alembic.md
    // for why that split exists).
    //
    // Layout:
    //   [FileHeader]
    //   [payload — zlib-compressed when FlagCompressed is set]
    //
    // The payload is a sequence of framed sections, each
    //   [SectionFrame { SectionId, ByteCount }] [ByteCount bytes of payload]
    // in this fixed order, each exactly once:
    //   Section 0 Info    — InfoSection (dimensions, voxel size, grid transform,
    //                       background value)
    //   Section 1 Density — raw f32 scalar density, ByteCount = W*H*D*4
    //
    // All multi-byte values are little-endian. FileHeader::Checksum is the
    // CRC32 of the payload bytes as stored on disk (i.e. of the COMPRESSED
    // bytes when FlagCompressed is set) — mirrors .olmap (LightmapBinaryFormat.h).
    //
    // Versioning (docs/agent-rules/binary-format-versioning.md): like .olmap, a
    // .olovol is a DERIVED artifact — fully regenerable by re-importing the
    // source .vdb. MinSupportedVersion and CurrentVersion move together: any
    // layout change bumps both, old files are rejected outright, and the cost
    // is one re-import rather than a migration path to maintain forever.
    // ============================================================================

    namespace OLoVolFormat
    {
        constexpr u32 MagicNumber = 0x4C4F564F; // "OVOL" in little-endian
        constexpr u32 CurrentVersion = 1;
        constexpr u32 MinSupportedVersion = 1; // == CurrentVersion, on purpose — see header comment

        constexpr u32 FlagCompressed = 1; // Bit 0: payload is zlib-compressed

        // ── Safety caps for deserialized values (defence against corrupt files) ──
        // Enforced symmetrically: the writer refuses to produce a file the
        // reader would reject. 512^3 dense f32 is 512 MiB — already a very
        // large single volume; a hostile/corrupt header cannot ask for more.
        constexpr u32 MaxAxisDimension = 512;
        constexpr u64 MaxUncompressedPayloadSize = 1'200'000'000; // ~1.1 GiB
        constexpr u64 MaxCompressedPayloadSize = 1'200'000'000;

        enum class SectionType : u16
        {
            Info = 0,
            Density = 1,
            Count = 2 // sentinel
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

        // Section 0 payload. GridTransform is the grid-index -> object-local
        // transform preserved from the source .vdb (translation + rotation +
        // non-uniform scale from OpenVDB's affine map), row-major 4x4 — this is
        // the metadata the issue names as the reason not to just bake slices in
        // a DCC. VoxelSize is redundant with the scale baked into GridTransform
        // but kept explicit: it is what a raymarch shader wants directly,
        // without decomposing a matrix per frame.
        struct InfoSection
        {
            u32 Width = 0;
            u32 Height = 0;
            u32 Depth = 0;
            u32 Pad0 = 0;
            f32 VoxelSize[3] = { 1.0f, 1.0f, 1.0f };
            f32 BackgroundValue = 0.0f;
            f32 GridTransform[16] = {
                1.0f, 0.0f, 0.0f, 0.0f,
                0.0f, 1.0f, 0.0f, 0.0f,
                0.0f, 0.0f, 1.0f, 0.0f,
                0.0f, 0.0f, 0.0f, 1.0f
            };
        };
    } // namespace OLoVolFormat

    // ── Compile-time ABI guards for wire-format structs ──────────────
    // Any padding or field-order change will break binary compatibility.

    static_assert(std::is_trivially_copyable_v<OLoVolFormat::FileHeader>);
    static_assert(std::is_standard_layout_v<OLoVolFormat::FileHeader>);
    static_assert(sizeof(OLoVolFormat::FileHeader) == 24);

    static_assert(std::is_trivially_copyable_v<OLoVolFormat::SectionFrame>);
    static_assert(std::is_standard_layout_v<OLoVolFormat::SectionFrame>);
    static_assert(sizeof(OLoVolFormat::SectionFrame) == 16);

    static_assert(std::is_trivially_copyable_v<OLoVolFormat::InfoSection>);
    static_assert(std::is_standard_layout_v<OLoVolFormat::InfoSection>);
    static_assert(sizeof(OLoVolFormat::InfoSection) == 96);

} // namespace OloEngine
