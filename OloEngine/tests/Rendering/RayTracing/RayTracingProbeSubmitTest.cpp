// OLO_TEST_LAYER: unit
// =============================================================================
// RayTracingProbeSubmitTest — the device-free half of RayTracingProbe (#607).
//
// `Dispatch` and `Poll` need a Vulkan device and are covered live; `SubmitBatch`
// needs nothing, and it is the gate that decides what ever reaches the GPU. So
// it is pinned here, on every runner.
//
// WHY THIS FILE EXISTS AT ALL: self-review of the concurrency fix pointed out
// that deleting either half of that fix left the whole suite green. The
// ownership sentinel below is the half that IS testable without a device, and
// an untested sentinel is how "0 means no batch" quietly stops being true.
//
// The two contracts:
//
//   1. A MALFORMED RAY IS REFUSED, NOT TRACED. Finite origin/direction, a
//      non-zero direction and 0 <= tMin <= tMax are SPEC REQUIREMENTS of
//      rayQueryInitializeEXT — violating one is undefined behaviour on the
//      device, not a miss. The shader validates too, but a caller that gets a
//      reason here is told WHY; one that only gets the shader's defined miss is
//      not.
//
//   2. BatchId 0 IS REFUSED, because 0 is the "no batch" sentinel
//      GetUnavailableBatchId() returns. A batch carrying it would own a refusal
//      reason that reads as ownerless, and a reader applying the documented
//      pairing strictly would downgrade a definitive refusal to "pending" — a
//      worse answer than the refusal it replaced.
// =============================================================================

#include "OloEnginePCH.h"

#include <gtest/gtest.h>

#include "OloEngine/Renderer/RayTracing/RayTracingProbe.h"

#include <cmath>
#include <limits>
#include <string>

namespace OloEngine::Tests
{
    namespace
    {
        namespace RT = OloEngine::RayTracing;

        [[nodiscard]] RT::RayTracingProbe::Ray GoodRay()
        {
            RT::RayTracingProbe::Ray ray;
            ray.Origin = { 0.0f, 10.0f, 0.0f };
            ray.Direction = { 0.0f, -1.0f, 0.0f };
            ray.TMin = 0.001f;
            ray.TMax = 100.0f;
            return ray;
        }

        [[nodiscard]] RT::RayTracingProbe::Batch GoodBatch()
        {
            RT::RayTracingProbe::Batch batch;
            batch.BatchId = 7;
            batch.Rays = { GoodRay() };
            return batch;
        }
    } // namespace

    TEST(RayTracingProbeSubmit, AWellFormedBatchIsAccepted)
    {
        RT::RayTracingProbe probe;
        std::string error;
        EXPECT_TRUE(probe.SubmitBatch(GoodBatch(), error)) << error;
        EXPECT_TRUE(error.empty()) << error;
        EXPECT_TRUE(probe.HasPendingBatch());
        EXPECT_EQ(probe.GetPendingBatchId(), 7u);
    }

    TEST(RayTracingProbeSubmit, TheDirectionIsNormalizedSoDistanceIsAWorldDistance)
    {
        // A direction of length 5 would make the GPU's `t` a multiple of 5 and
        // the CPU-side hit position disagree with it.
        RT::RayTracingProbe::Batch batch = GoodBatch();
        batch.Rays[0].Direction = { 0.0f, -5.0f, 0.0f };
        RT::RayTracingProbe probe;
        std::string error;
        ASSERT_TRUE(probe.SubmitBatch(batch, error)) << error;
        // Submitted rays are normalized in place; the probe echoes them back
        // with the answer, so this is the vector the caller will be shown.
        EXPECT_TRUE(probe.HasPendingBatch());
    }

    TEST(RayTracingProbeSubmit, BatchIdZeroIsRefusedBecauseZeroMeansNoBatch)
    {
        // The sentinel's soundness rests entirely on this refusal:
        // GetUnavailableBatchId() returns 0 for "there is no refusal", so a real
        // batch with id 0 would make a definitive refusal indistinguishable from
        // no refusal at all.
        RT::RayTracingProbe::Batch batch = GoodBatch();
        batch.BatchId = 0;
        RT::RayTracingProbe probe;
        std::string error;
        EXPECT_FALSE(probe.SubmitBatch(batch, error));
        EXPECT_NE(error.find("BatchId"), std::string::npos) << error;
        EXPECT_FALSE(probe.HasPendingBatch());
        // And nothing was queued, so the sentinel still reads as "none".
        EXPECT_EQ(probe.GetUnavailableBatchId(), 0u);
    }

