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

        // Check if this is a draw command.
        //
        // MUST match CommandBucket's own draw-call tally (CommandBucket.cpp) —
        // that list is the source of truth for "this packet issues a draw", and
        // this one silently disagreed with it for four types: DrawTerrainPatch,
        // DrawVoxelMesh, DrawDecal and DrawFoliageLayer. The consequence was
        // not cosmetic: MCP frame capture reported every terrain patch as
        // `isDraw:false` and FrameCaptureStats.DrawCalls undercounted by the
        // whole terrain/foliage/decal load, so a capture taken to answer "what
        // is this frame drawing?" answered it wrongly (issue #607, fixed with
        // #714 — which made it worse by collapsing terrain into ONE indirect
        // draw that also went uncounted).
        bool IsDrawCommand() const
        {
            switch (m_CommandType)
            {
                case CommandType::DrawMesh:
                case CommandType::DrawMeshInstanced:
                case CommandType::DrawQuad:
                case CommandType::DrawDecal:
                case CommandType::DrawFoliageLayer:
                case CommandType::DrawTerrainPatch:
                case CommandType::DrawVoxelMesh:
                case CommandType::DrawSkybox:
                case CommandType::DrawInfiniteGrid:
                case CommandType::DrawArrays:
                case CommandType::DrawIndexed:
                case CommandType::DrawIndexedInstanced:
                case CommandType::DrawLines:
                    return true;
                default:
                    return false;
            }
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

    // One render-graph pass's captured command bucket. The whole-frame capture
    // accumulates one of these per command-bucket pass that executed this frame
    // (SceneRenderPass, WaterRenderPass, FoliageRenderPass, DecalRenderPass,
    // ForwardOverlayPass), so olo_render_frame_breakdown can list every pass's
    // commands rather than only the scene pass's (issue #463 / #316).
    //
    // PassName is the graph node's GetName(); the three stage lists mirror
    // CapturedFrameData's own top-level lists (which remain the *source* / scene
    // pass for backward compatibility). The Has* flags distinguish "stage
    // captured, zero commands" from "stage not captured" — an empty PostBatch can
    // mean either, and the stats derivation at commit needs to tell them apart.
    // Stats carries this pass's own sort/batch/execute timings (zero when the
    // pass did not record them — only the scene pass does today).
    struct CapturedPassData
    {
        std::string PassName;

        std::vector<CapturedCommandData> PreSortCommands;   // Submission order
        std::vector<CapturedCommandData> PostSortCommands;  // After radix sort
        std::vector<CapturedCommandData> PostBatchCommands; // After batching

        bool HasPreSort = false;
        bool HasPostSort = false;
        bool HasPostBatch = false;

        FrameCaptureStats Stats;
    };

    // A fully captured frame with commands at different pipeline stages
    struct CapturedFrameData
    {
        u32 FrameNumber = 0;
        f64 TimestampSeconds = 0.0;

        // Name of the render-graph pass that drove this capture — i.e. the pass
        // whose command bucket the top-level PreSort/PostSort/PostBatch lists were
        // copied from (SceneRenderPass today; recorded rather than hard-coded so the
        // olo_render_frame_breakdown MCP tool can attribute every captured command
        // to a real graph pass). Empty when the capture was produced outside a named
        // pass (e.g. a synthetic test frame). The top-level lists describe THIS
        // (source) pass; `Passes` below holds every captured pass including this one.
        std::string SourcePassName;

        // Commands at different pipeline stages (the SOURCE / scene pass — kept as
        // the top-level view for backward compatibility with olo_perf_capture_frame,
        // the Command Bucket Inspector markdown report, and the single-pass tests).
        std::vector<CapturedCommandData> PreSortCommands;   // Submission order
        std::vector<CapturedCommandData> PostSortCommands;  // After radix sort
        std::vector<CapturedCommandData> PostBatchCommands; // After batching

        // Per-pass captured command buckets for the whole render graph (issue
        // #463 / #316). One entry per command-bucket pass that executed
        // this frame, in execution order. Empty for a legacy single-pass capture
        // (the top-level lists above are then the only view).
        std::vector<CapturedPassData> Passes;

        // Deep-copied snapshots of per-frame render state and material data tables.
        // These are captured at frame-end so that the debugger can inspect the exact
        // data that was active during the captured frame, rather than reading the live
        // FrameDataBuffer (which gets overwritten every frame).
        std::vector<PODRenderState> RenderStateSnapshot;
        std::vector<PODMaterialData> MaterialDataSnapshot;

        const PODRenderState* GetSnapshotRenderState(u16 index) const
        {
            return index < static_cast<u16>(RenderStateSnapshot.size()) ? &RenderStateSnapshot[index] : nullptr;
        }

        const PODMaterialData* GetSnapshotMaterialData(u16 index) const
        {
            return index < static_cast<u16>(MaterialDataSnapshot.size()) ? &MaterialDataSnapshot[index] : nullptr;
        }

        FrameCaptureStats Stats;
        std::string Notes;
    };

} // namespace OloEngine
