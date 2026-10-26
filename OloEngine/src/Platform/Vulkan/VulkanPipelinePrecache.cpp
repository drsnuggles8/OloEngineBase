#include "OloEnginePCH.h"

#if OLO_WITH_VULKAN

#include "Platform/Vulkan/VulkanPipelinePrecache.h"
#include "OloEngine/Core/DebugLevers.h"
#include "OloEngine/Core/FileSystem.h"
#include "Platform/Vulkan/VulkanComputeShader.h"
#include "Platform/Vulkan/VulkanDevice.h"
#include "Platform/Vulkan/VulkanShader.h"

#include <algorithm>
#include <chrono>
#include <format>
#include <fstream>
#include <sstream>

namespace OloEngine
{
    namespace
    {
        [[nodiscard]] VulkanRenderTargetDesc ToTargets(const PipelineSignature& signature)
        {
            VulkanRenderTargetDesc targets;
            targets.ColorCount = std::min(signature.ColorCount, PipelineSignature::kMaxColorAttachments);
            for (u32 i = 0; i < targets.ColorCount; ++i)
            {
                targets.ColorFormats[i] = static_cast<VkFormat>(signature.ColorFormats[i]);
            }
            targets.DepthFormat = static_cast<VkFormat>(signature.DepthFormat);
            targets.Samples = signature.Samples;
            return targets;
        }

        [[nodiscard]] VulkanRenderTargetDesc ToTargets(const VulkanPipelineBuilder::Key& key)
        {
            VulkanRenderTargetDesc targets;
            targets.ColorCount = std::min(key.ColorCount, 8u);
            targets.ColorFormats = key.ColorFormats;
            targets.DepthFormat = key.DepthFormat;
            targets.Samples = key.Samples;
            return targets;
        }

        [[nodiscard]] u32 GraphicsStageMask(const VulkanShader& shader)
        {
            u32 mask = 0;
            for (const VkShaderStageFlagBits stage :
                 { VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT, VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT,
                   VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT, VK_SHADER_STAGE_TASK_BIT_EXT,
                   VK_SHADER_STAGE_MESH_BIT_EXT })
            {
                if (shader.GetModule(stage) != VK_NULL_HANDLE)
                {
                    mask |= static_cast<u32>(stage);
                }
            }
            return mask;
        }

        [[nodiscard]] std::optional<std::string> ReadFile(const std::filesystem::path& path)
        {
            std::error_code ec;
            if (!std::filesystem::is_regular_file(path, ec))
            {
                return std::nullopt;
            }
            std::ifstream in(path, std::ios::binary);
            if (!in)
            {
                return std::nullopt;
            }
            std::ostringstream text;
            text << in.rdbuf();
            return text.str();
        }

        // A missing file is normal (first launch); an unreadable or malformed
        // one is logged and ignored — the manifest is an optimisation, and a
        // broken one must cost a cold start, never a failed launch.
        void LoadInto(PipelineManifest& manifest, const std::filesystem::path& path, const char* what)
        {
            const auto text = ReadFile(path);
            if (!text.has_value())
            {
                return;
            }
            std::string error;
            sizet rejected = 0;
            auto parsed = PipelineManifest::Parse(*text, &error, &rejected);
            if (!parsed.has_value())
            {
                OLO_CORE_WARN("[Vulkan/PSO] {} {} ignored: {}", what, path.string(), error);
                return;
            }
            OLO_CORE_INFO("[Vulkan/PSO] {} {}: {} pipeline key(s){}", what, path.string(), parsed->GetEntryCount(),
                          rejected > 0 ? std::format(", {} invalid entr{} dropped", rejected, rejected == 1 ? "y" : "ies")
                                       : std::string{});
            manifest.Merge(*parsed);
        }
    } // namespace

    VulkanPipelinePrecache& VulkanPipelinePrecache::Get()
    {
        static auto* s_Instance = new VulkanPipelinePrecache(); // deliberately leaked, like the builder
        return *s_Instance;
    }

