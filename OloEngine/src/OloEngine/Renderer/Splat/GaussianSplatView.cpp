#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Splat/GaussianSplatView.h"

#include "OloEngine/Debug/Instrumentor.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <vector>

namespace OloEngine::GaussianSplat
{
    namespace
    {
        // One plane of the frustum, in world space, as ax + by + cz + d = 0
        // with the normal pointing INTO the frustum.
        struct Plane
        {
            glm::vec3 Normal{ 0.0f };
            f32 Distance = 0.0f;
        };

        // Gribb/Hartmann extraction from the combined matrix, normalised so
        // `Distance` below is a true world-space distance and can be compared
        // against a splat radius.
        [[nodiscard]] std::array<Plane, 6> ExtractFrustumPlanes(const glm::mat4& viewProjection)
        {
            const glm::mat4& m = viewProjection;
            std::array<Plane, 6> planes{};
            const auto row = [&m](int i)
            { return glm::vec4(m[0][i], m[1][i], m[2][i], m[3][i]); };
            const glm::vec4 r3 = row(3);
            const std::array<glm::vec4, 6> raw{ r3 + row(0), r3 - row(0), r3 + row(1),
                                                r3 - row(1), r3 + row(2), r3 - row(2) };
            for (sizet i = 0; i < raw.size(); ++i)
            {
                const glm::vec3 n(raw[i]);
                const f32 len = glm::length(n);
                if (len > 1e-8f)
                {
                    planes[i].Normal = n / len;
                    planes[i].Distance = raw[i].w / len;
                }
                else
                {
                    // A degenerate plane must not cull anything, so it is set
                    // to one that every point is inside of.
                    planes[i].Normal = glm::vec3(0.0f, 0.0f, 1.0f);
                    planes[i].Distance = std::numeric_limits<f32>::max();
                }
            }
            return planes;
        }
    } // namespace

    auto DepthSortKey(f32 viewDepth) -> u32
    {
        // Only reached for strictly positive depths (the near-plane test runs
        // first), where the IEEE-754 bit pattern is monotonically increasing in
        // the value. A negative or NaN depth would break that, so it is mapped
        // to 0 -- the far end of the descending order, which draws first and
        // therefore cannot occlude a real splat.
        if (!(viewDepth > 0.0f))
            return 0u;
        return std::bit_cast<u32>(viewDepth);
    }

    auto ConservativeSigma(const GpuSplat& splat) -> f32
    {
        const std::array<f32, 6> sigma = UnpackCovariance(splat.CovXXXY, splat.CovXZYY, splat.CovYZZZ);
        const f32 trace = sigma[0] + sigma[3] + sigma[5];
        return std::sqrt(std::max(trace, 0.0f));
    }

    auto ProjectedExtentPixels(f32 worldSigma, f32 viewDepth, f32 focalPixels) -> f32
    {
        if (!(viewDepth > 0.0f))
            return std::numeric_limits<f32>::max();
        return 3.0f * worldSigma * focalPixels / viewDepth;
    }

    void RadixSortDescending(std::span<u32> keys, std::span<u32> indices, std::vector<u32>& keyScratch,
                             std::vector<u32>& indexScratch)
    {
        OLO_PROFILE_FUNCTION();

        OLO_CORE_ASSERT(keys.size() == indices.size(), "RadixSortDescending: keys and indices must be parallel");
        const sizet count = keys.size();
        if (count < 2)
            return;

        keyScratch.resize(count);
        indexScratch.resize(count);

        // DESCENDING BY COMPLEMENT, NOT BY REVERSING. An LSD radix sort is
        // naturally ascending, and the obvious way to flip it -- emit the last
        // pass's digit buckets in reverse -- is wrong the moment that pass is
        // skipped as uniform, which is the common case here because the top
        // byte of a float depth is the sign plus most of the exponent.
        // Complementing every key instead makes ascending order OF THE
        // COMPLEMENT equal descending order of the original, whichever passes
        // end up running, and keeps the sort stable.
        for (u32& key : keys)
            key = ~key;

        std::span<u32> srcKeys = keys;
        std::span<u32> srcIndices = indices;
        std::span<u32> dstKeys(keyScratch);
        std::span<u32> dstIndices(indexScratch);

        std::array<u32, 256> histogram{};
        for (u32 shift = 0; shift < 32; shift += 8)
        {
            histogram.fill(0u);
            for (sizet i = 0; i < count; ++i)
                ++histogram[(srcKeys[i] >> shift) & 0xFFu];

            // A pass whose digit is identical everywhere is a full copy for no
            // benefit.
            if (static_cast<sizet>(histogram[(srcKeys[0] >> shift) & 0xFFu]) == count)
                continue;

            u32 running = 0;
            for (u32& bucket : histogram)
            {
                const u32 n = bucket;
                bucket = running;
                running += n;
            }

            for (sizet i = 0; i < count; ++i)
            {
                const u32 digit = (srcKeys[i] >> shift) & 0xFFu;
                const u32 slot = histogram[digit]++;
                dstKeys[slot] = srcKeys[i];
                dstIndices[slot] = srcIndices[i];
            }

            std::swap(srcKeys, dstKeys);
            std::swap(srcIndices, dstIndices);
        }

        // An odd number of executed passes leaves the result in the scratch
        // buffers; copy it back so the caller's spans always hold the answer.
        if (srcKeys.data() != keys.data())
        {
            std::copy(srcKeys.begin(), srcKeys.end(), keys.begin());
            std::copy(srcIndices.begin(), srcIndices.end(), indices.begin());
        }

        for (u32& key : keys)
            key = ~key;
    }

