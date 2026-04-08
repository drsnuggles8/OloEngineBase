#pragma once

#include "OloEngine/Core/Base.h"

#include <array>

namespace OloEngine
{
    // ============================================================================
    // .omesh Binary Mesh Format — Version 1
    //
    // Layout:
    //   [FileHeader]
    //   [SectionDirectory]           — offsets + sizes for each section
    //   [Geometry Section]           — encoded vertices + indices
    //   [Submesh Section]            — submesh table
    //   [Material Section]           — material index → asset handle map
    //   [Skeleton Section]           — optional: bone hierarchy + transforms
    //   [BoneInfluence Section]      — optional: per-vertex bone weights
    //   [MorphTarget Section]        — optional: blend shape deltas
    //   [BoneInfo Section]           — optional: inverse bind poses
    //
    // All multi-byte values are little-endian.
    // String table entries: u32 length + UTF-8 bytes (no null terminator).
    // ============================================================================

    namespace OMeshFormat
    {
        constexpr u32 MagicNumber = 0x4853454D; // "MESH" in little-endian
        constexpr u32 CurrentVersion = 1;
        constexpr u32 FlagCompressed = 1;   // Payload is zlib-compressed
        constexpr u32 FlagPreOptimized = 2; // Mesh was already optimized before caching

        // ── Safety caps for deserialized counts (defence against corrupt files) ──
        constexpr u32 MaxVertexCount = 50'000'000;
        constexpr u32 MaxIndexCount = 150'000'000;
        constexpr u32 MaxSubmeshCount = 10'000;
        constexpr u32 MaxMaterialCount = 10'000;
        constexpr u32 MaxBoneCount = 1'024;
        constexpr u32 MaxMorphTargetCount = 1'000;
        constexpr u64 MaxEncodedSize = 2'000'000'000; // 2 GB

        // Section identifiers
        enum class SectionType : u16
        {
            Geometry = 0,
            Submeshes = 1,
            Materials = 2,
            Skeleton = 3,
            BoneInfluences = 4,
            MorphTargets = 5,
            BoneInfo = 6,
            Count = 7 // sentinel
        };

        constexpr auto kSectionCount = static_cast<u16>(SectionType::Count);

        struct FileHeader
        {
            u32 Magic = MagicNumber;
            u32 Version = CurrentVersion;
            u32 Flags = 0;                   // Bit 0: zlib-compressed payload
            u32 Checksum = 0;                // CRC32 of payload (after header)
            u64 SourceTimestamp = 0;         // Source file last-modified time (for cache invalidation)
            u64 TotalFileSize = 0;           // Total file size for integrity check
            u64 UncompressedPayloadSize = 0; // Size of payload before compression (0 if uncompressed)
        };

        struct SectionEntry
        {
            u64 Offset = 0; // Byte offset from start of file
            u64 Size = 0;   // Section size in bytes (0 = section not present)
        };

        struct SectionDirectory
        {
            std::array<SectionEntry, kSectionCount> Sections{};
        };

        // Geometry section header (immediately after geometry section offset)
        struct GeometryHeader
        {
            u32 VertexCount = 0;
            u32 IndexCount = 0;
            u32 VertexStride = 0; // sizeof(Vertex), for forward compat
            u32 ShadowIndexCount = 0;
            u64 EncodedVertexSize = 0;
            u64 EncodedIndexSize = 0;
            u64 EncodedShadowIndexSize = 0;
            // Followed by:
            //   [EncodedVertexSize bytes]  — meshoptimizer-encoded vertex data
            //   [EncodedIndexSize bytes]   — meshoptimizer-encoded index data
            //   [EncodedShadowIndexSize bytes] — meshoptimizer-encoded shadow indices
        };

        // Submesh section header
        struct SubmeshHeader
        {
            u32 SubmeshCount = 0;
            // Followed by SubmeshCount × SubmeshEntry
        };

        // Fixed-size submesh fields (written before variable-length strings)
        struct SubmeshEntry
        {
            f32 Transform[16]{};      // Column-major mat4
            f32 LocalTransform[16]{}; // Column-major mat4
            f32 BoundsMin[3]{};       // BoundingBox min
            f32 BoundsMax[3]{};       // BoundingBox max
            u32 BaseVertex = 0;
            u32 BaseIndex = 0;
            u32 MaterialIndex = 0;
            u32 IndexCount = 0;
            u32 VertexCount = 0;
            u8 IsRigged = 0;
            u8 Padding[3]{};
            // Followed by:
            //   u32 NodeNameLength + NodeName bytes
            //   u32 MeshNameLength + MeshName bytes
        };

        // Material section header
        struct MaterialHeader
        {
            u32 MaterialCount = 0;
            // Followed by MaterialCount × MaterialEntry
        };