    void VulkanPipelinePrecache::Start(const StartOptions& options)
    {
        {
            std::lock_guard lock(m_Mutex);
            if (m_Started)
            {
                return;
            }
            m_Persist = options.LoadAndSaveManifest;
            if (m_Persist)
            {
                LoadInto(m_Manifest, std::filesystem::path(PipelineManifest::kPackagedSeedPath), "shipped manifest");
                LoadInto(m_Manifest, PipelineManifest::MachineFilePath(), "manifest");
            }
            m_Manifest.BeginSession();
            // A session starts with no configuration: nothing published before
            // it (or after a previous Stop) describes what this one draws.
            // Until a tag is published the configuration is the empty tag (a
            // session that draws only 2D never publishes one).
            m_SelectedTags.Empty();
            m_SelectedTags.Add(FString());
            m_PublishedTag = {};
            m_FrameTags.Empty();
            m_LastFrameTags.Empty();
            m_PublishFrame = 0;
            m_TagLastSeen.clear();
            {
                std::lock_guard recordLock(m_RecordMutex);
                m_RecordTag = {};
                m_PendingRecords.Empty();
                m_Recording = true;
            }
            m_JobsPerFrame = static_cast<u32>(
                std::clamp<i64>(Levers::VulkanPsoPrecacheJobsPerFrame().value_or(kDefaultJobsPerFrame), 1, 1024));
            m_Started = true;
            // Shaders built before the device's precache started (boot shaders)
            // are requested now.
            RequestAllLocked();
        }

        if (options.StartWorkers)
        {
            const u32 workers = static_cast<u32>(
                std::clamp<i64>(Levers::VulkanPsoPrecacheWorkers().value_or(kDefaultWorkers), 1, kMaxWorkers));
            std::lock_guard workLock(m_WorkMutex);
            m_StopRequested = false;
            m_Tokens = 0;
            m_LastFrameTick = std::chrono::steady_clock::now();
            m_LastGrant = m_LastFrameTick;
            for (u32 i = 0; i < workers; ++i)
            {
                m_Workers[i] = std::thread([this] { WorkerLoop(); });
            }
            m_WorkerCount = workers;
        }
        OLO_CORE_INFO("[Vulkan/PSO] precache started: {} manifest key(s), {} job(s)/frame, {} worker(s){}",
                      m_Manifest.GetEntryCount(), m_JobsPerFrame, m_WorkerCount,
                      Levers::VulkanPsoPrecache() == Levers::Tristate::Off ? " — REQUESTS OFF (OLO_VK_PSO_PRECACHE=0)"
                                                                           : "");
    }

    void VulkanPipelinePrecache::StopWorkers()
    {
        {
            std::lock_guard workLock(m_WorkMutex);
            m_StopRequested = true;
        }
        m_WorkCv.notify_all();
        for (u32 i = 0; i < m_WorkerCount; ++i)
        {
            if (m_Workers[i].joinable())
            {
                m_Workers[i].join();
            }
        }
        m_WorkerCount = 0;
    }

    void VulkanPipelinePrecache::Stop()
    {
        StopWorkers();
        std::lock_guard lock(m_Mutex);
        if (!m_Started)
        {
            return;
        }
        SaveManifestLocked();
        m_Started = false;
        m_SelectedTags.Empty();
        m_PublishedTag = {};
        m_FrameTags.Empty();
        m_LastFrameTags.Empty();
        m_PublishFrame = 0;
        m_TagLastSeen.clear();
        std::lock_guard recordLock(m_RecordMutex);
        m_RecordTag = {};
        m_PendingRecords.Empty();
        m_Recording = false;
    }

    void VulkanPipelinePrecache::SaveManifest()
    {
        std::lock_guard lock(m_Mutex);
        SaveManifestLocked();
    }

    void VulkanPipelinePrecache::SaveManifestLocked()
    {
        DrainPendingRecordsLocked();
        if (!m_Started || !m_Persist || !m_Manifest.IsDirty())
        {
            return;
        }
        m_Manifest.Trim();
        const auto path = PipelineManifest::MachineFilePath();
        // Atomic (temporary + rename): another editor sharing the cache root
        // may be reading it, and Build Game copies it straight into a package.
        AtomicWriteOptions options;
        options.CreateParentDirectories = true;
        if (!FileSystem::WriteFileAtomically(path, m_Manifest.Serialize(), options))
        {
            OLO_CORE_WARN("[Vulkan/PSO] could not write {} — the next session starts without it", path.string());
            return;
        }
        m_Manifest.ClearDirty();
        OLO_CORE_INFO("[Vulkan/PSO] manifest saved: {} pipeline key(s) to {}", m_Manifest.GetEntryCount(),
                      path.string());
    }

