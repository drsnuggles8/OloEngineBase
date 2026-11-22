#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/Renderer3DInternal.h"

#include "OloEngine/Asset/AssetSystem/RepresentationStreaming.h"
#include "OloEngine/Core/DebugLevers.h"
#include "OloEngine/Renderer/Debug/RendererMemoryReport.h"
#include "OloEngine/Renderer/Debug/RendererMemoryTracker.h"
#include "OloEngine/Renderer/Passes/SceneRenderPass.h"
#include "OloEngine/Renderer/RenderCommand.h"

#include <algorithm>
#include <cmath>

// Renderer3D's side of whole-renderer memory admission (issue #1343). The policy is
// RendererMemoryBudget.h; this file feeds it and applies what it decides.
//
//   OpenPhysicalFrame  -> TickMemoryAdmission: one decision per physical frame, for every
//                         view, before any of them allocates.
//   BeginViewFrame     -> ApplyRealizedMemoryQuality: the realized values stand in for the
//                         requested ones in the view's settings for the length of its frame.
//   EndViewFrame       -> RestoreRequestedMemoryQuality: the request is put back, and what
//                         the frame was built for and measured is recorded.
//   OnWindowResize     -> ReadmitMemoryForResize: the new size is admitted before its
//                         targets are allocated.
//
// The realized configuration is substituted into the settings rather than read at each
// consumer because the pipeline reads those settings in several hundred places, at two
// points of the frame (PrepareFrame and EndScene). One substitution, bracketed by the
// view's frame, is the only way every one of them sees the same configuration. Outside
// the frame the settings are the request: the editor panels, the serializers and
// olo_renderer_settings_get never see a reduced value.
namespace OloEngine
{
    namespace
    {
        // The device budget is polled, not read every frame: vmaGetHeapBudgets takes the
        // allocator's lock, and the OS figure moves slowly.
        constexpr u32 kDeviceBudgetPollTicks = 30;
        // A view's measured owners are refreshed this often while nothing changes.
        constexpr u32 kInventoryRefreshFrames = 16;

        [[nodiscard]] MemoryTechnique TechniqueOfEffect(const TemporalHistoryEffect effect)
        {
            switch (effect)
            {
                case TemporalHistoryEffect::SSGI:
                    return MemoryTechnique::SSGI;
                case TemporalHistoryEffect::SSR:
                    return MemoryTechnique::SSR;
                case TemporalHistoryEffect::RayTracedShadow:
                    return MemoryTechnique::RayTracedShadows;
                case TemporalHistoryEffect::PathTracer:
                    return MemoryTechnique::PathTracer;
                case TemporalHistoryEffect::ReSTIRDI:
                    return MemoryTechnique::ReSTIRDI;
                case TemporalHistoryEffect::ReSTIRGI:
                    return MemoryTechnique::ReSTIRGI;
                case TemporalHistoryEffect::ReSTIRPT:
                    return MemoryTechnique::ReSTIRPT;
                case TemporalHistoryEffect::TAA:
                case TemporalHistoryEffect::Cloudscape:
                case TemporalHistoryEffect::VolumetricFog:
                    break;
            }
            // No rung controls these: they only scale with a resize.
            return MemoryTechnique::COUNT;
        }
    } // namespace