        struct MaterialEntry
        {
            u32 Index = 0;
            u64 Handle = 0; // AssetHandle
        };

        // Skeleton section header
        struct SkeletonHeader
        {
            u32 BoneCount = 0;
            // Followed by:
            //   BoneCount × i32 parent indices
            //   BoneCount × mat4 local transforms
            //   BoneCount × mat4 global transforms
            //   BoneCount × mat4 bind pose matrices
            //   BoneCount × mat4 inverse bind poses
            //   BoneCount × mat4 bind pose local transforms
            //   BoneCount × mat4 bone pre-transforms
            //   BoneCount × string (u32 len + bytes) bone names
        };

        // BoneInfluence section header
        struct BoneInfluenceHeader
        {
            u32 InfluenceCount = 0;  // Should equal vertex count
            u32 InfluenceStride = 0; // sizeof(BoneInfluence)
            u64 EncodedSize = 0;
            // Followed by EncodedSize bytes of meshoptimizer-encoded data
        };

        // BoneInfo section header
        struct BoneInfoHeader
        {
            u32 BoneInfoCount = 0;
            // Followed by BoneInfoCount × BoneInfoEntry
        };

        struct BoneInfoEntry
        {
            f32 InverseBindPose[16]{}; // Column-major mat4
            u32 BoneIndex = 0;
            u32 Padding = 0;
        };

        // MorphTarget section header
        struct MorphTargetHeader
        {
            u32 TargetCount = 0;
            u32 VertexCount = 0; // Per-target vertex count (dense form)
            // Followed by TargetCount × MorphTargetEntry
        };

        struct MorphTargetEntry
        {
            u32 SparseEntryCount = 0; // 0 = dense, >0 = sparse
            // Followed by:
            //   u32 NameLength + Name bytes
            //   If sparse: SparseEntryCount × (u32 vertexIndex + MorphTargetVertex)
            //   If dense:  VertexCount × MorphTargetVertex
        };

    } // namespace OMeshFormat

    // ============================================================================
    // .oanim Binary Animation Format — Version 1
    //
    // Layout:
    //   [FileHeader]
    //   [ClipCount (u32)]
    //   [ClipDirectory]  — per-clip offset + size
    //   [Clip 0 data]
    //   [Clip 1 data]
    //   ...
    //
    // Each clip:
    //   [ClipHeader]  — name, duration, channel count
    //   [BoneAnimation channels]
    //   [MorphTargetKeyframes]
    // ============================================================================

    namespace OAnimFormat
    {
        constexpr u32 MagicNumber = 0x4D494E41; // "ANIM" in little-endian
        constexpr u32 CurrentVersion = 1;
        constexpr u32 FlagCompressed = 1; // Payload is zlib-compressed

        struct FileHeader
        {
            u32 Magic = MagicNumber;
            u32 Version = CurrentVersion;
            u32 Flags = 0;    // Bit 0: zlib-compressed payload
            u32 Checksum = 0; // CRC32 of payload (after header)
            u64 SourceTimestamp = 0;
            u64 TotalFileSize = 0;
            u64 UncompressedPayloadSize = 0; // Size of payload before compression (0 if uncompressed)
        };

        struct ClipDirectoryEntry
        {
            u64 Offset = 0;
            u64 Size = 0;
        };

        struct ClipHeader
        {
            f32 Duration = 0.0f;
            u32 BoneChannelCount = 0;
            u32 MorphKeyframeCount = 0;
            u32 Reserved = 0; // For future: event marker count
            // Followed by:
            //   u32 NameLength + Name bytes
            //   BoneChannelCount × BoneChannelData
            //   MorphKeyframeCount × MorphKeyframeData
        };

        // Per-bone channel: name + keyframe arrays
        struct BoneChannelHeader
        {
            u32 PositionKeyCount = 0;
            u32 RotationKeyCount = 0;
            u32 ScaleKeyCount = 0;
            u32 Padding = 0;
            // Followed by:
            //   u32 BoneNameLength + BoneName bytes
            //   PositionKeyCount × PositionKey (f64 time + vec3)
            //   RotationKeyCount × RotationKey (f64 time + quat)
            //   ScaleKeyCount × ScaleKey (f64 time + vec3)
        };

        struct PositionKey
        {
            f64 Time;
            f32 Position[3];
            f32 Padding;
        };

        struct RotationKey
        {
            f64 Time;
            f32 Rotation[4]; // w, x, y, z
        };

        struct ScaleKey
        {
            f64 Time;
            f32 Scale[3];
            f32 Padding;
        };

        struct MorphKeyframe
        {
            f64 Time;
            f32 Weight;
            u32 Padding;
            // Followed by: u32 TargetNameLength + TargetName bytes
        };

    } // namespace OAnimFormat

} // namespace OloEngine
