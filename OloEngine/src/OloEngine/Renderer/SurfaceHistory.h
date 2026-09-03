#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/GPUScene/GPUSceneTypes.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <glm/glm.hpp>

namespace OloEngine
{
    enum class SurfaceHistoryFlag : u32
    {
        None = 0,
        Reactive = 1u << 0u,
        Disoccluded = 1u << 1u,
        HasHitDistance = 1u << 2u,
    };

    [[nodiscard]] constexpr auto operator|(SurfaceHistoryFlag lhs, SurfaceHistoryFlag rhs) -> SurfaceHistoryFlag
    {
        return static_cast<SurfaceHistoryFlag>(std::to_underlying(lhs) | std::to_underlying(rhs));
    }

    [[nodiscard]] constexpr bool HasSurfaceHistoryFlag(SurfaceHistoryFlag flags, SurfaceHistoryFlag flag)
    {
        return (std::to_underlying(flags) & std::to_underlying(flag)) != 0u;
    }

    // Backend-neutral decoded form of the compact per-pixel surface record.
    // The three identities use GPU Scene's canonical (slot, generation)
    // currency. Primitive names the GPUSceneGeometry/submesh record; a future
    // ray hit may additionally supply a triangle-local primitive index.
    struct SurfaceHistoryRecord
    {
        f32 LinearDepth = 0.0f;
        glm::vec3 GeometricNormal{ 0.0f, 0.0f, 1.0f };
        glm::vec3 ShadingNormal{ 0.0f, 0.0f, 1.0f };
        f32 Roughness = 1.0f;
        u32 MaterialClass = 0u;
        glm::vec2 Motion{ 0.0f };
        GPUSceneHandle Instance{};
        GPUSceneHandle Primitive{};
        GPUSceneHandle Material{};
        SurfaceHistoryFlag Flags = SurfaceHistoryFlag::None;
        f32 HitDistance = 0.0f;
        u32 PrimitiveLocalIndex = GPUSceneHandle::InvalidIndex;
    };

    enum class SurfaceHistoryRejection : u32
    {
        None = 0,
        NoHistory = 1u << 0u,
        OffScreen = 1u << 1u,
        NonFinite = 1u << 2u,
        DepthMismatch = 1u << 3u,
        GeometricNormalMismatch = 1u << 4u,
        ShadingNormalMismatch = 1u << 5u,
        InstanceMismatch = 1u << 6u,
        PrimitiveMismatch = 1u << 7u,
        MaterialMismatch = 1u << 8u,
        RoughnessMismatch = 1u << 9u,
        MotionMismatch = 1u << 10u,
        Reactive = 1u << 11u,
        Disoccluded = 1u << 12u,
        HitDistanceMismatch = 1u << 13u,
        IdentityUnavailable = 1u << 14u,
    };

    struct SurfaceHistoryValidity
    {
        SurfaceHistoryRejection Rejections = SurfaceHistoryRejection::None;
        glm::vec2 ReprojectedUV{ 0.0f };

        [[nodiscard]] constexpr bool Accepted() const
        {
            return Rejections == SurfaceHistoryRejection::None;
        }

        [[nodiscard]] constexpr bool Has(SurfaceHistoryRejection reason) const
        {
            return (std::to_underlying(Rejections) & std::to_underlying(reason)) != 0u;
        }
    };

    struct SurfaceHistoryValiditySettings
    {
        bool HistoryAvailable = true;
        bool TestGeometricNormal = true;
        bool TestShadingNormal = true;
        bool TestInstance = true;
        bool TestPrimitive = true;
        bool TestMaterial = true;
        bool TestRoughness = true;
        bool TestMotion = true;
        bool TestHitDistance = true;
        f32 RelativeDepthThreshold = 0.05f;
        f32 GeometricNormalCosineThreshold = 0.85f;
        f32 ShadingNormalCosineThreshold = 0.75f;
        f32 RoughnessThreshold = 0.15f;
        f32 MotionThresholdPixels = 64.0f;
        f32 RelativeHitDistanceThreshold = 0.1f;
        glm::vec2 PixelSize{ 1.0f };
    };