    bool VulkanPipelinePrecache::IsStarted() const
    {
        std::lock_guard lock(m_Mutex);
        return m_Started;
    }

    // -------------------------------------------------------------------------
    // Content
    // -------------------------------------------------------------------------

    void VulkanPipelinePrecache::RequestFromManifest(Tracked& tracked, VulkanShader* graphics,
                                                     VulkanComputeShader* compute)
    {
        tracked.SelectedKeys.Empty();
        tracked.HasSelectedEntries = false;
        if (!m_Started || Levers::VulkanPsoPrecache() == Levers::Tristate::Off)
        {
            return;
        }
        // Keys recorded since the last Tick count too.
        DrainPendingRecordsLocked();
        auto& builder = VulkanPipelineBuilder::Get();
        // EntriesFor's pointers stay valid while m_Mutex is held: only Record
        // (also under m_Mutex) grows the manifest.
        // Selected-configuration entries first, the others speculatively after.
        TArray<const PipelineManifestEntry*> entries =
            m_Manifest.EntriesFor(tracked.Name.ToView(), tracked.StageMask, std::string_view{});
        const auto isSelected = [this](const PipelineManifestEntry* entry)
        {
            return m_SelectedTags.ContainsByPredicate([entry](const FString& tag)
                                                      { return entry->SeenUnder(tag.ToView()); });
        };
        std::ranges::stable_partition(entries, isSelected);
        for (const PipelineManifestEntry* entry : entries)
        {
            const bool selected = isSelected(entry);
            const auto priority = selected ? PipelineRequestPriority::Selected : PipelineRequestPriority::Speculative;
            std::optional<VulkanPipelineBuilder::Key> key;
            if (compute != nullptr)
            {
                key = builder.RequestCompute(tracked.ShaderKey, compute->GetModule(), compute->GetRootDataLayout(),
                                             tracked.Name.ToView(), priority);
            }
            else if (graphics != nullptr)
            {
                key = builder.RequestGraphics(*graphics, ToTargets(entry->Signature),
                                              entry->Signature.PatchControlPoints, priority);
            }
            if (selected && key.has_value() && !tracked.SelectedKeys.Contains(*key))
            {
                tracked.SelectedKeys.Add(*key);
                tracked.HasSelectedEntries = true;
            }
        }
        m_ReadinessDirty = true;
    }

    void VulkanPipelinePrecache::RequestAllLocked()
    {
        for (auto& [shader, tracked] : m_Graphics)
        {
            RequestFromManifest(tracked, const_cast<VulkanShader*>(shader), nullptr);
        }
        for (auto& [shader, tracked] : m_Compute)
        {
            RequestFromManifest(tracked, nullptr, const_cast<VulkanComputeShader*>(shader));
        }
    }

    void VulkanPipelinePrecache::OnShaderReady(VulkanShader& shader)
    {
        if (shader.GetCompilationStatus() != ShaderCompilationStatus::Ready)
        {
            return;
        }
        std::lock_guard lock(m_Mutex);
        Tracked& tracked = m_Graphics[&shader];
        tracked.Name = FString(shader.GetName());
        tracked.StageMask = GraphicsStageMask(shader);
        tracked.ShaderKey = shader.GetPipelineIndexKey();
        tracked.Compute = false;
        tracked.DeclaredReady = false;
        RequestFromManifest(tracked, &shader, nullptr);
    }

