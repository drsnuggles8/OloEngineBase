#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/ResourceHandle.h"

#include <span>
#include <string>
#include <string_view>

namespace OloEngine
{
    class RGBuilder;
    class RGCommandContext;
    struct FrameBlackboard;

    enum class RenderGraphNodeFlags : u32
    {
        None = 0,
        Graphics = 1u << 0u,
        Compute = 1u << 1u,
        Copy = 1u << 2u,
        Present = 1u << 3u,
        Readback = 1u << 4u,
        NeverCull = 1u << 5u,
        UsesCommandBucket = 1u << 6u,
        ExternalSideEffect = 1u << 7u,
        AsyncCandidateMetadata = 1u << 8u,
    };

    [[nodiscard]] constexpr auto operator|(RenderGraphNodeFlags lhs, RenderGraphNodeFlags rhs) -> RenderGraphNodeFlags
    {
        return static_cast<RenderGraphNodeFlags>(static_cast<u32>(lhs) | static_cast<u32>(rhs));
    }

    [[nodiscard]] constexpr auto operator&(RenderGraphNodeFlags lhs, RenderGraphNodeFlags rhs) -> RenderGraphNodeFlags
    {
        return static_cast<RenderGraphNodeFlags>(static_cast<u32>(lhs) & static_cast<u32>(rhs));
    }

    [[nodiscard]] constexpr auto HasRenderGraphNodeFlag(RenderGraphNodeFlags flags, RenderGraphNodeFlags flag) -> bool
    {
        return (static_cast<u32>(flags) & static_cast<u32>(flag)) != 0u;
    }

    enum class RenderGraphSubmissionModel : u8
    {
        Unknown = 0,
        BucketOnly,
        ImmediateOnly,
        Mixed,
    };

    enum class RenderGraphPassWorkType : u8
    {
        Graphics = 0,
        Compute = 1,
        Copy = 2,
    };

    class RenderGraphNode : public RefCounted
    {
      public:
        ~RenderGraphNode() override = default;

        [[nodiscard]] virtual const std::string& GetName() const = 0;
        virtual void Setup(RGBuilder& builder, FrameBlackboard& blackboard) = 0;
        virtual void Execute(RGCommandContext& context) = 0;
        [[nodiscard]] virtual RenderGraphNodeFlags GetFlags() const = 0;
        [[nodiscard]] virtual std::span<const ResourceHandle> GetDeclaredReads() const
        {
            return {};
        }
        [[nodiscard]] virtual std::span<const ResourceHandle> GetDeclaredWrites() const
        {
            return {};
        }
        [[nodiscard]] virtual bool IsSideEffecting() const
        {
            const auto flags = GetFlags();
            return HasRenderGraphNodeFlag(flags, RenderGraphNodeFlags::Present) ||
                   HasRenderGraphNodeFlag(flags, RenderGraphNodeFlags::Readback) ||
                   HasRenderGraphNodeFlag(flags, RenderGraphNodeFlags::NeverCull) ||
                   HasRenderGraphNodeFlag(flags, RenderGraphNodeFlags::ExternalSideEffect);
        }
        [[nodiscard]] virtual RenderGraphPassWorkType GetPassWorkType() const
        {
            const auto flags = GetFlags();
            if (HasRenderGraphNodeFlag(flags, RenderGraphNodeFlags::Compute))
                return RenderGraphPassWorkType::Compute;
            if (HasRenderGraphNodeFlag(flags, RenderGraphNodeFlags::Copy))
                return RenderGraphPassWorkType::Copy;
            return RenderGraphPassWorkType::Graphics;
        }
        [[nodiscard]] virtual bool IsAsyncComputeCandidate() const
        {
            return HasRenderGraphNodeFlag(GetFlags(), RenderGraphNodeFlags::AsyncCandidateMetadata);
        }
        virtual void SetupFramebuffer(u32 /*width*/, u32 /*height*/) {}
        virtual void ResizeFramebuffer(u32 /*width*/, u32 /*height*/) {}
        virtual void ApplyRenderViewport(u32 /*width*/, u32 /*height*/) {}
        [[nodiscard]] virtual RenderGraphSubmissionModel GetSubmissionModel() const
        {
            return HasRenderGraphNodeFlag(GetFlags(), RenderGraphNodeFlags::UsesCommandBucket)
                       ? RenderGraphSubmissionModel::BucketOnly
                       : RenderGraphSubmissionModel::ImmediateOnly;
        }
    };
} // namespace OloEngine