    namespace SurfaceHistoryDetail
    {
        inline void AddRejection(SurfaceHistoryValidity& validity, SurfaceHistoryRejection reason)
        {
            validity.Rejections = static_cast<SurfaceHistoryRejection>(
                std::to_underlying(validity.Rejections) | std::to_underlying(reason));
        }

        [[nodiscard]] inline bool IsFinite(const glm::vec2& value)
        {
            return std::isfinite(value.x) && std::isfinite(value.y);
        }

        [[nodiscard]] inline bool IsFinite(const glm::vec3& value)
        {
            return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
        }

        [[nodiscard]] inline f32 NormalCosine(const glm::vec3& lhs, const glm::vec3& rhs)
        {
            const f32 lhsLengthSquared = glm::dot(lhs, lhs);
            const f32 rhsLengthSquared = glm::dot(rhs, rhs);
            if (!std::isfinite(lhsLengthSquared) || !std::isfinite(rhsLengthSquared) ||
                lhsLengthSquared <= 1.0e-12f || rhsLengthSquared <= 1.0e-12f)
            {
                return -1.0f;
            }
            return glm::dot(lhs, rhs) / std::sqrt(lhsLengthSquared * rhsLengthSquared);
        }

        [[nodiscard]] inline bool RelativeDifferenceExceeds(f32 lhs, f32 rhs, f32 threshold)
        {
            const f32 denominator = std::max(std::max(std::abs(lhs), std::abs(rhs)), 1.0e-4f);
            return std::abs(lhs - rhs) / denominator > threshold;
        }
    } // namespace SurfaceHistoryDetail

