#pragma once

#include "OloEngine/Core/Base.h"

#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>

namespace OloEngine
{
    // @brief Shared math for the clustered (froxel) light grid — issue #435.
    //
    // Single source of truth for the exponential depth-slice mapping and the
    // flat cluster index used by:
    //   - the C++ UBO fill (TiledForwardPlus::BindForShading),
    //   - the culling compute (LightCulling.comp, textual mirror),
    //   - fragment consumption (ForwardPlusCommon.glsl, textual mirror),
    //   - the froxel-fog compute passes (same slice formula, own grid dims),
    //   - the reflection-probe cluster cull + lookup (issue #705:
    //     ReflectionProbeCull.comp and include/ReflectionProbes.glsl's
    //     oloProbeClusterMask, textual mirrors on the same grid),
    //   - the CPU contract tests (ClusteredLightingMathTest.cpp).
    //
    // Slice k spans view depth [near·(far/near)^(k/Z), near·(far/near)^((k+1)/Z)];
    // inversely slice = floor(log2(viewZ)·scale + bias) with the scale/bias
    // below. viewZ is the POSITIVE distance along the camera forward axis
    // (-viewSpace.z in OpenGL conventions).
    namespace ClusteredLighting
    {
        // Fixed cluster grid dimensions (resolution-independent GPU memory:
        // the grid + index SSBOs are sized once from these and never resize
        // with the window). 32×18 keeps square-ish tiles at 16:9; 24 slices
        // matches the exponential-Z depth granularity of contemporary
        // clustered renderers (DOOM 2016: 16×8×24).
        inline constexpr u32 kClusterCountX = 32;
        inline constexpr u32 kClusterCountY = 18;
        inline constexpr u32 kClusterCountZ = 24;
        inline constexpr u32 kTotalClusters = kClusterCountX * kClusterCountY * kClusterCountZ;

        // Per-cluster light list cap. The compute shader's shared-memory array
        // (MAX_SHARED_LIGHTS = 256) is the hard upper bound; 128 is the sized
        // default — a light overlapping several slices appears in each, so
        // per-cluster counts run well below the old per-tile counts.
        inline constexpr u32 kMaxLightsPerCluster = 128;

        // Per-tile 2.5D occupancy resolution. One u32 mask keeps the depth
        // representation compact and makes the GPU overlap test a single AND.
        inline constexpr u32 kDepthCellCount = 32;
        inline constexpr u32 kDepthTileMetadataWordCount = 4;
        inline constexpr u32 kGlobalCounterAndDispatchWordCount = 4;
        inline constexpr u32 kIndirectDispatchOffsetBytes = sizeof(u32);

        struct DepthSliceParams
        {
            f32 Scale = 0.0f;
            f32 Bias = 0.0f;
        };

        struct DepthAwareFrameInputs
        {
            bool DepthPrepassAvailable = true;
            bool SingleSampleDepth = true;
            bool LeverEnabled = true;
            bool HasBlendedGeometry = false;
            bool HasVirtualGeometry = false;
            bool HasVolumetricFog = false;
        };

        [[nodiscard]] inline bool CanUseDepthAwareCulling(const DepthAwareFrameInputs& inputs)
        {
            return inputs.DepthPrepassAvailable && inputs.SingleSampleDepth && inputs.LeverEnabled &&
                   !inputs.HasBlendedGeometry && !inputs.HasVirtualGeometry && !inputs.HasVolumetricFog;
        }

        // Guard rails for degenerate camera planes: slicing needs 0 < near < far.
        inline constexpr f32 kMinNearPlane = 0.01f;

        inline DepthSliceParams ComputeDepthSliceParams(u32 sliceCount, f32 nearPlane, f32 farPlane)
        {
            const f32 n = std::max(nearPlane, kMinNearPlane);
            const f32 f = std::max(farPlane, n * (1.0f + 1e-3f));
            const f32 logFOverN = std::log2(f / n);
            DepthSliceParams params;
            params.Scale = static_cast<f32>(sliceCount) / logFOverN;
            params.Bias = -static_cast<f32>(sliceCount) * std::log2(n) / logFOverN;
            return params;
        }

        // Slice index for a positive view depth. Mirrors the GLSL:
        //   slice = clamp(int(floor(log2(viewZ) * scale + bias)), 0, Z-1)
        inline u32 SliceForViewDepth(f32 viewDepth, const DepthSliceParams& params, u32 sliceCount)
        {
            const f32 z = std::max(viewDepth, kMinNearPlane * 0.5f);
            const f32 raw = std::floor(std::log2(z) * params.Scale + params.Bias);
            const i32 slice = static_cast<i32>(raw);
            return static_cast<u32>(std::clamp(slice, 0, static_cast<i32>(sliceCount) - 1));
        }

