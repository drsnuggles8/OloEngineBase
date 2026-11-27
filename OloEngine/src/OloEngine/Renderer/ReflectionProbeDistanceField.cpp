#include "OloEnginePCH.h"
#include "OloEngine/Renderer/ReflectionProbeDistanceField.h"

#include "OloEngine/Core/Log.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace OloEngine
{
    Ref<ReflectionProbeDistanceField> ReflectionProbeDistanceField::CreateLayered(TArray<f32>&& mip0, u32 resolution)
    {
        if (!FMath::IsPowerOfTwo(resolution))
        {
            OLO_CORE_ERROR("ReflectionProbeDistanceField: resolution {} is not a power of two", resolution);
            return nullptr;
        }
        sizet const expected = static_cast<sizet>(resolution) * resolution * 6u * kProbeDistanceChannels;
        if (mip0.Num() != expected)
        {
            OLO_CORE_ERROR("ReflectionProbeDistanceField: mip0 has {} floats, expected {}", mip0.Num(), expected);
            return nullptr;
        }

        // dMax covers every surface a crossing can land on: d0 and d2 (a
        // gap's far wall can lie beyond every d0 the probe sees).
        TArray<f32> surfaces;
        surfaces.Reserve(static_cast<i32>(expected / 2u));
        for (sizet i = 0; i < expected; i += kProbeDistanceChannels)
        {
            surfaces.Add(mip0[i + 0u]);
            surfaces.Add(mip0[i + 2u]);
        }

        auto field = Ref<ReflectionProbeDistanceField>::Create();
        field->m_Resolution = resolution;
        field->m_MaxFiniteDistance = ComputeMaxFiniteProbeDistance({ surfaces.GetData(), static_cast<sizet>(surfaces.Num()) });
        field->m_Mips.Emplace(std::move(mip0));

        u32 res = resolution;
        while (res > 1)
        {
            field->m_Mips.Emplace(BuildNextMaxMip(field->GetMip(static_cast<u32>(field->m_Mips.Num() - 1)), res,
                                                  kProbeDistanceChannels));
            res /= 2;
        }
        return field;
    }

    Ref<ReflectionProbeDistanceField> ReflectionProbeDistanceField::Create(TArray<f32>&& mip0, u32 resolution)
    {
        sizet const texels = static_cast<sizet>(resolution) * resolution * 6u;
        if (mip0.Num() != texels)
        {
            OLO_CORE_ERROR("ReflectionProbeDistanceField: mip0 has {} texels, expected {}", mip0.Num(), texels);
            return nullptr;
        }
        TArray<f32> layered(static_cast<i32>(texels * kProbeDistanceChannels), 0.0f);
        for (sizet i = 0; i < texels; ++i)
        {
            f32 const d = mip0[i];
            layered[(i * kProbeDistanceChannels) + 0u] = d;
            layered[(i * kProbeDistanceChannels) + 1u] = d;
            layered[(i * kProbeDistanceChannels) + 2u] = d;
        }
        return CreateLayered(std::move(layered), resolution);
    }

    std::span<const f32> ReflectionProbeDistanceField::GetMip(u32 mip) const
    {
        if (mip >= m_Mips.Num())
        {
            return {};
        }
        return { m_Mips[mip].GetData(), static_cast<sizet>(m_Mips[mip].Num()) };
    }

    ProbeDistanceLayers ReflectionProbeDistanceField::SampleLayersNearest(const glm::vec3& direction, u32 mip) const
    {
        // Create() always builds at least mip 0, but a default-constructed
        // instance is representable — fail safe to "sky" rather than
        // underflowing the mip clamp below.
        if (m_Mips.IsEmpty())
        {
            return {};
        }
        u32 const clampedMip = std::min(mip, static_cast<u32>(m_Mips.Num()) - 1u);
        u32 const res = std::max(1u, m_Resolution >> clampedMip);
        CubeFaceTexel const texel = DirectionToCubeFaceTexel(direction, res);
        auto const& data = m_Mips[clampedMip];
        sizet const index = ((static_cast<sizet>(texel.Face) * res + texel.Y) * res + texel.X) * kProbeDistanceChannels;
        return { data[index + 0u], data[index + 1u], data[index + 2u] };
    }

    TArray<f32> BuildNextMaxMip(std::span<const f32> source, u32 resolution, u32 channels)
    {
        u32 const dstRes = std::max(1u, resolution / 2u);
        channels = std::max(1u, channels);
        const u64 floatCount = static_cast<u64>(dstRes) * dstRes * 6u * channels;
        if (floatCount > static_cast<u64>(std::numeric_limits<i32>::max()))
        {
            OLO_CORE_ERROR("BuildNextMaxMip: destination exceeds array capacity");
            return {};
        }
        TArray<f32> next(static_cast<i32>(floatCount), 0.0f);
        if (resolution < 2u || source.size() < static_cast<sizet>(resolution) * resolution * 6u * channels)
        {
            OLO_CORE_ERROR("BuildNextMaxMip: malformed source (resolution {}, {} floats, {} channels)", resolution,
                           source.size(), channels);
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
                    sizet const dst = dstFaceBase + static_cast<sizet>(y) * dstRes + x;
                    for (u32 c = 0; c < channels; ++c)
                    {
                        next[(dst * channels) + c] =
                            std::max(std::max(source[(s00 * channels) + c], source[(s01 * channels) + c]),
                                     std::max(source[(s10 * channels) + c], source[(s11 * channels) + c]));
                    }
                }
            }
        }
        return next;
    }

    f32 ComputeMaxFiniteProbeDistance(std::span<const f32> values)
    {
        f32 maxFinite = 0.0f;
        for (f32 const value : values)
        {
            if (value < kProbeDistanceMissThreshold)
            {
                maxFinite = std::max(maxFinite, value);
            }
        }
        // All-sky probes keep a valid (if useless) march bound.
        return maxFinite > 0.0f ? maxFinite : kProbeDistanceFar;
    }

    ProbeDistanceLayers NormalizeProbeDistanceLayers(f32 front, f32 back, f32 behind)
    {
        auto const clampDistance = [](f32 d)
        {
            // Geometry closer than the near plane was clipped anyway;
            // non-finite values must never reach the field.
            return std::isfinite(d) ? std::clamp(d, kProbeDistanceNear, kProbeDistanceFar) : kProbeDistanceFar;
        };
        ProbeDistanceLayers layers;
        layers.Front = clampDistance(front);
        // Each peel only kept fragments beyond the previous layer, so the
        // order holds by construction; max() keeps it under a readback glitch.
        layers.Back = std::max(layers.Front, clampDistance(back));
        layers.Behind = std::max(layers.Back, clampDistance(behind));
        // Nothing behind the first occluder: no gap (see the contract). The
        // first occluder's far side may itself be the sentinel (an open
        // surface with nothing beyond), which leaves d1 = d2 = far.
        if (layers.Behind >= kProbeDistanceMissThreshold)
        {
            layers.Behind = layers.Back;
        }
        return layers;
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
