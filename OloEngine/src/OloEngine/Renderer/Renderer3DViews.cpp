#include "OloEnginePCH.h"
#include "OloEngine/Core/DebugLevers.h"
#include "OloEngine/Renderer/Commands/CommandDispatch.h"
#include "OloEngine/Renderer/Commands/FrameResourceManager.h"
#include "OloEngine/Renderer/Debug/RenderGraphDebugRuntime.h"
#include "OloEngine/Renderer/Occlusion/OcclusionQueryPool.h"
#include "OloEngine/Renderer/Occlusion/OcclusionState.h"
#include "OloEngine/Renderer/RHI/RHIDescriptorHeap.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/Renderer3DInternal.h"

// Scene views and the physical frame (ADR 0026, issue #1352).
//
// A view's state is one struct, RendererViewResident, and the ACTIVE view's
// copy is the base sub-object of s_Data. Everything else in this file follows
// from that: activation moves the struct, creation fills one and brings its
// GPU objects up with that view active, destruction does the reverse.

namespace OloEngine
{
    Renderer3D::RendererViewSlot* Renderer3D::FindViewSlot(RenderViewId view)
    {
        for (RendererViewSlot& slot : s_Data.Views)
        {
            if (slot.Id == view)
                return &slot;
        }
        return nullptr;
    }

    void Renderer3D::ActivateView(RenderViewId view)
    {
        if (view == s_Data.ActiveView)
            return;

        // A scene in progress holds the active view's buckets, its allocator
        // and its recording view; nothing can take the view from under it.
        OLO_CORE_ASSERT(!s_Data.PhysicalFrame.InScene, "Renderer3D: a view cannot be activated between BeginScene and EndScene");
        RendererViewSlot* target = FindViewSlot(view);
        RendererViewSlot* current = FindViewSlot(s_Data.ActiveView);
        OLO_CORE_ASSERT(target && current, "Renderer3D::ActivateView: view {} is not alive", view.Value);
        if (!target || !current || s_Data.PhysicalFrame.InScene)
        {
            OLO_CORE_ERROR("Renderer3D::ActivateView: view {} was not activated (alive: {}, in scene: {})", view.Value,
                           target != nullptr, s_Data.PhysicalFrame.InScene);
            return;
        }

        auto& resident = static_cast<RendererViewResident&>(s_Data);
        // Negative control only (DebugLevers.inl, Fault injection): the camera
        // history stays where it is, one for every view.
        const bool shareCameraHistory = Levers::FaultShareViewCameraHistory();
        const RendererViewState sharedCameraHistory = shareCameraHistory ? resident.View : RendererViewState{};
        current->Parked = std::move(resident);
        resident = std::move(target->Parked);
        // The slot of the view that is now active owns nothing: emptied by
        // assignment, so a member whose move leaves its source holding a GPU
        // object cannot keep one alive there (a parked copy of the Hi-Z
        // generator's references once survived Shutdown that way).
        target->Parked = RendererViewResident{};
        if (shareCameraHistory)
            resident.View = sharedCameraHistory;
        s_Data.ActiveView = view;

        // What lives outside s_Data and names a view-owned object.
        RenderGraphDebugRuntime::SetActiveGraph(s_Data.RGraph);
        RGOutOfBand::SetActiveLedger(s_Data.RGraph ? &s_Data.RGraph->GetOutOfBandLedger() : nullptr);
        if (CommandDispatch::HasUBOReferences())
            CommandDispatch::SetForwardPlus(s_Data.ForwardPlus.get());
        OcclusionQueryPool::SetActiveView(view.Value);
        OcclusionStateManager::SetActiveView(view.Value);
    }

    void Renderer3D::InitializeActiveViewGPU(u32 width, u32 height)
    {
        s_Data.RGraph = Ref<RenderGraph>::Create();
        // The camera block is frame-published: the graph puts it back before
        // every pass, so no pass reads a predecessor's (issue #1687).
        s_Data.RGraph->SetPassEntryPublication(&CommandDispatch::PublishFrameCamera);
        // Every history this graph acquires is this view's and no other's.
        s_Data.RGraph->BindTemporalHistoriesToView(s_Data.ActiveView.Value);

        s_Data.OcclusionHZB.Initialize();
        s_Data.OcclusionHZB.SetReduceMode(HZBGenerator::ReduceMode::Max);

        // The same order Init uses for the main view: the passes first (they
        // take the address of this view's shadow map), then the maps, then the
        // light grid.
        SetupRenderGraph(width, height);
        s_Data.Shadow->Init();
        s_Data.ForwardPlus->Initialize(width, height);
    }

