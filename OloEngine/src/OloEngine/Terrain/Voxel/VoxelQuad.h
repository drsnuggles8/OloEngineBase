#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Terrain/Voxel/VoxelOverride.h"

#include <bit>

namespace OloEngine
{
    // =========================================================================
    // Packed quad record for the binary greedy mesher (issue #727)
    // =========================================================================
    //
    // One merged, axis-aligned quad per record. The GPU draws a shared unit
    // quad instanced once per record and rebuilds the corners in the vertex
    // stage — see assets/shaders/include/VoxelQuadUnpack.glsl, which is the
    // mirror of this file and MUST be edited in lockstep with it.
    //
    // ── Face direction ────────────────────────────────────────────────────
    // The numbering below is on the GPU wire. Changing it silently reorients
    // every quad, so it is a versioned contract, not an implementation detail.

    enum class VoxelFace : u8
    {
        PosX = 0,
        NegX = 1,
        PosY = 2,
        NegY = 3,
        PosZ = 4,
        NegZ = 5,
        Count = 6
    };

    namespace VoxelQuadEncoding
    {
        // Voxels per chunk edge. The encoding is derived from this, so a chunk
        // resize is a compile error (below) rather than a silent truncation.
        inline constexpr u32 kChunkSize = VoxelChunk::CHUNK_SIZE;

        // Bits per position component: enough to address [0, kChunkSize).
        inline constexpr u32 kPosBits = static_cast<u32>(std::bit_width(kChunkSize - 1u));

        // Bits per extent. Extents live in [1, kChunkSize] and are stored
        // BIASED BY ONE, so the same width that addresses a position also
        // expresses a full-chunk-span run.
        //
        // This is the one place we deliberately diverge from the reference
        // implementation (D:\repos\VoxelEngine, voxel_mesher.tpp
        // `_CompressQuadData`). It masks width/height with 0x1F and stores them
        // unbiased, so at a 32-voxel chunk a 32-wide run wraps to 0 and the
        // quad disappears — the documented "cannot express 32^3 chunks"
        // limitation. Biasing costs nothing and removes the limitation
        // outright, which matters here because VoxelChunk::CHUNK_SIZE IS 32:
        // an unbiased encoding would drop exactly the quads greedy meshing
        // exists to produce (a flat 32x32 chunk face is the best case).
        inline constexpr u32 kExtentBits = kPosBits;

        inline constexpr u32 kFaceBits = 3; // 6 directions

        inline constexpr u32 kXShift = 0;
        inline constexpr u32 kYShift = kXShift + kPosBits;
        inline constexpr u32 kZShift = kYShift + kPosBits;
        inline constexpr u32 kWidthShift = kZShift + kPosBits;
        inline constexpr u32 kHeightShift = kWidthShift + kExtentBits;
        inline constexpr u32 kFaceShift = kHeightShift + kExtentBits;
        inline constexpr u32 kUsedBits = kFaceShift + kFaceBits;

        inline constexpr u32 kPosMask = (1u << kPosBits) - 1u;
        inline constexpr u32 kExtentMask = (1u << kExtentBits) - 1u;
        inline constexpr u32 kFaceMask = (1u << kFaceBits) - 1u;

        // A 32-bit geometry word is the whole point: the instance stream stays
        // 8 bytes per quad (geometry + material) instead of the 120 bytes a
        // naive per-face mesh spends on 4 position/normal vertices plus 6
        // indices. Widening to 64 bits would be the move for a >32 chunk; this
        // assertion is what forces that decision to be made explicitly.
        static_assert(kUsedBits <= 32,
                      "VoxelQuad geometry no longer fits in 32 bits. VoxelChunk::CHUNK_SIZE grew past "
                      "32 - widen PackedQuad::Geometry to u64 and update VoxelQuadUnpack.glsl to match.");

        // The material word is a separate u32 rather than stuffed into the 4
        // spare geometry bits: 16 materials is not enough for a terrain palette,
        // and the instance stride is 8 bytes either way once the u32 pair is
        // aligned. Bits 8..31 are reserved (per-quad tint / baked AO are the
        // obvious next tenants) and MUST be written as zero so a future reader
        // can tell "unset" from "black".
        inline constexpr u32 kMaterialIndexBits = 8;
        inline constexpr u32 kMaterialIndexMask = (1u << kMaterialIndexBits) - 1u;
    } // namespace VoxelQuadEncoding

    // Per-instance GPU record. 8 bytes; must stay trivially copyable so it can
    // be memcpy'd straight into the instance VBO.
    struct PackedQuad
    {
        u32 Geometry = 0;
        u32 Material = 0;

        auto operator==(const PackedQuad& other) const -> bool = default;
    };

    static_assert(sizeof(PackedQuad) == 8, "PackedQuad must stay 8 bytes - the instance VBO stride depends on it");
    static_assert(std::is_trivially_copyable_v<PackedQuad>, "PackedQuad is memcpy'd into a GPU buffer");

    // CPU-side view of a decoded quad. Positions are the minimum corner of the
    // face-owning voxel, in chunk-local voxel units.
    struct VoxelQuadFields
    {
        u32 X = 0;
        u32 Y = 0;
        u32 Z = 0;
        u32 Width = 1;  // extent along the face's U axis, in voxels, [1, CHUNK_SIZE]
        u32 Height = 1; // extent along the face's V axis, in voxels, [1, CHUNK_SIZE]
        VoxelFace Face = VoxelFace::PosX;

