#pragma once

#include "OloEngine/Containers/Array.h"
#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/RenderingPath.h"

#include <array>
#include <optional>
#include <type_traits>

// Whole-renderer memory admission (issue #1343).
//
// RendererMemoryTracker (#1342) reports what the renderer holds. This file decides what it
// may hold: a requested quality configuration is ADMITTED against a device budget before
// its resources are allocated, or reduced along one documented ladder until it fits, and
// climbs back when the pressure is gone.
//
// Plain data and pure functions, so every rule here unit-tests with no device. Renderer3D
// feeds it measurements and applies its decision (Renderer3DMemoryAdmission.cpp).
//
// The rules:
//
//   1. THE REQUEST IS NEVER EDITED. The governor returns a realized configuration beside
//      the requested one. A surface that reports quality reports both, so nothing claims
//      the requested preset after a reduction.
//   2. ONE LADDER, IN ONE ORDER (kMemoryLadder). A level is "the first N rungs applied".
//      The same request under the same measurements always lands on the same level with
//      the same reason.
//   3. EVERY LEVEL IS A COMPLETE IMAGE. A rung removes samples, resolution of one history,
//      pooled slack, optional detail or a whole optional technique whose fallback is the
//      technique underneath it. No rung removes geometry or the lit scene. The last level
//      is the minimum complete image; when even that is over budget the governor says so
//      (OverBudgetAtFloor) and keeps rendering it.
//   4. A REPLACEMENT IS CHARGED TWICE UNTIL IT RETIRES. Memory an owner released stays
//      resident until the backend's deletion queue frees it, so a transition is admitted
//      against resident bytes (live + retiring) plus what the target newly allocates.
//   5. DOWN AT ONCE, UP ONLY AFTER A STABLE RUN. A climb needs headroom beyond the
//      reserve for RecoveryFrames consecutive frames, and a climb that had to be undone
//      doubles the wait.
namespace OloEngine
{
    // The optional techniques that hold screen-sized memory of their own. All but the
    // path tracer are rungs of the ladder, removed in this order; each one's fallback is
    // what the renderer draws without it.
    enum class MemoryTechnique : u8
    {
        ReSTIRGI = 0,         // falls back to SSGI / DDGI / image-based ambient
        ReSTIRDI,             // falls back to analytic direct lighting with shadow maps
        ReSTIRPT,             // falls back to the raster lit scene
        RayTracedReflections, // falls back to SSR / image-based reflections
        RayTracedShadows,     // falls back to shadow maps
        SSGI,                 // falls back to DDGI / image-based ambient
        SSR,                  // falls back to image-based reflections
        MotionBlur,           // falls back to none
        DepthOfField,         // falls back to none
        Bloom,                // falls back to none
        PathTracer,           // counted, never removed: see kMemoryLadder
        COUNT
    };

    inline constexpr u32 kMemoryTechniqueCount = static_cast<u32>(MemoryTechnique::COUNT);

    [[nodiscard]] constexpr u32 MemoryTechniqueBit(const MemoryTechnique technique)
    {
        return 1u << static_cast<u32>(technique);
    }

    // The part of a view's quality configuration that sizes renderer memory and that the
    // governor may reduce. Everything else a view is configured with is left alone.
    struct MemoryQualityConfig
    {
        RenderingPath Path = RenderingPath::Forward;
        // The graph's physical size, and the scene band below it under an upscale preset.
        u32 DisplayWidth = 0;
        u32 DisplayHeight = 0;
        u32 SceneWidth = 0;
        u32 SceneHeight = 0;
        // Deferred G-Buffer samples. 1 on the forward paths, which have no MSAA target.
        u32 MsaaSamples = 1;
        // SSGI traces and resolves at half the scene band (the history-resolution rung).
        bool SSGIHalfResolution = true;
        // TransientPool objects kept per idle descriptor bucket.
        u32 TransientPoolSpare = 2;
        // Optional groom / vegetation detail may be resident (#1257). A world-level
        // switch: the same on every view of a frame.
        bool OptionalDetail = true;
        // MemoryTechniqueBit of every optional technique that is on.
        u32 Techniques = 0;