    MemoryQualityConfig Renderer3D::CaptureMemoryQualityRequest(const RendererViewResident& view)
    {
        MemoryQualityConfig config;
        config.Path = view.Settings.Path;
        if (view.RGraph)
        {
            config.DisplayWidth = view.RGraph->GetPhysicalWidth();
            config.DisplayHeight = view.RGraph->GetPhysicalHeight();
            config.TransientPoolSpare = view.RGraph->GetTransientPoolMaxBucketSize();
        }
        // The scene band, as RenderPipeline::PrepareDeclarationInputs derives it.
        const f32 renderScale = UpscaleModeToRenderScale(view.PostProcess.Upscale);
        if (config.DisplayWidth != 0u && config.DisplayHeight != 0u)
        {
            config.SceneWidth = std::max(1u, static_cast<u32>(std::floor(static_cast<f32>(config.DisplayWidth) * renderScale)));
            config.SceneHeight = std::max(1u, static_cast<u32>(std::floor(static_cast<f32>(config.DisplayHeight) * renderScale)));
        }
        config.MsaaSamples = view.Settings.Path == RenderingPath::Deferred ? std::max(view.Settings.Deferred.MSAASampleCount, 1u) : 1u;
        config.SSGIHalfResolution = view.PostProcess.SSGIHalfResolution;
        config.OptionalDetail = RepresentationStreaming::Get().IsBudgeted();

        const auto set = [&config](const MemoryTechnique technique, const bool on)
        {
            if (on)
                config.Techniques |= MemoryTechniqueBit(technique);
        };
        // Only a technique that can run on this view is part of the request: one that is
        // on in the settings but stands down here (ReSTIR on Forward, any ray-traced
        // technique on OpenGL) holds nothing, and counting it would price the climb past
        // its rung at memory it never allocates, so quality would never come back. Its
        // own stand-down reason reports why it is not running.
        const bool deferred = view.Settings.Path == RenderingPath::Deferred;
        const bool rayTracing = deferred && RenderCommand::SupportsRayTracing();
        const PostProcessSettings& post = view.PostProcess;
        set(MemoryTechnique::ReSTIRGI, rayTracing && post.ReSTIRGI.Enabled);
        set(MemoryTechnique::ReSTIRDI, rayTracing && post.ReSTIRDI.Enabled);
        set(MemoryTechnique::ReSTIRPT, rayTracing && post.ReSTIRPT.Enabled);
        set(MemoryTechnique::RayTracedReflections, rayTracing && post.RayTracedReflection.Enabled);
        if (view.Shadow)
        {
            const ShadowSettings& shadows = view.Shadow->GetSettings();
            set(MemoryTechnique::RayTracedShadows, rayTracing && shadows.Enabled && shadows.Technique == ShadowTechnique::RayTraced);
        }
        set(MemoryTechnique::SSGI, deferred && post.SSGIEnabled);
        set(MemoryTechnique::SSR, deferred && post.SSREnabled);
        set(MemoryTechnique::MotionBlur, post.MotionBlurEnabled);
        set(MemoryTechnique::DepthOfField, post.DOFEnabled);
        set(MemoryTechnique::Bloom, post.BloomEnabled);
        set(MemoryTechnique::PathTracer, RenderCommand::SupportsRayTracing() && post.GpuPathTracer.Enabled);
        return config;
    }

    void Renderer3D::MeasureMemoryInventory(RendererViewResident& view)
    {
        ViewMemoryInventory inventory;
        if (!view.RGraph)
        {
            view.MemoryQuality.Inventory = inventory;
            return;
        }
        const RenderGraph::ScreenMemoryUsage usage = view.RGraph->MeasureScreenMemory();
        inventory.DisplayWidth = view.RGraph->GetPhysicalWidth();
        inventory.DisplayHeight = view.RGraph->GetPhysicalHeight();
        if (view.MemoryQuality.FrameRealized)
        {
            inventory.SceneWidth = view.MemoryQuality.FrameRealized->SceneWidth;
            inventory.SceneHeight = view.MemoryQuality.FrameRealized->SceneHeight;
        }
        inventory.NodeTargetBytes = usage.NodeTargetBytes;
        inventory.TransientDemandBytes = usage.TransientDemandBytes;
        inventory.TransientSlackBytes = usage.TransientSlackBytes;
        for (u32 effect = 0; effect < usage.HistoryBytesByEffect.size(); ++effect)
        {
            const MemoryTechnique technique = TechniqueOfEffect(static_cast<TemporalHistoryEffect>(effect));
            if (technique == MemoryTechnique::COUNT)
                inventory.OtherHistoryBytes += usage.HistoryBytesByEffect[effect];
            else
                inventory.TechniqueHistoryBytes[static_cast<u32>(technique)] += usage.HistoryBytesByEffect[effect];
        }
        // ReSTIR PT's path records are the pass's own pools, not registry histories.
        if (view.Pipeline && view.Pipeline->SceneCompositePasses.ReSTIRPT)
        {
            inventory.TechniqueHistoryBytes[static_cast<u32>(MemoryTechnique::ReSTIRPT)] +=
                view.Pipeline->SceneCompositePasses.ReSTIRPT->GetStats().ReservoirBytes;
        }
        view.MemoryQuality.Inventory = inventory;
        view.MemoryQuality.FramesSinceMeasured = 0;
    }

