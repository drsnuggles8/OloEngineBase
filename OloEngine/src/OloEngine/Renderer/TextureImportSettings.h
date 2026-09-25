#pragma once

#include "OloEngine/Core/Base.h"

#include <optional>
#include <string>
#include <string_view>

// Per-texture import settings (#624 item 2).
//
// The cook's automatic format choice (TextureCompression::CompressImageFile) can tell an
// HDR source from an LDR one and can guess a colour space from the filename, but it can
// NOT tell a two-channel tangent-space normal map from any other linear two/three-channel
// data — a roughness, AO or height map looks identical to it. That is why #440 shipped
// BC5 as an opt-in and why issue #624 asks for a *reliable signal* rather than a smarter
// filename guess: guessing wrong turns someone's roughness map into a two-channel normal
// map and silently discards a channel.
//
// The signal is a sidecar file next to the source image, "<image>.oloimport":
//
//     TextureImportSettings:
//       Version: 1
//       Format: BC5          # Auto | BC7 | BC5 | BC4 | BC6H | BC6HSigned
//       ColorSpace: Linear   # Auto | Linear | sRGB
//       GenerateMips: true   # omit for Auto
//       AlphaMipChain: Auto  # Auto | Coverage | Box
//
// Every field is optional and every omitted field means "Auto", i.e. exactly the
// behaviour the cook had before this file existed. A project with no sidecars cooks
// bit-identically to #440's pipeline.
//
// Where an alpha-tested texture's cutoff comes from (#1453): NOWHERE, because the cook
// does not need one. A cutout's mip chain is histogram-matched to level 0
// (AlphaCoverageMips.h), which keeps the alpha-test coverage of every level equal to
// level 0's at every cutoff at once, so the Mask material at 0.25 and the foliage
// layer at 0.5 that share one texture both get a correct chain and the author has
// nothing to keep in step. What the cook does need is to know the texture IS a
// cutout, and it measures that from the pixels (AlphaCoverageMips::IsCutoutAlpha:
// mostly transparent-or-opaque alpha). `AlphaMipChain` overrides the measurement for
// a texture it gets wrong: Coverage forces the cutout chain, Box forces the plain
// averaged chain an alpha-BLENDED texture wants.
//
// Sidecars are chosen over registry metadata deliberately: the cook runs from a *path*
// and must work with no project loaded (offline cooks and the unit tests both do), the
// setting survives asset-handle churn, and a text file next to the texture is
// diffable and reviewable.

namespace OloEngine
{
    struct TextureImportSettings
    {
        // Explicit block format for this texture. Auto keeps the cook's own choice
        // (BC6H for an HDR source, BC7 otherwise); BC5 is only ever reachable this way.
        enum class FormatChoice : u8
        {
            Auto = 0,
            BC7,
            BC5,
            BC6H,
            BC6HSigned,
            BC4, // single-channel; see TextureCompression::EncodeBC4
        };

        // Explicit colour space. Auto keeps the filename heuristic
        // (TextureCompression::IsLikelyColorTexture), which only ever applies to BC7.
        enum class ColorSpaceChoice : u8
        {
            Auto = 0,
            Linear,
            SRGB,
        };

        // How the cook builds a BC7 texture's alpha mips. Auto measures the alpha
        // (AlphaCoverageMips::IsCutoutAlpha); see the top of this file.
        enum class AlphaMipChainChoice : u8
        {
            Auto = 0,
            Coverage, // alpha cutout: coverage kept at every cutoff, chain stops at 64 texels
            Box,      // alpha averaged, full chain: blended or data alpha
        };

        FormatChoice Format = FormatChoice::Auto;
        ColorSpaceChoice ColorSpace = ColorSpaceChoice::Auto;
        // Unset means "keep the caller's choice"; set overrides it.
        std::optional<bool> GenerateMips;
        AlphaMipChainChoice AlphaMipChain = AlphaMipChainChoice::Auto;

        // True when nothing is overridden, i.e. loading this file changed nothing.
        [[nodiscard]] bool IsAllAuto() const
        {
            return Format == FormatChoice::Auto && ColorSpace == ColorSpaceChoice::Auto && !GenerateMips.has_value() &&
                   AlphaMipChain == AlphaMipChainChoice::Auto;
        }
    };

    namespace TextureImport
    {
        // The extension appended to the source image path, including the dot.
        inline constexpr std::string_view kSidecarExtension = ".oloimport";

        // "Assets/T/Foo_Normal.png" -> "Assets/T/Foo_Normal.png.oloimport". The full
        // source filename is kept (not replaced) so "Foo.png" and "Foo.tga" in one
        // directory keep separate settings.
        [[nodiscard]] std::string SidecarPathFor(std::string_view sourceImagePath);

        // Read the sidecar for `sourceImagePath` if one exists.
        // Returns true only when a sidecar was present AND parsed; a missing sidecar
        // returns false with `out` left at all-Auto and nothing logged (that is the
        // normal case), while a present-but-malformed one returns false and logs an
        // error — a typo in an import setting must not silently cook the wrong format.
        [[nodiscard]] bool LoadForImage(std::string_view sourceImagePath, TextureImportSettings& out);

        // Write the sidecar for `sourceImagePath`. Returns false (and logs) on I/O
        // failure.
        [[nodiscard]] bool SaveForImage(std::string_view sourceImagePath, const TextureImportSettings& settings);

        // Parse / emit the sidecar body. Split out from the file I/O so the format is
        // unit-testable without touching the filesystem.
        [[nodiscard]] bool Parse(std::string_view yaml, TextureImportSettings& out);
        [[nodiscard]] std::string Emit(const TextureImportSettings& settings);
    } // namespace TextureImport
} // namespace OloEngine
