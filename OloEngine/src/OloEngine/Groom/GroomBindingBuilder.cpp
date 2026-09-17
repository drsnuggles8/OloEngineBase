#include "OloEnginePCH.h"
#include "OloEngine/Groom/GroomBindingBuilder.h"

#include "OloEngine/Groom/GroomAsset.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <vector>

namespace OloEngine
{
    namespace
    {
        // A uniform grid over the target's triangles. Build-time only: it
        // changes how long a bind takes and never which triangle wins, because
        // the tie-break is on the triangle index (see the header).
        struct TriangleGrid
        {
            glm::vec3 Min{ 0.0f };
            glm::vec3 CellSize{ 1.0f };
            glm::ivec3 Dimensions{ 1 };
            std::vector<std::vector<u32>> Cells;

            [[nodiscard]] i32 CellIndex(const glm::ivec3& cell) const noexcept
            {
                return (cell.z * Dimensions.y + cell.y) * Dimensions.x + cell.x;
            }

            [[nodiscard]] glm::ivec3 ClampCell(const glm::vec3& position) const noexcept
            {
                glm::ivec3 cell{ 0 };
                for (i32 axis = 0; axis < 3; ++axis)
                {
                    const f32 relative = (position[axis] - Min[axis]) / CellSize[axis];
                    // The floor is taken in f32 and clamped in i32: a relative
                    // coordinate of 1e38 (a root at a corrupt coordinate that
                    // still passed the finiteness check) would overflow the
                    // conversion, which is UB, so it is clamped in float first.
                    const f32 clamped = std::clamp(relative, 0.0f, static_cast<f32>(Dimensions[axis] - 1));
                    cell[axis] = static_cast<i32>(std::floor(clamped));
                    cell[axis] = std::clamp(cell[axis], 0, Dimensions[axis] - 1);
                }
                return cell;
            }
        };

        [[nodiscard]] TriangleGrid BuildTriangleGrid(const GroomSurfaceView& target, u32 resolution)
        {
            TriangleGrid grid;

            glm::vec3 boundsMin{ std::numeric_limits<f32>::max() };
            glm::vec3 boundsMax{ std::numeric_limits<f32>::lowest() };
            for (u32 vertex = 0; vertex < target.VertexCount; ++vertex)
            {
                const glm::vec3 position = target.Position(vertex);
                if (!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z))
                {
                    continue; // a non-finite vertex must not poison the grid bounds
                }
                boundsMin = glm::min(boundsMin, position);
                boundsMax = glm::max(boundsMax, position);
            }
            if (boundsMin.x > boundsMax.x)
            {
                // Every vertex was non-finite. One cell holding every triangle
                // degenerates the grid to a brute-force search, which is the
                // honest behaviour: the search still finds the closest triangle,
                // it just does not go faster for it.
                boundsMin = glm::vec3{ 0.0f };
                boundsMax = glm::vec3{ 0.0f };
            }

            const glm::vec3 extent = boundsMax - boundsMin;
            const f32 longest = std::max({ extent.x, extent.y, extent.z, 1.0e-6f });
            const u32 clampedResolution = std::clamp(resolution, 1u, 256u);
            const f32 cell = longest / static_cast<f32>(clampedResolution);

            grid.Min = boundsMin;
            grid.CellSize = glm::vec3{ std::max(cell, 1.0e-6f) };
            for (i32 axis = 0; axis < 3; ++axis)
            {
                const i32 count = static_cast<i32>(std::floor(extent[axis] / grid.CellSize[axis])) + 1;
                grid.Dimensions[axis] = std::clamp(count, 1, static_cast<i32>(clampedResolution));
            }
            grid.Cells.resize(static_cast<sizet>(grid.Dimensions.x) * grid.Dimensions.y * grid.Dimensions.z);

