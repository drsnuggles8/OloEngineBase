#pragma once

#include "OloEngine/Containers/Array.h"
#include "OloEngine/Containers/String.h"
#include "OloEngine/Core/Base.h"

#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>

// ============================================================================
// Out-of-band work in the render graph (issue #1331)
//
// The graph derives order, reachability, barriers and lifetimes from declared
// resource accesses. Some frame work touches state the graph cannot back with a
// physical resource: the ray-tracing TLAS, deformed vertex buffers reached by
// device address, the retained occlusion pyramid, and CPU data one pass hands
// to another (a command bucket, a texture id, a parameter block). Before this
// file those edges were `DependsOnPass` strings, `NeverCull`, or registration
// order.
//
// An OUT-OF-BAND BOUNDARY names such a datum. Passes declare it in Setup():
//
//   builder.WriteOutOfBand(RGOutOfBandBoundaries::SceneTLAS);             // producer
//   builder.ReadOutOfBand(RGOutOfBandBoundaries::SceneTLAS);              // consumer
//   builder.ReadOutOfBand(RGOutOfBandBoundaries::OcclusionHZB,
//                         RGOutOfBandEpoch::PreviousFrame);               // retained read
//   builder.Publish(RGOutOfBandBoundaries::FroxelFogVolume);              // CPU publication
//   builder.ConsumePublication(RGOutOfBandBoundaries::FroxelFogVolume);
//
// A declaration is logical only. It orders passes, keeps producers reachable
// and is validated for hazards, but it never reaches the barrier, transient,
// registry or submission planners. The code that owns the object records its
// own barriers, exactly as before.
//
// The declaration is checked against what actually happens. Each access site
// calls RGOutOfBand::Note(); the ledger attributes the call to the executing
// pass (or to the frame prologue / epilogue around the graph), and
// RenderGraph::ValidateOutOfBandLedger() reports every access nobody declared
// and every read that ran on the wrong side of a write.
// ============================================================================

namespace OloEngine
{
    enum class RGOutOfBandKind : u8
    {
        // GPU memory the graph cannot back: a TLAS, a buffer reached by
        // device address, an engine-owned texture rebuilt in place.
        GpuResource = 0,
        // CPU state a pass prepares for another: a command bucket, a texture
        // id, a flag, a parameter block. Kept distinct from GPU hazards: an
        // edge orders the passes, it does not prove the right block is bound.
        CpuPublication = 1,
    };

    enum class RGOutOfBandEpoch : u8
    {
        // This frame's value: every in-graph writer runs first.
        CurrentFrame = 0,
        // The value the previous frame left behind: the read runs before
        // every in-graph writer of this frame (a retained pyramid that a
        // later pass rebuilds in place).
        PreviousFrame = 1,
    };

    enum class RGOutOfBandAccess : u8
    {
        Read = 0,
        ReadPreviousFrame = 1,
        Write = 2,
    };

    // What frame work outside RenderGraph::Execute does with a boundary.
    enum class RGFramePhaseUse : u8
    {
        None = 0,
        Read = 1,
        Write = 2,
        ReadWrite = 3,
    };

    [[nodiscard]] constexpr bool FramePhaseUseAllows(const RGFramePhaseUse use, const RGOutOfBandAccess access)
    {
        switch (access)
        {
            case RGOutOfBandAccess::Read:
            case RGOutOfBandAccess::ReadPreviousFrame:
                return use == RGFramePhaseUse::Read || use == RGFramePhaseUse::ReadWrite;
            case RGOutOfBandAccess::Write:
                return use == RGFramePhaseUse::Write || use == RGFramePhaseUse::ReadWrite;
        }
        return false;
    }

    struct RGOutOfBandBoundary
    {
        std::string_view Name;
        RGOutOfBandKind Kind = RGOutOfBandKind::GpuResource;
        // Access from the frame prologue (BeginScene through CompileFrameGraph)
        // and epilogue (after Execute, before EndScene returns). Anything else
        // outside a pass is undeclared.
        RGFramePhaseUse Prologue = RGFramePhaseUse::None;
        RGFramePhaseUse Epilogue = RGFramePhaseUse::None;
        // Where the object lives.
        std::string_view Owner;
        // Why the graph cannot back it as a resource.
        std::string_view Reason;
    };

