#include "OloEnginePCH.h"
#include "OloEngine/Groom/GroomBuilder.h"

#include <cmath>
#include <format>
#include <utility>

namespace OloEngine
{
    bool GroomBuilder::AddGroup(const std::string& name, u16& outGroupId, std::string& outReason)
    {
        if (name.empty())
        {
            outReason = "group name is empty";
            return false;
        }
        if (name.size() > GroomLimits::MaxGroupNameLength)
        {
            outReason = std::format("group name '{}' is {} bytes, above the maximum {}",
                                    name.substr(0, 64), name.size(), GroomLimits::MaxGroupNameLength);
            return false;
        }

        if (auto it = m_GroupIdsByName.find(name); it != m_GroupIdsByName.end())
        {
            outGroupId = it->second;
            return true;
        }

        if (m_GroupNames.size() >= GroomLimits::MaxGroupCount)
        {
            outReason = std::format("cannot add group '{}': the group budget of {} is exhausted",
                                    name, GroomLimits::MaxGroupCount);
            return false;
        }

        const auto id = static_cast<u16>(m_GroupNames.size());
        m_GroupNames.push_back(name);
        GroomCoatGroupDesc coat;
        coat.Role = static_cast<u8>(InferGroomCoatRole(name));
        m_GroupCoats.push_back(coat);
        m_GroupIdsByName.emplace(name, id);
        outGroupId = id;
        return true;
    }

    bool GroomBuilder::AddCurve(const GroomCurveInput& curve, std::string& outReason)
    {
        const u32 curveIndex = GetCurveCount();
        const sizet pointCount = curve.Points.size();

        if (pointCount < GroomLimits::MinPointsPerCurve)
        {
            outReason = std::format("curve {} has {} control points, below the minimum {} "
                                    "(a strand needs a root and a tip to have a direction)",
                                    curveIndex, pointCount, GroomLimits::MinPointsPerCurve);
            return false;
        }
        if (pointCount > GroomLimits::MaxPointsPerCurve)
        {
            outReason = std::format("curve {} has {} control points, above the maximum {}",
                                    curveIndex, pointCount, GroomLimits::MaxPointsPerCurve);
            return false;
        }
        if (curve.Widths.size() != pointCount)
        {
            outReason = std::format("curve {} has {} control points but {} widths",
                                    curveIndex, pointCount, curve.Widths.size());
            return false;
        }
        if (curveIndex >= GroomLimits::MaxCurveCount)
        {
            outReason = std::format("curve budget of {} is exhausted", GroomLimits::MaxCurveCount);
            return false;
        }
        if (static_cast<u64>(m_Points.size()) + pointCount > GroomLimits::MaxPointCount)
        {
            outReason = std::format("point budget of {} is exhausted at curve {}",
                                    GroomLimits::MaxPointCount, curveIndex);
            return false;
        }
        if (curve.GroupId >= m_GroupNames.size())
        {
            outReason = std::format("curve {} names group {} but only {} groups have been registered",
                                    curveIndex, curve.GroupId, m_GroupNames.size());
            return false;
        }

        // Every float from the source file is checked here, before it can reach
        // the bounds computation or the cooked buffer.
        for (sizet i = 0; i < pointCount; ++i)
        {
            const glm::vec3& point = curve.Points[i];
            if (!std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(point.z))
            {
                outReason = std::format("curve {} control point {} is not finite", curveIndex, i);
                return false;
            }
            if (std::abs(point.x) > GroomLimits::MaxCoordinate || std::abs(point.y) > GroomLimits::MaxCoordinate ||
                std::abs(point.z) > GroomLimits::MaxCoordinate)
            {
                outReason = std::format("curve {} control point {} lies outside the coordinate bound {} "
                                        "(a unit-scale mismatch in the source?)",
                                        curveIndex, i, GroomLimits::MaxCoordinate);
                return false;
            }

            const f32 width = curve.Widths[i];
            if (!std::isfinite(width))
            {
                outReason = std::format("curve {} width {} is not finite", curveIndex, i);
                return false;
            }
            if (width < 0.0f)
            {
                outReason = std::format("curve {} width {} is negative ({}); widths are diameters",
                                        curveIndex, i, width);
                return false;
            }
            if (width > GroomLimits::MaxWidth)
            {
                outReason = std::format("curve {} width {} is {}, above the maximum {}",
                                        curveIndex, i, width, GroomLimits::MaxWidth);
                return false;
            }
        }

        if (!std::isfinite(curve.RootUV.x) || !std::isfinite(curve.RootUV.y))
        {
            outReason = std::format("curve {} root UV is not finite", curveIndex);
            return false;
        }

        // Past this point nothing can fail, so the append is atomic in effect.
        m_Points.insert(m_Points.end(), curve.Points.begin(), curve.Points.end());
        m_PointWidths.insert(m_PointWidths.end(), curve.Widths.begin(), curve.Widths.end());
        m_CurveOffsets.push_back(static_cast<u32>(m_Points.size()));
        m_RootUVs.push_back(curve.RootUV);
        m_CurveGroupIds.push_back(curve.GroupId);
        m_CurveFlags.push_back(curve.IsGuide ? std::to_underlying(GroomCurveFlag::Guide)
                                             : std::to_underlying(GroomCurveFlag::None));
        return true;
    }