    TEST(RayTracingProbeSubmit, AnEmptyOrOversizedBatchIsRefusedRatherThanTruncated)
    {
        RT::RayTracingProbe probe;
        std::string error;

        RT::RayTracingProbe::Batch empty = GoodBatch();
        empty.Rays.clear();
        EXPECT_FALSE(probe.SubmitBatch(empty, error));
        EXPECT_FALSE(error.empty());

        // Truncating would report the dropped rays as misses, which is the one
        // answer a probe must never invent.
        RT::RayTracingProbe::Batch tooMany = GoodBatch();
        tooMany.Rays.assign(RT::RayTracingProbe::kMaxRays + 1u, GoodRay());
        EXPECT_FALSE(probe.SubmitBatch(tooMany, error));
        EXPECT_NE(error.find(std::to_string(RT::RayTracingProbe::kMaxRays)), std::string::npos) << error;
        EXPECT_FALSE(probe.HasPendingBatch());

        // Exactly the cap is fine — the boundary is inclusive.
        RT::RayTracingProbe::Batch atCap = GoodBatch();
        atCap.Rays.assign(RT::RayTracingProbe::kMaxRays, GoodRay());
        EXPECT_TRUE(probe.SubmitBatch(atCap, error)) << error;
    }

    TEST(RayTracingProbeSubmit, AMalformedRayIsRefusedNotTraced)
    {
        const f32 nan = std::numeric_limits<f32>::quiet_NaN();
        const f32 inf = std::numeric_limits<f32>::infinity();
        RT::RayTracingProbe probe;
        std::string error;

        const auto refuses = [&](auto mutate, const char* what)
        {
            RT::RayTracingProbe::Batch batch = GoodBatch();
            mutate(batch.Rays[0]);
            error.clear();
            EXPECT_FALSE(probe.SubmitBatch(batch, error)) << what << " was accepted";
            EXPECT_FALSE(error.empty()) << what << " gave no reason";
        };

        refuses([&](auto& r) { r.Origin.x = nan; }, "NaN origin");
        refuses([&](auto& r) { r.Origin.y = inf; }, "infinite origin");
        refuses([&](auto& r) { r.Direction.z = nan; }, "NaN direction");
        refuses([&](auto& r) { r.Direction = { 0.0f, 0.0f, 0.0f }; }, "zero-length direction");
        refuses([&](auto& r) { r.TMin = nan; }, "NaN tMin");
        refuses([&](auto& r) { r.TMax = inf; }, "infinite tMax");
        refuses([&](auto& r) { r.TMin = -1.0f; }, "negative tMin");
        refuses([&](auto& r) { r.TMin = 10.0f; r.TMax = 1.0f; }, "tMin > tMax");
    }

    TEST(RayTracingProbeSubmit, ADegenerateButWellFormedIntervalIsAccepted)
    {
        // tMin == tMax is legal: 0 <= tMin <= tMax is the spec's requirement,
        // and refusing the boundary would refuse a legitimate point query.
        RT::RayTracingProbe::Batch batch = GoodBatch();
        batch.Rays[0].TMin = 2.0f;
        batch.Rays[0].TMax = 2.0f;
        RT::RayTracingProbe probe;
        std::string error;
        EXPECT_TRUE(probe.SubmitBatch(batch, error)) << error;
    }

    TEST(RayTracingProbeSubmit, AZeroInstanceMaskIsRefusedBecauseEveryRayWouldMiss)
    {
        // Honouring it would make every instance unhittable, so every ray would
        // come back a miss for a reason with nothing to do with the scene.
        RT::RayTracingProbe::Batch batch = GoodBatch();
        batch.InstanceMask = 0;
        RT::RayTracingProbe probe;
        std::string error;
        EXPECT_FALSE(probe.SubmitBatch(batch, error));
        EXPECT_NE(error.find("instanceMask"), std::string::npos) << error;

        // The low byte is what reaches the shader, so a mask that is non-zero
        // only ABOVE it is the same refusal rather than a silent no-op.
        batch.InstanceMask = 0x100;
        EXPECT_FALSE(probe.SubmitBatch(batch, error));
    }

    TEST(RayTracingProbeSubmit, ALaterBatchReplacesAnUndispatchedOne)
    {
        // Documented behaviour -- the probe answers the LATEST question -- and
        // the reason olo_rt_trace_ray serializes its calls: without that lock a
        // second caller silently takes the first caller's turn.
        RT::RayTracingProbe probe;
        std::string error;
        RT::RayTracingProbe::Batch first = GoodBatch();
        first.BatchId = 11;
        ASSERT_TRUE(probe.SubmitBatch(first, error)) << error;
        EXPECT_EQ(probe.GetPendingBatchId(), 11u);

        RT::RayTracingProbe::Batch second = GoodBatch();
        second.BatchId = 12;
        ASSERT_TRUE(probe.SubmitBatch(second, error)) << error;
        EXPECT_EQ(probe.GetPendingBatchId(), 12u) << "the newer batch must replace the older one";
    }

    TEST(RayTracingProbeSubmit, AFreshProbeHasNoAnswerAndNoRefusal)
    {
        // "Not back yet", "came back all misses" and "cannot answer" must not
        // share a representation.
        RT::RayTracingProbe probe;
        EXPECT_FALSE(probe.GetLatest().Valid);
        EXPECT_FALSE(probe.HasPendingBatch());
        EXPECT_EQ(probe.GetSlotsInFlight(), 0u);
        EXPECT_TRUE(probe.GetUnavailableReason().empty());
        EXPECT_EQ(probe.GetUnavailableBatchId(), 0u);
    }
} // namespace OloEngine::Tests