            // One ordered pass, so every cell's list is sorted by triangle index
            // by construction — which is what makes the `<` tie-break below
            // resolve to the lowest index without a sort.
            const u32 triangleCount = target.TriangleCount();
            for (u32 triangle = 0; triangle < triangleCount; ++triangle)
            {
                if (!target.TriangleInRange(triangle))
                {
                    continue;
                }
                const glm::uvec3 corners = target.TriangleIndices(triangle);
                const glm::vec3 v0 = target.Position(corners.x);
                const glm::vec3 v1 = target.Position(corners.y);
                const glm::vec3 v2 = target.Position(corners.z);

                const glm::ivec3 lo = grid.ClampCell(glm::min(v0, glm::min(v1, v2)));
                const glm::ivec3 hi = grid.ClampCell(glm::max(v0, glm::max(v1, v2)));
                for (i32 z = lo.z; z <= hi.z; ++z)
                {
                    for (i32 y = lo.y; y <= hi.y; ++y)
                    {
                        for (i32 x = lo.x; x <= hi.x; ++x)
                        {
                            grid.Cells[static_cast<sizet>(grid.CellIndex({ x, y, z }))].push_back(triangle);
                        }
                    }
                }
            }
            return grid;
        }

        struct ClosestTriangle
        {
            u32 Triangle = 0;
            glm::vec3 Barycentric{ 1.0f, 0.0f, 0.0f };
            f32 DistanceSquared = std::numeric_limits<f32>::max();
            bool Interior = false;
            bool Found = false;
        };

        // Tests one triangle and keeps it when it is strictly closer, OR exactly
        // as close and lower-indexed.
        //
        // THE SECOND CLAUSE IS THE WHOLE TIE-BREAK, and a strict `<` alone was
        // not enough. Within one cell the lists are index-sorted, so "first one
        // wins" is "lowest index wins"; ACROSS cells it is not — the shells are
        // walked in z,y,x order, so a higher-indexed triangle in an earlier cell
        // would beat an equidistant lower-indexed one in a later cell, and
        // changing GridResolution would then change the cooked bytes. A root on
        // a shared edge is the common case on a closed mesh, so this is not a
        // corner to leave to traversal order.
        //
        // Expressed with `<` only, never `==`: comparing floats for equality is
        // forbidden here (cpp-coding-quality.md), and "not strictly worse" says
        // exactly what is meant without it.
        void ConsiderTriangle(const GroomSurfaceView& target, u32 triangle, const glm::vec3& root,
                              ClosestTriangle& best)
        {
            if (!target.TriangleInRange(triangle))
            {
                return;
            }
            const glm::uvec3 corners = target.TriangleIndices(triangle);
            glm::vec3 barycentric{ 1.0f, 0.0f, 0.0f };
            bool interior = false;
            const f32 distanceSquared = GroomBindingBuilder::ClosestPointOnTriangle(
                root, target.Position(corners.x), target.Position(corners.y), target.Position(corners.z),
                barycentric, interior);
            if (!std::isfinite(distanceSquared))
            {
                return;
            }
            const bool closer = distanceSquared < best.DistanceSquared;
            const bool equallyClose = !closer && !(best.DistanceSquared < distanceSquared);
            if (!closer && !(equallyClose && best.Found && triangle < best.Triangle))
            {
                return;
            }
            best.Triangle = triangle;
            best.Barycentric = barycentric;
            best.DistanceSquared = distanceSquared;
            best.Interior = interior;
            best.Found = true;
        }

