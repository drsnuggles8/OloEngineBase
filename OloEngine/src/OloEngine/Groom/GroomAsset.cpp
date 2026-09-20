#include "OloEnginePCH.h"
#include "OloEngine/Groom/GroomAsset.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <format>
#include <limits>

namespace OloEngine
{
    // =========================================================================
    // GroomCurveView / GroomLodLevel (issue #1252)
    // =========================================================================

    bool GroomCurveView::IsConsistent() const noexcept
    {
        if (CurveOffsets.empty())
        {
            // An EMPTY view is consistent and means "no curves". The build
            // emits nothing for it, which is the right answer for a groom whose
            // LOD level selected nothing at all.
            return Points.empty() && PointWidths.empty() && RootUVs.empty() && CurveGroupIds.empty() &&
                   CurveFlags.empty();
        }
        const auto curveCount = static_cast<sizet>(CurveOffsets.size() - 1);
        if (RootUVs.size() != curveCount || CurveGroupIds.size() != curveCount || CurveFlags.size() != curveCount)
        {
            return false;
        }
        if (PointWidths.size() != Points.size())
        {
            return false;
        }
        if (CurveOffsets.front() != 0u || CurveOffsets.back() != Points.size())
        {
            return false;
        }
        for (sizet i = 0; i + 1 < CurveOffsets.size(); ++i)
        {
            if (CurveOffsets[i + 1] < CurveOffsets[i])
            {
                return false;
            }
        }
        return true;
    }

    u64 GroomLodLevel::GetCpuMemoryBytes() const noexcept
    {
        u64 bytes = 0;
        bytes += static_cast<u64>(CurveOffsets.size()) * sizeof(u32);
        bytes += static_cast<u64>(Points.size()) * sizeof(glm::vec3);
        bytes += static_cast<u64>(PointWidths.size()) * sizeof(f32);
        bytes += static_cast<u64>(RootUVs.size()) * sizeof(glm::vec2);
        bytes += static_cast<u64>(CurveGroupIds.size()) * sizeof(u16);
        bytes += static_cast<u64>(CurveFlags.size()) * sizeof(u8);
        bytes += static_cast<u64>(SourceCurves.size()) * sizeof(u32);
        return bytes;
    }

