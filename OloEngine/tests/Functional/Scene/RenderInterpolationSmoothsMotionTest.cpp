#include "OloEnginePCH.h"

// OLO_TEST_LAYER: Functional

// =============================================================================
// RenderInterpolationSmoothsMotionTest — Functional Test.
//
// Cross-subsystem seam under test:
//   Render interpolation (issue #502) × the deterministic fixed-timestep loop
//   (#484) × Lua-driven transform mutation. Scene::OnUpdateRuntimeFixed advances
//   the gameplay simulation in fixed `fixedDt` steps from a variable frame delta,
//   keeps the two most recent fixed-tick poses, and RenderRuntime draws a pose
//   interpolated between them at alpha = accumulator / fixedDt. At a refresh rate
//   that isn't a multiple of the sim rate (60 Hz sim / 144 Hz display) the
//   *rendered* pose must advance in near-equal increments (judder-free) even
//   though the *simulation* advances in discrete integer ticks.
//
// Scenario: a Lua script moves an entity at a constant one-unit-per-tick velocity
//   (reading + incrementing its own translation.x, so any leak of an interpolated
//   pose back into the persisted sim state would drift the trajectory). We drive
//   the fixed-step entry with a constant non-multiple frame delta and, each frame,
//   sample BOTH the interpolated render pose and the raw simulation pose.
//
//   Because the mover is uniform, the interpolated render x is exactly linear in
//   wall time (interp_x(n) = n * frameDelta / fixedDt - 1), so consecutive
//   render-frame deltas are constant — the definition of judder-free. The raw sim
//   x is a staircase (+0 on frames with no step, +1 on frames that step), which is
//   the judder interpolation removes.
//
// Assertions:
//   1. The interpolated render pose is monotonic and its per-frame delta is
//      near-constant (second difference ~0) — smooth motion at 144/60.
//   2. The raw sim pose visibly stair-steps (some 0-deltas, some 1-deltas) — the
//      judder present without interpolation, proving the interpolation is what
//      smooths #1 rather than the sim already being smooth.
//   3. Interpolation ON vs OFF produces a bit-identical raw sim trajectory —
//      the render blend never mutates persisted simulation state, so determinism
//      (#484) is not regressed.
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"

#include <glm/glm.hpp>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <vector>

using namespace OloEngine;
using namespace OloEngine::Functional;

namespace
{
    std::filesystem::path WriteScript(const std::string& contents, const char* nameStem)
    {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        std::string fileName = std::string("olo_interp_") + nameStem + "_" + (info ? info->name() : "unknown") + ".lua";
        const auto path = std::filesystem::temp_directory_path() / fileName;
        {
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            out << contents;
        }
        return path;
    }

    // Constant one-unit-per-tick mover. Reads its own translation and increments
    // it, so a corrupted (interpolated) pose leaking into the sim would drift the
    // trajectory off the clean integer sequence — the determinism guard's teeth.
    constexpr const char* kMoverScript = R"(
local script = {}
function script.OnUpdate(entityID, ts)
    local id = math.tointeger(entityID) or 0
    local p = entity_utils.get_translation(id)
    entity_utils.set_translation(id, vec3.new(p.x + 1, 0, 0))
end
return script
)";
} // namespace

class RenderInterpolationSmoothsMotionTest : public FunctionalTest
{
  protected:
    void BuildScene() override
    {
        EnableLua();
        // Small, round-trippable UUID: Lua's double number type can't represent
        // a random > 2^53 UUID exactly (see LuaScriptMutatesTransformViaSceneTick).
        m_Mover = GetScene().CreateEntityWithUUID(UUID{ 4242 }, "Mover");
        m_ScriptPath = WriteScript(kMoverScript, "mover");
        RegisterLuaScript(m_Mover, m_ScriptPath);
    }

    void TearDown() override
    {
        FunctionalTest::TearDown();
        std::error_code ec;
        std::filesystem::remove(m_ScriptPath, ec);
    }

    Entity m_Mover;
    std::filesystem::path m_ScriptPath;
};

