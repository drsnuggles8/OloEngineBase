#pragma once

#include "OloEngine/Core/Base.h"

#include <span>
#include <string>
#include <string_view>
#include <vector>

// Offline block-texture compression (#440).
//
// This is the CPU / cook-time side of the texture-compression pipeline: it turns
// decoded 8-bit pixels into a GPU-ready BCn mip chain, (de)serializes that chain to
// a self-contained ".olotex" container, and offers a one-call file cook. The GL
// upload of the resulting blocks lives in the OpenGL backend
// (Platform/OpenGL/OpenGLTexture.cpp); this header intentionally has no GL / renderer
// dependency so it can be unit-tested without a graphics context.
//
// Encoders come from the vendored bc7enc_rdo library (BC7 via bc7enc, BC5 via rgbcx);
// they are pulled in only by TextureCompression.cpp so this header stays lightweight.

namespace OloEngine
{
    // BCn formats produced by the cook. Values are persisted in the .olotex container
    // header, so only ever append new entries — never renumber.
    enum class TextureCompressionFormat : u32
    {
        None = 0,
        BC7 = 1, // RGBA, high quality, 16 bytes / 4x4 block. Base color / albedo / emissive.
        BC5 = 2, // Two-channel (R,G), 16 bytes / 4x4 block. Tangent-space normal xy.
    };

    // A GPU-ready block-compressed image: a mip chain of raw BCn block bytes (mip 0 first).
    struct CompressedTextureImage
    {
        TextureCompressionFormat Format = TextureCompressionFormat::None;
        u32 Width = 0;         // base-level width in texels
        u32 Height = 0;        // base-level height in texels
        bool SRGB = false;     // source colour space; selects the sRGB GL internal format on upload
        bool HasAlpha = false; // true if the source carried a meaningful alpha channel (BC7 from
                               // 4-channel input); drives Texture::HasAlphaChannel() so an opaque
                               // BC7 albedo isn't mis-sorted into the transparent pass.
        // One entry per mip level (mip 0 = full resolution). Each holds the tightly
        // packed BCn blocks for that level: ceil(w/4) * ceil(h/4) * BlockSizeBytes.
        std::vector<std::vector<u8>> Mips;
        // Runtime-only: the source .olotex path, set by the asset loader (NOT persisted
        // by SerializeToBlob). Lets the GPU texture report GetPath() so the asset-pack
        // serializer can re-read + embed the container. Empty for test/procedural images.
        std::string SourcePath;

        [[nodiscard]] bool IsValid() const
        {
            return Format != TextureCompressionFormat::None && Width > 0 && Height > 0 && !Mips.empty();
        }

        [[nodiscard]] u32 MipLevels() const
        {
            return static_cast<u32>(Mips.size());
        }
    };

    namespace TextureCompression
    {
        // Filename -> "is this a colour (sRGB) texture?" heuristic. Single source of
        // truth shared by the offline cook and the asset loader
        // (TextureSerializer::IsLikelyColorTextureByName delegates here) so the two can
        // never disagree on the same filename. Data-texture keywords (normal / metallic /
        // roughness / AO / height / ...) veto colour keywords (albedo / diffuse / base
        // color / emissive). Returns true for colour.
        [[nodiscard]] bool IsLikelyColorTexture(std::string_view filename);
    } // namespace TextureCompression

    namespace TextureCompression
    {
        // ---- Block geometry ---------------------------------------------------
        // Bytes per 4x4 block (BC5 and BC7 are both 16). 0 for None.
        [[nodiscard]] u32 BlockSizeBytes(TextureCompressionFormat format);
        // Number of 4x4 blocks spanning a mip dimension: ceil(dim / 4), at least 1.
        [[nodiscard]] u32 BlockCount(u32 dimension);
        // Byte size of one mip level's block data for the given texel dimensions.
        [[nodiscard]] sizet MipByteSize(TextureCompressionFormat format, u32 width, u32 height);

        // ---- Encode -----------------------------------------------------------
        // Source pixels are tightly packed, `channels` bytes/texel, row-major.
        // `generateMips` builds the full box-filtered chain; otherwise mip 0 only.
        //
        // EncodeBC7 expands the source to RGBA (missing channels: G/B copy R for 1-ch,
        // A defaults to 255) before encoding all four channels.
        // EncodeBC5 encodes source channels 0 and 1 (R,G) — intended for normal xy.
        [[nodiscard]] CompressedTextureImage EncodeBC7(const u8* pixels, u32 width, u32 height, u32 channels, bool srgb, bool generateMips);
        [[nodiscard]] CompressedTextureImage EncodeBC5(const u8* pixels, u32 width, u32 height, u32 channels, bool generateMips);

        // ---- Decode (CPU) -----------------------------------------------------
        // Decompress one mip level to RGBA8 (4 bytes/texel). BC5 fills b=0, a=255.
        // Used by tests and the no-BPTC-hardware fallback upload path.
        [[nodiscard]] bool DecodeToRGBA8(const CompressedTextureImage& image, u32 mipLevel,
                                         std::vector<u8>& outRGBA8, u32& outWidth, u32& outHeight);

        // ---- Container (.olotex) ---------------------------------------------
        [[nodiscard]] std::vector<u8> SerializeToBlob(const CompressedTextureImage& image);
        [[nodiscard]] bool DeserializeFromBlob(std::span<const u8> blob, CompressedTextureImage& out);
        [[nodiscard]] bool WriteFile(const std::string& path, const CompressedTextureImage& image);
        [[nodiscard]] bool ReadFile(const std::string& path, CompressedTextureImage& out);

        // ---- Offline cook -----------------------------------------------------
        struct CompressOptions
        {
            // None => auto-pick from the filename: color textures (albedo/emissive/...)
            // become sRGB BC7, everything else linear BC5 is NOT auto-chosen (BC5 is a
            // deliberate opt-in for two-channel normal data); auto defaults to BC7.
            TextureCompressionFormat Format = TextureCompressionFormat::None;
            bool SRGB = false;            // color-space hint for BC7 (ignored for BC5)
            bool AutoSRGBFromName = true; // when true, override SRGB using the filename heuristic
            bool GenerateMips = true;
        };

        // Load `srcImagePath` (any stb-supported image), encode per `options`, and write
        // the `.olotex` container to `dstOlotexPath`. Returns false on load/encode/write
        // failure (details logged). Pixels are loaded with the same vertical flip the
        // runtime texture loader uses, so the stored blocks upload without re-flipping.
        [[nodiscard]] bool CompressTextureFile(const std::string& srcImagePath, const std::string& dstOlotexPath,
                                               const CompressOptions& options);
    } // namespace TextureCompression
} // namespace OloEngine
