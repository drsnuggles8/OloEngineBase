#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Commands/DrawKey.h"
#include "OloEngine/Renderer/Commands/RenderCommand.h"

#include <cstring>
#include <string>
#include <vector>

namespace OloEngine
{
    // Aggregate stats for a captured frame
    struct FrameCaptureStats
    {
        u32 TotalCommands = 0;
        u32 BatchedCommands = 0;
        u32 DrawCalls = 0;
        u32 StateChanges = 0;
        u32 ShaderBinds = 0;
        u32 TextureBinds = 0;
        f64 SortTimeMs = 0.0;
        f64 BatchTimeMs = 0.0;
        f64 ExecuteTimeMs = 0.0;
        f64 TotalFrameTimeMs = 0.0;
    };

    // Deep-copy of a single command packet for post-hoc analysis
    class CapturedCommandData
    {
      public:
        CapturedCommandData() = default;

        CapturedCommandData(CommandType type, const void* rawData, sizet dataSize,
                            const DrawKey& sortKey, u32 groupID, u32 executionOrder,
                            bool isStatic, bool dependsOnPrevious, const char* debugName,
                            u32 originalIndex)
            : m_CommandType(type), m_OriginalIndex(originalIndex), m_GroupID(groupID), m_ExecutionOrder(executionOrder), m_SortKey(sortKey), m_IsStatic(isStatic), m_DependsOnPrevious(dependsOnPrevious)
        {
            if (rawData && dataSize > 0)
            {
                m_CommandData.resize(dataSize);
                std::memcpy(m_CommandData.data(), rawData, dataSize);
            }

            if (debugName)
            {
                m_DebugName = debugName;
            }
        }

        ~CapturedCommandData() = default;

        // Allow move
        CapturedCommandData(CapturedCommandData&&) noexcept = default;
        CapturedCommandData& operator=(CapturedCommandData&&) noexcept = default;

        // Allow copy
        CapturedCommandData(const CapturedCommandData&) = default;
        CapturedCommandData& operator=(const CapturedCommandData&) = default;

        // Typed access to the deep-copied command data
        template<typename T>
        const T* GetCommandData() const
        {
            if (m_CommandData.size() >= sizeof(T))
            {
                return reinterpret_cast<const T*>(m_CommandData.data());
            }
            return nullptr;
        }

        const void* GetRawData() const
        {
            return m_CommandData.empty() ? nullptr : m_CommandData.data();
        }
        sizet GetDataSize() const
        {
            return m_CommandData.size();
        }

        CommandType GetCommandType() const
        {
            return m_CommandType;
        }
        u32 GetOriginalIndex() const
        {
            return m_OriginalIndex;
        }
        const DrawKey& GetSortKey() const
        {
            return m_SortKey;
        }
        u32 GetGroupID() const
        {
            return m_GroupID;
        }
        u32 GetExecutionOrder() const
        {
            return m_ExecutionOrder;
        }
        bool IsStatic() const
        {
            return m_IsStatic;
        }
        bool DependsOnPrevious() const
        {
            return m_DependsOnPrevious;
        }
        const std::string& GetDebugName() const
        {
            return m_DebugName;
        }

        f64 GetGpuTimeMs() const
        {
            return m_GpuTimeMs;
        }
        void SetGpuTimeMs(f64 timeMs)
        {
            m_GpuTimeMs = timeMs;
        }

        // Get command type as human-readable string
        const char* GetCommandTypeString() const;

        // Check if this is a draw command
        bool IsDrawCommand() const
        {
            return m_CommandType == CommandType::DrawMesh || m_CommandType == CommandType::DrawMeshInstanced || m_CommandType == CommandType::DrawQuad || m_CommandType == CommandType::DrawIndexed || m_CommandType == CommandType::DrawArrays || m_CommandType == CommandType::DrawLines || m_CommandType == CommandType::DrawSkybox || m_CommandType == CommandType::DrawInfiniteGrid || m_CommandType == CommandType::DrawIndexedInstanced;
        }

        // Check if this is a render-state command (explicit whitelist)
        bool IsStateCommand() const
        {
            switch (m_CommandType)
            {
                case CommandType::SetViewport:
                case CommandType::SetClearColor:
                case CommandType::SetBlendState:
                case CommandType::SetBlendFunc:
                case CommandType::SetBlendEquation:
                case CommandType::SetDepthTest:
                case CommandType::SetDepthMask:
                case CommandType::SetDepthFunc:
                case CommandType::SetStencilTest:
                case CommandType::SetStencilFunc:
                case CommandType::SetStencilMask:
                case CommandType::SetStencilOp:
                case CommandType::SetCulling:
                case CommandType::SetCullFace:
                case CommandType::SetLineWidth:
                case CommandType::SetPolygonMode:
                case CommandType::SetPolygonOffset:
                case CommandType::SetScissorTest:
                case CommandType::SetScissorBox:
                case CommandType::SetColorMask:
                case CommandType::SetMultisampling:
                    return true;
                default:
                    return false;
            }
        }

        // Check if this is a bind/resource command
        bool IsBindCommand() const
        {
            return m_CommandType == CommandType::BindTexture || m_CommandType == CommandType::BindDefaultFramebuffer || m_CommandType == CommandType::SetShaderResource;
        }

      private:
        CommandType m_CommandType = CommandType::Invalid;
        std::vector<u8> m_CommandData; // Deep-copied POD bytes
        u32 m_OriginalIndex = 0;       // Position in original submission order
        u32 m_GroupID = 0;
        u32 m_ExecutionOrder = 0;
        DrawKey m_SortKey;
        bool m_IsStatic = false;
        bool m_DependsOnPrevious = false;
        std::string m_DebugName;
        // GPU timing for this command (filled by GPU timer query readback).
        // Note: GPU timing values come from the *previous* frame's queries due to
        // double-buffered readback in GPUTimerQueryPool. They should be interpreted
        // as approximate per-command GPU costs rather than exact current-frame timings.
        f64 m_GpuTimeMs = 0.0;
    };

    // A fully captured frame with commands at different pipeline stages
    struct CapturedFrameData
    {
        u32 FrameNumber = 0;
        f64 TimestampSeconds = 0.0;

        // Commands at different pipeline stages
        std::vector<CapturedCommandData> PreSortCommands;   // Submission order
        std::vector<CapturedCommandData> PostSortCommands;  // After radix sort
        std::vector<CapturedCommandData> PostBatchCommands; // After batching

        FrameCaptureStats Stats;
        std::string Notes;
    };

} // namespace OloEngine
