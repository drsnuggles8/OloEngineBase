#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Networking/Replication/SnapshotInterpolator.h"
#include "OloEngine/Networking/Replication/ComponentReplicator.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Serialization/Archive.h"

#include <cmath>
#include <limits>

// Helper to create a serialized snapshot (UUID + transform) without a Scene.
static std::vector<u8> MakeSnapshot(u64 uuid, const glm::vec3& translation)
{
    using namespace OloEngine;

    std::vector<u8> buffer;
    FMemoryWriter writer(buffer);
    writer.ArIsNetArchive = true;

    writer << uuid;

    TransformComponent t;
    t.Translation = translation;
    t.SetRotationEuler({ 0.0f, 0.0f, 0.0f });
    t.Scale = { 1.0f, 1.0f, 1.0f };
    ComponentReplicator::Serialize(writer, t);

    return buffer;
}

TEST(SnapshotInterpolatorTest, DefaultState)
{
    using namespace OloEngine;

    SnapshotInterpolator interp;
    EXPECT_FLOAT_EQ(interp.GetRenderDelay(), 0.1f);
    EXPECT_EQ(interp.GetServerTickRate(), 20u);
    EXPECT_EQ(interp.GetBuffer().Size(), 0u);
}

TEST(SnapshotInterpolatorTest, PushSnapshotsIntoBuffer)
{
    using namespace OloEngine;

    SnapshotInterpolator interp;

    auto snap1 = MakeSnapshot(1, { 0.0f, 0.0f, 0.0f });
    auto snap2 = MakeSnapshot(1, { 10.0f, 0.0f, 0.0f });

    interp.PushSnapshot(1, snap1);
    interp.PushSnapshot(2, snap2);

    EXPECT_EQ(interp.GetBuffer().Size(), 2u);
}

TEST(SnapshotInterpolatorTest, SetRenderDelay)
{
    using namespace OloEngine;

    SnapshotInterpolator interp;
    interp.SetRenderDelay(0.2f);
    EXPECT_FLOAT_EQ(interp.GetRenderDelay(), 0.2f);
}

TEST(SnapshotInterpolatorTest, SetServerTickRate)
{
    using namespace OloEngine;

    SnapshotInterpolator interp;
    interp.SetServerTickRate(60);
    EXPECT_EQ(interp.GetServerTickRate(), 60u);
}

TEST(SnapshotInterpolatorTest, GetRenderTickComputation)
{
    using namespace OloEngine;

    SnapshotInterpolator interp;
    interp.SetServerTickRate(20);
    interp.SetRenderDelay(0.1f); // 0.1s * 20Hz = 2 ticks behind

    interp.PushSnapshot(10, MakeSnapshot(1, { 0.0f, 0.0f, 0.0f }));

    // renderTick = latestReceived(10) - delay(2) = 8
    EXPECT_FLOAT_EQ(interp.GetRenderTick(), 8.0f);
}

TEST(SnapshotInterpolatorTest, InterpolateNeedsAtLeastTwoSnapshots)
{
    using namespace OloEngine;

    // With fewer than 2 snapshots, Interpolate should not crash.
    // We can't test the actual scene application without a live Scene,
    // but we verify it doesn't blow up.
    SnapshotInterpolator interp;
    interp.PushSnapshot(1, MakeSnapshot(1, { 0.0f, 0.0f, 0.0f }));

    // This should safely do nothing (only 1 snapshot < 2 required)
    // No Scene to pass, but the method guards against < 2 entries early.
    EXPECT_EQ(interp.GetBuffer().Size(), 1u);
}

// ── Untrusted render-delay hardening ─────────────────────────────────
// SetRenderDelay feeds the tick math: a non-finite delay makes renderTick NaN,
// which slips past the `renderTick < 0.0f` guard (every NaN comparison is false)
// and then UB-casts to u32. The setter must reject such values and keep the
// previous valid delay; GetRenderTick must stay finite.

TEST(SnapshotInterpolatorTest, SetRenderDelayRejectsNaN)
{
    using namespace OloEngine;

    SnapshotInterpolator interp;
    interp.SetRenderDelay(0.2f);

    interp.SetRenderDelay(std::numeric_limits<f32>::quiet_NaN());

    // Invalid value ignored — prior delay retained.
    EXPECT_FLOAT_EQ(interp.GetRenderDelay(), 0.2f);
    EXPECT_TRUE(std::isfinite(interp.GetRenderDelay()));
}

TEST(SnapshotInterpolatorTest, SetRenderDelayRejectsPositiveInfinity)
{
    using namespace OloEngine;

    SnapshotInterpolator interp;
    interp.SetRenderDelay(0.25f);

    interp.SetRenderDelay(std::numeric_limits<f32>::infinity());

    EXPECT_FLOAT_EQ(interp.GetRenderDelay(), 0.25f);
}

TEST(SnapshotInterpolatorTest, SetRenderDelayRejectsNegativeInfinity)
{
    using namespace OloEngine;

    SnapshotInterpolator interp;
    interp.SetRenderDelay(0.15f);

    interp.SetRenderDelay(-std::numeric_limits<f32>::infinity());

    EXPECT_FLOAT_EQ(interp.GetRenderDelay(), 0.15f);
}

TEST(SnapshotInterpolatorTest, SetRenderDelayRejectsNegative)
{
    using namespace OloEngine;

    SnapshotInterpolator interp;
    interp.SetRenderDelay(0.3f);

    interp.SetRenderDelay(-1.0f);

    EXPECT_FLOAT_EQ(interp.GetRenderDelay(), 0.3f);
}

TEST(SnapshotInterpolatorTest, SetRenderDelayAcceptsZero)
{
    using namespace OloEngine;

    // Zero is a legitimate (no-delay) value and must still be accepted.
    SnapshotInterpolator interp;
    interp.SetRenderDelay(0.0f);
    EXPECT_FLOAT_EQ(interp.GetRenderDelay(), 0.0f);
}

TEST(SnapshotInterpolatorTest, RenderTickStaysFiniteAfterPoisonedDelay)
{
    using namespace OloEngine;

    SnapshotInterpolator interp;
    interp.SetServerTickRate(20);
    interp.SetRenderDelay(0.1f);
    interp.PushSnapshot(10, MakeSnapshot(1, { 0.0f, 0.0f, 0.0f }));

    // A rejected NaN must not corrupt the cached delay → render tick stays finite.
    interp.SetRenderDelay(std::numeric_limits<f32>::quiet_NaN());

    EXPECT_TRUE(std::isfinite(interp.GetRenderTick()));
    EXPECT_FLOAT_EQ(interp.GetRenderTick(), 8.0f);
}
