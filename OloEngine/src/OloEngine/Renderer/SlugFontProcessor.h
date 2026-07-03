#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/SlugData.h"

#include <stb_image/stb_truetype.h>

#include <utility>
#include <vector>

namespace OloEngine
{
    // Processes font glyph outlines into Slug curve/band GPU textures.
    // Given a stbtt_fontinfo and a set of glyph data, this processor:
    //   1. Extracts quadratic Bézier curves from each glyph contour
    //   2. Organizes curves into horizontal and vertical bands
    //   3. Packs curve control points into an RGBA16F texture
    //   4. Packs band headers + curve lists into an RG16UI texture
    //   5. Fills per-glyph SlugGlyphRenderData for vertex attribute packing
    class SlugFontProcessor
    {
      public:
        // Process all glyphs in fontData, generating curve/band textures.
        // fontInfo must remain valid for the duration of this call.
        // emScale converts from font units to em-space (1.0 / unitsPerEm).
        static void Process(const stbtt_fontinfo& fontInfo, f32 emScale, SlugFontData& fontData);

        // Upload the curve/band textures deferred by a headless Process() call
        // (SlugFontData::GpuUploadPending). No-op when nothing is pending or when
        // a GL context still isn't bound; frees the retained CPU texel data once
        // uploaded. Called by Font::GetCurveTexture()/GetBandTexture() so a font
        // first parsed before the renderer came up becomes renderable on the
        // first draw that needs it (issue #520).
        static void EnsureGpuTextures(SlugFontData& fontData);

      private:
        static constexpr u32 kBandTextureWidth = 4096;
        static constexpr u32 kLogBandTextureWidth = 12;
        static constexpr f32 kBandEpsilon = 1.0f / 1024.0f;

        struct GlyphCurveData
        {
            std::vector<SlugCurve> Curves;
            // Contour boundaries for endpoint sharing in the curve texture.
            // ContourStarts[i] = index of first curve in contour i.
            std::vector<u32> ContourStarts;
            u32 ContourCount = 0;
        };

        // Extract all quadratic Bézier curves from a glyph's outline.
        static GlyphCurveData ExtractCurves(const stbtt_fontinfo& fontInfo, int glyphIndex, f32 emScale);

        // Pack curve control points into the curve texture buffer.
        // Returns the starting texel index for this glyph's curves.
        struct CurvePackResult
        {
            // Maps local curve index → (texelX, texelY) in curve texture.
            std::vector<std::pair<u16, u16>> CurveLocations;
            bool Valid = true;
        };
        static CurvePackResult PackCurves(const GlyphCurveData& curves, std::vector<f32>& curveTexelData, u32& curveTexelCount);

        // Build band data for one glyph and append to the band texture buffer.
        static SlugGlyphRenderData BuildBands(const GlyphCurveData& curves,
                                              const CurvePackResult& curveLocations,
                                              const SlugGlyphData& glyphMetrics,
                                              std::vector<u16>& bandTexelData,
                                              u32& bandTexelCount);
    };
} // namespace OloEngine