    // Production boundary names. Declarations and access sites refer to these,
    // never to string literals, so a typo is a compile error.
    namespace RGOutOfBandBoundaries
    {
        inline constexpr std::string_view SceneTLAS = "SceneTLAS";
        inline constexpr std::string_view DeformedVertices = "DeformedVertices";
        inline constexpr std::string_view OcclusionHZB = "OcclusionHZB";
        inline constexpr std::string_view ForwardPlusLightClusters = "ForwardPlusLightClusters";
        inline constexpr std::string_view SceneOpaqueCommandBucket = "SceneOpaqueCommandBucket";
        inline constexpr std::string_view FroxelFogVolume = "FroxelFogVolume";
        inline constexpr std::string_view PlanarReflectionTexture = "PlanarReflectionTexture";
        inline constexpr std::string_view DDGIProbeVolume = "DDGIProbeVolume";
        inline constexpr std::string_view FluidIntermediates = "FluidIntermediates";
        inline constexpr std::string_view SSAOParameters = "SSAOParameters";
    } // namespace RGOutOfBandBoundaries

    [[nodiscard]] std::span<const RGOutOfBandBoundary> GetProductionOutOfBandBoundaries();

    // Frame work that runs wholly outside RenderGraph::Execute and touches no
    // in-graph boundary: GPU-scene uploads, simulations, readback closes. It
    // is ordered by construction (the prologue precedes every pass, the
    // epilogue follows every pass, on one queue) and named here so the
    // exposed schedule accounts for it.
    enum class RGFramePhase : u8
    {
        Prologue = 0,
        Epilogue = 1,
    };

    struct RGFramePhaseWork
    {
        std::string_view Name;
        RGFramePhase Phase = RGFramePhase::Prologue;
        std::string_view Owner;     // the function that issues it
        std::string_view Consumers; // who reads what it produces
        std::string_view Reason;    // why it is outside the graph
    };

    [[nodiscard]] std::span<const RGFramePhaseWork> GetFramePhaseWork();

    // Why each production side-effecting pass is kept alive. A pass that
    // sets a side effect without an entry here fails
    // RenderGraphOutOfBandSchedule.EverySideEffectIsNamed.
    struct RGSideEffectReason
    {
        std::string_view PassName;
        std::string_view Reason;
    };

    [[nodiscard]] std::span<const RGSideEffectReason> GetSideEffectReasons();
    [[nodiscard]] std::string_view FindSideEffectReason(std::string_view passName);

    [[nodiscard]] std::string_view ToString(RGOutOfBandKind kind);
    [[nodiscard]] std::string_view ToString(RGOutOfBandAccess access);
    [[nodiscard]] std::string_view ToString(RGFramePhaseUse use);
    [[nodiscard]] std::string_view ToString(RGFramePhase phase);

    // One declared out-of-band access, recorded per pass by RGBuilder.
    struct RGOutOfBandDeclaration
    {
        FString Boundary;
        RGOutOfBandKind Kind = RGOutOfBandKind::GpuResource;
        RGOutOfBandAccess Access = RGOutOfBandAccess::Read;
    };

    // ------------------------------------------------------------------------
    // Runtime ledger
    // ------------------------------------------------------------------------
    enum class RGLedgerPhase : u8
    {
        Idle = 0, // no frame open: accesses are not recorded
        Prologue = 1,
        Graph = 2,
        Epilogue = 3,
    };

    [[nodiscard]] std::string_view ToString(RGLedgerPhase phase);

    class RGOutOfBandLedger
    {
      public:
        struct Entry
        {
            FString Boundary;
            FString Pass; // empty outside a pass
            RGLedgerPhase Phase = RGLedgerPhase::Idle;
            RGOutOfBandAccess Access = RGOutOfBandAccess::Read;
        };