    MemoryBudgetObservation Renderer3D::ObserveMemoryBudget(const bool frameTick)
    {
        auto& state = s_Data.MemoryAdmission;
        const auto& tracker = RendererMemoryTracker::GetInstance();
        const u64 trackedResident = tracker.GetGpuResidentBytes();

        MemoryBudgetObservation observation;
        observation.ResidentBytes = trackedResident;
        observation.RetiringBytes = std::min(tracker.GetGpuRetiringBytes(), trackedResident);

        if (state.BudgetOverrideBytes)
        {
            observation.BudgetBytes = state.BudgetOverrideBytes;
            observation.Source = MemoryBudgetSource::Injected;
            return observation;
        }
        if (const auto megabytes = Levers::RendererMemoryBudgetMb())
        {
            observation.BudgetBytes = static_cast<u64>(*megabytes) * 1024ull * 1024ull;
            observation.Source = MemoryBudgetSource::Configured;
            return observation;
        }

        // The device's own budget. OpenGL has none to report; Vulkan reports the OS's
        // per-heap budget with VK_EXT_memory_budget, and VMA's heuristic without it.
        if (frameTick)
            ++state.TicksSincePoll;
        if (!state.DevicePolled || state.TicksSincePoll >= kDeviceBudgetPollTicks)
        {
            state.DevicePolled = true;
            state.TicksSincePoll = 0;
            state.DeviceBudgetBytes.reset();
            state.DeviceUsageBytes = 0;
            state.TrackedResidentAtPoll = trackedResident;
            BackendMemoryObservation backend;
            if (RendererMemoryTracker::ObserveActiveRendererAPI(backend) && backend.Residency != MemoryResidencyStatus::Unknown)
            {
                // The heaps a renderer allocation lands in: the device-local ones, or all
                // of them on a device that flags none (unified memory).
                bool anyDeviceLocal = false;
                for (const auto& heap : backend.Heaps)
                    anyDeviceLocal = anyDeviceLocal || heap.DeviceLocal;
                u64 budget = 0;
                u64 usage = 0;
                for (const auto& heap : backend.Heaps)
                {
                    if (anyDeviceLocal && !heap.DeviceLocal)
                        continue;
                    budget += heap.BudgetBytes;
                    usage += heap.UsageBytes;
                }
                if (budget != 0)
                {
                    state.DeviceBudgetBytes = budget;
                    state.DeviceUsageBytes = usage;
                    state.DeviceBudgetSource = backend.Residency == MemoryResidencyStatus::OsReported
                                                   ? MemoryBudgetSource::OsReported
                                                   : MemoryBudgetSource::AllocatorHeuristic;
                }
            }
        }
        if (!state.DeviceBudgetBytes)
            return observation; // Source None: nothing is enforced, and the report says so.

        observation.BudgetBytes = state.DeviceBudgetBytes;
        observation.Source = state.DeviceBudgetSource;
        // The device's usage counts what the tracker cannot see (the swapchain, the UI
        // backend). Between polls it is advanced by what the tracker booked since.
        const i64 sincePoll = static_cast<i64>(trackedResident) - static_cast<i64>(state.TrackedResidentAtPoll);
        const i64 resident = static_cast<i64>(state.DeviceUsageBytes) + sincePoll;
        observation.ResidentBytes = std::max(static_cast<u64>(std::max<i64>(resident, 0)), trackedResident);
        return observation;
    }

    void Renderer3D::BuildMemoryAdmissionInput(MemoryAdmissionInput& input, const bool frameTick)
    {
        auto& state = s_Data.MemoryAdmission;
        input.Budget = ObserveMemoryBudget(frameTick);
        input.Views.Empty(static_cast<i32>(s_Data.Views.size()));
        for (RendererViewSlot& slot : s_Data.Views)
        {
            const RendererViewResident& view = slot.Id == s_Data.ActiveView ? static_cast<const RendererViewResident&>(s_Data) : slot.Parked;
            MemoryAdmissionViewInput entry;
            entry.View = slot.Id.Value;
            entry.Requested = CaptureMemoryQualityRequest(view);
            entry.Allocated = view.MemoryQuality.Allocated;
            entry.Inventory = view.MemoryQuality.Inventory;
            input.Views.Add(entry);
        }

        input.OptionalDetailBytes = RepresentationStreaming::Get().GetOptionalResidentGpuBytes();
        state.KnownOptionalDetailBytes = std::max(state.KnownOptionalDetailBytes, input.OptionalDetailBytes);
        input.KnownOptionalDetailBytes = state.KnownOptionalDetailBytes;
    }

