#include "OloEnginePCH.h"
#include "SlugFontProcessor.h"

#include <glad/gl.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

namespace OloEngine
{
    // -------------------------------------------------------------------------
    // Curve extraction from stb_truetype glyph shapes
    // -------------------------------------------------------------------------

    SlugFontProcessor::GlyphCurveData SlugFontProcessor::ExtractCurves(
        const stbtt_fontinfo& fontInfo, int glyphIndex, f32 emScale)
    {
        GlyphCurveData result;

        stbtt_vertex* vertices = nullptr;
        const int vertexCount = stbtt_GetGlyphShape(&fontInfo, glyphIndex, &vertices);
        if (vertexCount <= 0 || !vertices)
        {
            return result;
        }

        glm::vec2 contourStart{};
        glm::vec2 currentPos{};
        bool inContour = false;

        auto addLineCurve = [&result](glm::vec2 from, glm::vec2 to)
        {
            // Convert line to degenerate quadratic: control point at midpoint.
            SlugCurve curve;
            curve.P1 = from;
            curve.P2 = (from + to) * 0.5f;
            curve.P3 = to;
            result.Curves.push_back(curve);
        };

        auto addQuadCurve = [&result](glm::vec2 from, glm::vec2 ctrl, glm::vec2 to)
        {
            SlugCurve curve;
            curve.P1 = from;
            curve.P2 = ctrl;
            curve.P3 = to;
            result.Curves.push_back(curve);
        };

        auto addCubicAsTwoQuadratics = [&addQuadCurve](glm::vec2 p0, glm::vec2 p1, glm::vec2 p2, glm::vec2 p3)
        {
            // Split cubic at t=0.5 using de Casteljau, then approximate each half.
            auto q01 = (p0 + p1) * 0.5f;
            auto q12 = (p1 + p2) * 0.5f;
            auto q23 = (p2 + p3) * 0.5f;
            auto q012 = (q01 + q12) * 0.5f;
            auto q123 = (q12 + q23) * 0.5f;
            auto mid = (q012 + q123) * 0.5f;

            // Left half: cubic (p0, q01, q012, mid) → quadratic with ctrl = (-p0 + 3*q01 + 3*q012 - mid) / 4
            // Simplified: the best single-quadratic fit for a short cubic has ctrl ≈ (3*c1 + 3*c2 - start - end) / 4
            auto ctrlL = (-p0 + 3.0f * q01 + 3.0f * q012 - mid) * 0.25f;
            addQuadCurve(p0, ctrlL, mid);

            auto ctrlR = (-mid + 3.0f * q123 + 3.0f * q23 - p3) * 0.25f;
            addQuadCurve(mid, ctrlR, p3);
        };

        for (int i = 0; i < vertexCount; ++i)
        {
            const auto& v = vertices[i];
            auto pos = glm::vec2(static_cast<f32>(v.x) * emScale, static_cast<f32>(v.y) * emScale);

            switch (v.type)
            {
                case STBTT_vmove:
                {
                    // Close previous contour if open.
                    if (inContour && result.Curves.size() > result.ContourStarts.back())
                    {
                        // Close with a line from current position back to contour start.
                        if (glm::length(currentPos - contourStart) > 1e-6f)
                        {
                            addLineCurve(currentPos, contourStart);
                        }
                    }
                    // Start new contour.
                    result.ContourStarts.push_back(static_cast<u32>(result.Curves.size()));
                    contourStart = pos;
                    currentPos = pos;
                    inContour = true;
                    break;
                }
                case STBTT_vline:
                {
                    if (inContour)
                    {
                        addLineCurve(currentPos, pos);
                        currentPos = pos;
                    }
                    break;
                }
                case STBTT_vcurve:
                {
                    if (inContour)
                    {
                        auto ctrl = glm::vec2(static_cast<f32>(v.cx) * emScale, static_cast<f32>(v.cy) * emScale);
                        addQuadCurve(currentPos, ctrl, pos);
                        currentPos = pos;
                    }
                    break;
                }
                case STBTT_vcubic:
                {
                    if (inContour)
                    {
                        auto c1 = glm::vec2(static_cast<f32>(v.cx) * emScale, static_cast<f32>(v.cy) * emScale);
                        auto c2 = glm::vec2(static_cast<f32>(v.cx1) * emScale, static_cast<f32>(v.cy1) * emScale);
                        addCubicAsTwoQuadratics(currentPos, c1, c2, pos);
                        currentPos = pos;
                    }
                    break;
                }
                default:
                    break;
            }
        }

        // Close the last contour.
        if (inContour && !result.ContourStarts.empty() && result.Curves.size() > result.ContourStarts.back())
        {
            if (glm::length(currentPos - contourStart) > 1e-6f)
            {
                addLineCurve(currentPos, contourStart);
            }
        }

        result.ContourCount = static_cast<u32>(result.ContourStarts.size());

        stbtt_FreeShape(&fontInfo, vertices);
        return result;
    }