        // Near edge (view depth) of slice k: near·(far/near)^(k/Z).
        inline f32 SliceNearDepth(u32 slice, u32 sliceCount, f32 nearPlane, f32 farPlane)
        {
            const f32 n = std::max(nearPlane, kMinNearPlane);
            const f32 f = std::max(farPlane, n * (1.0f + 1e-3f));
            return n * std::pow(f / n, static_cast<f32>(slice) / static_cast<f32>(sliceCount));
        }

        // Flat cluster index; the compute writes and the fragment reads with
        // the identical ordering: X fastest, then Y, slice outermost.
        inline u32 ClusterIndex(u32 tileX, u32 tileY, u32 slice, u32 countX, u32 countY)
        {
            return (slice * countY + tileY) * countX + tileX;
        }

        // DepthPrepare.comp stores one bit per active logarithmic slice for a
        // tile, then appends the corresponding flat cluster indices. These
        // helpers pin the compact-list count and ordering without a GPU.
        inline u32 ActiveSliceCount(u32 activeSliceMask, u32 sliceCount)
        {
            if (sliceCount == 0u)
                return 0u;
            if (sliceCount < 32u)
                activeSliceMask &= (1u << sliceCount) - 1u;

            u32 count = 0u;
            while (activeSliceMask != 0u)
            {
                activeSliceMask &= activeSliceMask - 1u;
                ++count;
            }
            return count;
        }

        inline u32 ClusterIndexFromTile(u32 tileIndex, u32 slice, u32 countX, u32 countY)
        {
            return slice * countX * countY + tileIndex;
        }

        // Quantise a positive view depth into 32 linear cells over a tile's
        // reduced min/max. The maximum maps to the final cell instead of
        // overflowing to cell 32.
        inline u32 DepthCellForViewDepth(f32 viewDepth, f32 tileMinDepth, f32 tileMaxDepth)
        {
            if (!std::isfinite(viewDepth) || !std::isfinite(tileMinDepth) || !std::isfinite(tileMaxDepth))
                return 0u;

            const f32 span = tileMaxDepth - tileMinDepth;
            if (span <= 1e-6f)
                return 0u;
            const f32 normalised = std::clamp((viewDepth - tileMinDepth) / span, 0.0f, 1.0f);
            return std::min(static_cast<u32>(normalised * static_cast<f32>(kDepthCellCount)),
                            kDepthCellCount - 1u);
        }

        // Conservative bit coverage for a light's positive view-depth span.
        // Reversed endpoints are accepted because callers naturally derive
        // them from view-space sphere bounds where sign ordering is easy to
        // invert. A range outside the tile cannot intersect its occupancy.
        inline u32 DepthCellMaskForViewRange(f32 rangeDepthA, f32 rangeDepthB,
                                             f32 tileMinDepth, f32 tileMaxDepth)
        {
            if (!std::isfinite(rangeDepthA) || !std::isfinite(rangeDepthB) ||
                !std::isfinite(tileMinDepth) || !std::isfinite(tileMaxDepth) ||
                tileMaxDepth < tileMinDepth)
            {
                return 0u;
            }

            const f32 rangeMin = std::min(rangeDepthA, rangeDepthB);
            const f32 rangeMax = std::max(rangeDepthA, rangeDepthB);
            if (rangeMax < tileMinDepth || rangeMin > tileMaxDepth)
                return 0u;
            if (tileMaxDepth - tileMinDepth <= 1e-6f)
                return 1u;

            const u32 firstCell = DepthCellForViewDepth(std::max(rangeMin, tileMinDepth),
                                                        tileMinDepth, tileMaxDepth);
            const u32 lastCell = DepthCellForViewDepth(std::min(rangeMax, tileMaxDepth),
                                                       tileMinDepth, tileMaxDepth);
            const u32 cellCount = lastCell - firstCell + 1u;
            if (cellCount >= kDepthCellCount)
                return 0xFFFFFFFFu;

            return ((1u << cellCount) - 1u) << firstCell;
        }

        inline bool DepthRangeIntersectsMask(u32 occupancyMask,
                                             f32 rangeDepthA, f32 rangeDepthB,
                                             f32 tileMinDepth, f32 tileMaxDepth)
        {
            return (occupancyMask & DepthCellMaskForViewRange(rangeDepthA, rangeDepthB,
                                                              tileMinDepth, tileMaxDepth)) != 0u;
        }