    void Renderer3D::ApplyWorldMemoryQuality()
    {
        // Optional groom / vegetation detail is the world's (#1257's governor owns it):
        // withheld when any view's realized configuration gave it up.
        const MemoryAdmissionSnapshot& snapshot = s_Data.MemoryAdmission.Governor.GetSnapshot();
        bool withheld = false;
        for (const auto& view : snapshot.Views)
            withheld = withheld || (view.Requested.OptionalDetail && !view.Realized.OptionalDetail);
        RepresentationStreaming::Get().SetOptionalDetailWithheld(withheld);
    }

    void Renderer3D::TickMemoryAdmission()
    {
        auto& state = s_Data.MemoryAdmission;
        if (Levers::RendererMemoryAdmissionOff())
        {
            // The A/B control: nothing is admitted and nothing is reduced.
            if (state.Governor.GetSnapshot().Ticks != 0u)
            {
                state.Governor.Reset();
                RepresentationStreaming::Get().SetOptionalDetailWithheld(false);
            }
            return;
        }

        BuildMemoryAdmissionInput(state.Input, /*frameTick=*/true);
        const u32 previousLevel = state.Governor.GetSnapshot().Level;
        const MemoryAdmissionSnapshot& snapshot = state.Governor.Tick(state.Input);
        if (snapshot.Level != previousLevel)
        {
            // Countable and said aloud, once per change (no-silent-fallbacks.md).
            OLO_CORE_WARN("[MemoryAdmission] level {} -> {} of {} ({}): budget {} MiB from {}, resident {} MiB, predicted steady {} MiB / peak {} MiB",
                          previousLevel, snapshot.Level, snapshot.FloorLevel, ToString(snapshot.Reason),
                          snapshot.BudgetBytes.value_or(0) / (1024ull * 1024ull), ToString(snapshot.BudgetSource),
                          snapshot.ResidentBytes / (1024ull * 1024ull), snapshot.PredictedSteadyBytes / (1024ull * 1024ull),
                          snapshot.PredictedPeakBytes / (1024ull * 1024ull));
        }
        ApplyWorldMemoryQuality();
    }

    void Renderer3D::ApplyRealizedMemoryQuality()
    {
        auto& quality = s_Data.MemoryQuality;
        // A BeginScene whose EndScene never ran left the realized values in the settings.
        if (quality.Saved.Active)
            RestoreRequestedMemoryQuality(/*frameRendered=*/false);

        // Derived from the request as it is NOW, at the governor's level: a setting
        // changed since the tick is reduced like the rest instead of being overwritten.
        const MemoryQualityConfig requested = CaptureMemoryQualityRequest(s_Data);
        const u32 level = Levers::RendererMemoryAdmissionOff() ? 0u : s_Data.MemoryAdmission.Governor.GetSnapshot().Level;
        const MemoryQualityConfig realized = ReduceMemoryQuality(requested, level);
        quality.FrameRealized = realized;
        if (realized == requested)
            return;

        auto& saved = quality.Saved;
        saved.Active = true;
        saved.MsaaSamples = s_Data.Settings.Deferred.MSAASampleCount;
        saved.SSGIHalfResolution = s_Data.PostProcess.SSGIHalfResolution;
        saved.ReSTIRGI = s_Data.PostProcess.ReSTIRGI.Enabled;
        saved.ReSTIRDI = s_Data.PostProcess.ReSTIRDI.Enabled;
        saved.ReSTIRPT = s_Data.PostProcess.ReSTIRPT.Enabled;
        saved.RayTracedReflections = s_Data.PostProcess.RayTracedReflection.Enabled;
        saved.SSGI = s_Data.PostProcess.SSGIEnabled;
        saved.SSR = s_Data.PostProcess.SSREnabled;
        saved.MotionBlur = s_Data.PostProcess.MotionBlurEnabled;
        saved.DepthOfField = s_Data.PostProcess.DOFEnabled;
        saved.Bloom = s_Data.PostProcess.BloomEnabled;
        saved.TransientPoolSpare = requested.TransientPoolSpare;
        saved.ShadowTechniqueReplaced = false;

        if (s_Data.Settings.Path == RenderingPath::Deferred)
            s_Data.Settings.Deferred.MSAASampleCount = realized.MsaaSamples;
        PostProcessSettings& post = s_Data.PostProcess;
        post.SSGIHalfResolution = realized.SSGIHalfResolution;
        // Only a technique the request runs and the realization removed is switched off:
        // one that is on but cannot run here keeps its own stand-down reason.
        const auto removed = [&requested, &realized](const MemoryTechnique technique)
        { return requested.Has(technique) && !realized.Has(technique); };
        if (removed(MemoryTechnique::ReSTIRGI))
            post.ReSTIRGI.Enabled = false;
        if (removed(MemoryTechnique::ReSTIRDI))
            post.ReSTIRDI.Enabled = false;
        if (removed(MemoryTechnique::ReSTIRPT))
            post.ReSTIRPT.Enabled = false;
        if (removed(MemoryTechnique::RayTracedReflections))
            post.RayTracedReflection.Enabled = false;
        if (removed(MemoryTechnique::SSGI))
            post.SSGIEnabled = false;
        if (removed(MemoryTechnique::SSR))
            post.SSREnabled = false;
        if (removed(MemoryTechnique::MotionBlur))
            post.MotionBlurEnabled = false;
        if (removed(MemoryTechnique::DepthOfField))
            post.DOFEnabled = false;
        if (removed(MemoryTechnique::Bloom))
            post.BloomEnabled = false;
        if (requested.Has(MemoryTechnique::RayTracedShadows) && !realized.Has(MemoryTechnique::RayTracedShadows) && s_Data.Shadow)
        {
            // The whole-technique fallback of ray-traced shadows is the shadow maps.
            ShadowSettings shadows = s_Data.Shadow->GetSettings();
            saved.RequestedShadowTechnique = shadows.Technique;
            saved.ShadowTechniqueReplaced = true;
            shadows.Technique = ShadowTechnique::ShadowMap;
            s_Data.Shadow->SetSettings(shadows);
        }
        if (s_Data.RGraph)
            s_Data.RGraph->SetTransientPoolMaxBucketSize(realized.TransientPoolSpare);
    }