    // -------------------------------------------------------------------------
    // Curve texture packing
    // -------------------------------------------------------------------------

    SlugFontProcessor::CurvePackResult SlugFontProcessor::PackCurves(
        const GlyphCurveData& curves, std::vector<f32>& curveTexelData, u32& curveTexelCount)
    {
        CurvePackResult result;
        auto curveCount = curves.Curves.size();
        result.CurveLocations.resize(curveCount);

        if (curveCount == 0)
        {
            return result;
        }

        // Pack curves contour-by-contour with endpoint sharing.
        // Within a contour, consecutive curves share the endpoint texel:
        //   Curve i: texels [loc, loc+1]
        //   Curve i+1: texels [loc+1, loc+2] (because P3 of curve i = P1 of curve i+1)
        //
        // Across contours, no sharing — each contour starts fresh.

        for (u32 contourIdx = 0; contourIdx < curves.ContourCount; ++contourIdx)
        {
            auto contourStart = curves.ContourStarts[contourIdx];
            auto contourEnd = (contourIdx + 1 < curves.ContourCount)
                                  ? curves.ContourStarts[contourIdx + 1]
                                  : static_cast<u32>(curveCount);
            auto contourCurveCount = contourEnd - contourStart;
            if (contourCurveCount == 0)
            {
                continue;
            }

            // First curve in contour gets a full 2-texel allocation.
            // Each subsequent curve in the contour reuses the previous endpoint texel.
            // Total texels for this contour: contourCurveCount + 1

            // Ensure the contour's texel run doesn't cross a texture row boundary.
            // The shader reads the partner texel at (curveLoc.x + 1, curveLoc.y)
            // without row-wrap logic, so the entire run must stay in one row.
            auto contourTexels = contourCurveCount + 1;
            if (contourTexels > kBandTextureWidth)
            {
                OLO_CORE_ERROR("SlugFontProcessor::PackCurves: contour {} has {} texels, exceeds row width {}", contourIdx, contourTexels, kBandTextureWidth);
                continue;
            }
            if (auto currentCol = curveTexelCount % kBandTextureWidth; currentCol + contourTexels > kBandTextureWidth)
            {
                auto padding = kBandTextureWidth - currentCol;
                curveTexelData.resize(curveTexelData.size() + static_cast<sizet>(padding) * 4, 0.0f);
                curveTexelCount += padding;
            }

            for (u32 localIdx = 0; localIdx < contourCurveCount; ++localIdx)
            {
                auto curveIdx = contourStart + localIdx;
                const auto& curve = curves.Curves[curveIdx];

                if (localIdx == 0)
                {
                    // First texel: (P1.x, P1.y, P2.x, P2.y)
                    auto texelY = curveTexelCount / kBandTextureWidth;
                    if (texelY > std::numeric_limits<u16>::max())
                    {
                        OLO_CORE_ERROR("SlugFontProcessor::PackCurves: curve texelY {} exceeds u16 max", texelY);
                        result.Valid = false;
                        return result;
                    }
                    auto texelX = static_cast<u16>(curveTexelCount % kBandTextureWidth);
                    result.CurveLocations[curveIdx] = { texelX, static_cast<u16>(texelY) };

                    curveTexelData.push_back(curve.P1.x);
                    curveTexelData.push_back(curve.P1.y);
                    curveTexelData.push_back(curve.P2.x);
                    curveTexelData.push_back(curve.P2.y);
                    ++curveTexelCount;
                }
                else
                {
                    // Shared texel: the previous curve's P3 texel is this curve's first texel.
                    // That shared texel is at index (curveTexelCount - 1), written by the
                    // previous iteration's second-texel step.
                    auto sharedTexelIdx = curveTexelCount - 1;
                    auto sharedTexelY = sharedTexelIdx / kBandTextureWidth;
                    if (sharedTexelY > std::numeric_limits<u16>::max())
                    {
                        OLO_CORE_ERROR("SlugFontProcessor::PackCurves: shared texelY {} exceeds u16 max", sharedTexelY);
                        result.Valid = false;
                        return result;
                    }
                    auto texelX = static_cast<u16>(sharedTexelIdx % kBandTextureWidth);
                    result.CurveLocations[curveIdx] = { texelX, static_cast<u16>(sharedTexelY) };

                    // The shared texel format is (P1.x, P1.y, P2.x, P2.y) where
                    // P1 = P3 of previous curve. Overwrite zw with this curve's P2.
                    auto sharedTexelDataIdx = static_cast<sizet>(sharedTexelIdx) * 4;
                    curveTexelData[sharedTexelDataIdx + 2] = curve.P2.x;
                    curveTexelData[sharedTexelDataIdx + 3] = curve.P2.y;
                }

                // Second texel for this curve: (P3.x, P3.y, 0, 0)
                // If there's a next curve in this contour, the zw will be overwritten with its P2.
                curveTexelData.push_back(curve.P3.x);
                curveTexelData.push_back(curve.P3.y);
                curveTexelData.push_back(0.0f); // placeholder for next curve's P2.x
                curveTexelData.push_back(0.0f); // placeholder for next curve's P2.y
                ++curveTexelCount;
            }
        }

        return result;
    }

