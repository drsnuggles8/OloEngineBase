#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Cinematic/CinematicSequence.h"
#include "OloEngine/Cinematic/CinematicSequenceSerializer.h"
#include "OloEngine/Core/Ref.h"

#include <glm/gtc/quaternion.hpp>

// =============================================================================
// CinematicSerializerTest — L3-style data round-trip for the `.olocine` YAML
// format. Builds a sequence touching every track type and channel, serializes
// to a string, deserializes, and asserts the structure + values survive. Also
// covers the malformed-input guard (no crash, returns null).
// =============================================================================

using namespace OloEngine;

namespace
{
    constexpr f32 kEps = 1e-4f;

    Ref<CinematicSequence> MakeRichSequence()
    {
        auto seq = Ref<CinematicSequence>::Create();
        seq->Name = "Intro";
        seq->Duration = 8.0f;

        CinematicTransformTrack tt;
        tt.Target = UUID(111);
        tt.Name = "Hero";
        tt.Translation.Keys.push_back({ 0.0f, glm::vec3(0, 0, 0), CinematicInterp::Linear });
        tt.Translation.Keys.push_back({ 4.0f, glm::vec3(1, 2, 3), CinematicInterp::EaseInOut });
        tt.Rotation.Keys.push_back({ 0.0f, glm::angleAxis(glm::radians(90.0f), glm::vec3(0, 1, 0)), CinematicInterp::Linear });
        tt.Scale.Keys.push_back({ 0.0f, glm::vec3(1.0f), CinematicInterp::Constant });
        seq->TransformTracks.push_back(std::move(tt));

        CinematicCameraTrack ct;
        ct.Target = UUID(222);
        ct.Name = "MainCam";
        ct.Position.Keys.push_back({ 1.0f, glm::vec3(5, 5, 5), CinematicInterp::Linear });
        ct.VerticalFovRadians.Keys.push_back({ 0.0f, glm::radians(45.0f), CinematicInterp::Linear });
        ct.VerticalFovRadians.Keys.push_back({ 8.0f, glm::radians(60.0f), CinematicInterp::Linear });
        seq->CameraTracks.push_back(std::move(ct));

        CinematicVisibilityTrack vt;
        vt.Target = UUID(333);
        vt.Name = "Prop";
        vt.Keys.push_back({ 0.0f, false });
        vt.Keys.push_back({ 2.0f, true });
        seq->VisibilityTracks.push_back(std::move(vt));

        CinematicEventTrack et;
        et.Name = "Cues";
        et.Keys.push_back({ 1.5f, "spawn" });
        et.Keys.push_back({ 6.0f, "explode" });
        seq->EventTracks.push_back(std::move(et));

        return seq;
    }
} // namespace

TEST(CinematicSerializerTest, RoundTripPreservesStructureAndValues)
{
    auto original = MakeRichSequence();

    const std::string yaml = CinematicSequenceSerializer::SerializeToString(original);
    ASSERT_FALSE(yaml.empty());

    auto restored = CinematicSequenceSerializer::DeserializeFromString(yaml);
    ASSERT_TRUE(restored);

    EXPECT_EQ(restored->Name, "Intro");
    EXPECT_NEAR(restored->Duration, 8.0f, kEps);

    // ---- Transform track ----
    ASSERT_EQ(restored->TransformTracks.size(), 1u);
    const auto& tt = restored->TransformTracks[0];
    EXPECT_EQ(static_cast<u64>(tt.Target), 111u);
    EXPECT_EQ(tt.Name, "Hero");
    ASSERT_EQ(tt.Translation.Keys.size(), 2u);
    EXPECT_NEAR(tt.Translation.Keys[1].Time, 4.0f, kEps);
    EXPECT_NEAR(tt.Translation.Keys[1].Value.x, 1.0f, kEps);
    EXPECT_NEAR(tt.Translation.Keys[1].Value.y, 2.0f, kEps);
    EXPECT_NEAR(tt.Translation.Keys[1].Value.z, 3.0f, kEps);
    EXPECT_EQ(tt.Translation.Keys[1].Interp, CinematicInterp::EaseInOut);
    ASSERT_EQ(tt.Rotation.Keys.size(), 1u);
    const glm::quat expectedRot = glm::angleAxis(glm::radians(90.0f), glm::vec3(0, 1, 0));
    EXPECT_NEAR(std::abs(glm::dot(tt.Rotation.Keys[0].Value, expectedRot)), 1.0f, kEps);
    ASSERT_EQ(tt.Scale.Keys.size(), 1u);
    EXPECT_EQ(tt.Scale.Keys[0].Interp, CinematicInterp::Constant);

    // ---- Camera track ----
    ASSERT_EQ(restored->CameraTracks.size(), 1u);
    const auto& ct = restored->CameraTracks[0];
    EXPECT_EQ(static_cast<u64>(ct.Target), 222u);
    EXPECT_EQ(ct.Name, "MainCam");
    ASSERT_EQ(ct.Position.Keys.size(), 1u);
    EXPECT_NEAR(ct.Position.Keys[0].Value.y, 5.0f, kEps);
    ASSERT_EQ(ct.VerticalFovRadians.Keys.size(), 2u);
    EXPECT_NEAR(ct.VerticalFovRadians.Keys[1].Value, glm::radians(60.0f), kEps);

    // ---- Visibility track ----
    ASSERT_EQ(restored->VisibilityTracks.size(), 1u);
    const auto& vt = restored->VisibilityTracks[0];
    EXPECT_EQ(static_cast<u64>(vt.Target), 333u);
    ASSERT_EQ(vt.Keys.size(), 2u);
    EXPECT_FALSE(vt.Keys[0].Visible);
    EXPECT_TRUE(vt.Keys[1].Visible);
    EXPECT_NEAR(vt.Keys[1].Time, 2.0f, kEps);

    // ---- Event track ----
    ASSERT_EQ(restored->EventTracks.size(), 1u);
    const auto& et = restored->EventTracks[0];
    EXPECT_EQ(et.Name, "Cues");
    ASSERT_EQ(et.Keys.size(), 2u);
    EXPECT_EQ(et.Keys[0].Name, "spawn");
    EXPECT_NEAR(et.Keys[0].Time, 1.5f, kEps);
    EXPECT_EQ(et.Keys[1].Name, "explode");
}