    void Renderer3D::ReleaseActiveViewGPU()
    {
        if (s_Data.DecalVisibilityQueriesInitialized)
        {
            RenderCommand::DeleteQueries(s_Data.DecalReceiverIntersectionQueries);
            RenderCommand::DeleteQueries(s_Data.DecalVisibilityQueries);
            s_Data.DecalReceiverIntersectionQueries = {};
            s_Data.DecalVisibilityQueries = {};
            s_Data.DecalVisibilityFrames = {};
            s_Data.DecalVisibilityQueriesInitialized = false;
            s_Data.DecalVisibilityTarget = -1;
            s_Data.LastDecalVisibilityEntity = -1;
            s_Data.LastDecalVisibilityObservation = {};
        }

        s_Data.OcclusionHZB.Shutdown();
        if (s_Data.ForwardPlus)
            s_Data.ForwardPlus->Shutdown();
        if (s_Data.Shadow)
            s_Data.Shadow->Shutdown();

        if (s_Data.RGraph)
            s_Data.RGraph->Shutdown();
        // The passes go while the graphics context and the RendererAPI are
        // alive: their destructors release textures and framebuffers.
        if (s_Data.Pipeline)
            s_Data.Pipeline->Reset();
        s_Data.RGraph.Reset();
    }

    RenderViewId Renderer3D::CreateView(const RenderViewDesc& desc)
    {
        OLO_PROFILE_FUNCTION();
        OLO_CORE_ASSERT(HasInitialized(), "Renderer3D::CreateView before Renderer3D::Init");
        OLO_CORE_ASSERT(!s_Data.PhysicalFrame.InScene, "Renderer3D::CreateView between BeginScene and EndScene");
        if (!HasInitialized() || s_Data.PhysicalFrame.InScene || desc.Width == 0 || desc.Height == 0)
        {
            OLO_CORE_ERROR("Renderer3D::CreateView refused: initialized={}, in scene={}, size={}x{}", HasInitialized(),
                           s_Data.PhysicalFrame.InScene, desc.Width, desc.Height);
            return kMainRenderView;
        }

        const RenderViewId view{ s_Data.NextViewId++ };
        // The creating view's configuration, taken before anything moves.
        const ShadowSettings shadowSettings = s_Data.Shadow->GetSettings();

        RendererViewSlot slot;
        slot.Id = view;
        slot.DebugName = desc.DebugName.empty() ? ("View " + std::to_string(view.Value)) : desc.DebugName;
        RendererViewResident& resident = slot.Parked;
        resident.Settings = s_Data.Settings;
        resident.PostProcess = s_Data.PostProcess;
        // The observer camera is a tool pointed at one view. A new view starts
        // unfrozen; SetCullingCameraFrozen is per view from here on.
        resident.Settings.ObserverCameraEnabled = false;
        // What a caller set through a Renderer3D setter and no settings field
        // carries; everything else here is derived from Settings by the
        // ApplyRendererSettings below.
        resident.DynamicCullingEnabled = s_Data.DynamicCullingEnabled;
        resident.DepthAwareClusterCullingEnabled = s_Data.DepthAwareClusterCullingEnabled;
        resident.HZBOcclusionDepthBias = s_Data.HZBOcclusionDepthBias;
        resident.EnableSelectionOutline = s_Data.EnableSelectionOutline;
        resident.Pipeline = std::make_unique<RenderPipeline>();
        resident.ForwardPlus = std::make_unique<TiledForwardPlus>();
        resident.Shadow = std::make_unique<ShadowMap>();
        s_Data.Views.push_back(std::move(slot));

        {
            const ScopedView scope(view);
            InitializeActiveViewGPU(desc.Width, desc.Height);
            s_Data.Shadow->SetSettings(shadowSettings);
            // Everything ApplyRendererSettings derives from the copied
            // settings: the culling toggles, the light-grid mode, the prepass.
            ApplyRendererSettings();
            // The view's sequences start where the run's seed starts every
            // view's, and its clocks at now, so its first dt is 0.
            ResetActiveViewSequences(s_Data.Clock.FrameSequenceSeed);
        }

        OLO_CORE_INFO("Renderer3D: created view {} '{}' at {}x{}", view.Value,
                      FindViewSlot(view) ? FindViewSlot(view)->DebugName : std::string{}, desc.Width, desc.Height);
        return view;
    }