    // -------------------------------------------------------------------------
    // Band computation and packing
    // -------------------------------------------------------------------------

    SlugGlyphRenderData SlugFontProcessor::BuildBands(
        const GlyphCurveData& curves,
        const CurvePackResult& curveLocations,
        const SlugGlyphData& glyphMetrics,
        std::vector<u16>& bandTexelData,
        u32& bandTexelCount)
    {
        SlugGlyphRenderData renderData;
        auto curveCount = curves.Curves.size();
        if (curveCount == 0)
        {
            return renderData;
        }

        // Validate glyph metrics — reject NaN/Inf/degenerate bounds.
        f32 boundsLeft = glyphMetrics.PlaneBoundsLeft;
        f32 boundsBottom = glyphMetrics.PlaneBoundsBottom;
        f32 boundsRight = glyphMetrics.PlaneBoundsRight;
        f32 boundsTop = glyphMetrics.PlaneBoundsTop;

        if (!std::isfinite(boundsLeft) || !std::isfinite(boundsBottom) || !std::isfinite(boundsRight) || !std::isfinite(boundsTop))
        {
            return renderData;
        }

        if (boundsRight <= boundsLeft || boundsTop <= boundsBottom)
        {
            return renderData;
        }

        f32 boundsWidth = boundsRight - boundsLeft;
        f32 boundsHeight = boundsTop - boundsBottom;

        // Choose band counts: sqrt of curve count, clamped to [1, 16].
        auto sqrtCurves = static_cast<u32>(std::ceil(std::sqrt(static_cast<f64>(curveCount))));
        u32 hbandCount = std::clamp(sqrtCurves, 1u, 16u);
        u32 vbandCount = std::clamp(sqrtCurves, 1u, 16u);

        // Precompute per-curve bounding boxes for band assignment.
        struct CurveBounds
        {
            f32 MinX, MaxX, MinY, MaxY;
            u32 Index;
            bool IsHorizontalLine;
            bool IsVerticalLine;
        };
        std::vector<CurveBounds> curveBounds(curveCount);
        for (sizet i = 0; i < curveCount; ++i)
        {
            const auto& c = curves.Curves[i];
            auto minX = std::min({ c.P1.x, c.P2.x, c.P3.x });
            auto maxX = std::max({ c.P1.x, c.P2.x, c.P3.x });
            auto minY = std::min({ c.P1.y, c.P2.y, c.P3.y });
            auto maxY = std::max({ c.P1.y, c.P2.y, c.P3.y });

            curveBounds[i].MinX = minX;
            curveBounds[i].MaxX = maxX;
            curveBounds[i].MinY = minY;
            curveBounds[i].MaxY = maxY;
            curveBounds[i].Index = static_cast<u32>(i);

            // Straight horizontal lines don't contribute to horizontal ray winding.
            auto yRange = maxY - minY;
            curveBounds[i].IsHorizontalLine = (yRange < 1e-6f);
            // Straight vertical lines don't contribute to vertical ray winding.
            auto xRange = maxX - minX;
            curveBounds[i].IsVerticalLine = (xRange < 1e-6f);
        }

        // Build curve lists for each band.
        f32 hbandHeight = boundsHeight / static_cast<f32>(hbandCount);
        f32 vbandWidth = boundsWidth / static_cast<f32>(vbandCount);

        // Horizontal bands (indexed by y position).
        std::vector<std::vector<u32>> hbandCurves(hbandCount);
        for (u32 band = 0; band < hbandCount; ++band)
        {
            auto bandBottom = boundsBottom + static_cast<f32>(band) * hbandHeight - kBandEpsilon;
            auto bandTop = boundsBottom + static_cast<f32>(band + 1) * hbandHeight + kBandEpsilon;

            for (sizet ci = 0; ci < curveCount; ++ci)
            {
                const auto& cb = curveBounds[ci];
                // Skip horizontal lines — they can't contribute to horizontal ray winding.
                if (cb.IsHorizontalLine)
                {
                    continue;
                }
                // Check if curve's y-range overlaps the band's y-range.
                if (cb.MaxY >= bandBottom && cb.MinY <= bandTop)
                {
                    hbandCurves[band].push_back(static_cast<u32>(ci));
                }
            }

            // Sort curves in descending order by max x coordinate.
            std::ranges::sort(hbandCurves[band],
                              [&curveBounds](u32 a, u32 b)
                              { return curveBounds[a].MaxX > curveBounds[b].MaxX; });
        }

        // Vertical bands (indexed by x position).
        std::vector<std::vector<u32>> vbandCurves(vbandCount);
        for (u32 band = 0; band < vbandCount; ++band)
        {
            auto bandLeft = boundsLeft + static_cast<f32>(band) * vbandWidth - kBandEpsilon;
            auto bandRight = boundsLeft + static_cast<f32>(band + 1) * vbandWidth + kBandEpsilon;

            for (sizet ci = 0; ci < curveCount; ++ci)
            {
                const auto& cb = curveBounds[ci];
                // Skip vertical lines — they can't contribute to vertical ray winding.
                if (cb.IsVerticalLine)
                {
                    continue;
                }
                if (cb.MaxX >= bandLeft && cb.MinX <= bandRight)
                {
                    vbandCurves[band].push_back(static_cast<u32>(ci));
                }
            }

            // Sort curves in descending order by max y coordinate.
            std::ranges::sort(vbandCurves[band],
                              [&curveBounds](u32 a, u32 b)
                              { return curveBounds[a].MaxY > curveBounds[b].MaxY; });
        }

        // --- Pack into band texture ---
        // Layout from glyphLoc:
        //   [0 .. hbandCount-1]                     : hband headers
        //   [hbandCount .. hbandCount+vbandCount-1]  : vband headers
        //   [hbandCount+vbandCount .. end]           : curve lists

        // Reserve space for headers (will fill in later).
        // Ensure the header block doesn't cross a texture row boundary.
        // The shader reads headers at (glyphLoc.x + bandIdx, glyphLoc.y)
        // without row-wrap logic, so all headers must stay in one row.
        auto totalHeaders = hbandCount + vbandCount;
        if (auto currentCol = bandTexelCount % kBandTextureWidth; currentCol + totalHeaders > kBandTextureWidth)
        {
            auto padding = kBandTextureWidth - currentCol;
            bandTexelData.resize(bandTexelData.size() + static_cast<sizet>(padding) * 2, 0);
            bandTexelCount += padding;
        }

        // Record glyph location in band texture (after possible alignment).
        renderData.BandTextureX = bandTexelCount % kBandTextureWidth;
        renderData.BandTextureY = bandTexelCount / kBandTextureWidth;
        renderData.HBandCount = hbandCount;
        renderData.VBandCount = vbandCount;

        // Compute band transform.
        renderData.BandScaleX = static_cast<f32>(vbandCount) / boundsWidth;
        renderData.BandScaleY = static_cast<f32>(hbandCount) / boundsHeight;
        renderData.BandOffsetX = -boundsLeft * renderData.BandScaleX;
        renderData.BandOffsetY = -boundsBottom * renderData.BandScaleY;

        auto headerStart = bandTexelCount;
        bandTexelCount += totalHeaders;

        // Ensure band texel data has enough room for headers (each header = 2 u16s = 1 RG16UI texel).
        bandTexelData.resize(static_cast<sizet>(bandTexelCount) * 2, 0);

        // Horizontal band curve lists.
        for (u32 band = 0; band < hbandCount; ++band)
        {
            auto headerIdx = headerStart + band;
            auto headerDataIdx = static_cast<sizet>(headerIdx) * 2;
            auto curveListOffset = bandTexelCount - headerStart; // relative to glyphLoc

            const auto& bandCurves = hbandCurves[band];

            // Ensure enough room.
            bandTexelData.resize(static_cast<sizet>(bandTexelCount + bandCurves.size()) * 2, 0);

            for (auto curveIdx : bandCurves)
            {
                const auto& loc = curveLocations.CurveLocations[curveIdx];
                auto dataIdx = static_cast<sizet>(bandTexelCount) * 2;
                bandTexelData[dataIdx] = loc.first;      // curve texture x
                bandTexelData[dataIdx + 1] = loc.second; // curve texture y
                ++bandTexelCount;
            }

            // Write header: (curveCount, offset to curve list).
            if (bandCurves.size() > std::numeric_limits<u16>::max() || curveListOffset > std::numeric_limits<u16>::max())
            {
                OLO_CORE_ERROR("SlugFontProcessor::BuildBands: hband {} values exceed u16 max (count={}, offset={})", band, bandCurves.size(), curveListOffset);
                renderData.HBandCount = 0;
                renderData.VBandCount = 0;
                return renderData;
            }
            bandTexelData[headerDataIdx] = static_cast<u16>(bandCurves.size());
            bandTexelData[headerDataIdx + 1] = static_cast<u16>(curveListOffset);
        }

        // Vertical band curve lists.
        for (u32 band = 0; band < vbandCount; ++band)
        {
            auto headerIdx = headerStart + hbandCount + band;
            auto headerDataIdx = static_cast<sizet>(headerIdx) * 2;
            auto curveListOffset = bandTexelCount - headerStart; // relative to glyphLoc

            const auto& bandCurves = vbandCurves[band];

            bandTexelData.resize(static_cast<sizet>(bandTexelCount + bandCurves.size()) * 2, 0);

            for (auto curveIdx : bandCurves)
            {
                const auto& loc = curveLocations.CurveLocations[curveIdx];
                auto dataIdx = static_cast<sizet>(bandTexelCount) * 2;
                bandTexelData[dataIdx] = loc.first;
                bandTexelData[dataIdx + 1] = loc.second;
                ++bandTexelCount;
            }

            if (bandCurves.size() > std::numeric_limits<u16>::max() || curveListOffset > std::numeric_limits<u16>::max())
            {
                OLO_CORE_ERROR("SlugFontProcessor::BuildBands: vband {} values exceed u16 max (count={}, offset={})", band, bandCurves.size(), curveListOffset);
                renderData.HBandCount = 0;
                renderData.VBandCount = 0;
                return renderData;
            }
            bandTexelData[headerDataIdx] = static_cast<u16>(bandCurves.size());
            bandTexelData[headerDataIdx + 1] = static_cast<u16>(curveListOffset);
        }

        return renderData;
    }

