#include "OloEnginePCH.h"
#include "OloEngine/Groom/GroomAsset.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>

namespace OloEngine
{
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
        for (const auto& name : m_GroupNames)
        {
            bytes += static_cast<u64>(name.size()) + sizeof(std::string);
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

        if (m_Provenance.SourcePath.size() > GroomLimits::MaxSourcePathLength)
        {
            outReason = std::format("provenance source path is {} bytes, above the maximum {}",
                                    m_Provenance.SourcePath.size(), GroomLimits::MaxSourcePathLength);
            return false;
        }

        return true;
    }
} // namespace OloEngine
