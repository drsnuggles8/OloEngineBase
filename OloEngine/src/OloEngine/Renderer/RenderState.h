#pragma once

#include "OloEngine/Core/Base.h"
#include <glad/gl.h>

namespace OloEngine
{
    // Types of renderer states that can be changed
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

    // Base state struct that all state types derive from
    struct RenderStateBase
    {
        StateType Type = StateType::None;
        
        virtual ~RenderStateBase() = default;
        virtual bool operator==(const RenderStateBase& other) const { return Type == other.Type; }
        virtual bool operator!=(const RenderStateBase& other) const { return !(*this == other); }
    };

    // Blending state
    struct BlendState : public RenderStateBase
    {
        BlendState() { Type = StateType::Blend; }
        
        bool Enabled = false;
        GLenum SrcFactor = GL_SRC_ALPHA;
        GLenum DstFactor = GL_ONE_MINUS_SRC_ALPHA;
        GLenum Equation = GL_FUNC_ADD;
        
        bool operator==(const RenderStateBase& other) const override
        {
            if (Type != other.Type) return false;
            auto& o = static_cast<const BlendState&>(other);
            return Enabled == o.Enabled && 
                   (!Enabled || (SrcFactor == o.SrcFactor && DstFactor == o.DstFactor && Equation == o.Equation));
        }
    };

    // Depth testing state
    struct DepthState : public RenderStateBase
    {
        DepthState() { Type = StateType::Depth; }
        
        bool TestEnabled = true;
        bool WriteMask = true;
        GLenum Function = GL_LESS;
        
        bool operator==(const RenderStateBase& other) const override
        {
            if (Type != other.Type) return false;
            auto& o = static_cast<const DepthState&>(other);
            return TestEnabled == o.TestEnabled && 
                   (!TestEnabled || (WriteMask == o.WriteMask && Function == o.Function));
        }
    };

    // Stencil testing state
    struct StencilState : public RenderStateBase
    {
        StencilState() { Type = StateType::Stencil; }
        
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
            if (Type != other.Type) return false;
            auto& o = static_cast<const StencilState&>(other);
            return Enabled == o.Enabled &&
                   (!Enabled || (Function == o.Function && 
                                Reference == o.Reference && 
                                ReadMask == o.ReadMask && 
                                WriteMask == o.WriteMask && 
                                StencilFail == o.StencilFail && 
                                DepthFail == o.DepthFail && 
                                DepthPass == o.DepthPass));
        }
    };

    // Culling state
    struct CullingState : public RenderStateBase
    {
        CullingState() { Type = StateType::Culling; }
        
        bool Enabled = false;
        GLenum Face = GL_BACK;
        
        bool operator==(const RenderStateBase& other) const override
        {
            if (Type != other.Type) return false;
            auto& o = static_cast<const CullingState&>(other);
            return Enabled == o.Enabled && (!Enabled || Face == o.Face);
        }
    };

    // Line width state
    struct LineWidthState : public RenderStateBase
    {
        LineWidthState() { Type = StateType::LineWidth; }
        
        f32 Width = 1.0f;
        
        bool operator==(const RenderStateBase& other) const override
        {
            if (Type != other.Type) return false;
            auto& o = static_cast<const LineWidthState&>(other);
            return Width == o.Width;
        }
    };

    // Polygon mode state
    struct PolygonModeState : public RenderStateBase
    {
        PolygonModeState() { Type = StateType::PolygonMode; }
        
        GLenum Face = GL_FRONT_AND_BACK;
        GLenum Mode = GL_FILL;
        
        bool operator==(const RenderStateBase& other) const override
        {
            if (Type != other.Type) return false;
            auto& o = static_cast<const PolygonModeState&>(other);
            return Face == o.Face && Mode == o.Mode;
        }
    };

    // Scissor test state
    struct ScissorState : public RenderStateBase
    {
        ScissorState() { Type = StateType::Scissor; }
        
        bool Enabled = false;
        GLint X = 0;
        GLint Y = 0;
        GLsizei Width = 0;
        GLsizei Height = 0;
        
        bool operator==(const RenderStateBase& other) const override
        {
            if (Type != other.Type) return false;
            auto& o = static_cast<const ScissorState&>(other);
            return Enabled == o.Enabled && 
                   (!Enabled || (X == o.X && Y == o.Y && Width == o.Width && Height == o.Height));
        }
    };

    // Color mask state
    struct ColorMaskState : public RenderStateBase
    {
        ColorMaskState() { Type = StateType::ColorMask; }
        
        bool Red = true;
        bool Green = true;
        bool Blue = true;
        bool Alpha = true;
        
        bool operator==(const RenderStateBase& other) const override
        {
            if (Type != other.Type) return false;
            auto& o = static_cast<const ColorMaskState&>(other);
            return Red == o.Red && Green == o.Green && Blue == o.Blue && Alpha == o.Alpha;
        }
    };

    // Polygon offset state
    struct PolygonOffsetState : public RenderStateBase
    {
        PolygonOffsetState() { Type = StateType::PolygonOffset; }
        
        bool Enabled = false;
        f32 Factor = 0.0f;
        f32 Units = 0.0f;
        
        bool operator==(const RenderStateBase& other) const override
        {
            if (Type != other.Type) return false;
            auto& o = static_cast<const PolygonOffsetState&>(other);
            return Enabled == o.Enabled && 
                   (!Enabled || (Factor == o.Factor && Units == o.Units));
        }
    };

    // Multisampling state
    struct MultisamplingState : public RenderStateBase
    {
        MultisamplingState() { Type = StateType::Multisampling; }
        
        bool Enabled = true;
        
        bool operator==(const RenderStateBase& other) const override
        {
            if (Type != other.Type) return false;
            auto& o = static_cast<const MultisamplingState&>(other);
            return Enabled == o.Enabled;
        }
    };
    
    // Composite state containing all render states
    struct RenderState
    {
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
}