    void VulkanPipelinePrecache::OnShaderReloaded(VulkanShader& shader,
                                                  const TArray<VulkanPipelineBuilder::Key>& invalidated)
    {
        std::lock_guard lock(m_Mutex);
        Tracked& tracked = m_Graphics[&shader];
        tracked.Name = FString(shader.GetName());
        tracked.StageMask = GraphicsStageMask(shader); // an edit can change the stage set
        tracked.ShaderKey = shader.GetPipelineIndexKey();
        tracked.Compute = false;
        tracked.DeclaredReady = false; // InvalidateShader cleared the ledger's claim with the entries
        RequestFromManifest(tracked, &shader, nullptr);
        if (!m_Started || Levers::VulkanPsoPrecache() == Levers::Tristate::Off)
        {
            return;
        }
        // Every pipeline the old modules had: the same targets, requested again
        // against the rebuilt shader (its layout may have changed, so the key is
        // re-derived, never reused). These were in use, so they go first.
        for (const VulkanPipelineBuilder::Key& old : invalidated)
        {
            if (const auto key = VulkanPipelineBuilder::Get().RequestGraphics(shader, ToTargets(old), old.PatchControlPoints);
                key.has_value() && !tracked.SelectedKeys.Contains(*key))
            {
                tracked.SelectedKeys.Add(*key);
                tracked.HasSelectedEntries = true;
            }
        }
        m_ReadinessDirty = true;
    }

    void VulkanPipelinePrecache::OnShaderDestroyed(const VulkanShader& shader)
    {
        std::lock_guard lock(m_Mutex);
        m_Graphics.erase(&shader);
        m_ReadinessDirty = true;
    }

    void VulkanPipelinePrecache::OnComputeShaderReady(VulkanComputeShader& shader)
    {
        if (!shader.IsValid())
        {
            return;
        }
        std::lock_guard lock(m_Mutex);
        Tracked& tracked = m_Compute[&shader];
        tracked.Name = FString(shader.GetName());
        tracked.StageMask = static_cast<u32>(VK_SHADER_STAGE_COMPUTE_BIT);
        tracked.ShaderKey = shader.GetPipelineIndexKey();
        tracked.Compute = true;
        tracked.DeclaredReady = false;
        RequestFromManifest(tracked, nullptr, &shader);
    }

    void VulkanPipelinePrecache::OnComputeShaderReloaded(VulkanComputeShader& shader,
                                                         const TArray<VulkanPipelineBuilder::Key>& invalidated)
    {
        std::lock_guard lock(m_Mutex);
        Tracked& tracked = m_Compute[&shader];
        tracked.Name = FString(shader.GetName());
        tracked.StageMask = static_cast<u32>(VK_SHADER_STAGE_COMPUTE_BIT);
        tracked.ShaderKey = shader.GetPipelineIndexKey();
        tracked.Compute = true;
        tracked.DeclaredReady = false;
        RequestFromManifest(tracked, nullptr, &shader);
        if (!m_Started || Levers::VulkanPsoPrecache() == Levers::Tristate::Off || invalidated.IsEmpty())
        {
            return;
        }
        // A compute shader has one key per layout; the rebuilt one is requested.
        if (const auto key = VulkanPipelineBuilder::Get().RequestCompute(tracked.ShaderKey, shader.GetModule(),
                                                                         shader.GetRootDataLayout(),
                                                                         tracked.Name.ToView());
            key.has_value() && !tracked.SelectedKeys.Contains(*key))
        {
            tracked.SelectedKeys.Add(*key);
            tracked.HasSelectedEntries = true;
        }
        m_ReadinessDirty = true;
    }

    void VulkanPipelinePrecache::OnComputeShaderDestroyed(const VulkanComputeShader& shader)
    {
        std::lock_guard lock(m_Mutex);
        m_Compute.erase(&shader);
        m_ReadinessDirty = true;
    }

    // -------------------------------------------------------------------------
    // Configuration, recording, per-frame work
    // -------------------------------------------------------------------------

    void VulkanPipelinePrecache::SetConfigurationTag(const std::string_view tag)
    {
        std::lock_guard lock(m_Mutex);
        if (m_PublishedTag.ToView() != tag)
        {
            m_PublishedTag = FString(tag);
            std::lock_guard recordLock(m_RecordMutex);
            m_RecordTag = m_PublishedTag;
        }
        if (!m_FrameTags.ContainsByPredicate([tag](const FString& t) { return t.ToView() == tag; }))
        {
            m_FrameTags.Add(FString(tag));
        }
        // The session's first configuration needs no window: there is no
        // previous selection to protect, and the boot requests are waiting.
        const bool nothingSelected =
            m_SelectedTags.IsEmpty() || (m_SelectedTags.Num() == 1 && m_SelectedTags[0].IsEmpty());
        if (nothingSelected && !tag.empty())
        {
            TArray<FString> first;
            first.Add(FString(tag));
            ApplySelectedTagsLocked(std::move(first));
            return;
        }
        // A readiness claim is a claim about the SELECTED configurations. A
        // draw under another one starts now, before the next Tick selects it,
        // and must not be judged against the old claim: withdraw it at once
        // (ClaimsAllowedLocked makes none until the drawn tags are selected).
        if (!IsSelectedLocked(tag))
        {
            RevokeReadyClaimsLocked();
        }
    }