    // -------------------------------------------------------------------------
    // Main processing entry point
    // -------------------------------------------------------------------------

    void SlugFontProcessor::Process(const stbtt_fontinfo& fontInfo, f32 emScale, SlugFontData& fontData)
    {
        if (!std::isfinite(emScale) || std::abs(emScale) < 1e-12f)
        {
            OLO_CORE_ERROR("SlugFontProcessor::Process: invalid emScale {}", emScale);
            return;
        }

        // Accumulate curve and band data across all glyphs.
        std::vector<f32> curveTexelData; // RGBA16F texels (4 floats each)
        std::vector<u16> bandTexelData;  // RG16UI texels (2 u16s each)
        u32 curveTexelCount = 0;
        u32 bandTexelCount = 0;

        // Reserve reasonable initial capacity.
        curveTexelData.reserve(4096 * 4);
        bandTexelData.reserve(8192 * 2);

        u32 processedGlyphs = 0;

        for (auto& [codepoint, glyphData] : fontData.Glyphs)
        {
            // Reset glyph render state before processing.
            glyphData.RenderData = SlugGlyphRenderData{};
            glyphData.HasCurves = false;

            const int glyphIndex = stbtt_FindGlyphIndex(&fontInfo, static_cast<int>(codepoint));
            if (glyphIndex == 0 && codepoint != ' ')
            {
                continue;
            }

            // Skip space and other whitespace characters — they have no outlines.
            if (codepoint == ' ' || codepoint == '\t' || codepoint == '\n' || codepoint == '\r')
            {
                continue;
            }

            auto curves = ExtractCurves(fontInfo, glyphIndex, emScale);
            if (curves.Curves.empty())
            {
                continue;
            }

            auto curveLocations = PackCurves(curves, curveTexelData, curveTexelCount);
            if (!curveLocations.Valid)
            {
                continue;
            }
            auto renderData = BuildBands(curves, curveLocations, glyphData, bandTexelData, bandTexelCount);

            // Only mark the glyph as having curves if BuildBands returned valid data.
            if (renderData.HBandCount > 0 && renderData.VBandCount > 0)
            {
                glyphData.RenderData = renderData;
                glyphData.HasCurves = true;
                ++processedGlyphs;
            }
        }

        // Create GPU textures only if a live GL context is bound. Headless
        // harnesses (Functional tests, asset preprocessors) load fonts for
        // their metrics + glyph data without ever rendering them; calling
        // glCreateTextures via Texture2D::Create without a context segfaults
        // through the null glad function pointer. Probing one DSA entry point
        // is sufficient because glad resolves them as a batch — if any one is
        // non-null, a context exists and the rest do too.
        const bool hasGLContext = (glad_glCreateTextures != nullptr);

        if (hasGLContext && curveTexelCount > 0)
        {
            auto curveWidth = std::min(curveTexelCount, kBandTextureWidth);
            auto curveHeight = (curveTexelCount + kBandTextureWidth - 1) / kBandTextureWidth;

            // Pad curve data to fill full texture dimensions (width * height * 4 floats).
            auto totalTexels = static_cast<sizet>(curveWidth) * curveHeight;
            curveTexelData.resize(totalTexels * 4, 0.0f);

            TextureSpecification curveSpec;
            curveSpec.Width = curveWidth;
            curveSpec.Height = curveHeight;
            curveSpec.Format = ImageFormat::RGBA16F;
            curveSpec.GenerateMips = false;
            fontData.CurveTexture = Texture2D::Create(curveSpec);
            fontData.CurveTexture->SetData(curveTexelData.data(),
                                           static_cast<u32>(totalTexels * 4 * sizeof(f32)));
        }

        if (hasGLContext && bandTexelCount > 0)
        {
            auto bandWidth = std::min(bandTexelCount, kBandTextureWidth);
            auto bandHeight = (bandTexelCount + kBandTextureWidth - 1) / kBandTextureWidth;

            auto totalTexels = static_cast<sizet>(bandWidth) * bandHeight;
            bandTexelData.resize(totalTexels * 2, 0);

            TextureSpecification bandSpec;
            bandSpec.Width = bandWidth;
            bandSpec.Height = bandHeight;
            bandSpec.Format = ImageFormat::RG16UI;
            bandSpec.GenerateMips = false;
            fontData.BandTexture = Texture2D::Create(bandSpec);
            fontData.BandTexture->SetData(bandTexelData.data(),
                                          static_cast<u32>(totalTexels * 2 * sizeof(u16)));
        }

        if (!hasGLContext)
        {
            OLO_CORE_TRACE("SlugFontProcessor: no GL context — skipping curve/band texture upload (metrics-only load).");
        }

        OLO_CORE_INFO("SlugFontProcessor: processed {} glyphs, {} curve texels, {} band texels",
                      processedGlyphs, curveTexelCount, bandTexelCount);
    }

} // namespace OloEngine