    void Renderer3D::DestroyView(RenderViewId view)
    {
        OLO_PROFILE_FUNCTION();
        if (view.IsMain() || view == s_Data.ActiveView || s_Data.PhysicalFrame.InScene || FindViewSlot(view) == nullptr)
        {
            OLO_CORE_ERROR("Renderer3D::DestroyView refused for view {}: main={}, active={}, in scene={}, alive={}",
                           view.Value, view.IsMain(), view == s_Data.ActiveView, s_Data.PhysicalFrame.InScene,
                           FindViewSlot(view) != nullptr);
            OLO_CORE_ASSERT(false, "Renderer3D::DestroyView: the main view, the active view, a view mid-scene and a dead view cannot be destroyed");
            return;
        }

        {
            const ScopedView scope(view);
            ReleaseActiveViewGPU();
        }
        OcclusionQueryPool::ReleaseView(view.Value);
        OcclusionStateManager::ReleaseView(view.Value);
        std::erase_if(s_Data.Views, [view](const RendererViewSlot& slot)
                      { return slot.Id == view; });
        OLO_CORE_INFO("Renderer3D: destroyed view {}", view.Value);
    }

    bool Renderer3D::AnyViewRendersDeferred()
    {
        bool deferred = false;
        ForEachViewResident([&deferred](const RendererViewResident& view)
                            { deferred = deferred || view.Settings.Path == RenderingPath::Deferred; });
        return deferred;
    }

    bool Renderer3D::IsViewAlive(RenderViewId view)
    {
        return FindViewSlot(view) != nullptr;
    }

    RenderViewId Renderer3D::GetActiveView()
    {
        return s_Data.ActiveView;
    }

    u32 Renderer3D::GetViewCount()
    {
        return static_cast<u32>(s_Data.Views.size());
    }

    RenderViewInfo Renderer3D::GetViewInfo(RenderViewId view)
    {
        RenderViewInfo info;
        const RendererViewSlot* slot = FindViewSlot(view);
        if (!slot)
            return info;

        const bool active = view == s_Data.ActiveView;
        const RendererViewResident& resident = active ? static_cast<const RendererViewResident&>(s_Data) : slot->Parked;
        info.Id = view;
        info.DebugName = slot->DebugName;
        if (resident.RGraph)
        {
            info.Width = resident.RGraph->GetPhysicalWidth();
            info.Height = resident.RGraph->GetPhysicalHeight();
        }
        info.FramesRendered = resident.ViewClock.FramesRendered;
        info.LastRenderedFrameSerial = resident.ViewClock.LastRenderedFrameSerial;
        info.WasPrimary = resident.ViewClock.WasPrimary;
        info.Active = active;
        info.LastFrameCpuMs = resident.ViewClock.LastFrameCpuMs;
        return info;
    }

    Renderer3D::ViewRetainedState Renderer3D::GetViewRetainedState(RenderViewId view)
    {
        ViewRetainedState state;
        const RendererViewSlot* slot = FindViewSlot(view);
        if (!slot)
            return state;

        const RendererViewResident& resident =
            view == s_Data.ActiveView ? static_cast<const RendererViewResident&>(s_Data) : slot->Parked;
        state.Alive = true;
        if (resident.RGraph)
        {
            const TemporalHistoryRegistry& registry = resident.RGraph->GetTemporalHistoryRegistry();
            for (const TemporalHistorySnapshot& history : registry.Snapshot())
            {
                const Ref<Texture2D> texture = registry.GetTexture(history.Token);
                state.Histories.push_back(ViewRetainedHistory{
                    .Key = history.Key,
                    .Age = history.Age,
                    .Valid = history.Valid,
                    .LineageCause = history.LineageCause,
                    .TextureIdentity = texture ? RHI::HashKey(texture->GetRHIHandle()) : 0u,
                });
            }
        }
        state.OcclusionPyramidValid = resident.View.OcclusionHZBValid && resident.OcclusionHZB.IsValid();
        state.OcclusionPyramidIdentity =
            resident.OcclusionHZB.IsValid() ? RHI::HashKey(resident.OcclusionHZB.GetHZBTexture()) : 0u;
        if (resident.Shadow)
        {
            state.ShadowCascadeIdentity = RHI::HashKey(resident.Shadow->GetCSMHandle());
            state.ShadowAtlasIdentity = RHI::HashKey(resident.Shadow->GetAtlasHandle());
        }
        return state;
    }

    std::vector<RenderViewInfo> Renderer3D::GetViewInfos()
    {
        std::vector<RenderViewInfo> infos;
        infos.reserve(s_Data.Views.size());
        for (const RendererViewSlot& slot : s_Data.Views)
            infos.push_back(GetViewInfo(slot.Id));
        return infos;
    }

    Renderer3D::ScopedView::ScopedView(RenderViewId view)
        : m_Previous(s_Data.ActiveView)
    {
        if (view != m_Previous)
        {
            ActivateView(view);
            m_Switched = s_Data.ActiveView == view;
        }
    }

    Renderer3D::ScopedView::~ScopedView()
    {
        if (m_Switched)
            ActivateView(m_Previous);
    }

    // ---------------------------------------------------------------------
    // The physical frame
    // ---------------------------------------------------------------------