        // Inverse of the fragment-side floor((pixel + 0.5) * tileCount /
        // screenExtent). Using this boundary in DepthPrepare.comp makes its
        // depth ownership exact at non-divisible viewport dimensions.
        inline u32 TilePixelBoundary(u32 boundary, u32 tileCount, u32 screenExtent)
        {
            if (boundary == 0u || tileCount == 0u)
                return 0u;
            const u64 numerator = 2ull * boundary * screenExtent + tileCount - 1u;
            return static_cast<u32>(numerator / (2ull * tileCount));
        }

        inline u32 TileForPixelCenter(u32 pixel, u32 tileCount, u32 screenExtent)
        {
            if (tileCount == 0u || screenExtent == 0u)
                return 0u;
            const u64 numerator = (2ull * pixel + 1ull) * tileCount;
            return std::min(static_cast<u32>(numerator / (2ull * screenExtent)), tileCount - 1u);
        }

        // Reconstruct positive view depth from a GL-shaped device-depth value.
        // Runtime callers upload RHI::AdjustedInverseForShaderReconstruction,
        // so this same arithmetic consumes row-correct depth on both backends.
        inline f32 ViewDepthFromDeviceDepth(f32 deviceDepth, const glm::mat4& inverseProjection)
        {
            const f32 depth = std::clamp(deviceDepth, 0.0f, 1.0f);
            const glm::vec4 view = inverseProjection * glm::vec4(0.0f, 0.0f, depth * 2.0f - 1.0f, 1.0f);
            if (!std::isfinite(view.z) || !std::isfinite(view.w) || std::abs(view.w) <= 1e-12f)
                return 0.0f;
            return std::max(-view.z / view.w, 0.0f);
        }

        // The shading-visible arrays stay at the front of their established
        // bindings. Depth-aware scratch data occupies suffixes so no new SSBO
        // binding is needed (the engine-wide namespace is already full).
        inline constexpr u32 ActiveClusterListOffsetWords(u32 totalClusters, u32 maxLightsPerCluster)
        {
            return totalClusters * maxLightsPerCluster;
        }

        inline constexpr u32 LightIndexStorageWords(u32 totalClusters, u32 maxLightsPerCluster)
        {
            return ActiveClusterListOffsetWords(totalClusters, maxLightsPerCluster) + totalClusters;
        }

        inline constexpr u32 DepthTileMetadataOffsetWords(u32 totalClusters)
        {
            return totalClusters * 2u;
        }

        inline constexpr u32 LightGridStorageWords(u32 totalClusters, u32 tileCount)
        {
            return DepthTileMetadataOffsetWords(totalClusters) + tileCount * kDepthTileMetadataWordCount;
        }

        // Extract the near/far clip planes from an OpenGL-convention
        // projection matrix (NDC z in [-1, 1]). Handles both perspective
        // (m[3][3] == 0) and orthographic (m[3][3] == 1) projections. Values
        // are sanitised so downstream slicing never sees near <= 0 or
        // far <= near, even for degenerate inputs.
        inline void ExtractClipPlanes(const glm::mat4& projection, f32& outNear, f32& outFar)
        {
            f32 nearPlane;
            f32 farPlane;
            // GLM matrices are column-major: m[col][row].
            if (std::abs(projection[3][3]) < 0.5f)
            {
                // Perspective: m[2][2] = -(f+n)/(f-n), m[3][2] = -2fn/(f-n).
                const f32 a = projection[2][2];
                const f32 b = projection[3][2];
                nearPlane = b / (a - 1.0f);
                farPlane = b / (a + 1.0f);
            }
            else
            {
                // Orthographic: m[2][2] = -2/(f-n), m[3][2] = -(f+n)/(f-n).
                const f32 a = projection[2][2];
                const f32 b = projection[3][2];
                if (std::abs(a) < 1e-12f)
                {
                    outNear = kMinNearPlane;
                    outFar = 1000.0f;
                    return;
                }
                nearPlane = (b + 1.0f) / a;
                farPlane = (b - 1.0f) / a;
            }

            if (!std::isfinite(nearPlane) || !std::isfinite(farPlane))
            {
                nearPlane = kMinNearPlane;
                farPlane = 1000.0f;
            }
            nearPlane = std::max(nearPlane, kMinNearPlane);
            farPlane = std::max(farPlane, nearPlane * (1.0f + 1e-3f));
            outNear = nearPlane;
            outFar = farPlane;
        }
    } // namespace ClusteredLighting
} // namespace OloEngine
