#pragma once

#include <glad/gl.h>
#include <glm/glm.hpp>
#include <memory>
#include <vector>
#include <cstdint>
#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"

namespace OloEngine
{
    // Enum for all renderer state types
    enum class StateType : u32
    {
        None = 0,
        Blend,
        Depth,
        Stencil,
        Culling,
        LineWidth,
        PolygonMode,
        Scissor,
        ColorMask,
        PolygonOffset,
        Multisampling
    };

    // Base for all state structs
    struct RenderStateBase
    {
        StateType Type = StateType::None;
        virtual ~RenderStateBase() = default;
        virtual bool operator==(const RenderStateBase& other) const
        {
            return Type == other.Type;
        }
        virtual bool operator!=(const RenderStateBase& other) const
        {
            return !(*this == other);
        }
    };

    struct BlendState : public RenderStateBase
    {
        BlendState()
        {
            Type = StateType::Blend;
        }
        bool Enabled = false;
        GLenum SrcFactor = GL_SRC_ALPHA;
        GLenum DstFactor = GL_ONE_MINUS_SRC_ALPHA;
        GLenum Equation = GL_FUNC_ADD;
        bool operator==(const RenderStateBase& other) const override
        {
            if (Type != other.Type)
                return false;
            const auto& o = static_cast<const BlendState&>(other);
            return Enabled == o.Enabled && (!Enabled || (SrcFactor == o.SrcFactor && DstFactor == o.DstFactor && Equation == o.Equation));
        }
    };

    struct DepthState : public RenderStateBase
    {
        DepthState()
        {
            Type = StateType::Depth;
        }
        bool TestEnabled = true;
        bool WriteMask = true;
        GLenum Function = GL_LESS;
        bool operator==(const RenderStateBase& other) const override
        {
            if (Type != other.Type)
                return false;
            const auto& o = static_cast<const DepthState&>(other);
            return TestEnabled == o.TestEnabled && (!TestEnabled || (WriteMask == o.WriteMask && Function == o.Function));
        }
    };

    struct StencilState : public RenderStateBase
    {
        StencilState()
        {
            Type = StateType::Stencil;
        }
        bool Enabled = false;
        GLenum Function = GL_ALWAYS;
        GLint Reference = 0;
        GLuint ReadMask = 0xFF;
        GLuint WriteMask = 0xFF;
        GLenum StencilFail = GL_KEEP;
        GLenum DepthFail = GL_KEEP;
        GLenum DepthPass = GL_KEEP;
        bool operator==(const RenderStateBase& other) const override
        {
            if (Type != other.Type)
                return false;
            const auto& o = static_cast<const StencilState&>(other);
            return Enabled == o.Enabled && (!Enabled || (Function == o.Function && Reference == o.Reference && ReadMask == o.ReadMask && WriteMask == o.WriteMask && StencilFail == o.StencilFail && DepthFail == o.DepthFail && DepthPass == o.DepthPass));
        }
    };

    struct CullingState : public RenderStateBase
    {
        CullingState()
        {
            Type = StateType::Culling;
        }
        bool Enabled = false;
        GLenum Face = GL_BACK;
        bool operator==(const RenderStateBase& other) const override
        {
            if (Type != other.Type)
                return false;
            const auto& o = static_cast<const CullingState&>(other);
            return Enabled == o.Enabled && (!Enabled || Face == o.Face);
        }
    };

    struct LineWidthState : public RenderStateBase
    {
        LineWidthState()
        {
            Type = StateType::LineWidth;
        }
        f32 Width = 1.0f;
        bool operator==(const RenderStateBase& other) const override
        {
            if (Type != other.Type)
                return false;
            const auto& o = static_cast<const LineWidthState&>(other);
            return Width == o.Width;
        }
    };

    struct PolygonModeState : public RenderStateBase
    {
        PolygonModeState()
        {
            Type = StateType::PolygonMode;
        }
        GLenum Face = GL_FRONT_AND_BACK;
        GLenum Mode = GL_FILL;
        bool operator==(const RenderStateBase& other) const override
        {
            if (Type != other.Type)
                return false;
            const auto& o = static_cast<const PolygonModeState&>(other);
            return Face == o.Face && Mode == o.Mode;
        }
    };

    struct ScissorState : public RenderStateBase
    {
        ScissorState()
        {
            Type = StateType::Scissor;
        }
        bool Enabled = false;
        GLint X = 0, Y = 0;
        GLsizei Width = 0, Height = 0;
        bool operator==(const RenderStateBase& other) const override
        {
            if (Type != other.Type)
                return false;
            const auto& o = static_cast<const ScissorState&>(other);
            return Enabled == o.Enabled && (!Enabled || (X == o.X && Y == o.Y && Width == o.Width && Height == o.Height));
        }
    };

    struct ColorMaskState : public RenderStateBase
    {
        ColorMaskState()
        {
            Type = StateType::ColorMask;
        }
        bool Red = true, Green = true, Blue = true, Alpha = true;
        bool operator==(const RenderStateBase& other) const override
        {
            if (Type != other.Type)
                return false;
            const auto& o = static_cast<const ColorMaskState&>(other);
            return Red == o.Red && Green == o.Green && Blue == o.Blue && Alpha == o.Alpha;
        }
    };

    struct PolygonOffsetState : public RenderStateBase
    {
        PolygonOffsetState()
        {
            Type = StateType::PolygonOffset;
        }
        bool Enabled = false;
        f32 Factor = 0.0f;
        f32 Units = 0.0f;
        bool operator==(const RenderStateBase& other) const override
        {
            if (Type != other.Type)
                return false;
            const auto& o = static_cast<const PolygonOffsetState&>(other);
            return Enabled == o.Enabled && (!Enabled || (Factor == o.Factor && Units == o.Units));
        }
    };

    struct MultisamplingState : public RenderStateBase
    {
        MultisamplingState()
        {
            Type = StateType::Multisampling;
        }
        bool Enabled = true;
        bool operator==(const RenderStateBase& other) const override
        {
            if (Type != other.Type)
                return false;
            const auto& o = static_cast<const MultisamplingState&>(other);
            return Enabled == o.Enabled;
        }
    };

    // Composite state for a draw call
    class RenderState : public RefCounted
    {
      public:
        BlendState Blend;
        DepthState Depth;
        StencilState Stencil;
        CullingState Culling;
        LineWidthState LineWidth;
        PolygonModeState PolygonMode;
        ScissorState Scissor;
        ColorMaskState ColorMask;
        PolygonOffsetState PolygonOffset;
        MultisamplingState Multisampling;
    };
} // namespace OloEngine