    bool VulkanPipelinePrecache::IsSelectedLocked(const std::string_view tag) const
    {
        return m_SelectedTags.ContainsByPredicate([tag](const FString& t) { return t.ToView() == tag; });
    }

    bool VulkanPipelinePrecache::ClaimsAllowedLocked() const
    {
        // Every configuration drawn in the last publishing frame and so far in
        // this one must be selected; with nothing published (a 2D-only
        // session) the last published tag (the empty one) is the configuration.
        if (m_LastFrameTags.IsEmpty() && m_FrameTags.IsEmpty())
        {
            return IsSelectedLocked(m_PublishedTag.ToView());
        }
        const auto selected = [this](const FString& tag) { return IsSelectedLocked(tag.ToView()); };
        return std::ranges::all_of(m_LastFrameTags, selected) && std::ranges::all_of(m_FrameTags, selected);
    }

    FString VulkanPipelinePrecache::JoinSelectedTagsLocked() const
    {
        FString joined;
        for (const FString& tag : m_SelectedTags)
        {
            if (!joined.IsEmpty())
            {
                joined += " + ";
            }
            joined += tag;
        }
        return joined;
    }

    void VulkanPipelinePrecache::ApplySelectedTagsLocked(TArray<FString> tags)
    {
        m_SelectedTags = std::move(tags);
        // The selected configuration moved: every live shader's entries for the
        // new one are requested at the front of the queue (already-created ones
        // are duplicates), readiness is judged against the new set, and every
        // pipeline's next draw records under the new tag.
        RevokeReadyClaimsLocked();
        VulkanPipelineBuilder::Get().BeginRecordingEpoch();
        RequestAllLocked();
    }

    void VulkanPipelinePrecache::RevokeReadyClaimsLocked()
    {
        for (auto& [shader, tracked] : m_Graphics)
        {
            if (tracked.DeclaredReady)
            {
                VulkanPipelineBuilder::Get().DeclareShaderReady(tracked.ShaderKey, false);
                tracked.DeclaredReady = false;
            }
        }
        for (auto& [shader, tracked] : m_Compute)
        {
            if (tracked.DeclaredReady)
            {
                VulkanPipelineBuilder::Get().DeclareShaderReady(tracked.ShaderKey, false);
                tracked.DeclaredReady = false;
            }
        }
        m_ReadinessDirty = true;
    }

    std::string VulkanPipelinePrecache::GetConfigurationTag() const
    {
        std::lock_guard lock(m_Mutex);
        return JoinSelectedTagsLocked().ToStdString();
    }

    void VulkanPipelinePrecache::RecordDrawCreation(const std::string_view shader, const u32 stageMask,
                                                    const PipelineSignature& signature)
    {
        std::lock_guard recordLock(m_RecordMutex);
        if (m_Recording)
        {
            m_PendingRecords.Add({ FString(shader), stageMask, signature, m_RecordTag });
        }
    }

    void VulkanPipelinePrecache::DrainPendingRecordsLocked()
    {
        TArray<VulkanPipelinePrecacheRecord> pending;
        {
            std::lock_guard recordLock(m_RecordMutex);
            if (m_PendingRecords.IsEmpty())
            {
                return;
            }
            pending = std::move(m_PendingRecords);
            m_PendingRecords.Empty();
        }
        for (const VulkanPipelinePrecacheRecord& record : pending)
        {
            (void)m_Manifest.Record(record.Shader.ToView(), record.StageMask, record.Signature, record.Tag.ToView());
        }
    }