        void BeginFrame();
        void BeginGraph();
        void BeginEpilogue();
        // Closes the frame; the entries stay readable until the next BeginFrame.
        void EndFrame();

        void SetActivePass(std::string_view passName);
        void ClearActivePass();

        void Note(std::string_view boundary, RGOutOfBandAccess access);

        [[nodiscard]] RGLedgerPhase GetPhase() const;
        [[nodiscard]] TArray64<Entry> GetEntries() const;
        // Advanced by every BeginFrame: identifies one scene frame.
        [[nodiscard]] u64 GetFrameSerial() const;

      private:
        mutable std::mutex m_Mutex;
        RGLedgerPhase m_Phase = RGLedgerPhase::Idle;
        FString m_ActivePass;
        TArray64<Entry> m_Entries;
        u64 m_FrameSerial = 0;
    };

    namespace RGOutOfBand
    {
        // The ledger that Note() records into; null disables recording. The
        // renderer points it at its graph's ledger for the lifetime of the
        // graph, tests at their own.
        void SetActiveLedger(RGOutOfBandLedger* ledger);
        [[nodiscard]] RGOutOfBandLedger* GetActiveLedger();

        // Record an access at the site that performs it. Cheap and a no-op
        // outside a frame.
        void Note(std::string_view boundary, RGOutOfBandAccess access);

        // The active ledger's frame serial, 0 without one. A publication that
        // a consumer gates on ("did the producer run THIS frame?") stamps it,
        // so the answer stays right when the producer is culled and its
        // Execute never gets to clear the flag.
        [[nodiscard]] u64 GetFrameSerial();

        // Attribution for the executor: the pass whose Prepare / Execute /
        // Publish is running on the caller.
        void SetActivePass(std::string_view passName);
        void ClearActivePass();

        // FAULT (#1331 negative control): drop matching out-of-band declarations
        // in RGBuilder as if the pass had never made them, while the access
        // site still runs. "Boundary" drops every pass's declaration of it,
        // "Pass/Boundary" one pass's. Seeded once from the text lever
        // Levers::FaultOmitOutOfBandDeclaration
        // (OLO_FAULT_OMIT_OUT_OF_BAND_DECLARATION); tests override it here.
        // Never set outside a negative control: it breaks ordering on purpose.
        void SetOmittedDeclarationFault(std::optional<std::string> spec);
        [[nodiscard]] bool IsDeclarationOmittedByFault(std::string_view passName, std::string_view boundary);

        // RAII form for a scope that runs one pass body.
        class ScopedActivePass
        {
          public:
            explicit ScopedActivePass(std::string_view passName)
            {
                SetActivePass(passName);
            }
            ~ScopedActivePass()
            {
                ClearActivePass();
            }
            ScopedActivePass(const ScopedActivePass&) = delete;
            ScopedActivePass& operator=(const ScopedActivePass&) = delete;
        };
    } // namespace RGOutOfBand

    // The owned names are heap-backed; everything else is value state.
    template<>
    struct TIsTriviallyRelocatable<RGOutOfBandDeclaration>
    {
        static constexpr bool Value = TIsTriviallyRelocatable_V<decltype(RGOutOfBandDeclaration::Boundary)> &&
                                      TIsTriviallyRelocatable_V<decltype(RGOutOfBandDeclaration::Kind)> &&
                                      TIsTriviallyRelocatable_V<decltype(RGOutOfBandDeclaration::Access)>;
    };
    template<>
    struct TIsTriviallyRelocatable<RGOutOfBandLedger::Entry>
    {
        static constexpr bool Value = TIsTriviallyRelocatable_V<decltype(RGOutOfBandLedger::Entry::Boundary)> &&
                                      TIsTriviallyRelocatable_V<decltype(RGOutOfBandLedger::Entry::Pass)> &&
                                      TIsTriviallyRelocatable_V<decltype(RGOutOfBandLedger::Entry::Phase)> &&
                                      TIsTriviallyRelocatable_V<decltype(RGOutOfBandLedger::Entry::Access)>;
    };
} // namespace OloEngine
