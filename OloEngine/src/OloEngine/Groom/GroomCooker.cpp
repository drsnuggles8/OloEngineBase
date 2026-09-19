#include "OloEnginePCH.h"
#include "OloEngine/Groom/GroomCooker.h"

#include "OloEngine/Asset/AssetSerializer.h"

#include <algorithm>
#include <format>
#include <numeric>
#include <utility>

namespace OloEngine
{
    namespace GroomCooker
    {
        bool Canonicalize(GroomAsset& groom, std::string& outReason)
        {
            // Validate BEFORE touching anything: reordering an inconsistent
            // groom would turn a clear "your arrays disagree" into a confusing
            // out-of-range failure halfway through the permutation.
            if (!groom.Validate(outReason))
            {
                return false;
            }

            const u32 curveCount = groom.GetCurveCount();

            // Already canonical is the common case (one group, or a DCC that
            // exported group by group). Test that FIRST: building the
            // permutation and sorting it before checking cost a CurveCount
            // allocation plus an O(n log n) pass on every cook, including the
            // many that then discarded both.
            const auto& groupIds = groom.m_CurveGroupIds;
            if (!std::is_sorted(groupIds.begin(), groupIds.end()))
            {
                // The permutation: curve indices ordered by group, stable within
                // a group. std::stable_sort, not sort — an unstable sort would
                // make the cooked bytes depend on the implementation's pivot
                // choices, which is exactly the determinism leak this file
                // exists to close.
                std::vector<u32> order(curveCount);
                std::iota(order.begin(), order.end(), 0u);
                std::stable_sort(order.begin(), order.end(),
                                 [&groupIds](u32 lhs, u32 rhs)
                                 { return groupIds[lhs] < groupIds[rhs]; });

                std::vector<u32> newOffsets;
                std::vector<glm::vec3> newPoints;
                std::vector<f32> newWidths;
                std::vector<glm::vec2> newRootUVs;
                std::vector<u16> newGroupIds;
                std::vector<u8> newFlags;

                newOffsets.reserve(static_cast<sizet>(curveCount) + 1);
                newPoints.reserve(groom.m_Points.size());
                newWidths.reserve(groom.m_PointWidths.size());
                newRootUVs.reserve(curveCount);
                newGroupIds.reserve(curveCount);
                newFlags.reserve(curveCount);

                newOffsets.push_back(0u);
                for (const u32 sourceCurve : order)
                {
                    const u32 first = groom.m_CurveOffsets[sourceCurve];
                    const u32 count = groom.m_CurveOffsets[sourceCurve + 1] - first;
                    newPoints.insert(newPoints.end(), groom.m_Points.begin() + first,
                                     groom.m_Points.begin() + first + count);
                    newWidths.insert(newWidths.end(), groom.m_PointWidths.begin() + first,
                                     groom.m_PointWidths.begin() + first + count);
                    newOffsets.push_back(static_cast<u32>(newPoints.size()));
                    newRootUVs.push_back(groom.m_RootUVs[sourceCurve]);
                    newGroupIds.push_back(groom.m_CurveGroupIds[sourceCurve]);
                    newFlags.push_back(groom.m_CurveFlags[sourceCurve]);
                }

                groom.m_CurveOffsets = std::move(newOffsets);
                groom.m_Points = std::move(newPoints);
                groom.m_PointWidths = std::move(newWidths);
                groom.m_RootUVs = std::move(newRootUVs);
                groom.m_CurveGroupIds = std::move(newGroupIds);
                groom.m_CurveFlags = std::move(newFlags);
            }

            groom.RecomputeDerivedData();

            // Every group must now be a non-empty contiguous range. An empty
            // one means a registered group that no curve joined — the importer
            // should not produce that, and silently dropping it would renumber
            // every later group id, so it is an error rather than a cleanup.
            for (u32 group = 0; group < groom.GetGroupCount(); ++group)
            {
                if (groom.GetGroupRanges()[group].CurveCount == 0)
                {
                    outReason = std::format("group {} ('{}') has no curves; every declared group must be used",
                                            group, groom.GetGroupNames()[group]);
                    return false;
                }
            }

            return groom.Validate(outReason);
        }

        bool CookToBytes(const GroomAsset& groom, std::vector<u8>& outBytes, std::string& outReason)
        {
            // The cook works on a COPY so a caller can cook an asset it is also
            // using — and so a failed cook cannot leave a half-permuted groom
            // behind for the next consumer.
            GroomAsset working;
            working.m_Name = groom.m_Name;
            working.m_CurveOffsets = groom.m_CurveOffsets;
            working.m_Points = groom.m_Points;
            working.m_PointWidths = groom.m_PointWidths;
            working.m_RootUVs = groom.m_RootUVs;
            working.m_CurveGroupIds = groom.m_CurveGroupIds;
            working.m_CurveFlags = groom.m_CurveFlags;
            working.m_GroupNames = groom.m_GroupNames;
            // The COAT TABLE (#1251). Copied here explicitly, like every other
            // array, and that explicitness is the whole trap: this function
            // copies field by field rather than cloning, so an array added to
            // GroomAsset and forgotten here is silently dropped by the COOK
            // alone. The loose asset would keep its guard-hair roles, the cooked
            // one would lose them, and the difference is a coat that is slightly
            // too uniform — which is criterion 1's "preserving density and
            // silhouette during cooking" failing in exactly the way it is
            // written to fail. GroomCoatAuthoringTest cooks and re-reads for
            // this reason.
            working.m_GroupCoats = groom.m_GroupCoats;
            working.m_Basis = groom.m_Basis;
            working.m_Provenance = groom.m_Provenance;

            if (!Canonicalize(working, outReason))
            {
                return false;
            }
            return GroomSerializer::EncodeToBytes(working, outBytes, outReason);
        }

        u64 HashSourceBytes(const void* data, sizet size) noexcept
        {
            // FNV-1a 64. Chosen over the engine's CRC32 because a 32-bit
            // provenance hash collides at a few tens of thousands of assets
            // (birthday bound), and this value's whole job is to answer "is the
            // .ologroom on disk stale relative to its source?".
            constexpr u64 offsetBasis = 14695981039346656037ull;
            constexpr u64 prime = 1099511628211ull;

            u64 hash = offsetBasis;
            const auto* bytes = static_cast<const u8*>(data);
            for (sizet i = 0; i < size; ++i)
            {
                hash ^= static_cast<u64>(bytes[i]);
                hash *= prime;
            }
            return hash;
        }
    } // namespace GroomCooker
} // namespace OloEngine
