#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/RHI/RHITypes.h"
#include "OloEngine/Renderer/RenderGraphDeclarationKey.h"

#include <string>
#include <string_view>
#include <type_traits>

namespace OloEngine
{
    enum class RenderingPath : u8;
    enum class AOTechnique : i32;
    enum class UpscaleMode : i32;
    enum class ColorBlindMode : i32;

    // =========================================================================
    // THE list of pipeline-level inputs a render-graph declaration depends on
    // (issue #1333). One line per field: X(Type, Name).
    //
    // The struct below, its cache key, its equality and its field-by-field diff
    // are all generated from this list, so a field cannot feed
    // PopulateBlackboard or a pass's Setup() without also feeding the key. That
    // replaces a fingerprint assembled by hand, where each input had to be
    // remembered twice and several were remembered once.
    //
    // What belongs here: a value that decides WHETHER something is declared,
    // WHICH resource is declared, or a declared resource's DESCRIPTOR (size,
    // format, samples), or an imported resource's IDENTITY.
    //
    // What does not: anything only Execute() reads. Camera matrices, jitter,
    // time, object transforms, UBO values and history GENERATIONS are
    // execution data; adding one here rebuilds the graph whenever it moves, and
    // RenderGraphFingerprint.ExecutionOnlyInputsLeaveTheKeyAlone names the
    // execution-only inputs that must leave the key alone.
    //
    // Per-pass state (enabled, ready, and whatever a pass's own Setup() branches
    // on) is not listed field by field: RenderPipeline::CaptureDeclarationConfig
    // walks every pass the pipeline owns and folds each one's
    // RenderGraphNode::AppendDeclarationInputs into PassStates.
    // =========================================================================
#define OLO_FRAME_GRAPH_DECLARATION_FIELDS(X)                                                         \
    /* Graph identity: bumped by every ResetTopology(), which wipes the blackboard. */                \
    X(u64, TopologyGeneration)                                                                        \
    /* Sizes. The display size drives the post chain, the scene band the lit scene. */                \
    X(u32, DisplayWidth)                                                                              \
    X(u32, DisplayHeight)                                                                             \
    X(u32, SceneBandWidth)                                                                            \
    X(u32, SceneBandHeight)                                                                           \
    /* Path and the G-Buffer populate declares from (the object, not the setting). */                 \
    X(RenderingPath, Path)                                                                            \
    X(u32, GBufferWidth)                                                                              \
    X(u32, GBufferHeight)                                                                             \
    X(u32, GBufferSamples)                                                                            \
    X(bool, OITEnabled)                                                                               \
    X(AOTechnique, GraphAOTechnique)                                                                  \
    /* Shadow storage, by identity. */                                                                \
    X(u32, ShadowResolution)                                                                          \
    X(u32, ShadowAtlasResolution)                                                                     \
    X(RHI::ResourceHandle, ShadowCSM)                                                                 \
    X(RHI::ResourceHandle, ShadowAtlas)                                                               \
    X(RHI::ResourceHandle, ShadowCSMRaw)                                                              \
    X(RHI::ResourceHandle, ShadowAtlasRaw)                                                            \
    /* Imported long-lived textures, by identity. */                                                  \
    X(RHI::ResourceHandle, IrradianceMap)                                                             \
    X(RHI::ResourceHandle, PrefilterMap)                                                              \
    X(RHI::ResourceHandle, BRDFLut)                                                                   \
    X(RHI::ResourceHandle, VolumetricShadowVolume)                                                    \
    /* Settings PopulateBlackboard gates a declaration on. Resolved verdicts where one exists. */     \
    X(bool, SSAOEnabled)                                                                              \
    X(bool, GTAOEnabled)                                                                              \
    X(bool, SSGIHalfResolution)                                                                       \
    X(UpscaleMode, Upscale)                                                                           \
    X(bool, TemporalUpscaleActive)                                                                    \
    X(bool, DOFEnabled)                                                                               \
    X(bool, MotionBlurEnabled)                                                                        \
    X(bool, EngineTAA)                                                                                \
    X(bool, CloudscapeEnabled)                                                                        \
    X(bool, PrecipitationScreenEffects)                                                               \
    X(bool, FogEnabled)                                                                               \
    X(bool, ChromaticAberrationEnabled)                                                               \
    X(bool, ColorGradingEnabled)                                                                      \
    X(bool, LateSharpen)                                                                              \
    X(bool, VignetteEnabled)                                                                          \
    X(bool, FXAAEnabled)                                                                              \
    X(bool, SkinDiffusionEnabled)                                                                     \
    X(bool, SelectionOutlineActive)                                                                   \
    X(bool, OverdrawDebugView)                                                                        \
    X(ColorBlindMode, ColorBlind)                                                                     \
    /* History imports: whether last frame left something to import, sampled AFTER storage resize. */ \
    X(bool, TAAHistoryValid)                                                                          \
    X(bool, CloudsHistoryValid)                                                                       \
    X(bool, SSRHistoryValid)                                                                          \
    X(u64, TemporalHistoryValidity)                                                                   \
    /* Every pipeline pass: present, enabled, ready, and its own Setup() inputs. */                   \
    X(u64, PassStates)

