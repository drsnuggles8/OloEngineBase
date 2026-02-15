#include "OloEnginePCH.h"
#include "CapturedFrameData.h"

namespace OloEngine
{
    const char* CapturedCommandData::GetCommandTypeString() const
    {
        switch (m_CommandType)
        {
            case CommandType::Invalid:              return "Invalid";
            case CommandType::Clear:                return "Clear";
            case CommandType::ClearStencil:         return "ClearStencil";
            case CommandType::DrawArrays:           return "DrawArrays";
            case CommandType::DrawIndexed:          return "DrawIndexed";
            case CommandType::DrawIndexedInstanced: return "DrawIndexedInstanced";
            case CommandType::DrawLines:            return "DrawLines";
            case CommandType::DrawMesh:             return "DrawMesh";
            case CommandType::DrawMeshInstanced:    return "DrawMeshInstanced";
            case CommandType::DrawSkybox:           return "DrawSkybox";
            case CommandType::DrawInfiniteGrid:     return "DrawInfiniteGrid";
            case CommandType::DrawQuad:             return "DrawQuad";
            case CommandType::BindDefaultFramebuffer: return "BindDefaultFramebuffer";
            case CommandType::BindTexture:          return "BindTexture";
            case CommandType::SetShaderResource:    return "SetShaderResource";
            case CommandType::SetViewport:          return "SetViewport";
            case CommandType::SetClearColor:        return "SetClearColor";
            case CommandType::SetBlendState:        return "SetBlendState";
            case CommandType::SetBlendFunc:         return "SetBlendFunc";
            case CommandType::SetBlendEquation:     return "SetBlendEquation";
            case CommandType::SetDepthTest:         return "SetDepthTest";
            case CommandType::SetDepthMask:         return "SetDepthMask";
            case CommandType::SetDepthFunc:         return "SetDepthFunc";
            case CommandType::SetStencilTest:       return "SetStencilTest";
            case CommandType::SetStencilFunc:       return "SetStencilFunc";
            case CommandType::SetStencilMask:       return "SetStencilMask";
            case CommandType::SetStencilOp:         return "SetStencilOp";
            case CommandType::SetCulling:           return "SetCulling";
            case CommandType::SetCullFace:          return "SetCullFace";
            case CommandType::SetLineWidth:         return "SetLineWidth";
            case CommandType::SetPolygonMode:       return "SetPolygonMode";
            case CommandType::SetPolygonOffset:     return "SetPolygonOffset";
            case CommandType::SetScissorTest:       return "SetScissorTest";
            case CommandType::SetScissorBox:        return "SetScissorBox";
            case CommandType::SetColorMask:         return "SetColorMask";
            case CommandType::SetMultisampling:     return "SetMultisampling";
            default:                                return "Unknown";
        }
    }
} // namespace OloEngine
