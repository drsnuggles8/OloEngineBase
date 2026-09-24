#pragma once

// =============================================================================
// The operation language of the renderer state-machine harness (issue #1349):
// what a trace is, what each operation does to the abstract configuration, how
// a seed becomes a trace, and how a trace is written to and read from a file.
//
// Everything here is pure CPU and device-free. The device harness
// (RendererStateMachineHarness.h) is what turns an operation into renderer
// calls; this file decides WHICH calls, so it is pinned in every job.
//
// Two properties the rest of the harness leans on:
//
//   * Every operation is TOTAL. It is legal in every configuration, and
//     applying it where it "does nothing" (MSAA on the forward path, removing a
//     mesh that is not there) is well defined. So every subsequence of a valid
//     trace is valid, and ddmin (TraceMinimizer.h) can drop any operation.
//   * `ModelConfig` is the complete abstract state the operations move. Two
//     traces that end in equal ModelConfigs must render equal frames: that is
//     the fresh-vs-sequence relation, and `ConfigureDirectly` in the harness
//     reaches a ModelConfig without any history at all.
//
// The generator draws from its own SplitMix64 rather than a std distribution,
// because those are not specified bit for bit and a seed must name the same
// trace on MSVC and libstdc++ (std-distributions-are-not-portable.md).
// =============================================================================

#include "OloEngine/Core/Base.h"

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace OloEngine::Tests::StateMachine
{
    enum class OpKind : u8
    {
        Resize,
        Path,
        Feature,
        Msaa,
        Upscale,
        ShaderReload,
        SceneReload,
        EntityChurn,
        FenceDrain,
        FramesInFlight,
        SceneSwap,
        HistoryAdvance,
        CameraMove,
        PoolTrim,
        Count
    };

    enum class FeatureId : u8
    {
        Bloom,
        FXAA,
        GTAO,
        GTAODenoise,
        SSR,
        VignetteGrading,
        Count
    };

    struct Size
    {
        u32 Width = 0;
        u32 Height = 0;
    };

    // The finite parameter sets. An operation's argument is an index into one
    // of these, so a trace can only name configurations the harness knows how
    // to reach directly.
    inline constexpr std::array<Size, 4> kSizes{ Size{ 320u, 180u }, Size{ 384u, 216u }, Size{ 321u, 181u },
                                                 Size{ 256u, 144u } };
    // Indices into UpscaleMode as the engine numbers it: Off, Performance, Quality.
    inline constexpr std::array<i32, 3> kUpscaleModes{ 0, 3, 1 };
    inline constexpr std::array<u32, 2> kMsaaSamples{ 1u, 4u };
    inline constexpr u32 kPoseCount = 3u;
    inline constexpr u32 kPathCount = 3u; // RenderingPath::Forward, ForwardPlus, Deferred

    struct Op
    {
        OpKind Kind = OpKind::Resize;
        // Meaning depends on Kind (see ToString). Always a small index or flag.
        u32 Arg = 0;

        auto operator==(const Op&) const -> bool = default;
    };

    // The abstract configuration. Every field is something at least one
    // operation sets and the harness can set directly.
    struct ModelConfig
    {
        u32 Path = 0;         // RenderingPath
        u32 SizeIndex = 0;    // kSizes
        u32 MsaaIndex = 0;    // kMsaaSamples
        u32 UpscaleIndex = 0; // kUpscaleModes
        u32 Features = 0;     // bit per FeatureId
        bool BlendedMesh = false;
        bool DoubleBuffering = true;
        u32 Pose = 0;

        [[nodiscard]] bool HasFeature(FeatureId feature) const
        {
            return (Features & (1u << static_cast<u32>(feature))) != 0u;
        }

        auto operator==(const ModelConfig&) const -> bool = default;
    };

    // What an operation does to the abstract configuration. Operations that
    // move only hidden state (reloads, fences, pool trims, scene swaps,
    // history) return the configuration unchanged: that they leave the frame
    // unchanged too is exactly what the harness checks.
    [[nodiscard]] ModelConfig Apply(ModelConfig config, const Op& op);

    struct Trace
    {
        // 0 for a hand-written trace (the regression corpus).
        u64 Seed = 0;
        ModelConfig Initial{};
        std::vector<Op> Ops;

        [[nodiscard]] ModelConfig Final() const;
    };

    // A deterministic trace of `length` operations from `seed`. The same seed
    // and length give the same trace on every platform and compiler.
    [[nodiscard]] Trace GenerateTrace(u64 seed, u32 length);

    // "resize 384x216", "feature ssr on", "path deferred", ... One operation,
    // no newline.
    [[nodiscard]] std::string ToString(const Op& op);
    [[nodiscard]] std::optional<Op> ParseOp(std::string_view text);

    // The file format:
    //
    //   olo-state-machine-trace 1
    //   seed 1349
    //   initial path=forward size=320x180 msaa=1 upscale=off features= blended=0 buffering=2 pose=0
    //   op path deferred
    //   op feature ssr on
    //   ...
    //
    // Blank lines and lines starting with '#' are ignored. Serialize then Parse
    // is the identity, which a CPU test pins.
    [[nodiscard]] std::string Serialize(const Trace& trace);
    [[nodiscard]] std::optional<Trace> ParseTrace(std::string_view text, std::string* error = nullptr);

    [[nodiscard]] std::string DescribeConfig(const ModelConfig& config);

    // SplitMix64: the generator's source of randomness. Exposed so the policy
    // tests can build randomised graphs from the same portable stream.
    class SplitMix64
    {
      public:
        explicit SplitMix64(u64 seed) : m_State(seed) {}

        u64 Next()
        {
            u64 z = (m_State += 0x9E3779B97F4A7C15ull);
            z = (z ^ (z >> 30u)) * 0xBF58476D1CE4E5B9ull;
            z = (z ^ (z >> 27u)) * 0x94D049BB133111EBull;
            return z ^ (z >> 31u);
        }

        // Uniform in [0, bound) by Lemire's multiply-shift on the high 32 bits.
        // Portable because it is integer arithmetic only.
        u32 Below(u32 bound)
        {
            return static_cast<u32>((static_cast<u64>(static_cast<u32>(Next() >> 32u)) * bound) >> 32u);
        }

      private:
        u64 m_State;
    };
} // namespace OloEngine::Tests::StateMachine