    [[nodiscard]] inline SurfaceHistoryValidity EvaluateSurfaceHistory(
        const SurfaceHistoryRecord& current,
        const SurfaceHistoryRecord& previous,
        const glm::vec2& reprojectedUV,
        const SurfaceHistoryValiditySettings& settings)
    {
        SurfaceHistoryValidity result{ .ReprojectedUV = reprojectedUV };
        const auto reject = [&result](SurfaceHistoryRejection reason)
        { SurfaceHistoryDetail::AddRejection(result, reason); };

        if (!settings.HistoryAvailable)
            reject(SurfaceHistoryRejection::NoHistory);
        if (!SurfaceHistoryDetail::IsFinite(reprojectedUV) || reprojectedUV.x < 0.0f || reprojectedUV.x > 1.0f ||
            reprojectedUV.y < 0.0f || reprojectedUV.y > 1.0f)
        {
            reject(SurfaceHistoryRejection::OffScreen);
        }

        const bool currentHasHitDistance = HasSurfaceHistoryFlag(current.Flags, SurfaceHistoryFlag::HasHitDistance);
        const bool previousHasHitDistance = HasSurfaceHistoryFlag(previous.Flags, SurfaceHistoryFlag::HasHitDistance);
        const bool finite =
            std::isfinite(current.LinearDepth) && std::isfinite(previous.LinearDepth) &&
            (!settings.TestGeometricNormal ||
             (SurfaceHistoryDetail::IsFinite(current.GeometricNormal) &&
              SurfaceHistoryDetail::IsFinite(previous.GeometricNormal))) &&
            (!settings.TestShadingNormal ||
             (SurfaceHistoryDetail::IsFinite(current.ShadingNormal) &&
              SurfaceHistoryDetail::IsFinite(previous.ShadingNormal))) &&
            (!settings.TestRoughness ||
             (std::isfinite(current.Roughness) && std::isfinite(previous.Roughness))) &&
            (!settings.TestMotion || SurfaceHistoryDetail::IsFinite(current.Motion)) &&
            (!settings.TestHitDistance ||
             ((!currentHasHitDistance || std::isfinite(current.HitDistance)) &&
              (!previousHasHitDistance || std::isfinite(previous.HitDistance))));
        if (!finite)
            reject(SurfaceHistoryRejection::NonFinite);

        if (SurfaceHistoryDetail::RelativeDifferenceExceeds(
                current.LinearDepth, previous.LinearDepth, settings.RelativeDepthThreshold))
        {
            reject(SurfaceHistoryRejection::DepthMismatch);
        }
        if (settings.TestGeometricNormal &&
            SurfaceHistoryDetail::NormalCosine(current.GeometricNormal, previous.GeometricNormal) <
                settings.GeometricNormalCosineThreshold)
        {
            reject(SurfaceHistoryRejection::GeometricNormalMismatch);
        }
        if (settings.TestShadingNormal &&
            SurfaceHistoryDetail::NormalCosine(current.ShadingNormal, previous.ShadingNormal) <
                settings.ShadingNormalCosineThreshold)
        {
            reject(SurfaceHistoryRejection::ShadingNormalMismatch);
        }

        const bool testedIdentityUnavailable =
            (settings.TestInstance && (!current.Instance.IsValid() || !previous.Instance.IsValid())) ||
            (settings.TestPrimitive && (!current.Primitive.IsValid() || !previous.Primitive.IsValid())) ||
            (settings.TestMaterial && (!current.Material.IsValid() || !previous.Material.IsValid()));
        if (testedIdentityUnavailable)
            reject(SurfaceHistoryRejection::IdentityUnavailable);
        if (settings.TestInstance && current.Instance != previous.Instance)
            reject(SurfaceHistoryRejection::InstanceMismatch);
        if (settings.TestPrimitive &&
            (current.Primitive != previous.Primitive || current.PrimitiveLocalIndex != previous.PrimitiveLocalIndex))
        {
            reject(SurfaceHistoryRejection::PrimitiveMismatch);
        }
        if (settings.TestMaterial &&
            (current.Material != previous.Material || current.MaterialClass != previous.MaterialClass))
        {
            reject(SurfaceHistoryRejection::MaterialMismatch);
        }
        if (settings.TestRoughness && std::abs(current.Roughness - previous.Roughness) > settings.RoughnessThreshold)
            reject(SurfaceHistoryRejection::RoughnessMismatch);

        const glm::vec2 motionPixels = current.Motion / glm::max(settings.PixelSize, glm::vec2(1.0e-8f));
        if (settings.TestMotion && glm::dot(motionPixels, motionPixels) >
                                       settings.MotionThresholdPixels * settings.MotionThresholdPixels)
        {
            reject(SurfaceHistoryRejection::MotionMismatch);
        }
        if (HasSurfaceHistoryFlag(current.Flags, SurfaceHistoryFlag::Reactive))
            reject(SurfaceHistoryRejection::Reactive);
        if (HasSurfaceHistoryFlag(current.Flags, SurfaceHistoryFlag::Disoccluded))
            reject(SurfaceHistoryRejection::Disoccluded);

        if (settings.TestHitDistance && currentHasHitDistance != previousHasHitDistance)
            reject(SurfaceHistoryRejection::HitDistanceMismatch);
        else if (settings.TestHitDistance && currentHasHitDistance &&
                 SurfaceHistoryDetail::RelativeDifferenceExceeds(
                     current.HitDistance, previous.HitDistance, settings.RelativeHitDistanceThreshold))
        {
            reject(SurfaceHistoryRejection::HitDistanceMismatch);
        }

        return result;
    }

    enum class TemporalSignalKind : u8
    {
        RGBRadiance,
        ScalarVisibility,
    };

    struct TemporalMoments
    {
        glm::vec4 First{ 0.0f };
        glm::vec4 Second{ 0.0f };
        f32 HistoryLength = 0.0f;
    };

    [[nodiscard]] inline TemporalMoments AccumulateTemporalMoments(
        const glm::vec4& current,
        const TemporalMoments& previous,
        bool historyAccepted,
        f32 maximumHistoryLength = 255.0f)
    {
        if (!historyAccepted)
        {
            return {
                .First = current,
                .Second = current * current,
                .HistoryLength = 1.0f,
            };
        }
        const f32 previousLength = std::max(previous.HistoryLength, 0.0f);
        const f32 length = std::min(previousLength + 1.0f, std::max(maximumHistoryLength, 1.0f));
        const f32 historyWeight = (length - 1.0f) / length;
        const f32 currentWeight = 1.0f / length;
        return {
            .First = previous.First * historyWeight + current * currentWeight,
            .Second = previous.Second * historyWeight + current * current * currentWeight,
            .HistoryLength = length,
        };
    }

    [[nodiscard]] inline glm::vec4 TemporalVariance(const TemporalMoments& moments)
    {
        return glm::max(moments.Second - moments.First * moments.First, glm::vec4(0.0f));
    }
} // namespace OloEngine
