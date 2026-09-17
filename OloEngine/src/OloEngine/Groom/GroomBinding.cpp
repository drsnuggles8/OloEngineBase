#include "OloEnginePCH.h"
#include "OloEngine/Groom/GroomBinding.h"

#include <cmath>
#include <format>

namespace OloEngine
{
    std::string_view ToString(GroomBindingRejectReason reason)
    {
        switch (reason)
        {
            case GroomBindingRejectReason::None:
                return "None";
            case GroomBindingRejectReason::NoBinding:
                return "NoBinding";
            case GroomBindingRejectReason::NoTarget:
                return "NoTarget";
            case GroomBindingRejectReason::TargetNotReady:
                return "TargetNotReady";
            case GroomBindingRejectReason::BinderVersionMismatch:
                return "BinderVersionMismatch";
            case GroomBindingRejectReason::RootCountMismatch:
                return "RootCountMismatch";
            case GroomBindingRejectReason::SourceSignatureMismatch:
                return "SourceSignatureMismatch";
            case GroomBindingRejectReason::TargetTopologyMismatch:
                return "TargetTopologyMismatch";
            case GroomBindingRejectReason::TriangleOutOfRange:
                return "TriangleOutOfRange";
            case GroomBindingRejectReason::Count:
                break;
        }
        return "None";
    }

    std::string_view DescribeGroomBindingReject(GroomBindingRejectReason reason)
    {
        // Every one of these is a sentence someone can act on, not a token.
        // They surface in the log, in the Groom Binding inspector and in the
        // renderer statistics panel, and a groom that is not deforming is
        // supposed to be explained by exactly one of them.
        switch (reason)
        {
            case GroomBindingRejectReason::None:
                return "the binding attached";
            case GroomBindingRejectReason::NoBinding:
                return "no binding asset is set on this groom — build one against the body it grows on";
            case GroomBindingRejectReason::NoTarget:
                return "the bound target entity does not exist or carries no mesh";
            case GroomBindingRejectReason::TargetNotReady:
                return "the target mesh has no vertices or triangles yet — it is still loading";
            case GroomBindingRejectReason::BinderVersionMismatch:
                return "the binding was built by a different binder version — rebuild it";
            case GroomBindingRejectReason::RootCountMismatch:
                return "the binding holds a different number of roots than the groom has curves — "
                       "it belongs to a different groom, or the groom was re-cooked";
            case GroomBindingRejectReason::SourceSignatureMismatch:
                return "the groom's roots moved since the binding was built — rebuild the binding";
            case GroomBindingRejectReason::TargetTopologyMismatch:
                return "the target mesh is not the surface this binding was built against — "
                       "rebuild the binding, or bind to the body it was made for";
            case GroomBindingRejectReason::TriangleOutOfRange:
                return "a root addresses a triangle the target does not have — the binding is corrupt";
            case GroomBindingRejectReason::Count:
                break;
        }
        return "the binding attached";
    }

    u64 GroomBindingAsset::GetCpuMemoryBytes() const noexcept
    {
        u64 bytes = sizeof(GroomBindingAsset);
        bytes += m_Roots.capacity() * sizeof(GroomRootBinding);
        bytes += m_Name.capacity() + m_GroomSourcePath.capacity() + m_TargetSourcePath.capacity();
        return bytes;
    }

    void GroomBindingAsset::RecomputeDerivedData()
    {
        for (auto& count : m_QualityCounts)
        {
            count = 0;
        }
        m_MaxRestDistance = 0.0f;

        for (const auto& root : m_Roots)
        {
            // An out-of-range quality is counted as Exact rather than dropped:
            // this is the DERIVED pass, and Validate is what refuses a record
            // whose quality is not a quality. Two places rejecting the same
            // corruption differently is how one of them ends up wrong.
            const auto quality = IsValidGroomRootBindQuality(static_cast<i32>(root.Quality))
                                     ? static_cast<GroomRootBindQuality>(root.Quality)
                                     : GroomRootBindQuality::Exact;
            ++m_QualityCounts[static_cast<sizet>(quality)];

            if (std::isfinite(root.RestDistance) && root.RestDistance > m_MaxRestDistance)
            {
                m_MaxRestDistance = root.RestDistance;
            }
        }
    }

