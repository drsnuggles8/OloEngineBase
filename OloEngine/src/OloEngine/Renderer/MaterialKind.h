#pragma once

#include "OloEngine/Core/Base.h"

namespace OloEngine
{

    // What a surface IS (issue #1231) — deliberately NOT what closure evaluates
    // it. `PBRModel` (Renderer/PBRModel.h) is the versioned *algorithm*: the
    // exact BRDF a material shades with, bumped when the maths is corrected.
    // `MaterialKind` is the *category*: skin scatters, snow sparkles, everything
    // else is a generic dielectric/metal. The two are orthogonal on purpose —
    // a Skin material can shade with Legacy or ClosureV2, and correcting the
    // GGX denominator must not change what a surface is. See
    // docs/adr/0024-material-kind-is-not-the-closure-version.md.
    //
    // Mirrored in GLSL as OLO_MATERIAL_KIND_* (include/PBRCommon.glsl), carried
    // per material in PBRMaterialUBO::MaterialKind and per pixel in the deferred
    // G-Buffer RT2 flags lane. The numbering is on disk (scene YAML, save-games)
    // — append, never renumber.
    enum class MaterialKind : u8
    {
        // The default. Every material that predates #1231 deserializes to this
        // and shades exactly as it did.
        Generic = 0,
        // Snow.
        //
        // SETTING THIS DOES NOT TURN A MATERIAL INTO SNOW, and it is not meant
        // to. Snow coverage is a WORLD-SPACE effect selected by the scene's snow
        // parameters (u_SnowFlags / include/SnowLayer.glsl): a material layer
        // blended over whatever a surface already shades as, whose blur mask
        // rides the diffusion hand-off lane in a range disjoint from skin's
        // (issue #1451). That contract is independent of this field.
        //
        // The enumerator exists because snow IS the engine's second surface
        // kind, and leaving it out would have made this enum "generic and skin"
        // — an enum that answers "which kinds exist?" with a wrong answer. It
        // also reserves the value, so the day the snow overlay becomes a
        // per-material opt-in it does not renumber anything on disk.
        Snow = 1,
        // Authored skin. Selects the split diffuse/specular transport and names
        // a SkinProfile asset (Renderer/SkinProfile.h) for the scattering
        // parameters #1241 will consume.
        Skin = 2,
        // Authored vegetation (issue #1234). A THIN TWO-SIDED SURFACE, not a
        // scattering volume: a leaf is one lamina a fraction of a millimetre
        // thick, so light that enters the lit face leaves the other face in
        // essentially the same place. That is why this is its own kind rather
        // than a Skin profile with a small radius -- Skin's transport diffuses
        // energy ACROSS the surface (screen-space in #1241), which is exactly
        // the effect a leaf does not have, and borrowing it would have blurred
        // every leaf edge into its neighbour.
        //
        // What it selects is the transmission lobe in include/FoliageSurface.glsl,
        // evaluated from a per-pixel THICKNESS map and shadowed by the same
        // factor the reflected lobe uses. Everything else about the surface --
        // albedo, normal, roughness, the reflected BRDF -- stays generic.
        Foliage = 3
    };

    // One past the last valid kind — the shared upper bound for every
    // reject-to-Generic validation site (scene YAML, save-games, Lua, the
    // material UBO).
    inline constexpr i32 kMaterialKindCount = 4;

    // The G-Buffer flags lane gives the kind a FIXED-WIDTH two-bit field (see
    // oloEncodeGBufferPbrFlags in include/PBRCommon.glsl). Foliage (#1234) took
    // the fourth and LAST value the field can carry, so the static_assert below
    // is now exactly tight: a fifth kind is a deliberate re-encoding of the
    // lane -- a wider kind field paid for out of the model field's headroom, or
    // a dedicated attachment -- and not an enumerator that silently aliases
    // onto Generic. This is where that conversation starts.
    inline constexpr i32 kMaterialKindGBufferMax = 3;
    static_assert(kMaterialKindCount - 1 <= kMaterialKindGBufferMax,
                  "MaterialKind index exceeds the two-bit kind field in the deferred G-Buffer flags "
                  "lane — see oloEncodeGBufferPbrFlags in include/PBRCommon.glsl.");

    [[nodiscard]] inline constexpr const char* MaterialKindToString(MaterialKind kind) noexcept
    {
        switch (kind)
        {
            case MaterialKind::Generic:
                return "Generic";
            case MaterialKind::Snow:
                return "Snow";
            case MaterialKind::Skin:
                return "Skin";
            case MaterialKind::Foliage:
                return "Foliage";
        }
        return "Generic";
    }

    // Range check used by every deserializer. A value outside the enum is
    // authoring corruption or a file from a future build; the caller reports it
    // and falls back to Generic rather than indexing the shader's kind switch
    // with a number it has no branch for.
    [[nodiscard]] inline constexpr bool IsValidMaterialKind(i32 value) noexcept
    {
        return value >= 0 && value < kMaterialKindCount;
    }

} // namespace OloEngine