    void Renderer3D::RestoreRequestedMemoryQuality(const bool frameRendered)
    {
        auto& quality = s_Data.MemoryQuality;
        if (quality.Saved.Active)
        {
            const auto& saved = quality.Saved;
            s_Data.Settings.Deferred.MSAASampleCount = saved.MsaaSamples;
            PostProcessSettings& post = s_Data.PostProcess;
            post.SSGIHalfResolution = saved.SSGIHalfResolution;
            post.ReSTIRGI.Enabled = saved.ReSTIRGI;
            post.ReSTIRDI.Enabled = saved.ReSTIRDI;
            post.ReSTIRPT.Enabled = saved.ReSTIRPT;
            post.RayTracedReflection.Enabled = saved.RayTracedReflections;
            post.SSGIEnabled = saved.SSGI;
            post.SSREnabled = saved.SSR;
            post.MotionBlurEnabled = saved.MotionBlur;
            post.DOFEnabled = saved.DepthOfField;
            post.BloomEnabled = saved.Bloom;
            if (saved.ShadowTechniqueReplaced && s_Data.Shadow)
            {
                ShadowSettings shadows = s_Data.Shadow->GetSettings();
                shadows.Technique = saved.RequestedShadowTechnique;
                s_Data.Shadow->SetSettings(shadows);
            }
            if (s_Data.RGraph)
                s_Data.RGraph->SetTransientPoolMaxBucketSize(saved.TransientPoolSpare);
            quality.Saved.Active = false;
        }

        if (!frameRendered || !quality.FrameRealized || !s_Data.RGraph)
            return;

        // The view's resources are now built for what this frame realized.
        const bool changed = !quality.Allocated || !(*quality.Allocated == *quality.FrameRealized);
        quality.Allocated = quality.FrameRealized;
        ++quality.FramesSinceMeasured;
        // The measurement only matters when there is a budget to admit against.
        const bool budgeted = s_Data.MemoryAdmission.Governor.GetSnapshot().BudgetBytes.has_value();
        if (budgeted && (changed || !quality.Inventory.IsMeasured() || quality.FramesSinceMeasured >= kInventoryRefreshFrames))
            MeasureMemoryInventory(s_Data);
    }