    void VulkanPipelinePrecache::Tick()
    {
        {
            std::lock_guard lock(m_Mutex);
            if (!m_Started)
            {
                return;
            }
            DrainPendingRecordsLocked();
            // The selection window (see SetConfigurationTag). A frame that
            // published nothing (no frame graph compiled) changes nothing.
            if (!m_FrameTags.IsEmpty())
            {
                ++m_PublishFrame;
                const auto byName = [](const FString& x, const FString& y) { return x.ToView() < y.ToView(); };
                const auto sameTags = [](const TArray<FString>& x, const TArray<FString>& y)
                {
                    return std::ranges::equal(x, y, [](const FString& l, const FString& r) { return l.ToView() == r.ToView(); });
                };
                for (const FString& tag : m_FrameTags)
                {
                    m_TagLastSeen[tag.ToStdString()] = m_PublishFrame;
                }
                std::ranges::sort(m_FrameTags, byName);
                if (!sameTags(m_FrameTags, m_LastFrameTags))
                {
                    m_ReadinessDirty = true;
                }
                m_LastFrameTags = std::move(m_FrameTags);
                m_FrameTags.Empty();

                std::erase_if(m_TagLastSeen, [this](const auto& item)
                              { return m_PublishFrame - item.second >= kTagWindowFrames; });
                TArray<FString> window;
                for (const auto& [tag, seen] : m_TagLastSeen)
                {
                    window.Add(FString(tag));
                }
                std::ranges::sort(window, byName);
                if (!sameTags(window, m_SelectedTags))
                {
                    ApplySelectedTagsLocked(std::move(window));
                }
            }
            EvaluateReadinessLocked(/*force=*/false);
        }
        // This frame's budget. Not accumulated: an idle frame does not buy a
        // larger burst later, so the per-frame cost stays bounded.
        {
            std::lock_guard workLock(m_WorkMutex);
            m_Tokens = m_WorkerCount > 0 ? m_JobsPerFrame : 0u;
            m_LastFrameTick = std::chrono::steady_clock::now();
            m_LastGrant = m_LastFrameTick;
        }
        m_WorkCv.notify_all();
    }

    void VulkanPipelinePrecache::WorkerLoop()
    {
        for (;;)
        {
            {
                std::unique_lock workLock(m_WorkMutex);
                while (!m_StopRequested && m_Tokens == 0)
                {
                    m_WorkCv.wait_for(workLock, kLoadingRefillInterval);
                    // No frame for a while: the process is loading (a packaged
                    // runtime compiles shaders and loads its scene for tens of
                    // seconds before its first frame). Refill at the per-frame
                    // rate, so requests made during loading are prepared on
                    // these threads instead of all landing on the first frame;
                    // nothing waits for them, so loading gains no stall. While
                    // frames are coming, even slow ones, only Tick grants.
                    const auto now = std::chrono::steady_clock::now();
                    if (m_Tokens == 0 && now - m_LastFrameTick >= kNoFrameThreshold &&
                        now - m_LastGrant >= kLoadingRefillInterval)
                    {
                        m_Tokens = m_JobsPerFrame;
                        m_LastGrant = now;
                    }
                }
                if (m_StopRequested)
                {
                    return;
                }
                --m_Tokens;
            }
            if (!VulkanPipelineBuilder::Get().PrepareNextQueued())
            {
                // Nothing queued: give the rest of this frame's budget back and
                // sleep until the next Tick.
                std::lock_guard workLock(m_WorkMutex);
                m_Tokens = 0;
            }
        }
    }

    sizet VulkanPipelinePrecache::PrepareQueued(const sizet maxJobs)
    {
        sizet ran = 0;
        while (ran < maxJobs && VulkanPipelineBuilder::Get().PrepareNextQueued())
        {
            ++ran;
        }
        return ran;
    }

    // -------------------------------------------------------------------------
    // Readiness
    // -------------------------------------------------------------------------

