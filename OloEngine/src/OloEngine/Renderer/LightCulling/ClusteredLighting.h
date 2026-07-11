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

        struct DepthSliceParams
        {
            f32 Scale = 0.0f;
            f32 Bias = 0.0f;
        };

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
