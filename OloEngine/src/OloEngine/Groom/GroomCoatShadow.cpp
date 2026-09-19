#include "OloEngine/Groom/GroomCoatShadow.h"

#include "OloEngine/Core/Base.h"
#include "OloEngine/Groom/GroomAsset.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace OloEngine::GroomCoatShadow
{
    namespace
    {
        constexpr f64 kPi = std::numbers::pi_v<f64>;

        // The expected value of sin(theta) between a ray and a uniformly
        // oriented fibre. Not a fudge factor: integrating sin over the sphere
        // and dividing by the solid angle gives exactly pi/4, and it is what
        // turns "fibre length times diameter per unit volume" into "expected
        // crossings per unit length of ray" for an uncombed coat.
        constexpr f64 kIsotropicMeanSine = kPi / 4.0;

        // A 32-bit integer hash, written entirely in defined unsigned
        // wraparound so the jitter is identical on every compiler and every
        // host. The same discipline GroomCoverage's stochastic hash uses, and
        // for the same reason: a reference that moved between machines would
        // not be a reference.
        [[nodiscard]] u32 HashU32(u32 x) noexcept
        {
            x ^= x >> 16u;
            x *= 0x7FEB352Du;
            x ^= x >> 15u;
            x *= 0x846CA68Bu;
            x ^= x >> 16u;
            return x;
        }

        [[nodiscard]] f32 HashUnitFloat(u32 x) noexcept
        {
            // 24 bits, so the value lands on the same 2^-24 grid a float can
            // represent exactly and the sequence is reproducible bit for bit.
            return static_cast<f32>(HashU32(x) >> 8u) * (1.0f / 16777216.0f);
        }

        [[nodiscard]] bool IsFiniteVec(const glm::vec3& v) noexcept
        {
            return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
        }

        // An orthonormal pair perpendicular to `n`, chosen without a branch on
        // a near-zero component. Duff et al.'s construction: it is stable for
        // every unit input including the poles, where the naive "cross with up"
        // degenerates.
        void BuildBasis(const glm::vec3& n, glm::vec3& outT, glm::vec3& outB) noexcept
        {
            const f32 sign = n.z >= 0.0f ? 1.0f : -1.0f;
            const f32 a = -1.0f / (sign + n.z);
            const f32 b = n.x * n.y * a;
            outT = glm::vec3(1.0f + sign * n.x * n.x * a, sign * b, -sign * n.x);
            outB = glm::vec3(b, sign + n.y * n.y * a, -n.y);
        }

        // The mean of the model matrix's three axis lengths — the same
        // object-to-world scale convention GroomCoverage::ProjectGroom and
        // AlembicGroomImporter use when a non-uniform transform is baked into
        // widths. Kept identical on purpose: three places deriving a coat's
        // apparent thickness slightly differently is how a measurement stops
        // describing what renders.
        [[nodiscard]] f32 MeanAxisScale(const glm::mat4& model) noexcept
        {
            const f32 sx = glm::length(glm::vec3(model[0]));
            const f32 sy = glm::length(glm::vec3(model[1]));
            const f32 sz = glm::length(glm::vec3(model[2]));
            return (sx + sy + sz) / 3.0f;
        }

        // Does the ray hit this segment's tapered cylinder, and at what
        // distance? FLAT ENDS, never a capsule: GroomStrandMesh emits a
        // square-ended band and a rounded cap adds occlusion from a shape
        // nothing draws. groom-strand-visibility.md rule 3 records what that
        // cost the coverage model; the same error here would invent shadow.
        //
        // The test is the classic closest-approach between two lines, with the
        // segment parameter clamped to its own ends rather than extended: a
        // near-miss past the tip is a miss.
        [[nodiscard]] bool RayHitsSegment(const glm::vec3& origin, const glm::vec3& direction,
                                          const CoatSegment& segment, f32 tMin, f32 tMax) noexcept
        {
            const glm::vec3 ab = segment.B - segment.A;
            const f32 abLenSq = glm::dot(ab, ab);
            const glm::vec3 ao = origin - segment.A;

            if (abLenSq <= 1.0e-18f)
            {
                // A zero-length segment is legal in a cooked groom (two
                // coincident control points). It occludes nothing measurable,
                // and treating it as a sphere would give it a cross-section the
                // raster never draws.
                return false;
            }

            const f32 dDotAb = glm::dot(direction, ab);
            const f32 aoDotAb = glm::dot(ao, ab);
            const f32 dDotAo = glm::dot(direction, ao);

            // direction is unit length, so the quadratic's leading term is
            // 1 - (d.ab)^2/|ab|^2.
            const f32 denom = 1.0f - (dDotAb * dDotAb) / abLenSq;

            if (denom <= 1.0e-8f)
            {
                // Ray parallel to the fibre. It runs ALONG the strand rather
                // than across it, so it crosses no cross-section; reporting a
                // hit here is what would make a coat opaque when lit straight
                // down the lay of the hair, which is exactly the regime the
                // anisotropic volume exists to get right.
                return false;
            }

            // The closest-approach solve, written once: setting both partial
            // derivatives of |(O + tD) - (A + s*ab)|^2 to zero and eliminating
            // s gives this t directly. s is then recovered FROM t rather than
            // solved again, so the two parameters come out of one solve and
            // cannot disagree about which pair of points is the closest.
            const f32 t = ((aoDotAb * dDotAb) / abLenSq - dDotAo) / denom;
            if (t < tMin || t > tMax)
            {
                return false;
            }

            // HALF-OPEN in s. A curve's interior node is the B end of one
            // segment and the A end of the next, so a closed [0,1] test lets a
            // ray passing exactly through the node hit BOTH and count one fibre
            // twice. Giving the node to exactly one of the two segments costs
            // only the very last tip's end face — one surface per strand rather
            // than one potential error per node.
            //
            // This is a correctness convention, not a measured fix: the ray
            // sets that exposed it are measure-zero, and A/B-ing the change on
            // a 40x40 lattice moved no reported number. The measurable trap in
            // this reference is the FOOTPRINT, not the node — see
            // ReferenceSettings::FootprintRadius.
            const f32 s = (aoDotAb + t * dDotAb) / abLenSq;
            if (s < 0.0f || s >= 1.0f)
            {
                return false;
            }

            const glm::vec3 pointOnRay = origin + direction * t;
            const glm::vec3 pointOnSegment = segment.A + ab * s;
            const f32 radius = segment.RadiusA + (segment.RadiusB - segment.RadiusA) * s;
            if (radius <= 0.0f)
            {
                return false;
            }

            const glm::vec3 delta = pointOnRay - pointOnSegment;
            return glm::dot(delta, delta) <= radius * radius;
        }

        // The slab test, returning the ray's entry and exit parameters for the
        // volume's box. Rays that start inside get tEnter = 0.
        [[nodiscard]] bool IntersectBox(const glm::vec3& origin, const glm::vec3& dir, const glm::vec3& lo,
                                        const glm::vec3& hi, f32& tEnter, f32& tExit) noexcept
        {
            f32 t0 = 0.0f;
            f32 t1 = std::numeric_limits<f32>::max();
            for (i32 axis = 0; axis < 3; ++axis)
            {
                const f32 d = dir[axis];
                const f32 o = origin[axis];
                if (std::abs(d) < 1.0e-12f)
                {
                    if (o < lo[axis] || o > hi[axis])
                    {
                        return false;
                    }
                    continue;
                }
                const f32 inv = 1.0f / d;
                // NOT named `near` / `far`: both are Win32 macros on this
                // platform and a local of either name fails to compile with
                // "expected unqualified-id", a full rebuild after the fact.
                f32 slabEnter = (lo[axis] - o) * inv;
                f32 slabExit = (hi[axis] - o) * inv;
                if (slabEnter > slabExit)
                {
                    std::swap(slabEnter, slabExit);
                }
                t0 = std::max(t0, slabEnter);
                t1 = std::min(t1, slabExit);
                if (t0 > t1)
                {
                    return false;
                }
            }
            tEnter = t0;
            tExit = t1;
            return true;
        }
        [[nodiscard]] sizet VoxelIndex(const glm::ivec3& dims, i32 x, i32 y, i32 z) noexcept
        {
            return static_cast<sizet>(x) + static_cast<sizet>(dims.x) * (static_cast<sizet>(y) +
                                                                         static_cast<sizet>(dims.y) *
                                                                             static_cast<sizet>(z));
        }
    } // namespace

    // =========================================================================
    // Geometry
    // =========================================================================

    u32 BuildCoatSegments(const GroomAsset& groom, const glm::mat4& model, const CoatSampleSettings& settings,
                          std::vector<CoatSegment>& outSegments)
    {
        outSegments.clear();

        const u32 curveCount = groom.GetCurveCount();
        if (curveCount == 0 || settings.MaxStrands == 0 || settings.MaxSegments == 0)
        {
            return 0;
        }
        if (!std::isfinite(settings.WidthScale) || settings.WidthScale <= 0.0f)
        {
            return 0;
        }

        // A STRIDE, never a prefix. The cook sorts curves so each group is
        // contiguous, so taking the first N takes one side of the animal and
        // leaves a bald flank — groom-strand-visibility.md rule 7. A density
        // built from half an animal is not a coarser measurement, it is a
        // measurement of a different coat.
        u32 eligible = 0;
        for (u32 curve = 0; curve < curveCount; ++curve)
        {
            if (!settings.GuidesOnly || groom.IsGuide(curve))
            {
                ++eligible;
            }
        }
        if (eligible == 0)
        {
            return 0;
        }
        const u32 stride = eligible > settings.MaxStrands ? (eligible + settings.MaxStrands - 1) / settings.MaxStrands
                                                          : 1u;

        const f32 objectScale = MeanAxisScale(model);
        // Halved EXACTLY ONCE, here, where an authored diameter becomes a
        // radius. groom-strand-visibility.md rule 8: a second halving
        // downstream yields a coat half as thick as authored, which looks
        // entirely plausible.
        const f32 radiusScale = 0.5f * settings.WidthScale * objectScale;

        const std::vector<glm::vec3>& points = groom.GetPoints();
        const std::vector<f32>& widths = groom.GetPointWidths();

        u32 emitted = 0;
        u32 eligibleIndex = 0;
        for (u32 curve = 0; curve < curveCount; ++curve)
        {
            if (settings.GuidesOnly && !groom.IsGuide(curve))
            {
                continue;
            }
            const u32 mine = eligibleIndex++;
            if ((mine % stride) != 0)
            {
                continue;
            }

            const u32 first = groom.GetCurveFirstPoint(curve);
            const u32 count = groom.GetCurvePointCount(curve);
            for (u32 i = 0; i + 1 < count; ++i)
            {
                if (emitted >= settings.MaxSegments)
                {
                    return emitted;
                }

                const u32 i0 = first + i;
                const u32 i1 = first + i + 1;
                if (i1 >= points.size())
                {
                    break;
                }

                CoatSegment segment;
                segment.A = glm::vec3(model * glm::vec4(points[i0], 1.0f));
                segment.B = glm::vec3(model * glm::vec4(points[i1], 1.0f));
                const f32 w0 = i0 < widths.size() ? widths[i0] : 0.0f;
                const f32 w1 = i1 < widths.size() ? widths[i1] : 0.0f;
                segment.RadiusA = w0 * radiusScale;
                segment.RadiusB = w1 * radiusScale;

                // DROPPED, not clamped. A non-finite point in the density
                // would spread a NaN through every voxel the march touches,
                // and the resulting black coat would read as a shading bug
                // rather than as the corrupt curve it is.
                if (!IsFiniteVec(segment.A) || !IsFiniteVec(segment.B))
                {
                    continue;
                }
                if (!std::isfinite(segment.RadiusA) || !std::isfinite(segment.RadiusB) || segment.RadiusA < 0.0f ||
                    segment.RadiusB < 0.0f)
                {
                    continue;
                }

                outSegments.push_back(segment);
                ++emitted;
            }
        }

        return emitted;
    }

    bool CoatSegmentBounds(std::span<const CoatSegment> segments, glm::vec3& outMin, glm::vec3& outMax)
    {
        if (segments.empty())
        {
            return false;
        }

        glm::vec3 lo(std::numeric_limits<f32>::max());
        glm::vec3 hi(std::numeric_limits<f32>::lowest());
        bool any = false;
        for (const CoatSegment& segment : segments)
        {
            const f32 r = std::max(segment.RadiusA, segment.RadiusB);
            lo = glm::min(lo, glm::min(segment.A, segment.B) - glm::vec3(r));
            hi = glm::max(hi, glm::max(segment.A, segment.B) + glm::vec3(r));
            any = true;
        }
        if (!any || !IsFiniteVec(lo) || !IsFiniteVec(hi))
        {
            return false;
        }

        outMin = lo;
        outMax = hi;
        return true;
    }

    // =========================================================================
    // Ground truth
    // =========================================================================

    f64 ReferenceCoatOpticalDepth(std::span<const CoatSegment> segments, const glm::vec3& origin,
                                  const glm::vec3& direction, const ReferenceSettings& settings)
    {
        if (segments.empty() || settings.Rays < 16)
        {
            return 0.0;
        }
        const f32 dirLen = glm::length(direction);
        if (!std::isfinite(dirLen) || dirLen <= 1.0e-8f || !IsFiniteVec(origin))
        {
            return 0.0;
        }
        const glm::vec3 dir = direction / dirLen;

        f32 tMax = settings.MaxDistance;
        if (!(tMax > 0.0f))
        {
            glm::vec3 lo;
            glm::vec3 hi;
            if (!CoatSegmentBounds(segments, lo, hi))
            {
                return 0.0;
            }
            // The diagonal bounds every path that stays inside the coat, so a
            // ray that leaves cannot come back and be missed.
            tMax = glm::length(hi - lo);
        }

        // The ray starts a hair off the probe point so the fibre the probe sits
        // ON does not count as an occluder of itself. Scaled to the footprint
        // rather than an absolute epsilon: an absolute one is either useless on
        // a centimetre coat or swallows the first millimetre of a metre one.
        const f32 tMin = std::max(settings.FootprintRadius * 0.25f, 1.0e-6f);

        glm::vec3 tangent;
        glm::vec3 bitangent;
        BuildBasis(dir, tangent, bitangent);

        u64 hits = 0;
        for (u32 ray = 0; ray < settings.Rays; ++ray)
        {
            // Concentric disc sampling of the footprint. sqrt on the radius, so
            // the samples are area-uniform; a linear radius would pile them at
            // the centre and measure the density of one strand's neighbourhood.
            const u32 base = HashU32(settings.Seed ^ (ray * 0x9E3779B9u));
            const f32 u0 = HashUnitFloat(base ^ 0x68BC21EBu);
            const f32 u1 = HashUnitFloat(base ^ 0x02E5BE93u);
            const f32 radius = settings.FootprintRadius * std::sqrt(u0);
            const f32 angle = static_cast<f32>(2.0 * kPi) * u1;
            const glm::vec3 offset = tangent * (radius * std::cos(angle)) + bitangent * (radius * std::sin(angle));
            const glm::vec3 rayOrigin = origin + offset;

            for (const CoatSegment& segment : segments)
            {
                if (RayHitsSegment(rayOrigin, dir, segment, tMin, tMax))
                {
                    ++hits;
                }
            }
        }

        return static_cast<f64>(hits) / static_cast<f64>(settings.Rays);
    }

    bool CoatSegmentGrid::IsValid() const noexcept
    {
        if (Dimensions.x <= 0 || Dimensions.y <= 0 || Dimensions.z <= 0)
        {
            return false;
        }
        const sizet cells = static_cast<sizet>(Dimensions.x) * static_cast<sizet>(Dimensions.y) *
                            static_cast<sizet>(Dimensions.z);
        return CellStart.size() == cells + 1;
    }

    u64 CoatSegmentGrid::CpuBytes() const noexcept
    {
        return static_cast<u64>(CellStart.size() + Indices.size()) * sizeof(u32);
    }

    bool BuildCoatSegmentGrid(std::span<const CoatSegment> segments, u32 cellsPerAxis, CoatSegmentGrid& outGrid)
    {
        outGrid = CoatSegmentGrid{};
        if (segments.empty() || cellsPerAxis < 1 || cellsPerAxis > 512)
        {
            return false;
        }

        glm::vec3 lo;
        glm::vec3 hi;
        if (!CoatSegmentBounds(segments, lo, hi))
        {
            return false;
        }

        const glm::vec3 extent = hi - lo;
        const f32 longest = std::max({ extent.x, extent.y, extent.z });
        if (!(longest > 0.0f))
        {
            return false;
        }
        const f32 cellSize = longest / static_cast<f32>(cellsPerAxis);
        glm::ivec3 dims;
        dims.x = std::clamp(static_cast<i32>(std::ceil(extent.x / cellSize)), 1, 512);
        dims.y = std::clamp(static_cast<i32>(std::ceil(extent.y / cellSize)), 1, 512);
        dims.z = std::clamp(static_cast<i32>(std::ceil(extent.z / cellSize)), 1, 512);

        outGrid.Dimensions = dims;
        outGrid.BoundsMin = lo;
        outGrid.BoundsMax = lo + glm::vec3(dims) * cellSize;

        const sizet cells = static_cast<sizet>(dims.x) * static_cast<sizet>(dims.y) * static_cast<sizet>(dims.z);
        const f32 invCell = 1.0f / cellSize;

        // The segment's bounding box, in cells. Conservative on purpose: a
        // diagonal segment is entered into every cell of its box, so the grid
        // over-reports candidates and never misses one. An exact voxelisation
        // would be smaller and would have to be right; this one only has to be
        // a superset, which is what makes the grid provably answer-preserving.
        const auto cellRange = [&](const CoatSegment& segment, glm::ivec3& outLo, glm::ivec3& outHi) {
            const f32 r = std::max(segment.RadiusA, segment.RadiusB);
            const glm::vec3 bLo = glm::min(segment.A, segment.B) - glm::vec3(r);
            const glm::vec3 bHi = glm::max(segment.A, segment.B) + glm::vec3(r);
            const glm::vec3 fLo = (bLo - outGrid.BoundsMin) * invCell;
            const glm::vec3 fHi = (bHi - outGrid.BoundsMin) * invCell;
            outLo = glm::clamp(glm::ivec3(glm::floor(fLo)), glm::ivec3(0), dims - 1);
            outHi = glm::clamp(glm::ivec3(glm::floor(fHi)), glm::ivec3(0), dims - 1);
        };

        // Counting pass, then a prefix sum, then a fill. Two passes rather than
        // a vector-of-vectors: the coat has hundreds of thousands of segments
        // and one allocation per cell is the part that would dominate.
        std::vector<u32> counts(cells, 0u);
        for (const CoatSegment& segment : segments)
        {
            glm::ivec3 cLo;
            glm::ivec3 cHi;
            cellRange(segment, cLo, cHi);
            for (i32 z = cLo.z; z <= cHi.z; ++z)
            {
                for (i32 y = cLo.y; y <= cHi.y; ++y)
                {
                    for (i32 x = cLo.x; x <= cHi.x; ++x)
                    {
                        counts[VoxelIndex(dims, x, y, z)] += 1u;
                    }
                }
            }
        }

        outGrid.CellStart.assign(cells + 1, 0u);
        u32 running = 0;
        for (sizet c = 0; c < cells; ++c)
        {
            outGrid.CellStart[c] = running;
            running += counts[c];
        }
        outGrid.CellStart[cells] = running;
        outGrid.Indices.assign(running, 0u);

        std::vector<u32> cursor(outGrid.CellStart.begin(), outGrid.CellStart.end() - 1);
        for (sizet i = 0; i < segments.size(); ++i)
        {
            glm::ivec3 cLo;
            glm::ivec3 cHi;
            cellRange(segments[i], cLo, cHi);
            for (i32 z = cLo.z; z <= cHi.z; ++z)
            {
                for (i32 y = cLo.y; y <= cHi.y; ++y)
                {
                    for (i32 x = cLo.x; x <= cHi.x; ++x)
                    {
                        outGrid.Indices[cursor[VoxelIndex(dims, x, y, z)]++] = static_cast<u32>(i);
                    }
                }
            }
        }

        return true;
    }

    f64 ReferenceCoatOpticalDepthGrid(std::span<const CoatSegment> segments, const CoatSegmentGrid& grid,
                                      const glm::vec3& origin, const glm::vec3& direction,
                                      const ReferenceSettings& settings)
    {
        if (segments.empty() || settings.Rays < 16 || !grid.IsValid())
        {
            return 0.0;
        }
        const f32 dirLen = glm::length(direction);
        if (!std::isfinite(dirLen) || dirLen <= 1.0e-8f || !IsFiniteVec(origin))
        {
            return 0.0;
        }
        const glm::vec3 dir = direction / dirLen;

        f32 tMax = settings.MaxDistance;
        if (!(tMax > 0.0f))
        {
            tMax = glm::length(grid.BoundsMax - grid.BoundsMin);
        }
        const f32 tMin = std::max(settings.FootprintRadius * 0.25f, 1.0e-6f);

        glm::vec3 tangent;
        glm::vec3 bitangent;
        BuildBasis(dir, tangent, bitangent);

        const glm::ivec3& dims = grid.Dimensions;
        const glm::vec3 cellSize = (grid.BoundsMax - grid.BoundsMin) / glm::vec3(dims);

        // A per-segment stamp, so a segment straddling several cells is tested
        // ONCE per ray. Without it the grid would count one fibre as many times
        // as its bounding box spans cells, which is the exact error the grid is
        // supposed not to introduce.
        std::vector<u32> stamp(segments.size(), 0u);

        u64 hits = 0;
        for (u32 ray = 0; ray < settings.Rays; ++ray)
        {
            const u32 base = HashU32(settings.Seed ^ (ray * 0x9E3779B9u));
            const f32 u0 = HashUnitFloat(base ^ 0x68BC21EBu);
            const f32 u1 = HashUnitFloat(base ^ 0x02E5BE93u);
            const f32 radius = settings.FootprintRadius * std::sqrt(u0);
            const f32 angle = static_cast<f32>(2.0 * kPi) * u1;
            const glm::vec3 offset = tangent * (radius * std::cos(angle)) + bitangent * (radius * std::sin(angle));
            const glm::vec3 rayOrigin = origin + offset;

            const u32 stampId = ray + 1u;

            f32 tEnter = 0.0f;
            f32 tExit = 0.0f;
            if (!IntersectBox(rayOrigin, dir, grid.BoundsMin, grid.BoundsMax, tEnter, tExit))
            {
                continue;
            }
            tExit = std::min(tExit, tMax);
            if (tExit <= tEnter)
            {
                continue;
            }

            // Amanatides-Woo DDA over the cells the ray actually enters.
            const glm::vec3 entry = rayOrigin + dir * tEnter;
            glm::ivec3 cell = glm::clamp(glm::ivec3(glm::floor((entry - grid.BoundsMin) / cellSize)), glm::ivec3(0),
                                         dims - 1);

            glm::ivec3 stepDir(0);
            glm::vec3 tMaxAxis(std::numeric_limits<f32>::max());
            glm::vec3 tDelta(std::numeric_limits<f32>::max());
            for (i32 axis = 0; axis < 3; ++axis)
            {
                if (std::abs(dir[axis]) < 1.0e-12f)
                {
                    continue;
                }
                stepDir[axis] = dir[axis] > 0.0f ? 1 : -1;
                const f32 boundary =
                    grid.BoundsMin[axis] + static_cast<f32>(cell[axis] + (stepDir[axis] > 0 ? 1 : 0)) * cellSize[axis];
                tMaxAxis[axis] = tEnter + (boundary - entry[axis]) / dir[axis];
                tDelta[axis] = std::abs(cellSize[axis] / dir[axis]);
            }

            f32 t = tEnter;
            // Bounded so a degenerate direction cannot spin forever; the bound
            // is the most cells a straight line can cross.
            const i32 maxSteps = (dims.x + dims.y + dims.z) * 2 + 8;
            for (i32 iteration = 0; iteration < maxSteps && t <= tExit; ++iteration)
            {
                const sizet cellIndex = VoxelIndex(dims, cell.x, cell.y, cell.z);
                const u32 begin = grid.CellStart[cellIndex];
                const u32 end = grid.CellStart[cellIndex + 1];
                for (u32 i = begin; i < end; ++i)
                {
                    const u32 index = grid.Indices[i];
                    if (stamp[index] == stampId)
                    {
                        continue;
                    }
                    stamp[index] = stampId;
                    if (RayHitsSegment(rayOrigin, dir, segments[index], tMin, tExit))
                    {
                        ++hits;
                    }
                }

                // Advance to the next cell along whichever axis boundary comes
                // first.
                i32 axis = 0;
                if (tMaxAxis.y < tMaxAxis.x)
                {
                    axis = 1;
                }
                if (tMaxAxis.z < tMaxAxis[axis])
                {
                    axis = 2;
                }
                if (stepDir[axis] == 0)
                {
                    break;
                }
                t = tMaxAxis[axis];
                cell[axis] += stepDir[axis];
                if (cell[axis] < 0 || cell[axis] >= dims[axis])
                {
                    break;
                }
                tMaxAxis[axis] += tDelta[axis];
            }
        }

        return static_cast<f64>(hits) / static_cast<f64>(settings.Rays);
    }

    f32 CoatTransmittance(f64 opticalDepth, f32 kappa) noexcept
    {
        if (!std::isfinite(opticalDepth) || opticalDepth <= 0.0)
        {
            // A non-finite tau is a build that went wrong. FULLY LIT is the
            // right answer for it, not fully shadowed: the failure mode of a
            // missing occlusion term must be a bright coat, which is visibly
            // "the feature did not run", rather than a black one, which is
            // indistinguishable from a correct silhouette.
            return 1.0f;
        }
        if (!std::isfinite(kappa) || kappa <= 0.0f)
        {
            return 1.0f;
        }
        const f64 t = std::exp(-static_cast<f64>(kappa) * opticalDepth);
        if (!std::isfinite(t))
        {
            return 0.0f;
        }
        return static_cast<f32>(std::clamp(t, 0.0, 1.0));
    }

    // =========================================================================
    // Density volume
    // =========================================================================

    bool DensityVolume::IsValid() const noexcept
    {
        if (Dimensions.x <= 0 || Dimensions.y <= 0 || Dimensions.z <= 0)
        {
            return false;
        }
        const sizet expected = static_cast<sizet>(Dimensions.x) * static_cast<sizet>(Dimensions.y) *
                               static_cast<sizet>(Dimensions.z);
        if (Density.size() != expected || Direction.size() != expected)
        {
            return false;
        }
        const glm::vec3 extent = BoundsMax - BoundsMin;
        return extent.x > 0.0f && extent.y > 0.0f && extent.z > 0.0f;
    }

    u64 DensityVolume::GpuBytes() const noexcept
    {
        if (Dimensions.x <= 0 || Dimensions.y <= 0 || Dimensions.z <= 0)
        {
            return 0;
        }
        const u64 voxels = static_cast<u64>(Dimensions.x) * static_cast<u64>(Dimensions.y) *
                           static_cast<u64>(Dimensions.z);
        // ONE RGBA32F, 16 bytes a voxel: what the renderer actually uploads.
        //
        // It is not what the packing would prefer -- RGBA16F would halve it --
        // but Texture3D's RGBA16F declares 8 bytes a texel while uploading its
        // client data as GL_FLOAT, so SetData rejects the only buffer it could
        // be handed. Quoting the smaller number here would make every memory
        // row in the analysis describe a texture the engine cannot currently
        // create.
        //
        // BOTH volume modes therefore cost the same bytes, and the isotropic
        // arm is a COMPUTE saving rather than a memory one. The memory lever at
        // runtime is the shadow LOD, which is cubic in the resolution.
        return voxels * 16ull;
    }

    glm::vec3 DensityVolume::VoxelSize() const noexcept
    {
        if (Dimensions.x <= 0 || Dimensions.y <= 0 || Dimensions.z <= 0)
        {
            return glm::vec3(0.0f);
        }
        return (BoundsMax - BoundsMin) / glm::vec3(Dimensions);
    }

    bool BuildDensityVolume(std::span<const CoatSegment> segments, const DensityVolumeSettings& settings,
                            DensityVolume& outVolume, DensityVolumeBuildStats* outStats)
    {
        outVolume = DensityVolume{};
        if (outStats != nullptr)
        {
            *outStats = DensityVolumeBuildStats{};
        }

        if (segments.empty() || settings.Resolution < 2 || settings.Resolution > 1024)
        {
            return false;
        }
        if (!std::isfinite(settings.BoundsPadding) || settings.BoundsPadding < 0.0f || settings.BoundsPadding > 1.0f)
        {
            return false;
        }

        glm::vec3 lo;
        glm::vec3 hi;
        if (!CoatSegmentBounds(segments, lo, hi))
        {
            return false;
        }

        glm::vec3 extent = hi - lo;
        const f32 longest = std::max({ extent.x, extent.y, extent.z });
        if (!(longest > 0.0f))
        {
            return false;
        }
        const f32 pad = longest * settings.BoundsPadding;
        lo -= glm::vec3(pad);
        hi += glm::vec3(pad);
        extent = hi - lo;

        // CUBIC voxels: the longest axis gets `Resolution`, the others get
        // whatever keeps the voxel a cube. A march step is then one length in
        // every direction, so the optical depth does not depend on which way
        // the light happens to point — which it would, invisibly, with
        // stretched voxels.
        const f32 voxelSize = std::max({ extent.x, extent.y, extent.z }) / static_cast<f32>(settings.Resolution);
        if (!(voxelSize > 0.0f))
        {
            return false;
        }
        glm::ivec3 dims;
        dims.x = std::max(1, static_cast<i32>(std::ceil(extent.x / voxelSize)));
        dims.y = std::max(1, static_cast<i32>(std::ceil(extent.y / voxelSize)));
        dims.z = std::max(1, static_cast<i32>(std::ceil(extent.z / voxelSize)));

        // The box is grown to the voxel grid rather than the grid squeezed
        // into the box, so the voxels stay cubes.
        hi = lo + glm::vec3(dims) * voxelSize;

        const sizet voxelCount = static_cast<sizet>(dims.x) * static_cast<sizet>(dims.y) * static_cast<sizet>(dims.z);
        outVolume.Dimensions = dims;
        outVolume.BoundsMin = lo;
        outVolume.BoundsMax = hi;
        outVolume.Density.assign(voxelCount, 0.0f);
        outVolume.Direction.assign(voxelCount, glm::vec3(0.0f));

        const f32 invVoxel = 1.0f / voxelSize;
        const f64 voxelVolume = static_cast<f64>(voxelSize) * static_cast<f64>(voxelSize) *
                                static_cast<f64>(voxelSize);

        u32 binned = 0;
        u32 rejected = 0;
        f64 totalMass = 0.0;

        for (const CoatSegment& segment : segments)
        {
            const glm::vec3 ab = segment.B - segment.A;
            const f32 length = glm::length(ab);
            if (!std::isfinite(length) || length <= 0.0f)
            {
                ++rejected;
                continue;
            }
            const glm::vec3 dir = ab / length;

            // Sub-voxel stepping so a segment crossing a voxel corner deposits
            // into both, and a segment shorter than a voxel still deposits its
            // whole length. Half a voxel is the coarsest step that cannot skip
            // a voxel entirely.
            const f32 step = voxelSize * 0.5f;
            const i32 steps = std::max(1, static_cast<i32>(std::ceil(length / step)));
            const f32 dl = length / static_cast<f32>(steps);

            for (i32 i = 0; i < steps; ++i)
            {
                // Sampled at the MIDPOINT of each sub-step, so the deposited
                // mass is the segment's own length and not a half-step longer
                // or shorter at the ends.
                const f32 s = (static_cast<f32>(i) + 0.5f) / static_cast<f32>(steps);
                const glm::vec3 p = segment.A + ab * s;
                const f32 radius = segment.RadiusA + (segment.RadiusB - segment.RadiusA) * s;
                const f32 diameter = radius * 2.0f;
                if (!(diameter > 0.0f))
                {
                    continue;
                }

                const glm::vec3 local = (p - lo) * invVoxel;
                const i32 vx = static_cast<i32>(std::floor(local.x));
                const i32 vy = static_cast<i32>(std::floor(local.y));
                const i32 vz = static_cast<i32>(std::floor(local.z));
                if (vx < 0 || vy < 0 || vz < 0 || vx >= dims.x || vy >= dims.y || vz >= dims.z)
                {
                    continue;
                }

                // NEAREST-voxel deposit, not a trilinear splat. Nearest
                // conserves the fibre area EXACTLY — the sum over voxels is the
                // sum over segments, which is what TotalArealMass asserts — and
                // a splat that leaked mass out of the boundary voxels would
                // lose shadow without saying so.
                const sizet index = VoxelIndex(dims, vx, vy, vz);
                const f32 mass = dl * diameter;
                outVolume.Density[index] += mass;
                outVolume.Direction[index] += dir * mass;
                totalMass += static_cast<f64>(mass);
            }

            ++binned;
        }

        // Mass -> areal density (1/length), and the accumulated direction ->
        // a mean direction whose LENGTH is the coherence in [0,1].
        u32 occupied = 0;
        for (sizet i = 0; i < voxelCount; ++i)
        {
            const f32 mass = outVolume.Density[i];
            if (mass > 0.0f)
            {
                ++occupied;
                outVolume.Direction[i] /= mass;
                outVolume.Density[i] = static_cast<f32>(static_cast<f64>(mass) / voxelVolume);
            }
            else
            {
                outVolume.Direction[i] = glm::vec3(0.0f);
                outVolume.Density[i] = 0.0f;
            }
        }

        if (outStats != nullptr)
        {
            outStats->SegmentsBinned = binned;
            outStats->SegmentsRejected = rejected;
            outStats->OccupiedVoxels = occupied;
            outStats->TotalVoxels = static_cast<u32>(std::min<sizet>(voxelCount, 0xFFFFFFFFull));
            outStats->TotalArealMass = totalMass;
        }

        return binned > 0;
    }

    namespace
    {
        // Trilinear fetch of the two channels at one point, in volume-local
        // voxel coordinates. Out of range reads zero density, which is "no coat
        // here" — the correct answer outside the bounds, and the one that keeps
        // a march leaving the box from picking up the boundary voxel's value
        // forever.
        void SampleVolumeAt(const DensityVolume& volume, const glm::vec3& local, f32& outDensity,
                            glm::vec3& outDirection) noexcept
        {
            outDensity = 0.0f;
            outDirection = glm::vec3(0.0f);

            const glm::ivec3& dims = volume.Dimensions;
            const glm::vec3 p = local - glm::vec3(0.5f);
            const glm::vec3 base = glm::floor(p);
            const glm::vec3 frac = p - base;

            for (i32 dz = 0; dz < 2; ++dz)
            {
                for (i32 dy = 0; dy < 2; ++dy)
                {
                    for (i32 dx = 0; dx < 2; ++dx)
                    {
                        const i32 x = static_cast<i32>(base.x) + dx;
                        const i32 y = static_cast<i32>(base.y) + dy;
                        const i32 z = static_cast<i32>(base.z) + dz;
                        if (x < 0 || y < 0 || z < 0 || x >= dims.x || y >= dims.y || z >= dims.z)
                        {
                            continue;
                        }
                        const f32 wx = dx == 0 ? 1.0f - frac.x : frac.x;
                        const f32 wy = dy == 0 ? 1.0f - frac.y : frac.y;
                        const f32 wz = dz == 0 ? 1.0f - frac.z : frac.z;
                        const f32 w = wx * wy * wz;
                        if (w <= 0.0f)
                        {
                            continue;
                        }
                        const sizet index = VoxelIndex(dims, x, y, z);
                        outDensity += volume.Density[index] * w;
                        outDirection += volume.Direction[index] * w;
                    }
                }
            }
        }

    } // namespace

    f64 SampleDensityVolume(const DensityVolume& volume, const glm::vec3& origin, const glm::vec3& direction,
                            bool anisotropic, f32 stepScale, u32* outSteps)
    {
        if (outSteps != nullptr)
        {
            *outSteps = 0;
        }
        if (!volume.IsValid())
        {
            return 0.0;
        }
        const f32 dirLen = glm::length(direction);
        if (!std::isfinite(dirLen) || dirLen <= 1.0e-8f || !IsFiniteVec(origin))
        {
            return 0.0;
        }
        const glm::vec3 dir = direction / dirLen;
        if (!std::isfinite(stepScale) || stepScale <= 0.0f)
        {
            return 0.0;
        }

        f32 tEnter = 0.0f;
        f32 tExit = 0.0f;
        if (!IntersectBox(origin, dir, volume.BoundsMin, volume.BoundsMax, tEnter, tExit))
        {
            return 0.0;
        }
        if (tExit <= tEnter)
        {
            return 0.0;
        }

        const glm::vec3 voxel = volume.VoxelSize();
        const f32 voxelLength = std::min({ voxel.x, voxel.y, voxel.z });
        if (!(voxelLength > 0.0f))
        {
            return 0.0;
        }
        const f32 step = voxelLength * stepScale;
        const f32 span = tExit - tEnter;
        // Bounded so a degenerate step cannot spin: the cap is the number of
        // steps a whole-diagonal march at one voxel per step would take, times
        // a small factor for sub-voxel steps.
        const i32 steps = std::clamp(static_cast<i32>(std::ceil(span / step)), 1, 8192);
        const f32 dt = span / static_cast<f32>(steps);

        const glm::vec3 invVoxel = 1.0f / voxel;

        f64 tau = 0.0;
        for (i32 i = 0; i < steps; ++i)
        {
            const f32 t = tEnter + dt * (static_cast<f32>(i) + 0.5f);
            const glm::vec3 p = origin + dir * t;
            const glm::vec3 local = (p - volume.BoundsMin) * invVoxel;

            f32 density = 0.0f;
            glm::vec3 meanDirection(0.0f);
            SampleVolumeAt(volume, local, density, meanDirection);
            if (density <= 0.0f)
            {
                continue;
            }

            // The projected cross-section a voxel presents to this ray. The
            // isotropic arm takes the sphere average of sin(theta); the
            // anisotropic arm blends towards the voxel's own mean direction by
            // how coherent the fibres in it are. Blending rather than switching
            // is what keeps a voxel holding two crossing strands from claiming
            // a direction it does not have.
            f64 meanSine = kIsotropicMeanSine;
            if (anisotropic)
            {
                const f32 coherence = std::clamp(glm::length(meanDirection), 0.0f, 1.0f);
                if (coherence > 1.0e-4f)
                {
                    const glm::vec3 axis = meanDirection / coherence;
                    const f32 cosTheta = std::clamp(glm::dot(axis, dir), -1.0f, 1.0f);
                    const f64 sinTheta = std::sqrt(std::max(0.0, 1.0 - static_cast<f64>(cosTheta) * cosTheta));
                    meanSine = static_cast<f64>(coherence) * sinTheta +
                               (1.0 - static_cast<f64>(coherence)) * kIsotropicMeanSine;
                }
            }

            tau += static_cast<f64>(density) * meanSine * static_cast<f64>(dt);
        }

        if (outSteps != nullptr)
        {
            *outSteps = static_cast<u32>(steps);
        }
        return std::isfinite(tau) ? tau : 0.0;
    }

    // =========================================================================
    // Deep opacity map
    // =========================================================================

    bool DeepOpacityMap::IsValid() const noexcept
    {
        if (Width == 0 || Height == 0 || Layers == 0)
        {
            return false;
        }
        const sizet texels = static_cast<sizet>(Width) * static_cast<sizet>(Height);
        return Front.size() == texels && Accumulated.size() == texels * static_cast<sizet>(Layers) &&
               LayerOffsets.size() == static_cast<sizet>(Layers);
    }

    u64 DeepOpacityMap::GpuBytes() const noexcept
    {
        const u64 texels = static_cast<u64>(Width) * static_cast<u64>(Height);
        // R32F front depth (a depth needs the precision) + an R16F array of
        // `Layers` slices for the accumulated crossings (a transmittance does
        // not).
        return texels * (4ull + 2ull * static_cast<u64>(Layers));
    }

    bool BuildDeepOpacityMap(std::span<const CoatSegment> segments, const glm::vec3& lightDirection,
                             const DeepOpacityMapSettings& settings, DeepOpacityMap& outMap,
                             DeepOpacityMapBuildStats* outStats)
    {
        outMap = DeepOpacityMap{};
        if (outStats != nullptr)
        {
            *outStats = DeepOpacityMapBuildStats{};
        }

        if (segments.empty() || settings.Width < 4 || settings.Height < 4 || settings.Layers < 1 ||
            settings.Layers > 16)
        {
            return false;
        }
        if (!std::isfinite(settings.LayerSpan) || settings.LayerSpan <= 0.0f)
        {
            return false;
        }

        const f32 lightLen = glm::length(lightDirection);
        if (!std::isfinite(lightLen) || lightLen <= 1.0e-8f)
        {
            return false;
        }
        // `lightDirection` is the direction the light TRAVELS — the convention
        // LightData::direction uses and every lit shader in the engine reads.
        // So the map looks ALONG it, and depth grows away from the light.
        const glm::vec3 forward = lightDirection / lightLen;
        glm::vec3 right;
        glm::vec3 up;
        BuildBasis(forward, right, up);

        // Fit the light-space box to the coat.
        f32 minU = std::numeric_limits<f32>::max();
        f32 maxU = std::numeric_limits<f32>::lowest();
        f32 minV = std::numeric_limits<f32>::max();
        f32 maxV = std::numeric_limits<f32>::lowest();
        f32 minW = std::numeric_limits<f32>::max();
        f32 maxW = std::numeric_limits<f32>::lowest();
        for (const CoatSegment& segment : segments)
        {
            const f32 r = std::max(segment.RadiusA, segment.RadiusB);
            for (const glm::vec3& p : { segment.A, segment.B })
            {
                const f32 u = glm::dot(p, right);
                const f32 v = glm::dot(p, up);
                const f32 w = glm::dot(p, forward);
                minU = std::min(minU, u - r);
                maxU = std::max(maxU, u + r);
                minV = std::min(minV, v - r);
                maxV = std::max(maxV, v + r);
                minW = std::min(minW, w - r);
                maxW = std::max(maxW, w + r);
            }
        }
        if (!(maxU > minU) || !(maxV > minV) || !(maxW > minW))
        {
            return false;
        }

        const f32 spanU = maxU - minU;
        const f32 spanV = maxV - minV;
        const f32 spanW = maxW - minW;

        outMap.Width = settings.Width;
        outMap.Height = settings.Height;
        outMap.Layers = settings.Layers;

        // The light transform, assembled as the rows that produce (u, v, w) in
        // [0,1]. Stored so the sampler and the builder cannot disagree about
        // the projection — the one place a deep map goes wrong in a way that
        // still looks like a shadow.
        glm::mat4 lightVP(0.0f);
        lightVP[0] = glm::vec4(right.x / spanU, up.x / spanV, forward.x / spanW, 0.0f);
        lightVP[1] = glm::vec4(right.y / spanU, up.y / spanV, forward.y / spanW, 0.0f);
        lightVP[2] = glm::vec4(right.z / spanU, up.z / spanV, forward.z / spanW, 0.0f);
        lightVP[3] = glm::vec4(-minU / spanU, -minV / spanV, -minW / spanW, 1.0f);
        outMap.LightViewProjection = lightVP;

        const sizet texels = static_cast<sizet>(settings.Width) * static_cast<sizet>(settings.Height);
        outMap.Front.assign(texels, std::numeric_limits<f32>::infinity());
        outMap.Accumulated.assign(texels * static_cast<sizet>(settings.Layers), 0.0f);

        // Layer offsets, in NORMALISED light depth, thin at the front. The
        // coat's visible structure is in the first few millimetres under the
        // surface — that is where the guard hairs stop and the undercoat
        // starts — so a uniform distribution spends most of its layers on the
        // interior, where everything is already dark.
        outMap.LayerOffsets.resize(settings.Layers);
        for (u32 k = 0; k < settings.Layers; ++k)
        {
            const f32 x = static_cast<f32>(k + 1) / static_cast<f32>(settings.Layers);
            outMap.LayerOffsets[k] = settings.LayerSpan * x * x;
        }

        const f32 texelWorldU = spanU / static_cast<f32>(settings.Width);
        const f32 texelWorldV = spanV / static_cast<f32>(settings.Height);
        const f64 texelArea = static_cast<f64>(texelWorldU) * static_cast<f64>(texelWorldV);
        if (!(texelArea > 0.0))
        {
            return false;
        }

        // Pass 1 collects the front depth and pass 2 accumulates, because a
        // layer's depth is defined RELATIVE to the front and a single pass
        // would have to accumulate before it knew where the layers were. That
        // relative start is the whole difference between deep opacity maps and
        // the flat opacity shadow maps that preceded them: flat layers slice
        // a curved coat and the slice edges read as bands.
        struct Sample
        {
            u32 Texel;
            f32 Depth;
            f32 Crossings;
        };
        std::vector<Sample> samples;

        u32 rasterised = 0;
        u32 rejected = 0;

        for (const CoatSegment& segment : segments)
        {
            const glm::vec3 ab = segment.B - segment.A;
            const f32 length = glm::length(ab);
            if (!std::isfinite(length) || length <= 0.0f)
            {
                ++rejected;
                continue;
            }
            const glm::vec3 dir = ab / length;
            // The projected width a piece of this fibre presents to the light.
            const f32 sinTheta = std::sqrt(std::max(0.0f, 1.0f - glm::dot(dir, forward) * glm::dot(dir, forward)));

            const f32 step = std::min(texelWorldU, texelWorldV) * 0.5f;
            const i32 steps = std::clamp(static_cast<i32>(std::ceil(length / std::max(step, 1.0e-8f))), 1, 4096);
            const f32 dl = length / static_cast<f32>(steps);

            for (i32 i = 0; i < steps; ++i)
            {
                const f32 s = (static_cast<f32>(i) + 0.5f) / static_cast<f32>(steps);
                const glm::vec3 p = segment.A + ab * s;
                const f32 radius = segment.RadiusA + (segment.RadiusB - segment.RadiusA) * s;
                const f32 diameter = radius * 2.0f;
                if (!(diameter > 0.0f))
                {
                    continue;
                }

                const f32 u = (glm::dot(p, right) - minU) / spanU;
                const f32 v = (glm::dot(p, up) - minV) / spanV;
                const f32 w = (glm::dot(p, forward) - minW) / spanW;
                if (!std::isfinite(u) || !std::isfinite(v) || !std::isfinite(w))
                {
                    continue;
                }
                const i32 tx = static_cast<i32>(u * static_cast<f32>(settings.Width));
                const i32 ty = static_cast<i32>(v * static_cast<f32>(settings.Height));
                if (tx < 0 || ty < 0 || tx >= static_cast<i32>(settings.Width) ||
                    ty >= static_cast<i32>(settings.Height))
                {
                    continue;
                }

                const u32 texel = static_cast<u32>(ty) * settings.Width + static_cast<u32>(tx);
                outMap.Front[texel] = std::min(outMap.Front[texel], w);

                // The fraction of this texel the piece occludes IS its expected
                // crossing contribution: projected area over texel area. Same
                // quantity the reference counts, arrived at by area rather than
                // by intersection, which is why the two are comparable at all.
                const f64 projected = static_cast<f64>(dl) * static_cast<f64>(diameter) * static_cast<f64>(sinTheta);
                samples.push_back(
                    Sample{ texel, w, static_cast<f32>(projected / texelArea) });
            }

            ++rasterised;
        }

        for (const Sample& sample : samples)
        {
            const f32 front = outMap.Front[sample.Texel];
            if (!std::isfinite(front))
            {
                continue;
            }
            const f32 relative = sample.Depth - front;
            f32* column = &outMap.Accumulated[static_cast<sizet>(sample.Texel) * settings.Layers];
            for (u32 k = 0; k < settings.Layers; ++k)
            {
                // A layer holds everything IN FRONT OF IT, so a piece adds to
                // every layer deeper than itself. Accumulated, not per-slab:
                // the lookup is then one interpolation rather than a prefix sum
                // in the shader.
                if (relative <= outMap.LayerOffsets[k])
                {
                    column[k] += sample.Crossings;
                }
            }
        }

        u32 covered = 0;
        for (const f32 front : outMap.Front)
        {
            if (std::isfinite(front))
            {
                ++covered;
            }
        }

        if (outStats != nullptr)
        {
            outStats->SegmentsRasterised = rasterised;
            outStats->SegmentsRejected = rejected;
            outStats->CoveredTexels = covered;
            outStats->TotalTexels = static_cast<u32>(std::min<sizet>(texels, 0xFFFFFFFFull));
        }

        return rasterised > 0;
    }

    f64 SampleDeepOpacityMap(const DeepOpacityMap& map, const glm::vec3& position)
    {
        if (!map.IsValid() || !IsFiniteVec(position))
        {
            return 0.0;
        }

        const glm::vec4 projected = map.LightViewProjection * glm::vec4(position, 1.0f);
        const f32 u = projected.x;
        const f32 v = projected.y;
        const f32 w = projected.z;
        if (!std::isfinite(u) || !std::isfinite(v) || !std::isfinite(w))
        {
            return 0.0;
        }
        if (u < 0.0f || u >= 1.0f || v < 0.0f || v >= 1.0f)
        {
            // OUTSIDE THE MAP IS UNSHADOWED, never fully shadowed. A point the
            // representation has no opinion about must come back lit, so a
            // footprint that is too small shows as a coat with no depth rather
            // than as a black rectangle — the same rule as clearing a
            // visibility mask to white.
            return 0.0;
        }

        const u32 tx = std::min(static_cast<u32>(u * static_cast<f32>(map.Width)), map.Width - 1);
        const u32 ty = std::min(static_cast<u32>(v * static_cast<f32>(map.Height)), map.Height - 1);
        const u32 texel = ty * map.Width + tx;

        const f32 front = map.Front[texel];
        if (!std::isfinite(front))
        {
            return 0.0;
        }
        const f32 relative = w - front;
        if (relative <= 0.0f)
        {
            // In front of the coat: nothing between this point and the light.
            return 0.0;
        }

        const f32* column = &map.Accumulated[static_cast<sizet>(texel) * map.Layers];

        // Below the first layer the accumulation ramps from zero, so a point
        // just under the surface is not handed the whole first layer's
        // occlusion — which is what would make a coat's outermost hairs read as
        // already deep inside it.
        if (relative <= map.LayerOffsets[0])
        {
            const f32 t = map.LayerOffsets[0] > 0.0f ? relative / map.LayerOffsets[0] : 1.0f;
            return static_cast<f64>(column[0]) * static_cast<f64>(t);
        }

        for (u32 k = 1; k < map.Layers; ++k)
        {
            if (relative <= map.LayerOffsets[k])
            {
                const f32 lo = map.LayerOffsets[k - 1];
                const f32 hi = map.LayerOffsets[k];
                const f32 t = hi > lo ? (relative - lo) / (hi - lo) : 1.0f;
                return static_cast<f64>(column[k - 1]) +
                       static_cast<f64>(column[k] - column[k - 1]) * static_cast<f64>(t);
            }
        }

        // Past the last layer: everything the column ever accumulated. Clamped
        // rather than extrapolated, because extrapolating an accumulation makes
        // a deep point arbitrarily dark for no reason the geometry supplies.
        return static_cast<f64>(column[map.Layers - 1]);
    }

    // =========================================================================
    // Shadow LOD
    // =========================================================================

    u32 SelectCoatLodStep(const CoatLodPolicy& policy, f32 pixelSize) noexcept
    {
        if (!std::isfinite(pixelSize) || pixelSize <= 0.0f)
        {
            return policy.MaxLodSteps;
        }
        if (!std::isfinite(policy.PixelSizeForLod0) || policy.PixelSizeForLod0 <= 0.0f)
        {
            return 0;
        }
        if (pixelSize >= policy.PixelSizeForLod0)
        {
            return 0;
        }

        u32 step = 0;
        f32 threshold = policy.PixelSizeForLod0;
        while (step < policy.MaxLodSteps)
        {
            threshold *= 0.5f;
            ++step;
            if (pixelSize >= threshold)
            {
                break;
            }
        }
        return step;
    }

    u32 CoatLodResolution(const CoatLodPolicy& policy, u32 lodStep) noexcept
    {
        const u32 clampedStep = std::min(lodStep, policy.MaxLodSteps);
        u32 resolution = policy.BaseResolution;
        for (u32 i = 0; i < clampedStep; ++i)
        {
            resolution /= 2;
        }
        return std::max(resolution, policy.MinResolution);
    }

    u32 ApplyCoatLodHysteresis(u32 current, u32 requested, u32 framesStable, u32 threshold) noexcept
    {
        if (requested == current)
        {
            return current;
        }
        if (requested < current)
        {
            // Refining is immediate. A coat that just got bigger and is still
            // at the coarse representation is visibly wrong; one that stays
            // fine a few frames too long is merely expensive, and only one of
            // those two is a picture the user can see.
            return requested;
        }
        return framesStable >= threshold ? requested : current;
    }

    // =========================================================================
    // The comparison
    // =========================================================================

    CoatShadowError CompareCoatShadow(std::span<const f32> candidate, std::span<const f32> reference, f64 meanSamples)
    {
        CoatShadowError error;
        if (candidate.empty() || candidate.size() != reference.size())
        {
            return error;
        }

        f64 sumAbs = 0.0;
        f64 sumSq = 0.0;
        f64 sumSigned = 0.0;
        f64 maxAbs = 0.0;
        u32 counted = 0;

        for (sizet i = 0; i < candidate.size(); ++i)
        {
            const f64 c = static_cast<f64>(candidate[i]);
            const f64 r = static_cast<f64>(reference[i]);
            if (!std::isfinite(c) || !std::isfinite(r))
            {
                continue;
            }
            const f64 d = c - r;
            sumAbs += std::abs(d);
            sumSq += d * d;
            sumSigned += d;
            maxAbs = std::max(maxAbs, std::abs(d));
            ++counted;
        }

        if (counted == 0)
        {
            return error;
        }

        const f64 n = static_cast<f64>(counted);
        error.MeanAbs = sumAbs / n;
        error.Rmse = std::sqrt(sumSq / n);
        error.MaxAbs = maxAbs;
        error.Bias = sumSigned / n;
        error.Probes = counted;
        error.MeanSamples = meanSamples;
        return error;
    }

    u32 BuildCoatProbes(std::span<const CoatSegment> segments, const glm::vec3& lightDirection, u32 maxProbes,
                        u32 seed, std::vector<CoatProbe>& outProbes)
    {
        outProbes.clear();
        if (segments.empty() || maxProbes == 0)
        {
            return 0;
        }
        const f32 lightLen = glm::length(lightDirection);
        if (!std::isfinite(lightLen) || lightLen <= 1.0e-8f)
        {
            return 0;
        }
        // Towards the light: the direction a shadow ray travels, which is the
        // negation of the direction the light travels.
        const glm::vec3 toLight = -lightDirection / lightLen;

        // A STRIDE over the segments, for the same reason BuildCoatSegments
        // strides over curves: the cook makes groups contiguous, so a prefix of
        // the segment list is one region of the coat and would measure the
        // density of a patch rather than of the animal.
        const sizet count = segments.size();
        const sizet stride = count > maxProbes ? (count + maxProbes - 1) / maxProbes : 1;

        outProbes.reserve(std::min<sizet>(count, maxProbes));
        for (sizet i = 0; i < count; i += stride)
        {
            const CoatSegment& segment = segments[i];
            // A hashed point ALONG the segment rather than its midpoint, so the
            // probe set samples roots and tips as well as middles. A midpoint
            // set would sit on one iso-surface through the coat and miss the
            // depth gradient, which is the thing being measured.
            const f32 s = HashUnitFloat(static_cast<u32>(i) ^ HashU32(seed));
            const glm::vec3 p = segment.A + (segment.B - segment.A) * s;
            if (!IsFiniteVec(p))
            {
                continue;
            }
            outProbes.push_back(CoatProbe{ p, toLight });
            if (outProbes.size() >= maxProbes)
            {
                break;
            }
        }

        return static_cast<u32>(outProbes.size());
    }

    std::vector<f32> EvaluateCoatShadowMode(CoatShadowMode mode, const DensityVolume& volume, const DeepOpacityMap& map,
                                            std::span<const CoatProbe> probes, f32 kappa, f64* outMeanSamples)
    {
        std::vector<f32> result;
        if (outMeanSamples != nullptr)
        {
            *outMeanSamples = 0.0;
        }
        if (probes.empty())
        {
            return result;
        }

        switch (mode)
        {
            case CoatShadowMode::None:
                // The control. Fully lit everywhere, which is #1247's state and
                // the number every other row is quoted against.
                result.assign(probes.size(), 1.0f);
                return result;

            case CoatShadowMode::IsotropicDensityVolume:
            case CoatShadowMode::AnisotropicDensityVolume:
            {
                if (!volume.IsValid())
                {
                    return result;
                }
                const bool anisotropic = mode == CoatShadowMode::AnisotropicDensityVolume;
                result.reserve(probes.size());
                f64 totalSteps = 0.0;
                for (const CoatProbe& probe : probes)
                {
                    u32 steps = 0;
                    const f64 tau =
                        SampleDensityVolume(volume, probe.Position, probe.Direction, anisotropic, 1.0f, &steps);
                    totalSteps += static_cast<f64>(steps);
                    result.push_back(CoatTransmittance(tau, kappa));
                }
                if (outMeanSamples != nullptr)
                {
                    *outMeanSamples = totalSteps / static_cast<f64>(probes.size());
                }
                return result;
            }

            case CoatShadowMode::DeepOpacityMap:
            {
                if (!map.IsValid())
                {
                    return result;
                }
                result.reserve(probes.size());
                for (const CoatProbe& probe : probes)
                {
                    const f64 tau = SampleDeepOpacityMap(map, probe.Position);
                    result.push_back(CoatTransmittance(tau, kappa));
                }
                if (outMeanSamples != nullptr)
                {
                    // One front fetch plus the two layers the interpolation
                    // brackets. A count, not a time — the analysis document
                    // carries the clock.
                    *outMeanSamples = 3.0;
                }
                return result;
            }

            case CoatShadowMode::Count:
                break;
        }

        return result;
    }

    std::vector<f32> EvaluateCoatShadowReference(std::span<const CoatSegment> segments,
                                                 std::span<const CoatProbe> probes, f32 kappa,
                                                 const ReferenceSettings& settings, const CoatSegmentGrid* grid)
    {
        std::vector<f32> result;
        if (probes.empty() || segments.empty())
        {
            return result;
        }

        const bool useGrid = grid != nullptr && grid->IsValid();

        result.reserve(probes.size());
        for (const CoatProbe& probe : probes)
        {
            const f64 tau = useGrid ? ReferenceCoatOpticalDepthGrid(segments, *grid, probe.Position, probe.Direction,
                                                                    settings)
                                    : ReferenceCoatOpticalDepth(segments, probe.Position, probe.Direction, settings);
            result.push_back(CoatTransmittance(tau, kappa));
        }
        return result;
    }
} // namespace OloEngine::GroomCoatShadow