    bool GroomBuilder::SetGroupCoat(u16 groupId, const GroomCoatGroupDesc& coat, std::vector<std::string>& outReasons)
    {
        if (static_cast<sizet>(groupId) >= m_GroupCoats.size())
        {
            outReasons.push_back(std::format("cannot set coat parameters: group {} does not exist ({} registered)",
                                             groupId, m_GroupCoats.size()));
            return false;
        }
        GroomCoatGroupDesc sanitised = coat;
        // Deliberately ignores the return: a repair is reported through
        // outReasons, and refusing the whole assignment because one tint channel
        // was out of range would leave the group at a description the caller
        // never chose.
        (void)SanitizeGroomCoatGroupDesc(sanitised, groupId, outReasons);
        m_GroupCoats[groupId] = sanitised;
        return true;
    }

    Ref<GroomAsset> GroomBuilder::Build(std::string& outReason)
    {
        auto groom = Ref<GroomAsset>::Create();
        groom->m_Name = std::move(m_Name);
        groom->m_CurveOffsets = std::move(m_CurveOffsets);
        groom->m_Points = std::move(m_Points);
        groom->m_PointWidths = std::move(m_PointWidths);
        groom->m_RootUVs = std::move(m_RootUVs);
        groom->m_CurveGroupIds = std::move(m_CurveGroupIds);
        groom->m_CurveFlags = std::move(m_CurveFlags);
        groom->m_GroupNames = std::move(m_GroupNames);
        groom->m_GroupCoats = std::move(m_GroupCoats);
        groom->m_Basis = m_Basis;
        groom->m_Provenance = std::move(m_Provenance);

        // A builder is single-use: reset to the empty-but-valid state so a
        // second Build() produces an empty groom that fails validation loudly,
        // rather than exposing moved-from vectors. EVERY member, not just the
        // offset table — GetCurveCount() reads m_RootUVs, so a caller that
        // queried the builder after Build() used to observe an unspecified
        // moved-from value.
        m_Name.clear();
        m_CurveOffsets.assign(1, 0u);
        m_Points.clear();
        m_PointWidths.clear();
        m_RootUVs.clear();
        m_CurveGroupIds.clear();
        m_CurveFlags.clear();
        m_GroupNames.clear();
        m_GroupCoats.clear();
        m_GroupIdsByName.clear();
        m_Provenance = {};

        groom->RecomputeDerivedData();
        if (!groom->Validate(outReason))
        {
            return nullptr;
        }
        return groom;
    }
} // namespace OloEngine