    void Renderer3D::ReadmitMemoryForResize(const u32 width, const u32 height)
    {
        auto& state = s_Data.MemoryAdmission;
        if (Levers::RendererMemoryAdmissionOff() || !s_Data.RGraph)
            return;
        if (s_Data.RGraph->GetPhysicalWidth() == width && s_Data.RGraph->GetPhysicalHeight() == height)
            return;

        // The request as it will be once the graph has the new size.
        BuildMemoryAdmissionInput(state.Input, /*frameTick=*/false);
        const f32 renderScale = UpscaleModeToRenderScale(s_Data.PostProcess.Upscale);
        for (auto& view : state.Input.Views)
        {
            if (view.View != s_Data.ActiveView.Value)
                continue;
            view.Requested.DisplayWidth = width;
            view.Requested.DisplayHeight = height;
            view.Requested.SceneWidth = std::max(1u, static_cast<u32>(std::floor(static_cast<f32>(width) * renderScale)));
            view.Requested.SceneHeight = std::max(1u, static_cast<u32>(std::floor(static_cast<f32>(height) * renderScale)));
        }
        const MemoryAdmissionSnapshot& snapshot = state.Governor.Readmit(state.Input);
        ApplyWorldMemoryQuality();

        // The G-Buffer follows the scene target through a resize at the sample count it
        // has. When the new size is admitted at another count, resizing it first would
        // allocate it twice: drop it, and the next frame builds it once, as admitted.
        const MemoryQualityConfig* realized = state.Governor.FindRealized(s_Data.ActiveView.Value);
        if (realized && s_Data.Pipeline && s_Data.Pipeline->FrameCorePasses.Scene)
        {
            auto& scene = *s_Data.Pipeline->FrameCorePasses.Scene;
            if (const auto& gbuffer = scene.GetGBuffer(); gbuffer && gbuffer->GetSampleCount() != realized->MsaaSamples)
                scene.ReleaseGBuffer();
        }
        if (snapshot.IsReduced())
        {
            OLO_CORE_WARN("[MemoryAdmission] resize to {}x{} admitted at level {} of {} ({})", width, height, snapshot.Level,
                          snapshot.FloorLevel, ToString(snapshot.Reason));
        }
    }

    bool Renderer3D::ReportMemoryAdmission(MemoryAdmissionSnapshot& out)
    {
        if (!HasInitialized() || Levers::RendererMemoryAdmissionOff())
            return false;
        out = s_Data.MemoryAdmission.Governor.GetSnapshot();
        return out.Ticks != 0u;
    }

    const MemoryAdmissionSnapshot& Renderer3D::GetMemoryAdmission()
    {
        return s_Data.MemoryAdmission.Governor.GetSnapshot();
    }

    bool Renderer3D::IsMemoryTechniqueWithheld(const RendererViewResident& view, const MemoryTechnique technique)
    {
        // THIS frame's decision: the view's request at the level the governor chose at the
        // top of the frame, exactly as ApplyRealizedMemoryQuality derives it. Not the
        // allocation of the last frame, which lags a level change by one frame.
        if (Levers::RendererMemoryAdmissionOff())
            return false;
        const MemoryQualityConfig requested = CaptureMemoryQualityRequest(view);
        if (!requested.Has(technique))
            return false;
        return !ReduceMemoryQuality(requested, s_Data.MemoryAdmission.Governor.GetSnapshot().Level).Has(technique);
    }

    void Renderer3D::SetMemoryBudgetOverride(const std::optional<u64> bytes)
    {
        auto& state = s_Data.MemoryAdmission;
        // The device figure was not polled while an override stood in for it: take a
        // fresh one when the device's budget comes back.
        if (state.BudgetOverrideBytes && !bytes)
            state.DevicePolled = false;
        state.BudgetOverrideBytes = bytes;
    }

    std::optional<u64> Renderer3D::GetMemoryBudgetOverride()
    {
        return s_Data.MemoryAdmission.BudgetOverrideBytes;
    }

    void Renderer3D::SetMemoryAdmissionPolicy(const MemoryAdmissionPolicy& policy)
    {
        s_Data.MemoryAdmission.Governor.SetPolicy(policy);
    }

    const MemoryAdmissionPolicy& Renderer3D::GetMemoryAdmissionPolicy()
    {
        return s_Data.MemoryAdmission.Governor.GetPolicy();
    }

    void Renderer3D::ResetMemoryAdmission()
    {
        auto& state = s_Data.MemoryAdmission;
        state.Governor.Reset();
        state.DevicePolled = false;
        state.TicksSincePoll = 0;
        state.KnownOptionalDetailBytes = 0;
        RepresentationStreaming::Get().SetOptionalDetailWithheld(false);
    }
} // namespace OloEngine
