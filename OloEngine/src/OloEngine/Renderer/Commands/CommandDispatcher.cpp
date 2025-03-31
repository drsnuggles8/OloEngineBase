#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Commands/CommandDispatcher.h"
#include "OloEngine/Renderer/RenderCommand.h"

namespace OloEngine
{
    std::unordered_map<CommandType, DispatchFn> CommandDispatcher::s_DispatchFunctions;

    void CommandDispatcher::Init()
    {
        OLO_PROFILE_FUNCTION();
        OLO_CORE_INFO("Initializing CommandDispatcher");
        
        // Register all dispatch functions
        RegisterDispatchFunction(CommandType::DrawIndexed, DispatchDrawIndexed);
        RegisterDispatchFunction(CommandType::DrawIndexedInstanced, DispatchDrawIndexedInstanced);
        RegisterDispatchFunction(CommandType::SetBlendState, DispatchSetBlendState);
        RegisterDispatchFunction(CommandType::SetDepthState, DispatchSetDepthState);
        RegisterDispatchFunction(CommandType::SetStencilState, DispatchSetStencilState);
        RegisterDispatchFunction(CommandType::SetCullingState, DispatchSetCullingState);
        RegisterDispatchFunction(CommandType::SetLineWidth, DispatchSetLineWidth);
        RegisterDispatchFunction(CommandType::SetPolygonMode, DispatchSetPolygonMode);
        RegisterDispatchFunction(CommandType::SetScissorState, DispatchSetScissorState);
        RegisterDispatchFunction(CommandType::SetColorMask, DispatchSetColorMask);
        RegisterDispatchFunction(CommandType::SetPolygonOffset, DispatchSetPolygonOffset);
        RegisterDispatchFunction(CommandType::SetMultisampling, DispatchSetMultisampling);
        RegisterDispatchFunction(CommandType::SetTexture, DispatchSetTexture);
    }

    void CommandDispatcher::Shutdown()
    {
        s_DispatchFunctions.clear();
    }

    void CommandDispatcher::RegisterDispatchFunction(CommandType type, DispatchFn dispatchFn)
    {
        s_DispatchFunctions[type] = dispatchFn;
    }

    DispatchFn CommandDispatcher::GetDispatchFunction(CommandType type)
    {
        auto it = s_DispatchFunctions.find(type);
        if (it != s_DispatchFunctions.end())
            return it->second;
            
        OLO_CORE_WARN("No dispatch function registered for command type {}", static_cast<int>(type));
        return nullptr;
    }

    void CommandDispatcher::Execute(const CommandPacket* packet)
    {
        if (!packet)
            return;
            
        auto dispatchFn = GetDispatchFunction(packet->Header.Type);
        if (dispatchFn)
        {
            dispatchFn(&packet->Header);
        }
    }

    // Dispatch implementations
    void CommandDispatcher::DispatchDrawIndexed(const void* commandData)
    {
        auto* cmd = static_cast<const DrawIndexedCommand*>(commandData);

        // Bind vertex array
        glBindVertexArray(cmd->VertexArrayID);
        
        // Bind index buffer if needed
        if (cmd->IndexBufferID != 0)
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, cmd->IndexBufferID);
        