    void VulkanPipelinePrecache::EvaluateReadinessLocked(const bool force)
    {
        auto& builder = VulkanPipelineBuilder::Get();
        const u64 completions = builder.GetCompletions();
        if (!force && !m_ReadinessDirty && completions == m_EvaluatedCompletions)
        {
            return;
        }
        m_EvaluatedCompletions = completions;
        m_ReadinessDirty = false;

        // Claims are made only while every configuration drawn is a selected
        // one; during a switch, before the new set settles, nothing is ready.
        const bool claimsAllowed = ClaimsAllowedLocked();

        PipelineReadinessSnapshot& r = m_LastReadiness;
        r.TrackedShaders = 0;
        r.ReadyShaders = 0;
        r.UnknownShaders = 0;
        r.PendingShaders.Empty();
        bool allReady = true;
        const auto evaluate = [&](Tracked& tracked)
        {
            ++r.TrackedShaders;
            if (!tracked.HasSelectedEntries)
            {
                // No claim either way: the manifest has never seen this shader
                // drawn under this configuration.
                ++r.UnknownShaders;
                return;
            }
            u32 prepared = 0;
            u32 failed = 0;
            for (const VulkanPipelineBuilder::Key& key : tracked.SelectedKeys)
            {
                const auto state = builder.GetRequestState(key);
                prepared += state == VulkanPipelineBuilder::RequestState::Ready ? 1u : 0u;
                failed += state == VulkanPipelineBuilder::RequestState::Failed ? 1u : 0u;
            }
            const u32 requested = static_cast<u32>(tracked.SelectedKeys.Num());
            const bool ready = claimsAllowed && prepared == requested;
            if (ready != tracked.DeclaredReady)
            {
                builder.DeclareShaderReady(tracked.ShaderKey, ready);
                tracked.DeclaredReady = ready;
            }
            if (ready)
            {
                ++r.ReadyShaders;
                return;
            }
            allReady = false;
            if (r.PendingShaders.Num() < static_cast<i32>(kMaxPendingShadersReported))
            {
                r.PendingShaders.Add({ tracked.Name, requested, prepared, failed });
            }
        };
        for (auto& [shader, tracked] : m_Graphics)
        {
            evaluate(tracked);
        }
        for (auto& [shader, tracked] : m_Compute)
        {
            evaluate(tracked);
        }
        r.RenderReady = allReady && r.ReadyShaders > 0;
    }

    void VulkanPipelinePrecache::FillSnapshot(PipelineReadinessSnapshot& snapshot)
    {
        const auto stats = VulkanPipelineBuilder::Get().GetRequestStats();
        std::lock_guard lock(m_Mutex);
        DrainPendingRecordsLocked();
        EvaluateReadinessLocked(/*force=*/true);
        snapshot.Model = PipelineReadinessModel::VulkanPipelineObjects;
        snapshot.Pipelines = stats.Counters;
        snapshot.PrecacheEnabled = m_Started && Levers::VulkanPsoPrecache() != Levers::Tristate::Off;
        snapshot.ConfigurationTag = JoinSelectedTagsLocked();
        snapshot.JobsPerFrame = m_WorkerCount > 0 ? m_JobsPerFrame : 0u;
        snapshot.Queued = stats.Queued;
        snapshot.InFlight = stats.InFlight;
        snapshot.CachedPipelines = stats.CachedPipelines;
        snapshot.ManifestEntries = m_Manifest.GetEntryCount();
        snapshot.TrackedShaders = m_LastReadiness.TrackedShaders;
        snapshot.ReadyShaders = m_LastReadiness.ReadyShaders;
        snapshot.UnknownShaders = m_LastReadiness.UnknownShaders;
        snapshot.RenderReady = m_LastReadiness.RenderReady && stats.Queued == 0 && stats.InFlight == 0;
        snapshot.PendingShaders = m_LastReadiness.PendingShaders;
    }

    void VulkanPipelinePrecache::ResetCounters()
    {
        VulkanPipelineBuilder::Get().ResetRequestCounters();
    }

    PipelineManifest VulkanPipelinePrecache::CopyManifest()
    {
        std::lock_guard lock(m_Mutex);
        DrainPendingRecordsLocked();
        return m_Manifest;
    }

    void VulkanPipelinePrecache::SetManifestForTesting(PipelineManifest manifest)
    {
        std::lock_guard lock(m_Mutex);
        m_Manifest = std::move(manifest);
    }
} // namespace OloEngine

#endif // OLO_WITH_VULKAN
