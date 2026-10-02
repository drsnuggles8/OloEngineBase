#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Containers/Array.h"
#include "OloEngine/Containers/String.h"
#include "OloEngine/Renderer/RHI/RHITypes.h"

#include <optional>
#include <span>
#include <string_view>

namespace OloEngine
{
    class RenderGraph;

    // One GPU image copy issued while a render graph executed (issue #1332).
    struct RenderTargetCopyRecord
    {
        // The pass that issued the copy; "<extraction>" for the end-of-frame
        // history and external sinks, which run after the last pass.
        FString Pass;
        // The graph resource each operand IS this frame: a texture name, a
        // framebuffer attachment as "Framebuffer[i]" / "Framebuffer[depth]", or
        // "<external>" for storage the graph does not know (a renderer-owned
        // history, a capture scratch).
        FString Source;
        FString Destination;
        // The issuing pass declared a write on Destination. A copy into a graph
        // resource its pass never declared is a write no edge or barrier covers.
        // An "<extraction>" copy is declared by its history or sink contract.
        bool DestinationDeclared = false;
        u32 Width = 0;
        u32 Height = 0;
        // Width x height x the texel size of whichever operand's format is known;
        // absent when neither is. A multisample source reads SourceSamples texels
        // for each one counted here.
        std::optional<u64> Bytes;
        // A framebuffer blit (an MSAA resolve, a depth hand-over) rather than an
        // image copy. Its operands name a framebuffer attachment.
        bool IsBlit = false;
        u32 SourceSamples = 1;
    };

    // Strings own relocatable storage; the rest are scalars.
    template<>
    struct TIsTriviallyRelocatable<RenderTargetCopyRecord>
    {
        static constexpr bool Value = TIsTriviallyRelocatable_V<decltype(RenderTargetCopyRecord::Pass)> &&
                                      TIsTriviallyRelocatable_V<decltype(RenderTargetCopyRecord::Source)> &&
                                      TIsTriviallyRelocatable_V<decltype(RenderTargetCopyRecord::Destination)> &&
                                      TIsTriviallyRelocatable_V<decltype(RenderTargetCopyRecord::Bytes)>;
    };

    // Every copy of one graph Execute, named after the frame finished.
    struct RenderTargetCopyFrame
    {
        // Counts graph Executes since recording started; 0 means nothing has
        // been recorded yet.
        u64 Serial = 0;
        TArray<RenderTargetCopyRecord> Copies;

        // The sum of every copy whose size is known.
        [[nodiscard]] u64 KnownBytes() const;
        // Copies whose destination is `destination` (exact name).
        [[nodiscard]] u32 CountInto(std::string_view destination) const;
        [[nodiscard]] u64 BytesInto(std::string_view destination) const;
    };

    // THE COPY LEDGER (issue #1332). An export copy -- a pass copying a live
    // attachment into a graph texture that consumers read instead -- is
    // bandwidth on every frame and a second identity a later writer can leave
    // stale. This counts them, from the one place every copy goes through
    // (RenderCommand::CopyImageSubData*), attributed to the executing pass and
    // named by the graph at the end of the frame.
    //
    // Off unless Levers::RenderGraphCopyLedger() (OLO_RG_COPY_LEDGER) is set;
    // off, a copy costs one lever read. Copies outside a graph Execute are not
    // recorded: they are not part of the frame graph's traffic.
    namespace RenderTargetCopyLedger
    {
        [[nodiscard]] bool IsRecording();

        // Called by RenderCommand for every image copy. Thread-safe: a pass
        // recorded on a worker lane names itself through ScopedPass.
        void Record(RHI::ResourceHandle source, RHI::ResourceHandle destination, u32 width, u32 height);
        // Likewise for every framebuffer blit; a colour blit reads the attachment
        // the last SetFramebufferReadAttachment on that framebuffer selected.
        void RecordBlit(RHI::ResourceHandle sourceFramebuffer, RHI::ResourceHandle destinationFramebuffer, u32 width,
                        u32 height, bool depth);
        void NoteReadAttachment(RHI::ResourceHandle framebuffer, u32 attachmentIndex);
        // A single selected draw attachment names a colour blit's destination;
        // anything else (a restore, several attachments) forgets the selection.
        void NoteDrawAttachments(RHI::ResourceHandle framebuffer, std::span<const u32> attachmentIndices);

        // The executing graph brackets its frame. EndGraphFrame names every
        // operand through the graph, which must still hold this frame's
        // physical resources -- so it runs before the transient pool takes them
        // back.
        void BeginGraphFrame(const RenderGraph& graph);
        void EndGraphFrame(const RenderGraph& graph);

        // The last frame EndGraphFrame named.
        [[nodiscard]] RenderTargetCopyFrame GetLastFrame();

        // Names the pass whose copies follow, on the calling thread.
        class ScopedPass
        {
          public:
            explicit ScopedPass(std::string_view passName);
            ~ScopedPass();

            ScopedPass(const ScopedPass&) = delete;
            ScopedPass& operator=(const ScopedPass&) = delete;

          private:
            std::string_view m_Previous;
        };
    } // namespace RenderTargetCopyLedger
} // namespace OloEngine
