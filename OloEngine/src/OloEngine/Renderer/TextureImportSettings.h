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
//       Format: BC5          # Auto | BC7 | BC5 | BC6H | BC6HSigned
//       ColorSpace: Linear   # Auto | Linear | sRGB
//       GenerateMips: true   # omit for Auto
//
// Every field is optional and every omitted field means "Auto", i.e. exactly the
// behaviour the cook had before this file existed. A project with no sidecars cooks
// bit-identically to #440's pipeline.
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
        };

        // Explicit colour space. Auto keeps the filename heuristic
        // (TextureCompression::IsLikelyColorTexture), which only ever applies to BC7.
        enum class ColorSpaceChoice : u8
        {
            Auto = 0,
            Linear,
            SRGB,
        };

        FormatChoice Format = FormatChoice::Auto;
        ColorSpaceChoice ColorSpace = ColorSpaceChoice::Auto;
        // Unset means "keep the caller's choice"; set overrides it.
        std::optional<bool> GenerateMips;

        // True when nothing is overridden, i.e. loading this file changed nothing.
        [[nodiscard]] bool IsAllAuto() const
        {
            return Format == FormatChoice::Auto && ColorSpace == ColorSpaceChoice::Auto && !GenerateMips.has_value();
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