    void Renderer3D::OpenPhysicalFrame(bool isExplicit)
    {
        auto& frame = s_Data.PhysicalFrame;
        ++frame.Serial;
        frame.Open = true;
        frame.Explicit = isExplicit;
        frame.InScene = false;
        frame.InPrimaryView = false;
        frame.ViewsBegun = 0;
        frame.SceneExtracted = false;
        frame.LinkingDraws = false;
        frame.SceneOwnerToken = 0;
        frame.FrameResourcesBegun = false;
        ++frame.Counters.PhysicalFrames;
    }

    void Renderer3D::ClosePhysicalFrame()
    {
        auto& frame = s_Data.PhysicalFrame;
        frame.Open = false;
        frame.Explicit = false;
        frame.InScene = false;
        frame.InPrimaryView = false;
        frame.LinkingDraws = false;
    }

    void Renderer3D::BeginFrame()
    {
        auto& frame = s_Data.PhysicalFrame;
        OLO_CORE_ASSERT(!frame.Open, "Renderer3D::BeginFrame: a physical frame is already open (brackets do not nest)");
        if (frame.Open)
            return;
        OpenPhysicalFrame(/*isExplicit=*/true);
        // Every view's render graph retires its transient descriptors when it
        // has executed. On a backend whose commands run behind the CPU the
        // descriptors of the frame's first view must still be there when its
        // commands execute, after the last view was recorded: the ring advances
        // once, when the bracket closes.
        RHI::DescriptorHeap::Get().HoldFrameTransientReset(true);
    }

    void Renderer3D::EndFrame()
    {
        auto& frame = s_Data.PhysicalFrame;
        OLO_CORE_ASSERT(frame.Open && frame.Explicit, "Renderer3D::EndFrame without a matching BeginFrame");
        OLO_CORE_ASSERT(!frame.InScene, "Renderer3D::EndFrame between BeginScene and EndScene");
        if (!frame.Open || !frame.Explicit)
            return;
        // The frame's allocators and deferred deletions end with its last view.
        if (frame.FrameResourcesBegun)
        {
            FrameResourceManager::Get().EndFrame();
            frame.FrameResourcesBegun = false;
        }
        ClosePhysicalFrame();
        RHI::DescriptorHeap::Get().HoldFrameTransientReset(false);
    }

    bool Renderer3D::IsFrameOpen()
    {
        return s_Data.PhysicalFrame.Open;
    }

    u64 Renderer3D::GetPhysicalFrameSerial()
    {
        return s_Data.PhysicalFrame.Serial;
    }

    bool Renderer3D::IsPrimaryViewFrame()
    {
        // The lever is a negative control only (DebugLevers.inl, Fault injection).
        return s_Data.PhysicalFrame.InScene &&
               (s_Data.PhysicalFrame.InPrimaryView || Levers::FaultAdvanceWorldEveryView());
    }

    Renderer3D::SceneAdvanceCounters Renderer3D::GetSceneAdvanceCounters()
    {
        return s_Data.PhysicalFrame.Counters;
    }

    void Renderer3D::BeginViewFrame()
    {
        auto& frame = s_Data.PhysicalFrame;
        OLO_CORE_ASSERT(!frame.InScene, "Renderer3D::BeginScene called twice before EndScene");
        if (!frame.Open)
            OpenPhysicalFrame(/*isExplicit=*/false);
        // A view's history rotates once per EndScene and is measured against
        // one frame of the world; rendering it twice in one frame would hand
        // its second pass a "previous frame" that is the same frame.
        OLO_CORE_ASSERT(s_Data.ViewClock.LastRenderedFrameSerial != frame.Serial,
                        "Renderer3D::BeginScene: view {} already rendered in this physical frame", s_Data.ActiveView.Value);

        frame.InScene = true;
        frame.InPrimaryView = frame.ViewsBegun == 0u;
        ++frame.ViewsBegun;
        ++frame.Counters.ViewFrames;
        s_Data.ViewClock.LastRenderedFrameSerial = frame.Serial;
        s_Data.ViewClock.WasPrimary = frame.InPrimaryView;
        ++s_Data.ViewClock.FramesRendered;
        s_Data.ViewClock.FrameBegan = std::chrono::steady_clock::now();
    }

    void Renderer3D::EndViewFrame()
    {
        auto& frame = s_Data.PhysicalFrame;
        if (frame.InScene)
            s_Data.ViewClock.LastFrameCpuMs =
                std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - s_Data.ViewClock.FrameBegan).count();
        frame.InScene = false;
        frame.InPrimaryView = false;
        frame.LinkingDraws = false;
        if (frame.Open && !frame.Explicit)
            ClosePhysicalFrame();
    }
} // namespace OloEngine
