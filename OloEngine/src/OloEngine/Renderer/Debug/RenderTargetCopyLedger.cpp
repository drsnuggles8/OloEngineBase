#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Debug/RenderTargetCopyLedger.h"

#include "OloEngine/Core/DebugLevers.h"
#include "OloEngine/Renderer/RenderGraph.h"

#include <algorithm>
#include <map>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace OloEngine
{
    namespace RenderTargetCopyLedgerDetail
    {
        // The handles as issued, named at EndGraphFrame: naming needs the
        // graph's maps, which a worker lane must not walk mid-frame.
        struct PendingCopy
        {
            FString Pass;
            // Textures for an image copy, framebuffers for a blit.
            RHI::ResourceHandle Source;
            RHI::ResourceHandle Destination;
            u32 Width = 0;
            u32 Height = 0;
            bool IsBlit = false;
            bool Depth = false;
            u32 ReadAttachment = 0;
            // The draw attachment a colour blit writes; the read index when the
            // destination recorded no single selection.
            u32 DrawAttachment = 0;
        };
    } // namespace RenderTargetCopyLedgerDetail

    // The string owns relocatable storage; the rest are scalars and handles.
    template<>
    struct TIsTriviallyRelocatable<RenderTargetCopyLedgerDetail::PendingCopy>
    {
        static constexpr bool Value = TIsTriviallyRelocatable_V<decltype(RenderTargetCopyLedgerDetail::PendingCopy::Pass)>;
    };

    namespace RenderTargetCopyLedgerDetail
    {
        // One executing graph's copies. Graphs nest (a graph executed from
        // inside another's pass), so the open frames are a stack and a copy
        // belongs to the innermost one.
        struct OpenFrame
        {
            const RenderGraph* Graph = nullptr;
            TArray<PendingCopy> Pending;
        };
    } // namespace RenderTargetCopyLedgerDetail

    template<>
    struct TIsTriviallyRelocatable<RenderTargetCopyLedgerDetail::OpenFrame>
    {
        static constexpr bool Value = TIsTriviallyRelocatable_V<decltype(RenderTargetCopyLedgerDetail::OpenFrame::Pending)>;
    };

    u64 RenderTargetCopyFrame::KnownBytes() const
    {
        u64 total = 0;
        for (const auto& copy : Copies)
            total += copy.Bytes.value_or(0);
        return total;
    }

    u32 RenderTargetCopyFrame::CountInto(std::string_view destination) const
    {
        u32 count = 0;
        for (const auto& copy : Copies)
            count += copy.Destination.ToView() == destination ? 1u : 0u;
        return count;
    }

    u64 RenderTargetCopyFrame::BytesInto(std::string_view destination) const
    {
        u64 total = 0;
        for (const auto& copy : Copies)
            if (copy.Destination.ToView() == destination)
                total += copy.Bytes.value_or(0);
        return total;
    }

    namespace RenderTargetCopyLedger
    {
        namespace
        {
            using RenderTargetCopyLedgerDetail::OpenFrame;
            using RenderTargetCopyLedgerDetail::PendingCopy;

            struct LedgerState
            {
                std::mutex Mutex;
                TArray<OpenFrame> Open;
                // The colour attachment each framebuffer's blits read from.
                std::unordered_map<RHI::ResourceHandle, u32> ReadAttachments;
                std::unordered_map<RHI::ResourceHandle, u32> DrawAttachments;
                RenderTargetCopyFrame Last;
                u64 Serial = 0;
            };

            LedgerState& State()
            {
                static LedgerState s_State;
                return s_State;
            }

            thread_local std::string_view t_ActivePass;

            // The pass a copy outside every pass is charged to.
            [[nodiscard]] FString PassOrPhase()
            {
                return t_ActivePass.empty() ? FString("<extraction>") : FString(t_ActivePass);
            }

            constexpr u64 kLogEveryFrames = 120;

            void LogSummary(const RenderTargetCopyFrame& frame)
            {
                OLO_CORE_INFO("[CopyLedger] graph frame {}: {} copies and blits, {} bytes of known size",
                              frame.Serial, frame.Copies.Num(), frame.KnownBytes());
                // Grouped by pass and destination so a steady frame logs a few lines.
                std::map<std::pair<std::string, std::string>, std::pair<u32, u64>> groups;
                for (const auto& copy : frame.Copies)
                {
                    auto& [count, bytes] = groups[{ copy.Pass.ToStdString(),
                                                    std::string(copy.IsBlit ? "blit " : "copy ") + copy.Source.ToStdString() +
                                                        " -> " + copy.Destination.ToStdString() +
                                                        (copy.DestinationDeclared ? "" : " (UNDECLARED)") }];
                    ++count;
                    bytes += copy.Bytes.value_or(0);
                }
                for (const auto& [key, value] : groups)
                    OLO_CORE_INFO("[CopyLedger]   {}: {} x{} ({} bytes)", key.first, key.second, value.first, value.second);
            }
        } // namespace

        bool IsRecording()
        {
            return Levers::RenderGraphCopyLedger();
        }

        void Record(const RHI::ResourceHandle source, const RHI::ResourceHandle destination, const u32 width, const u32 height)
        {
            auto& state = State();
            std::scoped_lock lock(state.Mutex);
            if (state.Open.IsEmpty())
                return;
            state.Open.Last().Pending.Add(PendingCopy{
                .Pass = PassOrPhase(),
                .Source = source,
                .Destination = destination,
                .Width = width,
                .Height = height,
            });
        }

        void RecordBlit(const RHI::ResourceHandle sourceFramebuffer, const RHI::ResourceHandle destinationFramebuffer,
                        const u32 width, const u32 height, const bool depth)
        {
            auto& state = State();
            std::scoped_lock lock(state.Mutex);
            if (state.Open.IsEmpty())
                return;
            const auto readIt = state.ReadAttachments.find(sourceFramebuffer);
            const u32 readAttachment = readIt != state.ReadAttachments.end() ? readIt->second : 0u;
            const auto drawIt = state.DrawAttachments.find(destinationFramebuffer);
            state.Open.Last().Pending.Add(PendingCopy{
                .Pass = PassOrPhase(),
                .Source = sourceFramebuffer,
                .Destination = destinationFramebuffer,
                .Width = width,
                .Height = height,
                .IsBlit = true,
                .Depth = depth,
                .ReadAttachment = readAttachment,
                .DrawAttachment = drawIt != state.DrawAttachments.end() ? drawIt->second : readAttachment,
            });
        }

        void NoteReadAttachment(const RHI::ResourceHandle framebuffer, const u32 attachmentIndex)
        {
            auto& state = State();
            std::scoped_lock lock(state.Mutex);
            state.ReadAttachments[framebuffer] = attachmentIndex;
        }

        void NoteDrawAttachments(const RHI::ResourceHandle framebuffer, const std::span<const u32> attachmentIndices)
        {
            auto& state = State();
            std::scoped_lock lock(state.Mutex);
            if (attachmentIndices.size() == 1u)
                state.DrawAttachments[framebuffer] = attachmentIndices[0];
            else
                state.DrawAttachments.erase(framebuffer);
        }

        void BeginGraphFrame(const RenderGraph& graph)
        {
            if (!IsRecording())
                return;
            auto& state = State();
            std::scoped_lock lock(state.Mutex);
            state.Open.Add(OpenFrame{ .Graph = &graph });
        }

        void EndGraphFrame(const RenderGraph& graph)
        {
            TArray<PendingCopy> pending;
            {
                auto& state = State();
                std::scoped_lock lock(state.Mutex);
                // Innermost first: a nested graph closes before its host.
                i32 index = state.Open.Num() - 1;
                while (index >= 0 && state.Open[index].Graph != &graph)
                    --index;
                if (index < 0)
                    return;
                pending = std::move(state.Open[index].Pending);
                state.Open.RemoveAt(index);
            }

            RenderTargetCopyFrame frame;
            frame.Copies.Reserve(pending.Num());
            for (const auto& copy : pending)
            {
                const std::optional<u32> sourceAttachment = copy.Depth ? std::nullopt : std::optional<u32>(copy.ReadAttachment);
                const auto source = copy.IsBlit ? graph.DescribeBlitOperand(copy.Source, sourceAttachment, copy.Pass.ToView(), false)
                                                : graph.DescribeCopyOperand(copy.Source, copy.Pass.ToView(), false);
                const std::optional<u32> destinationAttachment = copy.Depth ? std::nullopt : std::optional<u32>(copy.DrawAttachment);
                const auto destination = copy.IsBlit ? graph.DescribeBlitOperand(copy.Destination, destinationAttachment, copy.Pass.ToView(), true)
                                                     : graph.DescribeCopyOperand(copy.Destination, copy.Pass.ToView(), true);
                // A blit moves the SOURCE texels (an MSAA resolve reads more);
                // a copy is format-identical, so either operand sizes it.
                const std::optional<u32> texelBytes = copy.IsBlit ? (source.BytesPerTexel ? source.BytesPerTexel : destination.BytesPerTexel)
                                                                  : (destination.BytesPerTexel ? destination.BytesPerTexel : source.BytesPerTexel);
                // Declared by a contract rather than a pass: the end-of-frame
                // history and external sinks, and a debug hook's capture clone.
                const bool declaredOutsideAPass = copy.Pass.ToView() == "<extraction>" ||
                                                  copy.Pass.ToView() == "<post-pass hook>";
                frame.Copies.Add(RenderTargetCopyRecord{
                    .Pass = copy.Pass,
                    .Source = source.Name,
                    .Destination = destination.Name,
                    .DestinationDeclared = declaredOutsideAPass || destination.DeclaredByPass,
                    .Width = copy.Width,
                    .Height = copy.Height,
                    .Bytes = texelBytes ? std::optional<u64>(static_cast<u64>(copy.Width) * copy.Height * *texelBytes)
                                        : std::nullopt,
                    .IsBlit = copy.IsBlit,
                    .SourceSamples = std::max(source.Samples, 1u),
                });
            }

            auto& state = State();
            std::scoped_lock lock(state.Mutex);
            frame.Serial = ++state.Serial;
            if (frame.Serial % kLogEveryFrames == 1u)
                LogSummary(frame);
            state.Last = std::move(frame);
        }

        RenderTargetCopyFrame GetLastFrame()
        {
            auto& state = State();
            std::scoped_lock lock(state.Mutex);
            return state.Last;
        }

        ScopedPass::ScopedPass(const std::string_view passName) : m_Previous(t_ActivePass)
        {
            t_ActivePass = passName;
        }

        ScopedPass::~ScopedPass()
        {
            t_ActivePass = m_Previous;
        }
    } // namespace RenderTargetCopyLedger
} // namespace OloEngine