        [[nodiscard]] bool Has(const MemoryTechnique technique) const
        {
            return (Techniques & MemoryTechniqueBit(technique)) != 0u;
        }

        auto operator==(const MemoryQualityConfig&) const -> bool = default;
    };

    enum class MemoryReductionKind : u8
    {
        TransientPoolSlack = 0, // keep no idle pooled transient beyond a frame's demand
        HistoryResolution,      // SSGI traces, resolves and accumulates at half resolution
        MsaaSamples,            // fewer G-Buffer samples (Argument = the new count)
        OptionalDetailPools,    // optional groom / vegetation detail falls to its pinned floor
        TechniqueFallback       // one optional technique off (Argument = MemoryTechnique)
    };

    struct MemoryLadderRung
    {
        MemoryReductionKind Kind = MemoryReductionKind::TransientPoolSlack;
        u8 Argument = 0;
        // The stable reason-code spelling of this rung: reports, logs and tests use it.
        const char* Name = "";
    };

    // THE ladder. Level N is rungs [0, N) applied in this order. Reordering it changes
    // which quality a given budget buys; docs/guides/renderer-memory-budget.md lists it.
    inline constexpr std::array<MemoryLadderRung, 16> kMemoryLadder = { {
        { MemoryReductionKind::TransientPoolSlack, 0, "transientPoolSlack" },
        { MemoryReductionKind::HistoryResolution, 0, "historyResolution" },
        { MemoryReductionKind::MsaaSamples, 4, "msaa4" },
        { MemoryReductionKind::MsaaSamples, 2, "msaa2" },
        { MemoryReductionKind::MsaaSamples, 1, "msaaOff" },
        { MemoryReductionKind::OptionalDetailPools, 0, "optionalDetailPools" },
        { MemoryReductionKind::TechniqueFallback, static_cast<u8>(MemoryTechnique::ReSTIRGI), "restirGIFallback" },
        { MemoryReductionKind::TechniqueFallback, static_cast<u8>(MemoryTechnique::ReSTIRDI), "restirDIFallback" },
        { MemoryReductionKind::TechniqueFallback, static_cast<u8>(MemoryTechnique::ReSTIRPT), "restirPTFallback" },
        { MemoryReductionKind::TechniqueFallback, static_cast<u8>(MemoryTechnique::RayTracedReflections), "rayTracedReflectionFallback" },
        { MemoryReductionKind::TechniqueFallback, static_cast<u8>(MemoryTechnique::RayTracedShadows), "rayTracedShadowFallback" },
        { MemoryReductionKind::TechniqueFallback, static_cast<u8>(MemoryTechnique::SSGI), "ssgiFallback" },
        { MemoryReductionKind::TechniqueFallback, static_cast<u8>(MemoryTechnique::SSR), "ssrFallback" },
        { MemoryReductionKind::TechniqueFallback, static_cast<u8>(MemoryTechnique::MotionBlur), "motionBlurFallback" },
        { MemoryReductionKind::TechniqueFallback, static_cast<u8>(MemoryTechnique::DepthOfField), "depthOfFieldFallback" },
        { MemoryReductionKind::TechniqueFallback, static_cast<u8>(MemoryTechnique::Bloom), "bloomFallback" },
    } };
    // The path tracer is deliberately not a rung: it REPLACES the lit image, so dropping
    // it under pressure would swap the picture the user asked for with a different
    // renderer's. Its accumulation is counted (it is in the footprint), never reduced.

    inline constexpr u32 kMemoryLadderRungCount = static_cast<u32>(kMemoryLadder.size());

    // Applies one rung. Returns whether it changed `config`: a rung that has nothing to
    // remove (MSAA on a forward path, a technique that is already off) is a no-op.
    bool ApplyMemoryLadderRung(MemoryQualityConfig& config, const MemoryLadderRung& rung);

    // `requested` with rungs [0, level) applied. `appliedMask`, when given, receives one
    // bit per rung that changed the configuration.
    [[nodiscard]] MemoryQualityConfig ReduceMemoryQuality(const MemoryQualityConfig& requested, u32 level,
                                                          u32* appliedMask = nullptr);