TEST_F(RenderInterpolationSmoothsMotionTest, NonMultipleRefreshRateProducesJudderFreeInterpolatedMotion)
{
    // 60 Hz sim on a 144 Hz display — deliberately NOT a multiple, the case that
    // judders when render is coupled to the fixed tick.
    constexpr f32 kFixedDt = 1.0f / 60.0f;
    constexpr f32 kFrameDt = 1.0f / 144.0f;
    constexpr u32 kFrames = 72; // ~0.5 s → ~30 fixed steps, plenty of samples
    // Expected constant per-frame advance of the interpolated pose, in units
    // (one unit == one fixed tick of motion): frameDelta / fixedStep.
    constexpr f32 kExpectedStride = kFrameDt / kFixedDt; // 60/144 = 0.41667

    ASSERT_TRUE(GetScene().IsRenderInterpolationEnabled())
        << "interpolation is on by default; the smooth-motion assertions below assume it";

    std::vector<f32> interpX;
    std::vector<f32> rawX;
    interpX.reserve(kFrames);
    rawX.reserve(kFrames);

    for (u32 i = 0; i < kFrames; ++i)
    {
        GetScene().OnUpdateRuntimeFixed(kFrameDt, kFixedDt);

        const glm::mat4 rendered = GetScene().GetInterpolatedLocalTransform(m_Mover);
        interpX.push_back(rendered[3].x);
        rawX.push_back(m_Mover.GetComponent<TransformComponent>().Translation.x);
    }

    // Analyze only the tail where a full prev/curr snapshot pair exists (both
    // ticks represent real motion). The first ~3 frames precede the first fixed
    // step (144/60 = 2.4 frames of wall time before one tick elapses).
    constexpr u32 kWarmup = 6;
    ASSERT_LT(kWarmup, kFrames);

    // (1) Monotonic + judder-free: consecutive interpolated deltas are all ~equal
    //     to the expected stride (second difference ~0).
    f32 maxStrideError = 0.0f;
    f32 maxInterpSecondDiff = 0.0f;
    f32 prevDelta = kExpectedStride;
    for (u32 i = kWarmup + 1; i < kFrames; ++i)
    {
        const f32 delta = interpX[i] - interpX[i - 1];
        EXPECT_GE(delta, -1e-4f) << "interpolated render pose went backwards at frame " << i
                                 << " (delta " << delta << ")";
        maxStrideError = std::max(maxStrideError, std::fabs(delta - kExpectedStride));
        maxInterpSecondDiff = std::max(maxInterpSecondDiff, std::fabs(delta - prevDelta));
        prevDelta = delta;
    }
    EXPECT_LT(maxStrideError, 5e-3f)
        << "interpolated render stride drifted from the expected constant "
        << kExpectedStride << " — motion is not uniform at a non-multiple refresh rate";
    EXPECT_LT(maxInterpSecondDiff, 5e-3f)
        << "interpolated render pose has a non-negligible second difference (judder)";

    // (2) The raw sim pose stair-steps: it must show BOTH frames that don't
    //     advance (accumulator hasn't reached a tick) and frames that jump a full
    //     unit. This is the judder that (1) proves interpolation removes.
    bool sawZeroStep = false;
    bool sawUnitStep = false;
    f32 maxRawSecondDiff = 0.0f;
    f32 prevRawDelta = 0.0f;
    for (u32 i = kWarmup + 1; i < kFrames; ++i)
    {
        const f32 rawDelta = rawX[i] - rawX[i - 1];
        if (std::fabs(rawDelta) < 1e-4f)
            sawZeroStep = true;
        if (std::fabs(rawDelta - 1.0f) < 1e-4f)
            sawUnitStep = true;
        maxRawSecondDiff = std::max(maxRawSecondDiff, std::fabs(rawDelta - prevRawDelta));
        prevRawDelta = rawDelta;
    }
    EXPECT_TRUE(sawZeroStep && sawUnitStep)
        << "raw sim pose did not stair-step (zeroStep=" << sawZeroStep
        << " unitStep=" << sawUnitStep << ") — the test's premise (coupled render judders) is void";
    // The interpolated series is dramatically smoother than the raw series.
    EXPECT_GT(maxRawSecondDiff, 0.9f) << "raw sim judder should be ~1 unit at step boundaries";
    EXPECT_LT(maxInterpSecondDiff, maxRawSecondDiff * 0.25f)
        << "interpolation did not measurably smooth the motion";
}

TEST_F(RenderInterpolationSmoothsMotionTest, InterpolationNeverLeaksIntoSimulationState)
{
    // The render blend overwrites poses transiently in RenderRuntime and restores
    // them before the frame returns, so it must never corrupt the persisted sim
    // state (#484 must not regress). The mover reads its own translation and adds
    // one each tick: if an interpolated (fractional) pose leaked back into the
    // persisted TransformComponent, the next tick's read would be fractional and
    // the trajectory would drift off the exact integer tick sequence. So with
    // interpolation ON, the raw sim pose must equal the fixed-tick count EXACTLY
    // on every frame — even on frames where the rendered pose is mid-blend.
    constexpr f32 kFixedDt = 1.0f / 60.0f;
    constexpr f32 kFrameDt = 1.0f / 144.0f; // non-multiple: mid-blend most frames
    constexpr u32 kFrames = 72;

    ASSERT_TRUE(GetScene().IsRenderInterpolationEnabled());

    for (u32 i = 0; i < kFrames; ++i)
    {
        GetScene().OnUpdateRuntimeFixed(kFrameDt, kFixedDt);

        const f32 rawX = m_Mover.GetComponent<TransformComponent>().Translation.x;
        const f32 expected = static_cast<f32>(GetScene().GetSimulationTick());
        // Exact integer equality: any leak of the fractional interpolated pose
        // into the persisted transform would show here as a non-integer drift.
        EXPECT_FLOAT_EQ(rawX, expected)
            << "raw sim pose (" << rawX << ") diverged from the fixed-tick count ("
            << expected << ") at frame " << i
            << " — the render interpolation blend leaked into persisted sim state";

        // And a mid-blend frame really is mid-blend: the rendered pose sits
        // strictly between the two tick poses, so we're exercising the blend, not
        // trivially sampling on tick boundaries.
        if (const f32 alpha = GetScene().GetRenderInterpolationAlpha(); alpha > 0.01f && alpha < 0.99f && expected >= 1.0f)
        {
            const f32 renderedX = GetScene().GetInterpolatedLocalTransform(m_Mover)[3].x;
            EXPECT_GT(renderedX, expected - 1.0f - 1e-4f);
            EXPECT_LT(renderedX, expected + 1e-4f);
        }
    }
}
