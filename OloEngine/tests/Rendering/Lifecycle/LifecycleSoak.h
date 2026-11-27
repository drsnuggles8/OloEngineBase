#pragma once

// =============================================================================
// The device-free half of the lifecycle soak (issue #1796): how two composited
// frames are compared, how a run of memory samples is judged bounded or
// growing, and how a run is written down for replay. Pure CPU, so it is pinned
// in every job (LifecycleSoakStatsTest.cpp); the device half is
// LifecycleSoakTest.cpp.
// =============================================================================

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/FrameTimeTail.h"

#include <iosfwd>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace OloEngine::Tests::Lifecycle
{
    // One composited frame (UIComposite RT0, RGBA8, rows as ReadbackComposite
    // returns them).
    struct Frame
    {
        std::vector<u8> Rgba;
        u32 Width = 0;
        u32 Height = 0;
    };

    // How far apart two frames are. The tile term is the per-tile, per-channel
    // mean difference (0..255): it averages the per-pixel jitter of a temporal
    // pass away but still sees a region that changed, which a whole-frame mean
    // does not. `DifferingFraction` counts pixels with any channel more than
    // kPixelThreshold apart.
    struct FrameDiff
    {
        bool ShapeMismatch = false;
        f64 MaxTileShift = 0.0;
        f64 DifferingFraction = 0.0;
        u32 MaxDelta = 0;
        [[nodiscard]] std::string Describe() const;
    };

    inline constexpr u32 kTileSize = 16u;
    inline constexpr u32 kPixelThreshold = 8u;

    [[nodiscard]] FrameDiff CompareFrames(const Frame& a, const Frame& b);

    // A comparison against a control is held to the control's own noise floor
    // (the same state rendered twice), scaled, with absolute minimums so a
    // bit-exact control does not turn one rounding step into a failure.
    inline constexpr f64 kFloorScale = 4.0;
    inline constexpr f64 kMinTileShift = 1.0;           // of 255
    inline constexpr f64 kMinDifferingFraction = 0.002; // 0.2 % of the pixels

    struct FrameVerdict
    {
        bool Held = true;
        FrameDiff Diff;
        FrameDiff Floor;
        [[nodiscard]] std::string Describe() const;
    };

    [[nodiscard]] FrameVerdict JudgeAgainstControl(const FrameDiff& diff, const FrameDiff& floor);

    // A series of one memory figure, one sample per cycle after warm-up.
    // Bounded when the last sample is no more than `toleranceBytes` above the
    // first: a warm cache fills during warm-up and then stays, a leak keeps
    // adding every cycle. The slope is reported, not judged, because two
    // cycles (the per-PR smoke) cannot fit one.
    struct GrowthVerdict
    {
        u64 First = 0;
        u64 Last = 0;
        u64 Peak = 0;
        i64 Growth = 0;
        f64 SlopePerCycle = 0.0;
        u64 ToleranceBytes = 0;
        bool Bounded = true;
        [[nodiscard]] std::string Describe(std::string_view what) const;
    };

    [[nodiscard]] GrowthVerdict JudgeGrowth(std::span<const u64> samples, u64 toleranceBytes);

    // Everything a cycle measured at its "returned to A" checkpoint.
    struct CycleRecord
    {
        u32 Cycle = 0;
        bool WarmUp = false;
        u64 FrameIndexAtReturn = 0;
        u64 GpuResidentBytes = 0;
        u64 GpuRetiringBytes = 0;
        u64 ProcessResidentBytes = 0;
        FrameDiff ReturnToA;
        FrameTimeTailStats FrameTimes;
    };

    // One run of the soak, as written to --olo-lifecycle-soak-report.
    struct Report
    {
        std::string Leg;     // "editor-loose", "runtime-cooked"
        std::string Backend; // "OpenGL", "Vulkan"
        std::string Build;   // commit and configuration the binary was built from
        u64 Seed = 0;        // the sequence is fixed; 0 says so
        u32 WarmUpCycles = 0;
        u32 Cycles = 0;
        std::vector<std::string> Operations; // one cycle's operations, in order
        std::vector<std::string> Skipped;    // "<operation>: <reason>"
        std::vector<std::pair<std::string, u32>> Executed;
        std::vector<CycleRecord> Records;
        FrameTimeTailStats AllFrameTimes;
        std::optional<GrowthVerdict> GpuGrowth;
        std::optional<GrowthVerdict> ProcessGrowth;
        std::optional<FrameDiff> AfterRestart;
        std::optional<FrameDiff> FreshProcessControl;
        u32 ValidationDiagnostics = 0;
    };

    void WriteJson(std::ostream& out, const Report& report);

    // A frame as a small binary file, for the fresh-process control: the soak
    // process reads what a process that only opened A wrote.
    [[nodiscard]] bool SaveFrame(const std::string& path, const Frame& frame);
    [[nodiscard]] std::optional<Frame> LoadFrame(const std::string& path);
} // namespace OloEngine::Tests::Lifecycle
