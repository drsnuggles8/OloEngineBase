#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Commands/DrawKey.h"
#include "OloEngine/Renderer/Commands/RenderCommand.h"

#include <cstring>
#include "OloEngine/Containers/String.h"
#include "OloEngine/Containers/Array.h"

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
                m_CommandData.SetNum(dataSize, EAllowShrinking::No);
                std::memcpy(m_CommandData.GetData(), rawData, dataSize);
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
            if (m_CommandData.Num() >= sizeof(T))
            {
                return reinterpret_cast<const T*>(m_CommandData.GetData());
            }
            return nullptr;
        }

        const void* GetRawData() const
        {
            return m_CommandData.IsEmpty() ? nullptr : m_CommandData.GetData();
        }
        sizet GetDataSize() const
        {
            return m_CommandData.Num();
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
        const FString& GetDebugName() const
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

        friend struct TIsTriviallyRelocatable<CapturedCommandData>;

      private:
        CommandType m_CommandType = CommandType::Invalid;
        TArray<u8> m_CommandData; // Deep-copied POD bytes
        u32 m_OriginalIndex = 0;  // Position in original submission order
        u32 m_GroupID = 0;
        u32 m_ExecutionOrder = 0;
        DrawKey m_SortKey;
        bool m_IsStatic = false;
        bool m_DependsOnPrevious = false;
        FString m_DebugName;
        // GPU timing for this command (filled by GPU timer query readback).
        // Note: GPU timing values come from the *previous* frame's queries due to
        // double-buffered readback in GPUTimerQueryPool. They should be interpreted
        // as approximate per-command GPU costs rather than exact current-frame timings.
        f64 m_GpuTimeMs = 0.0;
    };

    // Owns external TArray byte storage and FString text; remaining command metadata is scalar/DrawKey values.
    template<>
    struct TIsTriviallyRelocatable<CapturedCommandData>
    {
        static constexpr bool Value = TIsTriviallyRelocatable<decltype(CapturedCommandData::m_CommandType)>::Value &&
                                      TIsTriviallyRelocatable<decltype(CapturedCommandData::m_CommandData)>::Value &&
                                      TIsTriviallyRelocatable<decltype(CapturedCommandData::m_OriginalIndex)>::Value &&
                                      TIsTriviallyRelocatable<decltype(CapturedCommandData::m_GroupID)>::Value &&
                                      TIsTriviallyRelocatable<decltype(CapturedCommandData::m_ExecutionOrder)>::Value &&
                                      TIsTriviallyRelocatable<decltype(CapturedCommandData::m_SortKey)>::Value &&
                                      TIsTriviallyRelocatable<decltype(CapturedCommandData::m_IsStatic)>::Value &&
                                      TIsTriviallyRelocatable<decltype(CapturedCommandData::m_DependsOnPrevious)>::Value &&
                                      TIsTriviallyRelocatable<decltype(CapturedCommandData::m_DebugName)>::Value &&
                                      TIsTriviallyRelocatable<decltype(CapturedCommandData::m_GpuTimeMs)>::Value;
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
        FString PassName;

        TArray<CapturedCommandData> PreSortCommands;   // Submission order
        TArray<CapturedCommandData> PostSortCommands;  // After radix sort
        TArray<CapturedCommandData> PostBatchCommands; // After batching

        bool HasPreSort = false;
        bool HasPostSort = false;
        bool HasPostBatch = false;

        FrameCaptureStats Stats;
    };

    // Owns FString and command arrays; capture flags and FrameCaptureStats are values.
    template<>
    struct TIsTriviallyRelocatable<CapturedPassData>
    {
        static constexpr bool Value = TIsTriviallyRelocatable<decltype(CapturedPassData::PassName)>::Value &&
                                      TIsTriviallyRelocatable<decltype(CapturedPassData::PreSortCommands)>::Value &&
                                      TIsTriviallyRelocatable<decltype(CapturedPassData::PostSortCommands)>::Value &&
                                      TIsTriviallyRelocatable<decltype(CapturedPassData::PostBatchCommands)>::Value &&
                                      TIsTriviallyRelocatable<decltype(CapturedPassData::HasPreSort)>::Value &&
                                      TIsTriviallyRelocatable<decltype(CapturedPassData::HasPostSort)>::Value &&
                                      TIsTriviallyRelocatable<decltype(CapturedPassData::HasPostBatch)>::Value &&
                                      TIsTriviallyRelocatable<decltype(CapturedPassData::Stats)>::Value;
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
        FString SourcePassName;

        // Commands at different pipeline stages (the SOURCE / scene pass — kept as
        // the top-level view for backward compatibility with olo_perf_capture_frame,
        // the Command Bucket Inspector markdown report, and the single-pass tests).
        TArray<CapturedCommandData> PreSortCommands;   // Submission order
        TArray<CapturedCommandData> PostSortCommands;  // After radix sort
        TArray<CapturedCommandData> PostBatchCommands; // After batching

        // Per-pass captured command buckets for the whole render graph (issue
        // #463 / #316). One entry per command-bucket pass that executed
        // this frame, in execution order. Empty for a legacy single-pass capture
        // (the top-level lists above are then the only view).
        TArray<CapturedPassData> Passes;

        // Deep-copied snapshots of per-frame render state and material data tables.
        // These are captured at frame-end so that the debugger can inspect the exact
        // data that was active during the captured frame, rather than reading the live
        // FrameDataBuffer (which gets overwritten every frame).
        TArray<PODRenderState> RenderStateSnapshot;
        TArray<PODMaterialData> MaterialDataSnapshot;

        const PODRenderState* GetSnapshotRenderState(u16 index) const
        {
            return index < static_cast<u16>(RenderStateSnapshot.Num()) ? &RenderStateSnapshot[index] : nullptr;
        }

        const PODMaterialData* GetSnapshotMaterialData(u16 index) const
        {
            return index < static_cast<u16>(MaterialDataSnapshot.Num()) ? &MaterialDataSnapshot[index] : nullptr;
        }

        FrameCaptureStats Stats;
        FString Notes;
    };

    // All owned storage is FString/TArray; frame counters, timestamps and stats are values.
    template<>
    struct TIsTriviallyRelocatable<CapturedFrameData>
    {
        static constexpr bool Value = TIsTriviallyRelocatable<decltype(CapturedFrameData::FrameNumber)>::Value &&
                                      TIsTriviallyRelocatable<decltype(CapturedFrameData::TimestampSeconds)>::Value &&
                                      TIsTriviallyRelocatable<decltype(CapturedFrameData::SourcePassName)>::Value &&
                                      TIsTriviallyRelocatable<decltype(CapturedFrameData::PreSortCommands)>::Value &&
                                      TIsTriviallyRelocatable<decltype(CapturedFrameData::PostSortCommands)>::Value &&
                                      TIsTriviallyRelocatable<decltype(CapturedFrameData::PostBatchCommands)>::Value &&
                                      TIsTriviallyRelocatable<decltype(CapturedFrameData::Passes)>::Value &&
                                      TIsTriviallyRelocatable<decltype(CapturedFrameData::RenderStateSnapshot)>::Value &&
                                      TIsTriviallyRelocatable<decltype(CapturedFrameData::MaterialDataSnapshot)>::Value &&
                                      TIsTriviallyRelocatable<decltype(CapturedFrameData::Stats)>::Value &&
                                      TIsTriviallyRelocatable<decltype(CapturedFrameData::Notes)>::Value;
    };
} // namespace OloEngine