TEST(CinematicSerializerTest, EmptySequenceRoundTrips)
{
    auto seq = Ref<CinematicSequence>::Create();
    seq->Name = "Empty";

    const std::string yaml = CinematicSequenceSerializer::SerializeToString(seq);
    ASSERT_FALSE(yaml.empty());
    auto restored = CinematicSequenceSerializer::DeserializeFromString(yaml);
    ASSERT_TRUE(restored);
    EXPECT_TRUE(restored->IsEmpty());
    EXPECT_EQ(restored->Name, "Empty");
}

TEST(CinematicSerializerTest, MalformedYamlReturnsNullWithoutCrashing)
{
    // Not a cinematic document.
    auto a = CinematicSequenceSerializer::DeserializeFromString("just: some: invalid: yaml: [[[");
    EXPECT_FALSE(a);

    // Valid YAML but missing the root node.
    auto b = CinematicSequenceSerializer::DeserializeFromString("SomethingElse:\n  x: 1\n");
    EXPECT_FALSE(b);
}

TEST(CinematicSerializerTest, NullSequenceSerializesToEmptyString)
{
    Ref<CinematicSequence> nullSeq;
    EXPECT_TRUE(CinematicSequenceSerializer::SerializeToString(nullSeq).empty());
}

TEST(CinematicSerializerTest, BezierTangentsRoundTrip)
{
    // A Bezier key's interp + in/out tangents must survive the YAML round-trip on
    // every keyed channel type (vec3 / float / quat).
    auto seq = Ref<CinematicSequence>::Create();
    seq->Name = "Bez";
    seq->Duration = 4.0f;

    CinematicTransformTrack tt;
    tt.Target = UUID(7);
    tt.Translation.Keys.push_back({ 0.0f, glm::vec3(0.0f), CinematicInterp::Bezier, /*In*/ -0.5f, /*Out*/ 2.5f });
    tt.Translation.Keys.push_back({ 2.0f, glm::vec3(1.0f), CinematicInterp::Linear, /*In*/ 1.25f, /*Out*/ 0.0f });
    tt.Rotation.Keys.push_back({ 0.0f, glm::angleAxis(glm::radians(45.0f), glm::vec3(0, 1, 0)), CinematicInterp::Bezier, 0.3f, -0.8f });
    seq->TransformTracks.push_back(std::move(tt));

    CinematicCameraTrack ct;
    ct.Target = UUID(8);
    ct.VerticalFovRadians.Keys.push_back({ 0.0f, glm::radians(40.0f), CinematicInterp::Bezier, 0.0f, 3.0f });
    seq->CameraTracks.push_back(std::move(ct));

    const std::string yaml = CinematicSequenceSerializer::SerializeToString(seq);
    ASSERT_FALSE(yaml.empty());
    auto restored = CinematicSequenceSerializer::DeserializeFromString(yaml);
    ASSERT_TRUE(restored);

    ASSERT_EQ(restored->TransformTracks.size(), 1u);
    const auto& tr = restored->TransformTracks[0];
    ASSERT_EQ(tr.Translation.Keys.size(), 2u);
    EXPECT_EQ(tr.Translation.Keys[0].Interp, CinematicInterp::Bezier);
    EXPECT_NEAR(tr.Translation.Keys[0].InTangent, -0.5f, kEps);
    EXPECT_NEAR(tr.Translation.Keys[0].OutTangent, 2.5f, kEps);
    // The non-Bezier right key's in-tangent must also round-trip (a Bezier segment
    // reads it), so emission can't be gated on the key's own mode.
    EXPECT_NEAR(tr.Translation.Keys[1].InTangent, 1.25f, kEps);
    ASSERT_EQ(tr.Rotation.Keys.size(), 1u);
    EXPECT_EQ(tr.Rotation.Keys[0].Interp, CinematicInterp::Bezier);
    EXPECT_NEAR(tr.Rotation.Keys[0].InTangent, 0.3f, kEps);
    EXPECT_NEAR(tr.Rotation.Keys[0].OutTangent, -0.8f, kEps);

    ASSERT_EQ(restored->CameraTracks.size(), 1u);
    ASSERT_EQ(restored->CameraTracks[0].VerticalFovRadians.Keys.size(), 1u);
    const auto& fk = restored->CameraTracks[0].VerticalFovRadians.Keys[0];
    EXPECT_EQ(fk.Interp, CinematicInterp::Bezier);
    EXPECT_NEAR(fk.OutTangent, 3.0f, kEps);
}