        auto operator==(const VoxelQuadFields& other) const -> bool = default;
    };

    namespace VoxelQuadCodec
    {
        // True when every field is inside the range the encoding can express.
        // Callers that build quads from untrusted data should test this first;
        // EncodeQuad asserts on it in debug and masks in release.
        [[nodiscard]] constexpr bool IsEncodable(const VoxelQuadFields& q) noexcept
        {
            using namespace VoxelQuadEncoding;
            return q.X < kChunkSize && q.Y < kChunkSize && q.Z < kChunkSize &&
                   q.Width >= 1u && q.Width <= kChunkSize &&
                   q.Height >= 1u && q.Height <= kChunkSize &&
                   static_cast<u32>(q.Face) < static_cast<u32>(VoxelFace::Count);
        }

        [[nodiscard]] constexpr u32 EncodeGeometry(const VoxelQuadFields& q) noexcept
        {
            using namespace VoxelQuadEncoding;
            return ((q.X & kPosMask) << kXShift) |
                   ((q.Y & kPosMask) << kYShift) |
                   ((q.Z & kPosMask) << kZShift) |
                   (((q.Width - 1u) & kExtentMask) << kWidthShift) |
                   (((q.Height - 1u) & kExtentMask) << kHeightShift) |
                   ((static_cast<u32>(q.Face) & kFaceMask) << kFaceShift);
        }

        [[nodiscard]] constexpr VoxelQuadFields DecodeGeometry(u32 geometry) noexcept
        {
            using namespace VoxelQuadEncoding;
            VoxelQuadFields q;
            q.X = (geometry >> kXShift) & kPosMask;
            q.Y = (geometry >> kYShift) & kPosMask;
            q.Z = (geometry >> kZShift) & kPosMask;
            q.Width = ((geometry >> kWidthShift) & kExtentMask) + 1u;
            q.Height = ((geometry >> kHeightShift) & kExtentMask) + 1u;
            q.Face = static_cast<VoxelFace>((geometry >> kFaceShift) & kFaceMask);
            return q;
        }

        [[nodiscard]] constexpr u32 EncodeMaterial(u8 materialIndex) noexcept
        {
            return static_cast<u32>(materialIndex) & VoxelQuadEncoding::kMaterialIndexMask;
        }

        [[nodiscard]] constexpr u8 DecodeMaterialIndex(u32 material) noexcept
        {
            return static_cast<u8>(material & VoxelQuadEncoding::kMaterialIndexMask);
        }

        [[nodiscard]] constexpr PackedQuad Encode(const VoxelQuadFields& q, u8 materialIndex) noexcept
        {
            return PackedQuad{ EncodeGeometry(q), EncodeMaterial(materialIndex) };
        }
    } // namespace VoxelQuadCodec

    // =========================================================================
    // Face basis - the CPU half of the contract the vertex shader mirrors.
    // =========================================================================
    //
    // For each direction: the outward normal, and the two unit axes U and V the
    // quad extends along. U/V are ordered so cross(U, V) == normal, which is
    // what makes the shared unit quad's fixed {0,1,2, 2,3,0} winding come out
    // counter-clockwise from outside for all six directions. Swapping a row's
    // U and V inverts that face - the classic inside-out-quad bug, invisible
    // until you fly past that one direction with backface culling on.

    namespace VoxelFaceBasis
    {
        // Outward normal per VoxelFace, as integer axis components.
        inline constexpr i32 kNormal[6][3] = {
            { 1, 0, 0 },  // PosX
            { -1, 0, 0 }, // NegX
            { 0, 1, 0 },  // PosY
            { 0, -1, 0 }, // NegY
            { 0, 0, 1 },  // PosZ
            { 0, 0, -1 }, // NegZ
        };

        // Quad U axis (scaled by Width) per VoxelFace.
        inline constexpr i32 kAxisU[6][3] = {
            { 0, 1, 0 }, // PosX -> +Y
            { 0, 0, 1 }, // NegX -> +Z
            { 0, 0, 1 }, // PosY -> +Z
            { 1, 0, 0 }, // NegY -> +X
            { 1, 0, 0 }, // PosZ -> +X
            { 0, 1, 0 }, // NegZ -> +Y
        };

        // Quad V axis (scaled by Height) per VoxelFace.
        inline constexpr i32 kAxisV[6][3] = {
            { 0, 0, 1 }, // PosX -> +Z
            { 0, 1, 0 }, // NegX -> +Y
            { 1, 0, 0 }, // PosY -> +X
            { 0, 0, 1 }, // NegY -> +Z
            { 0, 1, 0 }, // PosZ -> +Y
            { 1, 0, 0 }, // NegZ -> +X
        };

        // Offset from the face-owning voxel's minimum corner to the quad's
        // origin corner: the positive-facing directions sit one voxel further
        // along their axis.
        inline constexpr i32 kOriginOffset[6][3] = {
            { 1, 0, 0 }, // PosX
            { 0, 0, 0 }, // NegX
            { 0, 1, 0 }, // PosY
            { 0, 0, 0 }, // NegY
            { 0, 0, 1 }, // PosZ
            { 0, 0, 0 }, // NegZ
        };
    } // namespace VoxelFaceBasis
} // namespace OloEngine