    // What a view's renderer measured the last time it drew: the screen-sized bytes no
    // formula here can derive from a configuration. Format estimates on OpenGL.
    struct ViewMemoryInventory
    {
        // The display size and scene band the figures were measured at; 0 before the
        // first frame. Pass targets live at either, so a change to either rescales them.
        u32 DisplayWidth = 0;
        u32 DisplayHeight = 0;
        u32 SceneWidth = 0;
        u32 SceneHeight = 0;
        // Persistent pass targets (the scene MRT, the AO chains, the post chain).
        u64 NodeTargetBytes = 0;
        // What the last frame acquired from the transient pool, and what the pool holds
        // beyond that.
        u64 TransientDemandBytes = 0;
        u64 TransientSlackBytes = 0;
        // Histories no rung controls (TAA, the cloudscape): they only scale with a resize.
        u64 OtherHistoryBytes = 0;
        // Each technique's histories as the registry holds them now. 0 for a technique
        // that is on in the settings but not running on this backend or path.
        std::array<u64, kMemoryTechniqueCount> TechniqueHistoryBytes{};

        [[nodiscard]] bool IsMeasured() const
        {
            return DisplayWidth != 0u && DisplayHeight != 0u;
        }
    };

    // Format-estimate bytes of a Deferred G-Buffer: the (multisample) target plus the
    // single-sample resolve target when samples > 1. 0 on the forward paths.
    [[nodiscard]] u64 EstimateGBufferBytes(const MemoryQualityConfig& config);

    // The registry histories `technique` holds at `config`'s resolution, from the layouts
    // RenderPipeline declares. An upper bound: a technique running without its temporal
    // resolve holds less.
    [[nodiscard]] u64 EstimateTechniqueHistoryBytes(MemoryTechnique technique, const MemoryQualityConfig& config);

    // What changing a view from `allocated` (nullptr: nothing allocated yet) to `target`
    // costs. NewBytes are allocated while everything `allocated` holds is still resident;
    // ReleasedBytes are what retires afterwards.
    struct MemoryTransition
    {
        u64 NewBytes = 0;
        u64 ReleasedBytes = 0;
        // False when a measured component had no measurement to scale (a view that has
        // not drawn yet): NewBytes is then a lower bound.
        bool Complete = true;

        MemoryTransition& operator+=(const MemoryTransition& other)
        {
            NewBytes += other.NewBytes;
            ReleasedBytes += other.ReleasedBytes;
            Complete = Complete && other.Complete;
            return *this;
        }
    };

    [[nodiscard]] MemoryTransition EstimateViewTransition(const MemoryQualityConfig* allocated,
                                                          const MemoryQualityConfig& target,
                                                          const ViewMemoryInventory& inventory);

    enum class MemoryBudgetSource : u8
    {
        None = 0,           // no budget is known: nothing is enforced, and the report says so
        OsReported,         // VK_EXT_memory_budget
        AllocatorHeuristic, // VMA without that extension: a fixed share of the heap
        Configured,         // OLO_RENDERER_MEMORY_BUDGET_MB (the only budget OpenGL can have)
        Injected            // Renderer3D::SetMemoryBudgetOverride: tests and olo_memory_budget_set
    };

    // The device side of an admission: what may be used, and what is.
    struct MemoryBudgetObservation
    {
        std::optional<u64> BudgetBytes;
        MemoryBudgetSource Source = MemoryBudgetSource::None;
        // Live + retiring bytes charged against the budget, and the retiring part of them.
        u64 ResidentBytes = 0;
        u64 RetiringBytes = 0;

        [[nodiscard]] u64 LiveBytes() const
        {
            return ResidentBytes >= RetiringBytes ? ResidentBytes - RetiringBytes : 0;
        }
    };

    // Stable reason codes. The reduced ones name the pressure, not the rung: the rungs in
    // effect are MemoryAdmissionSnapshot::ActiveRungMask.
    enum class MemoryAdmissionReason : u8
    {
        Admitted = 0,      // the requested configuration is active
        BudgetUnknown,     // no budget to admit against; the requested configuration is active
        DeviceBudget,      // reduced: the requested configuration's steady footprint does not fit
        TransitionOverlap, // reduced: old + new during the replacement would not fit
        RecoveryHold,      // reduced: the headroom is back and is being timed
        OverBudgetAtFloor  // the minimum complete image is active and is still over budget
    };

