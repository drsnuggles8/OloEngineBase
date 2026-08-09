#include "OloEnginePCH.h"
#include "OloEngine/Renderer/ReflectionProbeDistanceField.h"

#include "OloEngine/Core/Log.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace OloEngine
{
    Ref<ReflectionProbeDistanceField> ReflectionProbeDistanceField::Create(std::vector<f32>&& mip0, u32 resolution)
    {
        if (!FMath::IsPowerOfTwo(resolution))
        {
            OLO_CORE_ERROR("ReflectionProbeDistanceField: resolution {} is not a power of two", resolution);
            return nullptr;
        }
        sizet const expected = static_cast<sizet>(resolution) * resolution * 6u;
        if (mip0.size() != expected)
        {
            OLO_CORE_ERROR("ReflectionProbeDistanceField: mip0 has {} texels, expected {}", mip0.size(), expected);
            return nullptr;
        }

        auto field = Ref<ReflectionProbeDistanceField>::Create();
        field->m_Resolution = resolution;
        field->m_MaxFiniteDistance = ComputeMaxFiniteProbeDistance(mip0);
        field->m_Mips.emplace_back(std::move(mip0));

        u32 res = resolution;
        while (res > 1)
        {
            field->m_Mips.emplace_back(BuildNextMaxMip(field->m_Mips.back(), res));
            res /= 2;
        }
        return field;
    }

    std::span<const f32> ReflectionProbeDistanceField::GetMip(u32 mip) const
    {
        if (mip >= m_Mips.size())
        {
            return {};
        }
        return m_Mips[mip];
    }

    f32 ReflectionProbeDistanceField::SampleNearest(const glm::vec3& direction, u32 mip) const
    {
        u32 const clampedMip = std::min(mip, static_cast<u32>(m_Mips.size()) - 1u);
        u32 const res = std::max(1u, m_Resolution >> clampedMip);
        CubeFaceTexel const texel = DirectionToCubeFaceTexel(direction, res);
        auto const& data = m_Mips[clampedMip];
        sizet const index = (static_cast<sizet>(texel.Face) * res + texel.Y) * res + texel.X;
        return data[index];
    }

    std::vector<f32> BuildNextMaxMip(std::span<const f32> source, u32 resolution)
    {
        u32 const dstRes = std::max(1u, resolution / 2u);
        std::vector<f32> next(static_cast<sizet>(dstRes) * dstRes * 6u, 0.0f);
        if (resolution < 2u || source.size() < static_cast<sizet>(resolution) * resolution * 6u)
        {
            OLO_CORE_ERROR("BuildNextMaxMip: malformed source (resolution {}, {} texels)", resolution, source.size());
            return next;
        }

        for (u32 face = 0; face < 6u; ++face)
        {
            sizet const srcFaceBase = static_cast<sizet>(face) * resolution * resolution;
            sizet const dstFaceBase = static_cast<sizet>(face) * dstRes * dstRes;
            for (u32 y = 0; y < dstRes; ++y)
            {
                for (u32 x = 0; x < dstRes; ++x)
                {
                    sizet const s00 = srcFaceBase + (static_cast<sizet>(y) * 2u) * resolution + (static_cast<sizet>(x) * 2u);
                    sizet const s01 = s00 + 1u;
                    sizet const s10 = s00 + resolution;
                    sizet const s11 = s10 + 1u;
                    next[dstFaceBase + static_cast<sizet>(y) * dstRes + x] =
                        std::max(std::max(source[s00], source[s01]), std::max(source[s10], source[s11]));
                }
            }
        }
        return next;
    }

    f32 ComputeMaxFiniteProbeDistance(std::span<const f32> mip0)
    {
        f32 maxFinite = 0.0f;
        for (f32 const value : mip0)
        {
            if (value < kProbeDistanceMissThreshold)
            {
                maxFinite = std::max(maxFinite, value);
            }
        }
        // All-sky probes keep a valid (if useless) march bound.
        return maxFinite > 0.0f ? maxFinite : kProbeDistanceFar;
    }

    CubeFaceTexel DirectionToCubeFaceTexel(const glm::vec3& direction, u32 resolution)
    {
        // GL 4.6 spec table 8.19: pick the major axis, then map (sc, tc) to
        // face UVs. This must agree with how the capture cameras were aimed
        // (ReflectionProbeBaker's face target/up tables) and with GLSL's
        // samplerCube addressing.
        f32 const ax = std::abs(direction.x);
        f32 const ay = std::abs(direction.y);
        f32 const az = std::abs(direction.z);

        u32 face = 0;
        f32 sc = 0.0f;
        f32 tc = 0.0f;
        f32 ma = 0.0f;

        if (ax >= ay && ax >= az)
        {
            ma = ax;
            if (direction.x >= 0.0f)
            {
                face = 0; // +X
                sc = -direction.z;
                tc = -direction.y;
            }
            else
            {
                face = 1; // -X
                sc = direction.z;
                tc = -direction.y;
            }
        }
        else if (ay >= az)
        {
            ma = ay;
            if (direction.y >= 0.0f)
            {
                face = 2; // +Y
                sc = direction.x;
                tc = direction.z;
            }
            else
            {
                face = 3; // -Y
                sc = direction.x;
                tc = -direction.z;
            }
        }
        else
        {
            ma = az;
            if (direction.z >= 0.0f)
            {
                face = 4; // +Z
                sc = direction.x;
                tc = -direction.y;
            }
            else
            {
                face = 5; // -Z
                sc = -direction.x;
                tc = -direction.y;
            }
        }

        if (ma <= 0.0f)
        {
            return { 0u, 0u, 0u }; // zero direction — callers guarantee non-zero, fail safe
        }

        f32 const u = 0.5f * (sc / ma + 1.0f);
        f32 const v = 0.5f * (tc / ma + 1.0f);
        auto const maxTexel = static_cast<i32>(resolution) - 1;
        auto const x = std::clamp(static_cast<i32>(u * static_cast<f32>(resolution)), 0, maxTexel);
        auto const y = std::clamp(static_cast<i32>(v * static_cast<f32>(resolution)), 0, maxTexel);
        return { face, static_cast<u32>(x), static_cast<u32>(y) };
    }
} // namespace OloEngine