        // Draw
        if (cmd->IndexCount > 0)
        {
            glDrawElements(
                GL_TRIANGLES, 
                cmd->IndexCount, 
                GL_UNSIGNED_INT, 
                reinterpret_cast<const void*>(static_cast<intptr_t>(cmd->StartIndex * sizeof(u32)))
            );
        }
        else
        {
            // Draw all indices
            GLint indexCount;
            glGetBufferParameteriv(GL_ELEMENT_ARRAY_BUFFER, GL_BUFFER_SIZE, &indexCount);
            indexCount /= sizeof(u32);
            glDrawElements(GL_TRIANGLES, indexCount, GL_UNSIGNED_INT, nullptr);
        }
    }

    void CommandDispatcher::DispatchDrawIndexedInstanced(const void* commandData)
    {
        auto* cmd = static_cast<const DrawIndexedInstancedCommand*>(commandData);
        
        // Bind vertex array
        glBindVertexArray(cmd->VertexArrayID);
        
        // Bind index buffer if needed
        if (cmd->IndexBufferID != 0)
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, cmd->IndexBufferID);
        
        // Draw instanced
        if (cmd->IndexCount > 0)
        {
            glDrawElementsInstanced(
                GL_TRIANGLES, 
                cmd->IndexCount, 
                GL_UNSIGNED_INT, 
                reinterpret_cast<const void*>(static_cast<intptr_t>(cmd->StartIndex * sizeof(u32))), 
                cmd->InstanceCount
            );
        }
        else
        {
            // Draw all indices
            GLint indexCount;
            glGetBufferParameteriv(GL_ELEMENT_ARRAY_BUFFER, GL_BUFFER_SIZE, &indexCount);
            indexCount /= sizeof(u32);
            glDrawElementsInstanced(GL_TRIANGLES, indexCount, GL_UNSIGNED_INT, nullptr, cmd->InstanceCount);
        }
    }

    void CommandDispatcher::DispatchSetBlendState(const void* commandData)
    {
        auto* cmd = static_cast<const SetBlendStateCommand*>(commandData);
        
        if (cmd->Enabled)
        {
            glEnable(GL_BLEND);
            glBlendFunc(cmd->SrcFactor, cmd->DstFactor);
            glBlendEquation(cmd->Equation);
        }
        else
        {
            glDisable(GL_BLEND);
        }
    }

    void CommandDispatcher::DispatchSetDepthState(const void* commandData)
    {
        auto* cmd = static_cast<const SetDepthStateCommand*>(commandData);
        
        if (cmd->TestEnabled)
        {
            glEnable(GL_DEPTH_TEST);
            glDepthFunc(cmd->Function);
        }
        else
        {
            glDisable(GL_DEPTH_TEST);
        }
        
        glDepthMask(cmd->WriteMask ? GL_TRUE : GL_FALSE);
    }

    void CommandDispatcher::DispatchSetStencilState(const void* commandData)
    {
        auto* cmd = static_cast<const SetStencilStateCommand*>(commandData);
        
        if (cmd->Enabled)
        {
            glEnable(GL_STENCIL_TEST);
            glStencilFunc(cmd->Function, cmd->Reference, cmd->ReadMask);
            glStencilMask(cmd->WriteMask);
            glStencilOp(cmd->StencilFail, cmd->DepthFail, cmd->DepthPass);
        }
        else
        {
            glDisable(GL_STENCIL_TEST);
        }
    }

    void CommandDispatcher::DispatchSetCullingState(const void* commandData)
    {
        auto* cmd = static_cast<const SetCullingStateCommand*>(commandData);
        
        if (cmd->Enabled)
        {
            glEnable(GL_CULL_FACE);
            glCullFace(cmd->Face);
        }
        else
        {
            glDisable(GL_CULL_FACE);
        }
    }

    void CommandDispatcher::DispatchSetLineWidth(const void* commandData)
    {
        auto* cmd = static_cast<const SetLineWidthCommand*>(commandData);
        glLineWidth(cmd->Width);
    }

    void CommandDispatcher::DispatchSetPolygonMode(const void* commandData)
    {
        auto* cmd = static_cast<const SetPolygonModeCommand*>(commandData);
        glPolygonMode(cmd->Face, cmd->Mode);
    }

    void CommandDispatcher::DispatchSetScissorState(const void* commandData)
    {
        auto* cmd = static_cast<const SetScissorStateCommand*>(commandData);
        
        if (cmd->Enabled)
        {
            glEnable(GL_SCISSOR_TEST);
            glScissor(cmd->X, cmd->Y, cmd->Width, cmd->Height);
        }
        else
        {
            glDisable(GL_SCISSOR_TEST);
        }
    }

    void CommandDispatcher::DispatchSetColorMask(const void* commandData)
    {
        auto* cmd = static_cast<const SetColorMaskCommand*>(commandData);
        glColorMask(
            cmd->Red ? GL_TRUE : GL_FALSE,
            cmd->Green ? GL_TRUE : GL_FALSE,
            cmd->Blue ? GL_TRUE : GL_FALSE,
            cmd->Alpha ? GL_TRUE : GL_FALSE
        );
    }

    void CommandDispatcher::DispatchSetPolygonOffset(const void* commandData)
    {
        auto* cmd = static_cast<const SetPolygonOffsetCommand*>(commandData);
        
        if (cmd->Enabled)
        {
            glEnable(GL_POLYGON_OFFSET_FILL);
            glPolygonOffset(cmd->Factor, cmd->Units);
        }
        else
        {
            glDisable(GL_POLYGON_OFFSET_FILL);
        }
    }

    void CommandDispatcher::DispatchSetMultisampling(const void* commandData)
    {
        auto* cmd = static_cast<const SetMultisamplingCommand*>(commandData);
        
        if (cmd->Enabled)
        {
            glEnable(GL_MULTISAMPLE);
        }
        else
        {
            glDisable(GL_MULTISAMPLE);
        }
    }

    void CommandDispatcher::DispatchSetTexture(const void* commandData)
    {
        auto* cmd = static_cast<const SetTextureCommand*>(commandData);
        
        glActiveTexture(GL_TEXTURE0 + cmd->Slot);
        glBindTexture(cmd->Target, cmd->TextureID);
    }
}