    struct MemoryAdmissionPolicy
    {
        // The safety margin kept free for in-flight replacements: the larger of the two.
        u64 ReserveBytes = 64ull * 1024ull * 1024ull;
        f32 ReserveFraction = 0.05f;
        // Extra headroom, as a share of the budget, a climb must leave below the reserve.
        f32 RecoveryBandFraction = 0.05f;
        // Consecutive frames a climb must fit before it is taken.
        u32 RecoveryFrames = 30;
        // Frames after a transition before pressure is judged again: the backends hold a
        // released resource for two frames, and a frame more for the new one to be booked.
        u32 SettleFrames = 4;
        // How many times a failed climb may double RecoveryFrames.
        u32 MaxRecoveryBackoffShift = 4;
    };

    struct MemoryAdmissionViewInput
    {
        u64 View = 0;
        MemoryQualityConfig Requested;
        // What the view's resources were last built for, if it has drawn.
        std::optional<MemoryQualityConfig> Allocated;
        ViewMemoryInventory Inventory;
    };

    struct MemoryAdmissionInput
    {
        MemoryBudgetObservation Budget;
        TArray<MemoryAdmissionViewInput> Views;
        // Optional groom / vegetation detail resident now, and the most it has been:
        // what the optional-detail rung gives back, and what climbing past it costs.
        u64 OptionalDetailBytes = 0;
        u64 KnownOptionalDetailBytes = 0;
    };

    struct MemoryAdmissionViewDecision
    {
        u64 View = 0;
        MemoryQualityConfig Requested;
        MemoryQualityConfig Realized;

        [[nodiscard]] bool IsReduced() const
        {
            return !(Requested == Realized);
        }
    };

    // Requested against realized quality, and why: the one record the renderer stats, the
    // memory report, olo_memory_report and the editor panel all read.
    struct MemoryAdmissionSnapshot
    {
        MemoryBudgetSource BudgetSource = MemoryBudgetSource::None;
        std::optional<u64> BudgetBytes;
        u64 ReserveBytes = 0;
        u64 ResidentBytes = 0;
        u64 RetiringBytes = 0;

        MemoryAdmissionReason Reason = MemoryAdmissionReason::BudgetUnknown;
        // Rungs applied, and the level of the minimum complete image for this request.
        u32 Level = 0;
        u32 FloorLevel = 0;
        // One bit per kMemoryLadder rung that changed some view's configuration.
        u32 ActiveRungMask = 0;

        // What the governor expected of the level it chose: bytes once the replacement
        // has retired, and the most that is resident while it has not.
        u64 PredictedSteadyBytes = 0;
        u64 PredictedPeakBytes = 0;
        bool EstimateComplete = true;

        u32 RecoveryProgressFrames = 0;
        u32 RecoveryRequiredFrames = 0;

        // Since the governor was reset.
        u64 Ticks = 0;
        u64 Transitions = 0;      // level changes the governor made (not request changes)
        u64 Degradations = 0;     // of those, downwards
        u64 Recoveries = 0;       // of those, upwards
        u64 FailedRecoveries = 0; // climbs that met pressure again within one recovery run
        u64 Readmissions = 0;     // fresh admissions of a changed request (a resize is one)
        u64 TicksAtLevel = 0;
        // The most that was resident since the budget or the request last changed.
        u64 PeakResidentBytes = 0;

        TArray<MemoryAdmissionViewDecision> Views;

        [[nodiscard]] bool IsReduced() const
        {
            return Level != 0u;
        }
    };

    // The admission state machine. One per renderer (device-level state, ADR 0025).
    class RendererMemoryGovernor
    {
      public:
        void SetPolicy(const MemoryAdmissionPolicy& policy)
        {
            m_Policy = policy;
        }
        [[nodiscard]] const MemoryAdmissionPolicy& GetPolicy() const
        {
            return m_Policy;
        }

        // Once per physical frame, before any view of the frame allocates.
        const MemoryAdmissionSnapshot& Tick(const MemoryAdmissionInput& input);