    void BuildViewOrdering(const SplatCloud& cloud,
                           const glm::mat4& view,
                           const glm::mat4& projection,
                           const glm::vec2& viewportPixels,
                           const ViewSettings& settings,
                           ViewOrdering& out)
    {
        OLO_PROFILE_FUNCTION();

        out.Indices.clear();
        out.Stats = ViewStats{};

        const std::span<const GpuSplat> splats = cloud.Splats();
        out.Stats.Total = static_cast<u32>(splats.size());
        if (splats.empty())
            return;

        // View depth is -(view * p).z, which is the third row of `view` negated
        // applied to p plus the negated translation. Pulled out of the loop so
        // the hot path is three multiply-adds.
        const glm::vec3 forward(-view[0][2], -view[1][2], -view[2][2]);
        const f32 forwardOffset = -view[3][2];

        const std::array<Plane, 6> planes = ExtractFrustumPlanes(projection * view);

        // projection[1][1] is 1/tan(fovY/2) for a standard perspective matrix,
        // so this is the vertical focal length in pixels.
        const f32 focalPixels = 0.5f * viewportPixels.y * std::abs(projection[1][1]);

        struct Candidate
        {
            u32 Index = 0;
            u32 Key = 0;
            f32 Score = 0.0f; // screen area x alpha: what the budget ranks on
        };
        std::vector<Candidate> candidates;
        candidates.reserve(splats.size());

        for (u32 i = 0; i < static_cast<u32>(splats.size()); ++i)
        {
            const GpuSplat& splat = splats[i];

            const f32 alpha = static_cast<f32>((splat.ColorOpacity >> 24) & 0xFFu) / 255.0f;
            if (alpha < settings.MinAlpha)
            {
                ++out.Stats.TooFaint;
                continue;
            }

            const f32 depth = glm::dot(forward, splat.Position) + forwardOffset;
            const f32 sigma = ConservativeSigma(splat);
            const f32 radius = 3.0f * sigma;

            if (depth - radius < settings.NearClip)
            {
                ++out.Stats.BehindNearPlane;
                continue;
            }

            bool inside = true;
            for (const Plane& plane : planes)
            {
                if (glm::dot(plane.Normal, splat.Position) + plane.Distance < -radius)
                {
                    inside = false;
                    break;
                }
            }
            if (!inside)
            {
                ++out.Stats.FrustumCulled;
                continue;
            }

            const f32 extent = ProjectedExtentPixels(sigma, depth, focalPixels);
            if (extent < settings.MinScreenExtentPixels)
            {
                ++out.Stats.TooSmall;
                continue;
            }

            candidates.push_back({ i, DepthSortKey(depth), extent * extent * alpha });
        }

        // Budget. Ranking on screen area x alpha keeps the splats a viewer can
        // actually see; ties break on cloud index so the same camera always
        // yields the same set (a budget that flickers between two equal-scoring
        // splats is a shimmer the eye picks up immediately).
        if (settings.MaxSplats > 0 && candidates.size() > settings.MaxSplats)
        {
            const auto keep = static_cast<std::ptrdiff_t>(settings.MaxSplats);
            std::nth_element(candidates.begin(), candidates.begin() + keep, candidates.end(),
                             [](const Candidate& a, const Candidate& b)
                             {
                                 // No float == anywhere: two > tests decide
                                 // "greater", "less" and "equivalent" between
                                 // them (cpp-coding-quality.md 2a).
                                 if (a.Score > b.Score)
                                     return true;
                                 if (b.Score > a.Score)
                                     return false;
                                 return a.Index < b.Index;
                             });
            out.Stats.OverBudget = static_cast<u32>(candidates.size()) - settings.MaxSplats;
            candidates.resize(settings.MaxSplats);
        }

        const sizet drawn = candidates.size();
        std::vector<u32> keys(drawn);
        out.Indices.resize(drawn);
        for (sizet i = 0; i < drawn; ++i)
        {
            keys[i] = candidates[i].Key;
            out.Indices[i] = candidates[i].Index;
        }

        // nth_element left the survivors in an order that depends on the
        // partition, so re-establish cloud order before the stable sort. That
        // is what makes equal-depth ties deterministic.
        if (out.Stats.OverBudget > 0)
        {
            std::vector<sizet> order(drawn);
            for (sizet i = 0; i < drawn; ++i)
                order[i] = i;
            std::sort(order.begin(), order.end(),
                      [&](sizet a, sizet b)
                      { return out.Indices[a] < out.Indices[b]; });
            std::vector<u32> sortedKeys(drawn);
            std::vector<u32> sortedIndices(drawn);
            for (sizet i = 0; i < drawn; ++i)
            {
                sortedKeys[i] = keys[order[i]];
                sortedIndices[i] = out.Indices[order[i]];
            }
            keys.swap(sortedKeys);
            out.Indices.swap(sortedIndices);
        }

        std::vector<u32> keyScratch;
        std::vector<u32> indexScratch;
        RadixSortDescending(keys, out.Indices, keyScratch, indexScratch);

        out.Stats.Drawn = static_cast<u32>(drawn);
    }
} // namespace OloEngine::GaussianSplat