        // Searches outward in shells of grid cells, stopping as soon as the
        // closest triangle found so far is nearer than the nearest point of the
        // next shell — the standard early-out, and it changes nothing about
        // WHICH triangle wins, only how many are tested.
        [[nodiscard]] ClosestTriangle FindClosestTriangle(const GroomSurfaceView& target, const TriangleGrid& grid,
                                                          const glm::vec3& root)
        {
            ClosestTriangle best;
            const glm::ivec3 centre = grid.ClampCell(root);
            const i32 maxRing = std::max({ grid.Dimensions.x, grid.Dimensions.y, grid.Dimensions.z });
            const f32 smallestCell = std::min({ grid.CellSize.x, grid.CellSize.y, grid.CellSize.z });

            for (i32 ring = 0; ring < maxRing; ++ring)
            {
                if (best.Found)
                {
                    // Everything in ring N is at least (N-1) cells away from the
                    // root's own cell. Once the best hit is nearer than that,
                    // nothing further out can beat it.
                    const f32 shellDistance = static_cast<f32>(ring - 1) * smallestCell;
                    if (shellDistance > 0.0f && shellDistance * shellDistance > best.DistanceSquared)
                    {
                        break;
                    }
                }

                const glm::ivec3 lo = glm::max(centre - ring, glm::ivec3{ 0 });
                const glm::ivec3 hi = glm::min(centre + ring, grid.Dimensions - 1);
                for (i32 z = lo.z; z <= hi.z; ++z)
                {
                    for (i32 y = lo.y; y <= hi.y; ++y)
                    {
                        for (i32 x = lo.x; x <= hi.x; ++x)
                        {
                            // Only the SHELL of the box, not its interior: the
                            // interior was covered by a previous ring.
                            const bool onShell = std::abs(x - centre.x) == ring || std::abs(y - centre.y) == ring ||
                                                 std::abs(z - centre.z) == ring;
                            if (!onShell)
                            {
                                continue;
                            }
                            for (const u32 triangle : grid.Cells[static_cast<sizet>(grid.CellIndex({ x, y, z }))])
                            {
                                ConsiderTriangle(target, triangle, root, best);
                            }
                        }
                    }
                }
            }
            return best;
        }
    } // anonymous namespace

    f32 GroomBindingBuilder::ClosestPointOnTriangle(const glm::vec3& point, const glm::vec3& v0, const glm::vec3& v1,
                                                    const glm::vec3& v2, glm::vec3& outBarycentric,
                                                    bool& outInterior) noexcept
    {
        // Ericson, Real-Time Collision Detection §5.1.5 — the Voronoi-region
        // form. Chosen over "project onto the plane and clamp" because that one
        // is wrong for an obtuse triangle: the clamped projection can land
        // outside the triangle entirely, and a root bound there gets a frame
        // from a face it does not touch.
        outInterior = false;
        outBarycentric = glm::vec3{ 1.0f, 0.0f, 0.0f };

        const glm::vec3 ab = v1 - v0;
        const glm::vec3 ac = v2 - v0;
        const glm::vec3 ap = point - v0;

        const f32 d1 = glm::dot(ab, ap);
        const f32 d2 = glm::dot(ac, ap);
        if (d1 <= 0.0f && d2 <= 0.0f)
        {
            outBarycentric = { 1.0f, 0.0f, 0.0f };
            return glm::dot(ap, ap);
        }

        const glm::vec3 bp = point - v1;
        const f32 d3 = glm::dot(ab, bp);
        const f32 d4 = glm::dot(ac, bp);
        if (d3 >= 0.0f && d4 <= d3)
        {
            outBarycentric = { 0.0f, 1.0f, 0.0f };
            return glm::dot(bp, bp);
        }

        const f32 vc = d1 * d4 - d3 * d2;
        if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f)
        {
            const f32 denominator = d1 - d3;
            const f32 v = denominator != 0.0f ? d1 / denominator : 0.0f;
            outBarycentric = { 1.0f - v, v, 0.0f };
            const glm::vec3 closest = v0 + ab * v;
            return glm::dot(point - closest, point - closest);
        }

        const glm::vec3 cp = point - v2;
        const f32 d5 = glm::dot(ab, cp);
        const f32 d6 = glm::dot(ac, cp);
        if (d6 >= 0.0f && d5 <= d6)
        {
            outBarycentric = { 0.0f, 0.0f, 1.0f };
            return glm::dot(cp, cp);
        }

        const f32 vb = d5 * d2 - d1 * d6;
        if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f)
        {
            const f32 denominator = d2 - d6;
            const f32 w = denominator != 0.0f ? d2 / denominator : 0.0f;
            outBarycentric = { 1.0f - w, 0.0f, w };
            const glm::vec3 closest = v0 + ac * w;
            return glm::dot(point - closest, point - closest);
        }

        const f32 va = d3 * d6 - d5 * d4;
        if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f)
        {
            const f32 denominator = (d4 - d3) + (d5 - d6);
            const f32 w = denominator != 0.0f ? (d4 - d3) / denominator : 0.0f;
            outBarycentric = { 0.0f, 1.0f - w, w };
            const glm::vec3 closest = v1 + (v2 - v1) * w;
            return glm::dot(point - closest, point - closest);
        }

        const f32 denominator = va + vb + vc;
        if (denominator == 0.0f || !std::isfinite(denominator))
        {
            // A degenerate triangle reaches here. The first corner is as good an
            // answer as any and keeps the barycentric a partition of unity;
            // MakeGroomSurfaceFrame is what refuses to give it a frame.
            outBarycentric = { 1.0f, 0.0f, 0.0f };
            return glm::dot(ap, ap);
        }
        const f32 v = vb / denominator;
        const f32 w = vc / denominator;
        outBarycentric = { 1.0f - v - w, v, w };
        outInterior = true;
        const glm::vec3 closest = v0 + ab * v + ac * w;
        return glm::dot(point - closest, point - closest);
    }

    u64 GroomBindingBuilder::HashBytes(const void* data, sizet size) noexcept
    {
        // 64-bit FNV-1a, the same constants GroomCooker::HashSourceBytes uses.
        u64 hash = 1469598103934665603ull;
        const auto* bytes = static_cast<const u8*>(data);
        for (sizet i = 0; i < size; ++i)
        {
            hash ^= static_cast<u64>(bytes[i]);
            hash *= 1099511628211ull;
        }
        return hash;
    }

    GroomBindingSourceSignature GroomBindingBuilder::SignGroom(const GroomAsset& groom) noexcept
    {
        GroomBindingSourceSignature signature;
        signature.CurveCount = groom.GetCurveCount();
        signature.GuideCount = groom.GetGuideCount();

        // Roots and root UVs only. Hashing the tips as well would refuse a
        // binding after a re-cook that changed nothing a binding depends on —
        // see the comment on GroomBindingSourceSignature.
        //
        // The BIT PATTERNS, not the values: a hash over formatted floats would
        // depend on the locale, and a hash over the values themselves would need
        // an equality on floats, which this repo does not do.
        u64 hash = 1469598103934665603ull;
        const auto mix = [&hash](const void* data, sizet size)
        {
            const auto* bytes = static_cast<const u8*>(data);
            for (sizet i = 0; i < size; ++i)
            {
                hash ^= static_cast<u64>(bytes[i]);
                hash *= 1099511628211ull;
            }
        };
        const auto& points = groom.GetPoints();
        const auto& rootUVs = groom.GetRootUVs();
        for (u32 curve = 0; curve < signature.CurveCount; ++curve)
        {
            const u32 first = groom.GetCurveFirstPoint(curve);
            if (first < points.size())
            {
                mix(&points[first], sizeof(glm::vec3));
            }
            if (curve < rootUVs.size())
            {
                mix(&rootUVs[curve], sizeof(glm::vec2));
            }
        }
        signature.RootHash = hash;
        return signature;
    }

    GroomBindingTargetSignature GroomBindingBuilder::SignTarget(const GroomSurfaceView& target) noexcept
    {
        GroomBindingTargetSignature signature;
        signature.VertexCount = target.VertexCount;
        signature.IndexCount = target.IndexCount;
        signature.BoneCount = target.BoneCount;
        signature.SkeletonNameHash = target.SkeletonNameHash;

        if (target.Indices != nullptr && target.IndexCount > 0u)
        {
            signature.IndexHash = HashBytes(target.Indices, static_cast<sizet>(target.IndexCount) * sizeof(u32));
        }

        // Positions are hashed one at a time rather than as a block, because the
        // view is STRIDED — a block hash over a Vertex array would fold the
        // normals and UVs in, and a re-export that changed only a UV would then
        // read as a moved surface.
        u64 positionHash = 1469598103934665603ull;
        for (u32 vertex = 0; vertex < target.VertexCount; ++vertex)
        {
            const glm::vec3 position = target.Position(vertex);
            const auto* bytes = reinterpret_cast<const u8*>(&position);
            for (sizet i = 0; i < sizeof(glm::vec3); ++i)
            {
                positionHash ^= static_cast<u64>(bytes[i]);
                positionHash *= 1099511628211ull;
            }
        }
        signature.RestPositionHash = positionHash;
        return signature;
    }

    bool GroomBindingBuilder::Build(const GroomAsset& groom, const GroomSurfaceView& target,
                                    const std::string& targetSourcePath,
                                    const GroomBindingBuildSettings& settings, Ref<GroomBindingAsset>& outBinding,
                                    GroomBindingBuildStats& outStats, std::string& outReason)
    {
        outStats = GroomBindingBuildStats{};

        if (targetSourcePath.size() > GroomLimits::MaxSourcePathLength)
        {
            outReason = std::format("target source path is {} bytes, above the cap {}", targetSourcePath.size(),
                                    GroomLimits::MaxSourcePathLength);
            return false;
        }

        if (!std::isfinite(settings.SearchRadius) || settings.SearchRadius <= 0.0f)
        {
            outReason = std::format("search radius {} is not a positive finite length", settings.SearchRadius);
            return false;
        }
        if (!target.IsUsable())
        {
            outReason = std::format("target surface is not usable: {} vertices, {} indices, stride {}",
                                    target.VertexCount, target.IndexCount, target.PositionStride);
            return false;
        }
        const u32 triangleCount = target.TriangleCount();
        if (triangleCount > GroomBindingLimits::MaxTargetTriangleCount)
        {
            outReason = std::format("target has {} triangles, above the cap {}", triangleCount,
                                    GroomBindingLimits::MaxTargetTriangleCount);
            return false;
        }

        const u32 curveCount = groom.GetCurveCount();
        if (curveCount > GroomBindingLimits::MaxRootCount)
        {
            outReason = std::format("groom has {} curves, above the binding cap {}", curveCount,
                                    GroomBindingLimits::MaxRootCount);
            return false;
        }

        // A target whose index buffer overruns its vertex array is refused
        // outright rather than bound around: every root placed on a valid
        // triangle would be correct, and the coat would have a hole exactly
        // where the mesh is corrupt, which reads as a groom problem.
        for (u32 triangle = 0; triangle < triangleCount; ++triangle)
        {
            if (!target.TriangleInRange(triangle))
            {
                ++outStats.TrianglesOutOfRange;
            }
        }
        if (outStats.TrianglesOutOfRange != 0u)
        {
            outReason = std::format("target has {} triangles whose indices address vertices it does not have",
                                    outStats.TrianglesOutOfRange);
            return false;
        }

        const TriangleGrid grid = BuildTriangleGrid(target, settings.GridResolution);
        const f32 searchRadiusSquared = settings.SearchRadius * settings.SearchRadius;

        auto binding = Ref<GroomBindingAsset>::Create();
        binding->m_Roots.resize(curveCount);
        binding->m_BinderVersion = kGroomBinderVersion;
        binding->m_Source = SignGroom(groom);
        binding->m_Target = SignTarget(target);

        const auto& points = groom.GetPoints();
        f64 distanceSum = 0.0;

        for (u32 curve = 0; curve < curveCount; ++curve)
        {
            const u32 first = groom.GetCurveFirstPoint(curve);
            // BOUNDS FIRST, then read. The finiteness guard below is documented
            // as covering a groom built in memory by a test or a tool rather
            // than loaded through GroomAsset::Validate — and such a groom can
            // just as easily carry an offset table that does not match its point
            // array, in which case the read that feeds the guard is already out
            // of bounds. SignGroom in this same file checks before it reads; so
            // does this now.
            if (first >= points.size())
            {
                outReason = std::format("curve {} starts at point {} but the groom holds {}", curve, first,
                                        points.size());
                return false;
            }
            const glm::vec3 root = points[first];

            GroomRootBinding& record = binding->m_Roots[curve];

            if (!std::isfinite(root.x) || !std::isfinite(root.y) || !std::isfinite(root.z))
            {
                // GroomAsset::Validate already refuses a non-finite point, so
                // this is unreachable through any loaded groom. It is handled
                // rather than asserted because the builder is also reachable
                // from a groom built in memory by a test or a tool, and a NaN
                // root would otherwise become a NaN frame and then a NaN coat.
                outReason = std::format("curve {} has a non-finite root position", curve);
                return false;
            }

            const ClosestTriangle closest = FindClosestTriangle(target, grid, root);
            if (!closest.Found)
            {
                outReason = std::format("curve {} found no triangle on a target with {} of them", curve,
                                        triangleCount);
                return false;
            }

            const glm::uvec3 corners = target.TriangleIndices(closest.Triangle);
            const GroomSurfaceFrame frame = MakeGroomSurfaceFrame(
                target.Position(corners.x), target.Position(corners.y), target.Position(corners.z),
                closest.Barycentric);

            const f32 distance = std::sqrt(std::max(closest.DistanceSquared, 0.0f));

            record.TriangleIndex = closest.Triangle;
            record.Barycentric = closest.Barycentric;
            record.RestOrigin = frame.Origin;
            record.RestRotation = frame.Rotation;
            record.RestDistance = distance;
            record.Pad0 = 0.0f;

            GroomRootBindQuality quality = GroomRootBindQuality::Exact;
            if (closest.DistanceSquared > searchRadiusSquared)
            {
                quality = GroomRootBindQuality::Distant;
            }
            else if (!closest.Interior)
            {
                quality = GroomRootBindQuality::Clamped;
            }
            record.Quality = static_cast<u32>(quality);

            if (!frame.Valid)
            {
                ++outStats.RootsOnDegenerateTriangles;
            }

            ++outStats.RootsBound;
            switch (quality)
            {
                case GroomRootBindQuality::Exact:
                    ++outStats.RootsExact;
                    break;
                case GroomRootBindQuality::Clamped:
                    ++outStats.RootsClamped;
                    break;
                case GroomRootBindQuality::Distant:
                    ++outStats.RootsDistant;
                    break;
                case GroomRootBindQuality::Count:
                    break;
            }
            outStats.MaxRestDistance = std::max(outStats.MaxRestDistance, distance);
            distanceSum += static_cast<f64>(distance);
        }

        outStats.MeanRestDistance =
            curveCount != 0u ? static_cast<f32>(distanceSum / static_cast<f64>(curveCount)) : 0.0f;

        binding->m_GroomSourcePath = groom.GetProvenance().SourcePath;
        binding->m_TargetSourcePath = targetSourcePath;
        // Carried on the in-memory asset only: the cooked file does NOT store a
        // name, and GroomBindingSerializer::TryLoadData derives one from the
        // file stem. That is deliberate — a binding's name is a property of
        // where it lives, and storing it would give a renamed file two names
        // that disagree.
        binding->m_Name = groom.GetName();
        binding->RecomputeDerivedData();

        // The builder refuses to produce a binding the reader would reject, so a
        // corrupt one can only ever come from outside this process — the
        // symmetric-validation rule GroomSerializer states for .ologroom.
        if (!binding->Validate(outReason))
        {
            return false;
        }

        outBinding = binding;
        return true;
    }
} // namespace OloEngine
