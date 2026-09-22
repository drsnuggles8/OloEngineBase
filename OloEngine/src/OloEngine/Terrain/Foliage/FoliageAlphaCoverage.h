#pragma once

#include "OloEngine/Containers/Array.h"
#include "OloEngine/Containers/String.h"
#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Vertex.h"

#include <glm/glm.hpp>

#include <array>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

// FoliageAlphaCoverage.h — how much of a foliage layer's albedo survives its
// AlphaCutoff, and whether that is plausible for what the texture is drawn on
// (issue #1399).
//
// A layer pairs an albedo with a cutoff and nothing checked that the two agree.
// A tree whose canopy discards four fifths of its pixels generates instances,
// submits draws and passes every contract test; the only symptom is a
// see-through tree that cost a whole acceptance pass to find by eye. This is
// the one log line that turns that hunt into an answer.
//
// It is a DIAGNOSTIC. Nothing here changes what is rendered, clamps a cutoff or
// refuses a layer: the configuration is legal and an author may want it.
//
// ── What is measured: the surface, not the sheet ─────────────────────────────
//
// The fraction is taken over the texels the draw actually samples, weighted by
// how much surface samples them — not over the whole image. The two differ by
// a factor of 2.5 on the assets that ship: `fern_fronds.png` passes 17.7% of
// its SHEET and 45.0% of the mesh SURFACE that samples it, because much of
// that atlas is unused space. A sheet-level check would flag the fern as a
// see-through plant and teach authors to ignore the warning
// (docs/agent-rules/vegetation-asset-import.md rule 3).
//
// Every figure in this file is measured the way the ENGINE samples, which is
// not always the way the asset was authored: Model flips the v of every OBJ it
// imports (for legacy OBJ atlases), so an OBJ written with standard v-up UVs is
// sampled upside down. The diagnostic follows the engine on purpose — it
// reports what is drawn — and the difference is tracked as its own defect.
//
// For the flat card the sheet IS the surface — the quad maps the whole image
// once — so the card is measured over every texel. A mesh is measured at
// points spread over its triangles in proportion to their area, and the
// texture is then looked up at those points' UVs.
//
// ── Why the plausible band depends on the role ───────────────────────────────
//
// A billboard is SUPPOSED to be mostly empty: the shipped cards pass 6.1% to
// 40.5% of their texels, and 18% on a grass card is correct and idiomatic. The
// same 18% over the surface of an authored mesh means the texture is not the
// one the mesh was modelled for. One global threshold would either warn on
// every grass card or miss every see-through tree, so each role has its own:
//
//   Card          [2%, 90%]. Below 2% the billboard draws as a few stray
//                 pixels. Above 90% it draws as a solid rectangle, which is
//                 what pointing AlbedoPath at a plant's source atlas, or at an
//                 image without alpha, produces (vegetation-asset-import.md
//                 rule 5). Neither end is reached by any shipped card.
//   AuthoredMesh  >= 30% of the part's surface. Plant meshes here are built
//                 from cross-cards baked out of real geometry, so their
//                 transparency is real coverage: the shipped foliage parts
//                 pass 41.2% (dry grass) to 56.3% (shrub), trunks and bark
//                 100%. The stand-in art #1398 replaced — `grass.png` on the
//                 procedural pine — passes 19.4% of the pine's surface, and
//                 that tree was visibly see-through. 30% sits between them.
//                 There is no ceiling: solid geometry may be fully opaque.
//   ImpostorBake  the same floor as AuthoredMesh. The bake draws the layer
//                 albedo onto the mesh's UVs and alpha-tests it, so it is
//                 solid geometry textured exactly as the near mesh would be.
//
// ── Matching the shader's comparison exactly ────────────────────────────────
//
// Every foliage fragment stage discards on `alpha < cutoff`, with alpha the
// UNORM byte / 255 (sRGB decoding never touches alpha). A texel therefore
// passes iff `!(byte / 255.0f < cutoff)`, which is what `PassFraction`
// evaluates per histogram bin — including the NaN case, where the shader
// discards nothing. Only a 4-channel image has an alpha: a 1-, 2- or 3-channel
// file uploads as R8 / RG8 / RGB8 and samples alpha as 1.

namespace OloEngine::FoliageAlphaCoverage
{
    enum class Role : u8
    {
        Card,         // the layer albedo on the flat billboard quad
        AuthoredMesh, // one submesh of the authored plant mesh, with its own texture
        ImpostorBake, // the layer albedo on the whole mesh, as the impostor bake draws it
    };

    enum class Verdict : u8
    {
        Plausible,
        TooSparse, // passes less than the role's floor
        TooSolid,  // passes more than the role's ceiling (cards only)
    };

    // The inclusive range of pass fractions a role is expected to land in.
    struct Band
    {
        f32 Min = 0.0f;
        f32 Max = 1.0f;
    };

    [[nodiscard("Store this!")]] Band PlausibleBand(Role role) noexcept;
    [[nodiscard("Store this!")]] Verdict Judge(Role role, f32 passFraction) noexcept;
    [[nodiscard("Store this!")]] const char* RoleName(Role role) noexcept;

    // A texture's alpha channel, TOP-DOWN (row 0 is the top of the image file,
    // as stb decodes it without the flip Texture2D applies for upload).
    struct AlphaPlane
    {
        TArray<u8> Alpha;
        u32 Width = 0;
        u32 Height = 0;

