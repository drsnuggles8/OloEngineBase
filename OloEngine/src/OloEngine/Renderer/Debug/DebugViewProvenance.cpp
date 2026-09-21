#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Debug/DebugViewProvenance.h"

#include <mutex>

namespace OloEngine
{
    namespace
    {
        std::mutex s_Mutex;
        DebugViewProvenance s_Record{};
    } // namespace

    const char* DebugViewStageName(DebugViewStage stage) noexcept
    {
        switch (stage)
        {
            case DebugViewStage::FinalGBuffer:
                return "final";
            case DebugViewStage::Intermediate:
                return "intermediate";
            case DebugViewStage::None:
                break;
        }
        return "none";
    }

    void DebugViewProvenanceRegistry::Publish(const DebugViewProvenance& record)
    {
        const std::lock_guard lock(s_Mutex);
        s_Record = record;
        s_Record.Valid = true;
    }

    void DebugViewProvenanceRegistry::NoteGBufferWrite(u32 newVersion, const char* writer)
    {
        const std::lock_guard lock(s_Mutex);
        if (!s_Record.Valid || newVersion <= s_Record.GBufferWriteVersion)
            return;
        // A writer landed after the extraction published. Whatever the
        // extracting pass believed, the image it produced is now a picture of
        // an earlier version of the G-Buffer than the one the frame ends on.
        s_Record.GBufferFinalVersion = newVersion;
        s_Record.Stage = DebugViewStage::Intermediate;
        s_Record.LastGBufferWriter = writer ? writer : "";
    }

    void DebugViewProvenanceRegistry::Invalidate()
    {
        const std::lock_guard lock(s_Mutex);
        s_Record = {};
    }

    DebugViewProvenance DebugViewProvenanceRegistry::Get()
    {
        const std::lock_guard lock(s_Mutex);
        return s_Record;
    }

    bool DebugViewProvenanceRegistry::IsCurrent(u64 currentFrame)
    {
        const std::lock_guard lock(s_Mutex);
        // See the header: the producer stamps the in-flight frame, the reader
        // sees the completed count, so the two legitimately differ by one.
        return s_Record.Valid && (currentFrame == s_Record.Frame || currentFrame == s_Record.Frame + 1u);
    }

    std::string DebugViewProvenanceRegistry::Describe(u64 currentFrame)
    {
        const DebugViewProvenance record = Get();
        if (!record.Valid)
            return "no debug view extracted";

        // The freshness verdict comes FIRST, because it is the one a reader
        // acts on: an image from an earlier frame is not wrong, it is simply
        // not an answer about the frame in front of them.
        std::string out;
        if (IsCurrent(currentFrame))
        {
            out = "current (frame " + std::to_string(record.Frame) + ")";
        }
        else
        {
            out = "STALE: produced at frame " + std::to_string(record.Frame) + ", now at frame " +
                  std::to_string(currentFrame);
        }

        out += " | pass " + (record.Pass.empty() ? std::string("<unknown>") : record.Pass);
        out += " | channel " + std::to_string(record.Channel);
        out += " | G-Buffer v" + std::to_string(record.GBufferWriteVersion);
        if (record.GBufferFinalVersion != 0u && record.GBufferFinalVersion != record.GBufferWriteVersion)
        {
            out += " of v" + std::to_string(record.GBufferFinalVersion);
            out += " — A LATE WRITER CHANGED THE G-BUFFER AFTER THIS IMAGE WAS TAKEN";
        }
        out += " (" + std::string(DebugViewStageName(record.Stage)) + ")";
        if (!record.LastGBufferWriter.empty())
            out += " | last writer " + record.LastGBufferWriter;
        if (record.SampleCount > 1u)
        {
            out += " | MSAA " + std::to_string(record.SampleCount) + "x " +
                   (record.PerSampleLighting ? "per-sample" : "resolve-before-lighting");
            out += record.ResolvedAfterLateWriters ? " (resolved after late writers)" : "";
        }
        return out;
    }
} // namespace OloEngine