        // A replacement outside the frame (a resize): the changed request is admitted now,
        // before its targets are allocated, without advancing the frame clocks.
        const MemoryAdmissionSnapshot& Readmit(const MemoryAdmissionInput& input);

        [[nodiscard]] const MemoryAdmissionSnapshot& GetSnapshot() const
        {
            return m_Snapshot;
        }

        // The realized configuration of `view` in the last decision, or nullptr.
        [[nodiscard]] const MemoryQualityConfig* FindRealized(u64 view) const;

        void Reset();

      private:
        struct LevelEstimate
        {
            u64 NewBytes = 0;
            u64 SteadyBytes = 0;
            u64 PeakBytes = 0;
            bool Complete = true;
        };

        const MemoryAdmissionSnapshot& Evaluate(const MemoryAdmissionInput& input, bool frameTick);
        [[nodiscard]] LevelEstimate EstimateLevel(const MemoryAdmissionInput& input, u32 level) const;
        [[nodiscard]] bool SameRealization(const MemoryAdmissionInput& input, u32 a, u32 b) const;
        [[nodiscard]] u32 Canonical(const MemoryAdmissionInput& input, u32 level) const;
        [[nodiscard]] bool RequestChanged(const MemoryAdmissionInput& input) const;
        // `keepLearned`: the budget has not moved, so what earlier descents and climbs
        // measured about the rungs still applies to the changed request.
        void AdmitFresh(const MemoryAdmissionInput& input, u64 limit, u64 budget, bool keepLearned);
        // What climbing from the current level to `level` costs beyond the model.
        [[nodiscard]] u64 LearnedClimbBytes(u32 level) const;
        void ForgetLearning();
        [[nodiscard]] u32 ChooseDescent(const MemoryAdmissionInput& input, u64 limit, u64 budget, u64 reserve) const;
        void Publish(const MemoryAdmissionInput& input, u64 reserve, u32 recoveryRequired);

        MemoryAdmissionPolicy m_Policy;
        MemoryAdmissionSnapshot m_Snapshot;

        // The request the current level was admitted for.
        TArray<MemoryAdmissionViewDecision> m_AdmittedRequest;
        bool m_HasAdmitted = false;
        // The budget the backoff state below was learned under.
        std::optional<u64> m_BudgetAnchor;

        u32 m_Level = 0;
        // Why the level is below the request: DeviceBudget or TransitionOverlap.
        MemoryAdmissionReason m_ReducedCause = MemoryAdmissionReason::DeviceBudget;
        // The floor is active and measured (or, on a fresh admission, predicted) over the limit.
        bool m_OverAtFloor = false;
        u32 m_SettleTicks = 0;
        u32 m_RecoveryProgress = 0;
        u32 m_RecoveryBackoffShift = 0;

        // The last climb, until it has held for a whole recovery run: where it came from
        // and what it was predicted to cost. Pressure before then is a failed climb.
        bool m_ClimbOutstanding = false;
        u32 m_LevelBeforeClimb = 0;
        u64 m_ClimbPredictedSteady = 0;
        u64 m_TicksSinceClimb = 0;
        // Bytes climbing past each rung costs beyond the model's estimate, learned two
        // ways: a descent that gave back more than the model said (a technique's
        // transients are only measurable), and a climb that had to be undone. Kept per
        // rung, so a climb is charged only for the rungs it re-enables, and a climb that
        // held clears the rungs it passed: their cost is in the live bytes from then on.
        std::array<u64, kMemoryLadderRungCount> m_LearnedRungBytes{};
        // The last descent, until the frame that measures what it really gave back.
        bool m_DescentUnmeasured = false;
        u32 m_DescentLevel = 0;
        u64 m_DescentPredictedSteady = 0;
    };

    [[nodiscard]] const char* ToString(MemoryTechnique technique);
    [[nodiscard]] const char* ToString(MemoryBudgetSource source);
    [[nodiscard]] const char* ToString(MemoryAdmissionReason reason);

    // The TArray elements above are plain values: TArray relocates them bitwise.
    static_assert(std::is_trivially_copyable_v<MemoryAdmissionViewInput>);
    static_assert(std::is_trivially_copyable_v<MemoryAdmissionViewDecision>);
} // namespace OloEngine