        [[nodiscard("Store this!")]] bool IsEmpty() const noexcept
        {
            return Width == 0 || Height == 0 || Alpha.IsEmpty();
        }
        // The texel a sampler reads at `uv` under REPEAT wrap and nearest
        // filtering, in the engine's convention: v = 0 is the BOTTOM of the
        // image, because Texture2D flips on load.
        [[nodiscard("Store this!")]] u8 AlphaAt(glm::vec2 uv) const noexcept;
    };

    // Decodes `path`'s alpha exactly as Texture2D::Create would see it: a
    // 4-channel image keeps its alpha, anything else is fully opaque. Returns
    // false, with `error` filled, when the file cannot be decoded on the CPU
    // (missing, or a cooked block-compressed container).
    [[nodiscard("Check the result")]] bool DecodeAlpha(const std::filesystem::path& path, AlphaPlane& out,
                                                       std::string& error);

    // 256 bins, one per UNORM alpha byte, holding how much surface samples a
    // texel of that alpha. A cutoff moves nothing but the bin boundary, which
    // is why the inspector can re-evaluate it on every slider tick.
    struct Histogram
    {
        std::array<f64, 256> Weight{};
        f64 Total = 0.0;

        void Add(u8 alpha, f64 weight = 1.0) noexcept
        {
            Weight[alpha] += weight;
            Total += weight;
        }
        [[nodiscard("Store this!")]] bool IsEmpty() const noexcept
        {
            return !(Total > 0.0);
        }
        // Fraction of the measured surface the shader's alpha test keeps at
        // `cutoff`. 0 for an empty histogram.
        [[nodiscard("Store this!")]] f32 PassFraction(f32 cutoff) const noexcept;
    };

    // Every texel once: the card's surface.
    [[nodiscard("Store this!")]] Histogram MeasureSheet(const AlphaPlane& plane);

    // The texture looked up at a precomputed set of surface points.
    [[nodiscard("Store this!")]] Histogram MeasureAtUVs(const AlphaPlane& plane, std::span<const glm::vec2> uvs);

    // 8192 points put the standard error of a pass fraction under 0.6
    // percentage points, far inside any band above, for 64 KiB per surface.
    inline constexpr u32 kDefaultSurfaceSamples = 8192;

    // `sampleCount` UVs spread over the triangles `indices` names, each
    // triangle receiving samples in proportion to its WORLD area. Stratified
    // and seeded, so the same mesh always yields the same points on every
    // platform. Degenerate or out-of-range triangles are skipped; an empty
    // result means the surface has no area to measure.
    [[nodiscard("Store this!")]] TArray<glm::vec2> SampleSurfaceUVs(std::span<const Vertex> vertices,
                                                                    std::span<const u32> indices,
                                                                    u32 sampleCount = kDefaultSurfaceSamples);

    // One measured (texture, surface) pair of a layer.
    struct Entry
    {
        Role Kind = Role::Card;
        FString Texture; // the file measured, as the log and the inspector name it
        FString Surface; // what samples it: "card", "part 2 of 'pine.obj'", ...
        Histogram Coverage;
        bool Measured = false; // false: the texture could not be decoded
        bool Warned = false;   // the implausible verdict was already logged
    };

    // The warning for an implausible entry: names the layer, the texture, what
    // samples it, the cutoff, the measured fraction and the role's band, and
    // says what the likely mistake is. Empty when the entry is plausible or
    // was not measured.
    [[nodiscard("Store this!")]] std::string Describe(const Entry& entry, std::string_view layerName, f32 cutoff);
} // namespace OloEngine::FoliageAlphaCoverage

namespace OloEngine
{
    // A TArray of bytes and two extents.
    template<>
    struct TIsTriviallyRelocatable<FoliageAlphaCoverage::AlphaPlane>
    {
        static constexpr bool Value = TIsTriviallyRelocatable<decltype(FoliageAlphaCoverage::AlphaPlane::Alpha)>::Value &&
                                      TIsTriviallyRelocatable<decltype(FoliageAlphaCoverage::AlphaPlane::Width)>::Value &&
                                      TIsTriviallyRelocatable<decltype(FoliageAlphaCoverage::AlphaPlane::Height)>::Value;
    };

    // FStrings (engine-relocatable), a trivially copyable histogram, and flags.
    template<>
    struct TIsTriviallyRelocatable<FoliageAlphaCoverage::Entry>
    {
        static constexpr bool Value = TIsTriviallyRelocatable<decltype(FoliageAlphaCoverage::Entry::Kind)>::Value &&
                                      TIsTriviallyRelocatable<decltype(FoliageAlphaCoverage::Entry::Texture)>::Value &&
                                      TIsTriviallyRelocatable<decltype(FoliageAlphaCoverage::Entry::Surface)>::Value &&
                                      TIsTriviallyRelocatable<decltype(FoliageAlphaCoverage::Entry::Coverage)>::Value &&
                                      TIsTriviallyRelocatable<decltype(FoliageAlphaCoverage::Entry::Measured)>::Value &&
                                      TIsTriviallyRelocatable<decltype(FoliageAlphaCoverage::Entry::Warned)>::Value;
    };
} // namespace OloEngine