    bool GroomBindingAsset::Validate(std::string& outReason) const
    {
        const auto rootCount = static_cast<u64>(m_Roots.size());
        if (rootCount > GroomBindingLimits::MaxRootCount)
        {
            outReason = std::format("binding holds {} roots, above the cap {}", rootCount,
                                    GroomBindingLimits::MaxRootCount);
            return false;
        }
        if (m_Source.CurveCount != m_Roots.size())
        {
            outReason = std::format("binding holds {} roots but its source signature declares {} curves",
                                    rootCount, m_Source.CurveCount);
            return false;
        }
        if (m_Source.GuideCount > m_Source.CurveCount)
        {
            outReason = std::format("source signature declares {} guides among {} curves",
                                    m_Source.GuideCount, m_Source.CurveCount);
            return false;
        }

        if (m_Target.IndexCount % 3u != 0u)
        {
            outReason = std::format("target signature declares {} indices, which is not a whole number of triangles",
                                    m_Target.IndexCount);
            return false;
        }
        const u32 triangleCount = m_Target.IndexCount / 3u;
        if (triangleCount > GroomBindingLimits::MaxTargetTriangleCount)
        {
            outReason = std::format("target has {} triangles, above the cap {}", triangleCount,
                                    GroomBindingLimits::MaxTargetTriangleCount);
            return false;
        }
        // A binding with roots must address a surface that has triangles; the
        // empty binding (zero roots) is legal and addresses nothing.
        if (!m_Roots.empty() && triangleCount == 0u)
        {
            outReason = "binding holds roots but its target signature declares no triangles";
            return false;
        }

        for (sizet i = 0; i < m_Roots.size(); ++i)
        {
            const auto& root = m_Roots[i];
            if (!IsValidGroomRootBindQuality(static_cast<i32>(root.Quality)))
            {
                outReason = std::format("root {} carries quality {}, which is not a GroomRootBindQuality",
                                        i, root.Quality);
                return false;
            }
            if (root.TriangleIndex >= triangleCount)
            {
                outReason = std::format("root {} addresses triangle {} but the target has {}",
                                        i, root.TriangleIndex, triangleCount);
                return false;
            }

            // Every float that came off disk or out of a builder is checked for
            // finiteness before anything multiplies by it: one NaN barycentric
            // sends a whole strand to infinity, and a strand at infinity
            // silently poisons the groom's bounds rather than failing.
            if (!std::isfinite(root.Barycentric.x) || !std::isfinite(root.Barycentric.y) ||
                !std::isfinite(root.Barycentric.z))
            {
                outReason = std::format("root {} has a non-finite barycentric coordinate", i);
                return false;
            }
            const f32 barySum = root.Barycentric.x + root.Barycentric.y + root.Barycentric.z;
            if (std::fabs(barySum - 1.0f) > GroomBindingLimits::BarycentricSumTolerance)
            {
                outReason = std::format("root {} has barycentric coordinates summing to {}, not 1", i, barySum);
                return false;
            }

            if (!std::isfinite(root.RestOrigin.x) || !std::isfinite(root.RestOrigin.y) ||
                !std::isfinite(root.RestOrigin.z))
            {
                outReason = std::format("root {} has a non-finite rest origin", i);
                return false;
            }
            if (std::fabs(root.RestOrigin.x) > GroomBindingLimits::MaxCoordinate ||
                std::fabs(root.RestOrigin.y) > GroomBindingLimits::MaxCoordinate ||
                std::fabs(root.RestOrigin.z) > GroomBindingLimits::MaxCoordinate)
            {
                outReason = std::format("root {} has a rest origin outside +/-{} — a unit-scale accident",
                                        i, GroomBindingLimits::MaxCoordinate);
                return false;
            }
            if (!std::isfinite(root.RestDistance) || root.RestDistance < 0.0f)
            {
                outReason = std::format("root {} has rest distance {}, which is not a non-negative finite length",
                                        i, root.RestDistance);
                return false;
            }

            if (!std::isfinite(root.RestRotation.x) || !std::isfinite(root.RestRotation.y) ||
                !std::isfinite(root.RestRotation.z) || !std::isfinite(root.RestRotation.w))
            {
                outReason = std::format("root {} has a non-finite rest rotation", i);
                return false;
            }
            // A quaternion that is not unit-length is not a rotation, and the
            // deformation composes with it — an unnormalised one scales every
            // strand it carries. The tolerance is the f32 round trip, not taste.
            const f32 quatLength = std::sqrt(root.RestRotation.x * root.RestRotation.x +
                                             root.RestRotation.y * root.RestRotation.y +
                                             root.RestRotation.z * root.RestRotation.z +
                                             root.RestRotation.w * root.RestRotation.w);
            if (std::fabs(quatLength - 1.0f) > 1.0e-3f)
            {
                outReason = std::format("root {} has a rest rotation of length {}, which is not a unit quaternion",
                                        i, quatLength);
                return false;
            }
        }

        return true;
    }

    GroomBindingRejectReason GroomBindingAsset::CheckCompatibility(
        const GroomBindingSourceSignature& groom, const GroomBindingTargetSignature& target) const noexcept
    {
        // Ordered most-fundamental first. A binding built by a different binder
        // may disagree about every other field for reasons that have nothing to
        // do with the assets in front of it, so that is checked before anything
        // is compared.
        if (m_BinderVersion != kGroomBinderVersion)
        {
            return GroomBindingRejectReason::BinderVersionMismatch;
        }
        if (m_Source.CurveCount != groom.CurveCount || m_Roots.size() != groom.CurveCount)
        {
            return GroomBindingRejectReason::RootCountMismatch;
        }
        if (m_Source.RootHash != groom.RootHash || m_Source.GuideCount != groom.GuideCount)
        {
            return GroomBindingRejectReason::SourceSignatureMismatch;
        }
        if (target.VertexCount == 0u || target.IndexCount < 3u)
        {
            return GroomBindingRejectReason::TargetNotReady;
        }
        if (!m_Target.MatchesTopology(target))
        {
            return GroomBindingRejectReason::TargetTopologyMismatch;
        }

        // Reachable only if a file passed Validate against a signature that
        // then matched a target with fewer triangles — i.e. never, unless
        // MatchesTopology is weakened. Checked anyway because the consequence
        // is an out-of-bounds read in the deformation loop, and "cannot happen"
        // is not a bounds check.
        const u32 triangleCount = target.IndexCount / 3u;
        for (const auto& root : m_Roots)
        {
            if (root.TriangleIndex >= triangleCount)
            {
                return GroomBindingRejectReason::TriangleOutOfRange;
            }
        }

        return GroomBindingRejectReason::None;
    }
} // namespace OloEngine
