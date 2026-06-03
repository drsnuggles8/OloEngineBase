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