    bool GroomLodLevel::operator==(const GroomLodLevel& other) const
    {
        // The float arrays go through memcmp rather than std::vector's own
        // operator==, which would compare f32 with `==` (cpp-coding-quality
        // §2a). Bit equality is what a cache-invalidation or round-trip check
        // actually wants here anyway: two grooms that differ by one ulp are two
        // different cooked artifacts.
        const auto bytesEqual = [](const auto& a, const auto& b)
        {
            if (a.size() != b.size())
            {
                return false;
            }
            return a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(a[0])) == 0;
        };
        return Representation == other.Representation &&
               Math::BitwiseEqual(SourcePixelSize, other.SourcePixelSize) && CurveOffsets == other.CurveOffsets &&
               CurveGroupIds == other.CurveGroupIds && CurveFlags == other.CurveFlags &&
               SourceCurves == other.SourceCurves && bytesEqual(Points, other.Points) &&
               bytesEqual(PointWidths, other.PointWidths) && bytesEqual(RootUVs, other.RootUVs);
    }

    bool GroomLodLevel::Validate(u32 baseCurveCount, std::string& outReason) const
    {
        const u32 curveCount = GetCurveCount();
        if (curveCount == 0)
        {
            outReason = "LOD level has zero curves; a level that stands in for nothing must not be cooked";
            return false;
        }
        if (curveCount > GroomLimits::MaxCurveCount)
        {
            outReason = std::format("LOD level curve count {} exceeds the format cap {}", curveCount,
                                    GroomLimits::MaxCurveCount);
            return false;
        }
        if (!IsValidGroomRepresentation(static_cast<i32>(Representation)))
        {
            outReason =
                std::format("LOD level representation {} is not a GroomRepresentation", static_cast<i32>(Representation));
            return false;
        }
        if (Representation == GroomRepresentation::Strand)
        {
            // The strand tier IS the base groom. A level claiming it would make
            // FindLodLevel answer with a coarser curve set for the close-up
            // tier, which is the one thing the scope boundary forbids.
            outReason = "a LOD level may not claim the Strand representation; that tier is the base groom";
            return false;
        }
        const GroomCurveView view = GetCurveView();
        if (!view.IsConsistent())
        {
            outReason = "LOD level arrays disagree with its curve offset table";
            return false;
        }
        if (SourceCurves.size() != curveCount)
        {
            outReason = std::format("LOD level source map has {} entries but the level has {} curves",
                                    SourceCurves.size(), curveCount);
            return false;
        }
        for (u32 curve = 0; curve < curveCount; ++curve)
        {
            if (SourceCurves[curve] >= baseCurveCount)
            {
                // Out of bounds here is an out-of-bounds READ of the binding's
                // root-transform array in the innermost loop of the renderer,
                // so it is rejected at the boundary rather than clamped.
                outReason = std::format("LOD level curve {} maps to base curve {} but the base groom has {} curves",
                                        curve, SourceCurves[curve], baseCurveCount);
                return false;
            }
            const u32 points = view.GetCurvePointCount(curve);
            if (points < GroomLimits::MinPointsPerCurve || points > GroomLimits::MaxPointsPerCurve)
            {
                outReason = std::format("LOD level curve {} has {} control points, outside [{}, {}]", curve, points,
                                        GroomLimits::MinPointsPerCurve, GroomLimits::MaxPointsPerCurve);
                return false;
            }
        }
        for (sizet p = 0; p < Points.size(); ++p)
        {
            const glm::vec3& point = Points[p];
            if (!std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(point.z))
            {
                outReason = std::format("LOD level point {} is not finite", p);
                return false;
            }
            if (std::abs(point.x) > GroomLimits::MaxCoordinate || std::abs(point.y) > GroomLimits::MaxCoordinate ||
                std::abs(point.z) > GroomLimits::MaxCoordinate)
            {
                outReason =
                    std::format("LOD level point {} lies outside the coordinate bound {}", p, GroomLimits::MaxCoordinate);
                return false;
            }
            const f32 width = PointWidths[p];
            if (!std::isfinite(width) || width < 0.0f || width > GroomLimits::MaxWidth)
            {
                outReason = std::format("LOD level width {} is {}, outside [0, {}]", p, width, GroomLimits::MaxWidth);
                return false;
            }
        }
        for (sizet c = 0; c < RootUVs.size(); ++c)
        {
            if (!std::isfinite(RootUVs[c].x) || !std::isfinite(RootUVs[c].y))
            {
                outReason = std::format("LOD level root UV of curve {} is not finite", c);
                return false;
            }
        }
        if (!std::isfinite(SourcePixelSize) || SourcePixelSize < 0.0f)
        {
            outReason = "LOD level source pixel size is not a finite, non-negative number";
            return false;
        }
        return true;
    }

    const GroomLodLevel* GroomAsset::FindLodLevel(GroomRepresentation representation) const noexcept
    {
        for (const GroomLodLevel& level : m_LodLevels)
        {
            if (level.Representation == representation)
            {
                return &level;
            }
        }
        return nullptr;
    }

    u64 GroomAsset::GetCpuMemoryBytes() const noexcept
    {
        u64 bytes = 0;
        bytes += static_cast<u64>(m_CurveOffsets.size()) * sizeof(u32);
        bytes += static_cast<u64>(m_Points.size()) * sizeof(glm::vec3);
        bytes += static_cast<u64>(m_PointWidths.size()) * sizeof(f32);
        bytes += static_cast<u64>(m_RootUVs.size()) * sizeof(glm::vec2);
        bytes += static_cast<u64>(m_CurveGroupIds.size()) * sizeof(u16);
        bytes += static_cast<u64>(m_CurveFlags.size()) * sizeof(u8);
        bytes += static_cast<u64>(m_GroupRanges.size()) * sizeof(GroomGroupRange);
        bytes += static_cast<u64>(m_GroupCoats.size()) * sizeof(GroomCoatGroupDesc);
        for (const auto& name : m_GroupNames)
        {
            bytes += static_cast<u64>(name.size()) + sizeof(std::string);
        }
        // The cooked LOD levels are resident whenever the base groom is, so a
        // readout that left them out would under-report the asset by however
        // much the card tier costs — which is the number criterion 4 asks for
        // by representation.
        for (const GroomLodLevel& level : m_LodLevels)
        {
            bytes += level.GetCpuMemoryBytes();
        }
        return bytes;
    }

    void GroomAsset::RecomputeDerivedData()
    {
        // ── Bounds ──
        // An empty groom gets a ZERO box rather than the inverted-infinity
        // sentinel a naive min/max seed leaves behind: the inverted box is the
        // value that silently propagates into a culling test and makes an empty
        // groom "visible from everywhere".
        if (m_Points.empty())
        {
            m_BoundsMin = glm::vec3(0.0f);
            m_BoundsMax = glm::vec3(0.0f);
        }
        else
        {
            m_BoundsMin = glm::vec3(std::numeric_limits<f32>::max());
            m_BoundsMax = glm::vec3(std::numeric_limits<f32>::lowest());
            for (const glm::vec3& p : m_Points)
            {
                m_BoundsMin = glm::min(m_BoundsMin, p);
                m_BoundsMax = glm::max(m_BoundsMax, p);
            }
            // Widen by the largest RADIUS so the box contains the rendered
            // strand, not just its centreline. Widths are diameters.
            f32 maxRadius = 0.0f;
            for (f32 const width : m_PointWidths)
            {
                maxRadius = std::max(maxRadius, width * 0.5f);
            }
            m_BoundsMin -= glm::vec3(maxRadius);
            m_BoundsMax += glm::vec3(maxRadius);
        }

        // ── Guide count ──
        m_GuideCount = 0;
        for (u8 const flags : m_CurveFlags)
        {
            if (HasGroomFlag(flags, GroomCurveFlag::Guide))
            {
                ++m_GuideCount;
            }
        }

        // ── Group ranges ──
        // Only meaningful once curves are sorted by group (GroomCooker does
        // that). A groom whose group ids are NOT contiguous would produce
        // overlapping ranges, which Validate() rejects — so this stays a pure
        // derivation and never silently "fixes" an unsorted groom.
        m_GroupRanges.assign(m_GroupNames.size(), GroomGroupRange{});
        const u32 curveCount = GetCurveCount();
        for (u32 curve = 0; curve < curveCount; ++curve)
        {
            const u16 group = m_CurveGroupIds[curve];
            if (group >= m_GroupRanges.size())
            {
                continue; // Validate() reports this; do not index out of range here.
            }
            auto& range = m_GroupRanges[group];
            if (range.CurveCount == 0)
            {
                range.FirstCurve = curve;
            }
            ++range.CurveCount;
        }
    }

    bool GroomAsset::Validate(std::string& outReason) const
    {
        const u32 curveCount = GetCurveCount();

        if (m_CurveOffsets.empty())
        {
            outReason = "curve offset table is empty (a groom must have at least one curve)";
            return false;
        }
        if (curveCount == 0)
        {
            outReason = "groom has zero curves";
            return false;
        }
        if (curveCount > GroomLimits::MaxCurveCount)
        {
            outReason = std::format("curve count {} exceeds the format cap {}", curveCount, GroomLimits::MaxCurveCount);
            return false;
        }
        if (m_Points.size() > GroomLimits::MaxPointCount)
        {
            outReason = std::format("point count {} exceeds the format cap {}", m_Points.size(), GroomLimits::MaxPointCount);
            return false;
        }

        // ── Parallel-array agreement ──
        if (m_PointWidths.size() != m_Points.size())
        {
            outReason = std::format("width array has {} entries but there are {} points", m_PointWidths.size(), m_Points.size());
            return false;
        }
        if (m_RootUVs.size() != curveCount)
        {
            outReason = std::format("root-UV array has {} entries but there are {} curves", m_RootUVs.size(), curveCount);
            return false;
        }
        if (m_CurveGroupIds.size() != curveCount)
        {
            outReason = std::format("group-id array has {} entries but there are {} curves", m_CurveGroupIds.size(), curveCount);
            return false;
        }
        if (m_CurveFlags.size() != curveCount)
        {
            outReason = std::format("flag array has {} entries but there are {} curves", m_CurveFlags.size(), curveCount);
            return false;
        }

        // ── Offset table ──
        if (m_CurveOffsets.front() != 0)
        {
            outReason = std::format("curve offset table starts at {} rather than 0", m_CurveOffsets.front());
            return false;
        }
        if (m_CurveOffsets.back() != m_Points.size())
        {
            outReason = std::format("curve offset table ends at {} but there are {} points",
                                    m_CurveOffsets.back(), m_Points.size());
            return false;
        }
        for (u32 curve = 0; curve < curveCount; ++curve)
        {
            if (m_CurveOffsets[curve + 1] < m_CurveOffsets[curve])
            {
                outReason = std::format("curve offset table is not monotonic at curve {}", curve);
                return false;
            }
            const u32 points = m_CurveOffsets[curve + 1] - m_CurveOffsets[curve];
            if (points < GroomLimits::MinPointsPerCurve)
            {
                outReason = std::format("curve {} has {} control points, below the minimum {}",
                                        curve, points, GroomLimits::MinPointsPerCurve);
                return false;
            }
            if (points > GroomLimits::MaxPointsPerCurve)
            {
                outReason = std::format("curve {} has {} control points, above the maximum {}",
                                        curve, points, GroomLimits::MaxPointsPerCurve);
                return false;
            }
        }

        // ── Groups ──
        if (m_GroupNames.empty())
        {
            outReason = "groom has no groups (every curve belongs to exactly one)";
            return false;
        }
        if (m_GroupNames.size() > GroomLimits::MaxGroupCount)
        {
            outReason = std::format("group count {} exceeds the format cap {}", m_GroupNames.size(), GroomLimits::MaxGroupCount);
            return false;
        }
        for (sizet g = 0; g < m_GroupNames.size(); ++g)
        {
            if (m_GroupNames[g].empty())
            {
                outReason = std::format("group {} has an empty name", g);
                return false;
            }
            if (m_GroupNames[g].size() > GroomLimits::MaxGroupNameLength)
            {
                outReason = std::format("group {} name is {} bytes, above the maximum {}",
                                        g, m_GroupNames[g].size(), GroomLimits::MaxGroupNameLength);
                return false;
            }
        }
        for (u32 curve = 0; curve < curveCount; ++curve)
        {
            if (m_CurveGroupIds[curve] >= m_GroupNames.size())
            {
                outReason = std::format("curve {} names group {} but only {} groups exist",
                                        curve, m_CurveGroupIds[curve], m_GroupNames.size());
                return false;
            }
        }

        // ── Coat table (issue #1251) ──
        // Exactly two shapes are legal: EMPTY (a groom with no coat authoring,
        // which every consumer reads as identity) or one entry per group. A
        // SHORT table is the dangerous middle: GetGroupCoat would answer
        // identity for the tail groups, so the guard hairs of a long-coated
        // animal would silently lose their role and their density and the
        // picture would be a slightly-too-uniform coat with no error anywhere.
        if (!m_GroupCoats.empty() && m_GroupCoats.size() != m_GroupNames.size())
        {
            outReason = std::format("coat table has {} entries but the groom has {} groups; it must be empty or "
                                    "one entry per group",
                                    m_GroupCoats.size(), m_GroupNames.size());
            return false;
        }
        for (sizet g = 0; g < m_GroupCoats.size(); ++g)
        {
            const GroomCoatGroupDesc& coat = m_GroupCoats[g];
            if (!std::isfinite(coat.Density) || !std::isfinite(coat.Length) || !std::isfinite(coat.Width) ||
                !std::isfinite(coat.Clump) || !std::isfinite(coat.Tint.r) || !std::isfinite(coat.Tint.g) ||
                !std::isfinite(coat.Tint.b))
            {
                outReason = std::format("group {} ('{}') has a non-finite coat parameter", g, m_GroupNames[g]);
                return false;
            }
            if (!IsValidGroomCoatRole(static_cast<i32>(coat.Role)))
            {
                outReason = std::format("group {} ('{}') has coat role {}, which is not a GroomCoatRole",
                                        g, m_GroupNames[g], coat.Role);
                return false;
            }
        }

        // ── Finite-value validation (CLAUDE.md: validate every float) ──
        for (sizet p = 0; p < m_Points.size(); ++p)
        {
            const glm::vec3& point = m_Points[p];
            if (!std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(point.z))
            {
                outReason = std::format("point {} is not finite", p);
                return false;
            }
            if (std::abs(point.x) > GroomLimits::MaxCoordinate || std::abs(point.y) > GroomLimits::MaxCoordinate ||
                std::abs(point.z) > GroomLimits::MaxCoordinate)
            {
                outReason = std::format("point {} lies outside the coordinate bound {} (a unit-scale mismatch?)",
                                        p, GroomLimits::MaxCoordinate);
                return false;
            }
            const f32 width = m_PointWidths[p];
            if (!std::isfinite(width))
            {
                outReason = std::format("width {} is not finite", p);
                return false;
            }
            if (width < 0.0f || width > GroomLimits::MaxWidth)
            {
                outReason = std::format("width {} is {}, outside [0, {}]", p, width, GroomLimits::MaxWidth);
                return false;
            }
        }
        for (sizet c = 0; c < m_RootUVs.size(); ++c)
        {
            const glm::vec2& uv = m_RootUVs[c];
            if (!std::isfinite(uv.x) || !std::isfinite(uv.y))
            {
                outReason = std::format("root UV of curve {} is not finite", c);
                return false;
            }
            // Deliberately NOT clamped to [0,1]: a UDIM / tiled groom legally
            // parameterises outside the unit square. The finite check is the
            // real invariant.
        }

        if (!IsValidGroomCurveBasis(static_cast<i32>(m_Basis)))
        {
            outReason = std::format("curve basis {} is not one this build knows", static_cast<i32>(m_Basis));
            return false;
        }

        // ── LOD levels (issue #1252) ──
        // Validated against THIS groom's curve count, because the source map is
        // an index into it. A DUPLICATE representation is rejected rather than
        // resolved first-wins: FindLodLevel would otherwise answer with
        // whichever level happened to be written first, and which one that is
        // would depend on the cook's iteration order — a determinism hole in a
        // format whose whole contract is determinism.
        {
            std::array<bool, GroomRepresentationCount> seen{};
            for (sizet i = 0; i < m_LodLevels.size(); ++i)
            {
                std::string levelReason;
                if (!m_LodLevels[i].Validate(curveCount, levelReason))
                {
                    outReason = std::format("LOD level {}: {}", i, levelReason);
                    return false;
                }
                const auto slot = static_cast<sizet>(m_LodLevels[i].Representation);
                if (seen[slot])
                {
                    outReason = std::format("LOD level {} is a second level for representation {}", i,
                                            ToString(m_LodLevels[i].Representation));
                    return false;
                }
                seen[slot] = true;
            }
        }

        if (m_Provenance.SourcePath.size() > GroomLimits::MaxSourcePathLength)
        {
            outReason = std::format("provenance source path is {} bytes, above the maximum {}",
                                    m_Provenance.SourcePath.size(), GroomLimits::MaxSourcePathLength);
            return false;
        }

        return true;
    }
} // namespace OloEngine
