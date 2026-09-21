#pragma once

// =============================================================================
// DebugViewProvenance.h — "which frame, which pass, which version of the
// G-Buffer is this debug image?" (issue #1329).
//
// A G-Buffer debug view is read by someone diagnosing a defect, so the one
// thing it may never do is describe a surface state the lighting consumer
// never saw. The extraction itself is ordered correctly by the render graph
// (GBufferDebugPass runs after every late G-Buffer writer); this record is the
// part that makes that ORDERING VISIBLE — and, more importantly, makes its
// absence visible. Three failure modes it is here to name:
//
//   * The extraction ran before a late writer (virtual geometry, deferred
//     two-phase occlusion, opaque decals). `GBufferWriteVersion` then trails
//     `GBufferFinalVersion` and `Stage` reads `Intermediate`.
//   * The extraction did not run at all this frame — the debug channel was
//     switched off, the path is no longer Deferred, the editor is iconified —
//     and the viewport is still showing the last image it produced. `Frame`
//     then trails the frame being presented, and `IsCurrent()` says so.
//   * The extraction ran on a deliberately intermediate version. That is
//     allowed; it is not allowed to be indistinguishable from the final one.
//
// A record is data a diagnostic tool prints, never something the renderer
// branches on: nothing in the frame reads it back.
// =============================================================================

#include "OloEngine/Core/Base.h"

#include <string>

namespace OloEngine
{
    // Where in the G-Buffer's write sequence a debug view was extracted.
    enum class DebugViewStage : u8
    {
        // Nothing has been extracted (yet, or since the view was switched off).
        None = 0,
        // Extracted after every G-Buffer writer that ran this frame — the
        // version DeferredLightingPass goes on to consume.
        FinalGBuffer = 1,
        // Extracted before at least one writer that went on to change the
        // attachments. Legitimate for a deliberate mid-pipeline capture; a
        // defect anywhere else.
        Intermediate = 2,
    };

    [[nodiscard]] const char* DebugViewStageName(DebugViewStage stage) noexcept;

    // One debug extraction. Filled in by the pass that performs it.
    struct DebugViewProvenance
    {
        // The producing frame's ordinal: FrameResourceManager's COMPLETED-frame
        // count as it stood while the extraction ran. Compared against the
        // current count to tell a live image from a frozen one -- see
        // IsCurrent(), which owns the one-frame offset that count implies.
        u64 Frame = 0;
        // DeferredSettings::DebugChannel (or 5's forward-path analogue).
        u32 Channel = 0;
        // GBuffer::GetWriteVersion() at the moment of extraction, and the
        // highest version the G-Buffer reached afterwards. Equal means the
        // image describes the version lighting consumed.
        //
        // THE SECOND NUMBER IS NOT WRITTEN BY THE EXTRACTING PASS, and that is
        // the whole point. A pass can only report the version it read; whether
        // that was the last one is a fact about what happens AFTER it, so the
        // producer would always be able to say "final" and be believed. So
        // GBuffer::MarkWritten reports every subsequent write to the registry
        // instead, and a write that lands after a published record demotes it
        // to Intermediate. Reorder the deferred chain and the two numbers stop
        // agreeing on their own, with nobody having to remember to update a
        // flag.
        u32 GBufferWriteVersion = 0;
        u32 GBufferFinalVersion = 0;
        u32 SampleCount = 1;
        bool PerSampleLighting = false;
        // True when the single-sample attachments this image came from were
        // refreshed by the LAST G-Buffer writer after its own writes — the
        // per-sample MSAA path's requirement, and the thing that makes the
        // image post-decal rather than post-ScenePass. False when there was
        // nothing to resolve (the attachments are already single-sample).
        bool ResolvedAfterLateWriters = false;
        DebugViewStage Stage = DebugViewStage::None;
        // Graph node that performed the extraction, and the last pass that
        // wrote the G-Buffer before it. When those disagree with what the
        // reader expects, the ordering is the thing to look at.
        std::string Pass;
        std::string LastGBufferWriter;
        bool Valid = false;
    };

    // Process-global "last debug extraction". One producer per frame (the
    // active debug pass) and any number of diagnostic readers.
    class DebugViewProvenanceRegistry
    {
      public:
        static void Publish(const DebugViewProvenance& record);

        // Called by GBuffer::MarkWritten for every G-Buffer write. A write with
        // a version above the published record's demotes that record to
        // Intermediate and records the version it is now behind — which is how
        // "the debug image predates a late writer" becomes observable rather
        // than asserted by the pass that would be wrong about it.
        static void NoteGBufferWrite(u32 newVersion, const char* writer);

        // Drop the record. Called when a debug view is switched off, so the
        // next reader sees "nothing is being extracted" rather than the last
        // image's metadata, which would read as current for one more frame.
        static void Invalidate();

        [[nodiscard]] static DebugViewProvenance Get();

        // True when the stored record describes the image now on screen.
        //
        // THE OFF-BY-ONE IS THE POINT, and it lives here so no caller has to
        // know about it. FrameResourceManager counts COMPLETED frames and
        // increments in EndFrame, so a producer running inside frame N stamps
        // N, while every reader -- the settings panel, a test, a tool -- runs
        // after EndFrame and sees N + 1. Both are accepted; two or more frames
        // of daylight mean the extraction stopped running while its image
        // stayed on screen, which is the staleness this record exists to name.
        [[nodiscard]] static bool IsCurrent(u64 currentFrame);

        // One line for a diagnostic surface (the Renderer Settings panel, a
        // log line, an MCP response). Always states the frame and the stage,
        // and says "stale" out loud rather than leaving the reader to compare
        // two numbers.
        [[nodiscard]] static std::string Describe(u64 currentFrame);
    };
} // namespace OloEngine