TEST(CinematicSerializerTest, LegacyFileWithoutTangentsLoadsAsFlatDefaults)
{
    // A pre-v2 file has no Version and no InTangent/OutTangent fields. It must
    // load with tangents defaulting to 0 (flat), and a Bezier (Interp:3) key with
    // no tangents must therefore behave like the old EaseInOut.
    const std::string yaml = R"(CinematicSequence:
  Name: Legacy
  Duration: 2
  TransformTracks:
    - Target: 5
      Translation:
        - { Time: 0.0, Value: [0, 0, 0], Interp: 3 }
        - { Time: 1.0, Value: [10, 0, 0], Interp: 2 }
)";

    auto seq = CinematicSequenceSerializer::DeserializeFromString(yaml);
    ASSERT_TRUE(seq);
    ASSERT_EQ(seq->TransformTracks.size(), 1u);
    const auto& tt = seq->TransformTracks[0];
    ASSERT_EQ(tt.Translation.Keys.size(), 2u);
    // Both interp values round-trip; the Bezier key governs the evaluated segment.
    EXPECT_EQ(tt.Translation.Keys[0].Interp, CinematicInterp::Bezier);
    EXPECT_EQ(tt.Translation.Keys[1].Interp, CinematicInterp::EaseInOut);
    for (const auto& k : tt.Translation.Keys)
    {
        EXPECT_NEAR(k.InTangent, 0.0f, kEps);
        EXPECT_NEAR(k.OutTangent, 0.0f, kEps);
    }
    // The segment [0->1] is governed by the left (Bezier) key with flat tangents,
    // so it must evaluate as smoothstep: midpoint == half.
    EXPECT_NEAR(tt.Translation.Evaluate(0.5f).x, 5.0f, 1e-3f);
}

TEST(CinematicSerializerTest, OutOfRangeInterpClampsButBezierIsAccepted)
{
    const std::string yaml = R"(CinematicSequence:
  Name: Clamp
  Duration: 2
  TransformTracks:
    - Target: 1
      Translation:
        - { Time: 0.0, Value: [0, 0, 0], Interp: 3, InTangent: 1.5, OutTangent: 2.0 }
        - { Time: 1.0, Value: [1, 0, 0], Interp: 9, InTangent: .nan, OutTangent: 0.5 }
)";

    auto seq = CinematicSequenceSerializer::DeserializeFromString(yaml);
    ASSERT_TRUE(seq);
    ASSERT_EQ(seq->TransformTracks.size(), 1u); // guard the [0] access below
    const auto& keys = seq->TransformTracks[0].Translation.Keys;
    ASSERT_EQ(keys.size(), 2u);
    EXPECT_EQ(keys[0].Interp, CinematicInterp::Bezier); // 3 is in range now
    EXPECT_NEAR(keys[0].InTangent, 1.5f, kEps);
    EXPECT_EQ(keys[1].Interp, CinematicInterp::Linear); // 9 -> clamped to Linear
    EXPECT_NEAR(keys[1].InTangent, 0.0f, kEps);         // NaN tangent rejected -> 0
    EXPECT_NEAR(keys[1].OutTangent, 0.5f, kEps);        // finite tangent kept
}

TEST(CinematicSerializerTest, MalformedKeysAreSkippedNotMaterialized)
{
    // A Translation channel with one well-formed key plus two malformed ones
    // (no Time, no Value). The bad keys must be skipped, not materialised as
    // synthetic t==0 / value==0 keys.
    const std::string yaml = R"(CinematicSequence:
  Name: Malformed
  Duration: 2
  TransformTracks:
    - Target: 5
      Translation:
        - { Value: [9, 9, 9], Interp: 1 }
        - { Time: 1.0, Value: [1, 2, 3], Interp: 1 }
        - { Time: 1.5, Interp: 1 }
)";

    auto seq = CinematicSequenceSerializer::DeserializeFromString(yaml);
    ASSERT_TRUE(seq);
    ASSERT_EQ(seq->TransformTracks.size(), 1u);
    const auto& tt = seq->TransformTracks[0];
    ASSERT_EQ(tt.Translation.Keys.size(), 1u); // only the well-formed key survives
    EXPECT_NEAR(tt.Translation.Keys[0].Time, 1.0f, 1e-4f);
    EXPECT_NEAR(tt.Translation.Keys[0].Value.x, 1.0f, 1e-4f);
}