    struct FrameGraphDeclarationConfig
    {
#define OLO_DECLARATION_CONFIG_FIELD(Type, Name) Type Name{};
        OLO_FRAME_GRAPH_DECLARATION_FIELDS(OLO_DECLARATION_CONFIG_FIELD)
#undef OLO_DECLARATION_CONFIG_FIELD

        // Calls `visitor(std::string_view name, Type FrameGraphDeclarationConfig::* member)`
        // once per field, in list order. The key, the diff and the tests all
        // walk the fields through this, so none of them keeps a list of its own.
        template<typename Visitor>
        static constexpr void ForEachField(Visitor&& visitor)
        {
#define OLO_DECLARATION_CONFIG_VISIT(Type, Name) visitor(std::string_view{ #Name }, &FrameGraphDeclarationConfig::Name);
            OLO_FRAME_GRAPH_DECLARATION_FIELDS(OLO_DECLARATION_CONFIG_VISIT)
#undef OLO_DECLARATION_CONFIG_VISIT
        }

        static constexpr u32 kFieldCount = []
        {
            u32 count = 0;
#define OLO_DECLARATION_CONFIG_COUNT(Type, Name) ++count;
            OLO_FRAME_GRAPH_DECLARATION_FIELDS(OLO_DECLARATION_CONFIG_COUNT)
#undef OLO_DECLARATION_CONFIG_COUNT
            return count;
        }();

        [[nodiscard]] u64 ComputeKey() const noexcept
        {
            RGDeclarationKey key;
            ForEachField([this, &key](std::string_view, auto member)
                         { key.Add(this->*member); });
            return key.Get();
        }

        [[nodiscard]] auto operator==(const FrameGraphDeclarationConfig&) const -> bool = default;

        // Comma-separated names of the fields that differ from `other`, or an
        // empty string. This is what a rebuild is attributed to in the stats and
        // the diagnostics log: "Upscale,SceneBandWidth" instead of "the hash moved".
        [[nodiscard]] std::string DescribeDifferences(const FrameGraphDeclarationConfig& other) const
        {
            std::string changed;
            ForEachField([this, &other, &changed](std::string_view name, auto member)
                         {
                             if (!(this->*member == other.*member))
                             {
                                 if (!changed.empty())
                                     changed += ',';
                                 changed += name;
                             } });
            return changed;
        }
    };

    static_assert(std::is_trivially_copyable_v<FrameGraphDeclarationConfig>,
                  "FrameGraphDeclarationConfig is copied onto the blackboard every populate; keep it a value type.");

    // How the declaration cache behaved, since the last reset (issue #1333).
    // Every frame lands in exactly one of Compiles or CacheHits.
    struct FrameGraphDeclarationStats
    {
        u64 Frames = 0;
        // The key moved: the blackboard was repopulated and every Setup() re-run.
        u64 Compiles = 0;
        // The key held: both layers were served from the cache.
        u64 CacheHits = 0;
        // A compile whose plan came out identical to the one it replaced. The
        // key moved for an input that changed nothing: over-invalidation. The
        // first compile after a reset has nothing to compare with and is not
        // counted.
        u64 RedundantCompiles = 0;
        // Verify mode (OLO_RG_VERIFY_DECLARATION_CACHE) only: cache hits that
        // were rebuilt anyway and compared, and how many of those rebuilds
        // produced a different plan than the cache held. A detection is an
        // input missing from the configuration: under-invalidation.
        u64 VerifiedHits = 0;
        u64 StaleCacheDetections = 0;

        // CPU time of populate + upload + BuildFrameGraph, split by outcome.
        f64 CompileMicrosTotal = 0.0;
        f64 CacheHitMicrosTotal = 0.0;
        f64 LastCompileMicros = 0.0;
        f64 LastCacheHitMicros = 0.0;
        // Of CompileMicrosTotal and verified rebuilds: the plan digest alone.
        f64 DigestMicrosTotal = 0.0;

        // What moved the key on the last compile: configuration field names,
        // and for PassStates, the names of the passes whose inputs changed.
        std::string LastCompileCause;
        // The plan entries ("pass:X", "resource:Y") that differed on the last
        // stale-cache detection, together with the key's cause at the time.
        std::string LastStaleCacheDetail;
    };
} // namespace OloEngine
