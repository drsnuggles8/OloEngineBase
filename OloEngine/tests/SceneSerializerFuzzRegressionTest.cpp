// =============================================================================
// SceneSerializerFuzzRegressionTest.cpp
//
// Regression tests for crashes found by the libFuzzer harness
// `FuzzSceneYaml` against `SceneSerializer::DeserializeFromYAML`. The
// rule under test: a malformed YAML payload must always return `false`
// from the deserializer and never escape an uncaught exception or hit
// a null dereference deep in helper functions.
//
// Tracks GH issue #240 (Bug 2: NULL pointer dereference @ 0x8 in
// SceneSerializer). The crashing input identified in CI was the 6 bytes
// `S{}\ncn` (Base64 `U3t9CmNu`); other inputs in this file cover the
// same bug class — wrong-typed root, wrong-typed Scene, malformed
// Entities sequence, etc.
//
// These tests deliberately do NOT mount the editor asset root: the bug
// is in the YAML schema-walk before any asset path is touched, and we
// want the test to run fast in any environment.
// =============================================================================

#include "OloEnginePCH.h"

#include <gtest/gtest.h>

#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/SceneSerializer.h"

#include <string>
#include <string_view>

namespace OloEngine::Tests
{
    namespace
    {
        // Drive a single deserialise the way the fuzz harness does: empty
        // scene, no asset root, raw bytes in. Returns the deserializer's
        // boolean result — the test asserts it didn't crash by virtue of
        // returning at all.
        bool DeserializeBytes(std::string_view bytes)
        {
            auto scene = OloEngine::Scene::Create();
            if (!scene)
                return false;
            OloEngine::SceneSerializer serializer(scene);
            return serializer.DeserializeFromYAML(std::string(bytes));
        }
    } // namespace

    // -------------------------------------------------------------------------
    // The original CI repro
    // -------------------------------------------------------------------------

    TEST(SceneSerializerFuzzRegression, CrashInput_OriginalSixBytes)
    {
        // GH #240, Bug 2: ASan reported `access-violation on 0x000000000008`
        // for these 6 bytes (a null member-access at offset 8). Before the
        // fix, this either crashed inside yaml-cpp/helpers or propagated an
        // uncaught exception out of DeserializeFromYAML. Now it must return
        // false cleanly.
        constexpr std::string_view kCrasher{ "S{}\ncn", 6 };
        EXPECT_FALSE(DeserializeBytes(kCrasher));
    }

    // -------------------------------------------------------------------------
    // Wrong-typed roots
    // -------------------------------------------------------------------------

    TEST(SceneSerializerFuzzRegression, EmptyInput)
    {
        EXPECT_FALSE(DeserializeBytes(""));
    }

    TEST(SceneSerializerFuzzRegression, RootIsScalar)
    {
        EXPECT_FALSE(DeserializeBytes("hello"));
    }

    TEST(SceneSerializerFuzzRegression, RootIsSequence)
    {
        EXPECT_FALSE(DeserializeBytes("[1, 2, 3]\n"));
    }

    TEST(SceneSerializerFuzzRegression, RootIsEmptyMap)
    {
        EXPECT_FALSE(DeserializeBytes("{}\n"));
    }

    TEST(SceneSerializerFuzzRegression, SceneKeyMissing)
    {
        EXPECT_FALSE(DeserializeBytes("Foo: bar\n"));
    }

    TEST(SceneSerializerFuzzRegression, SceneIsMap)
    {
        // `Scene` must be scalar — when it's a map, `.as<string>()` throws.
        EXPECT_FALSE(DeserializeBytes("Scene: {nested: thing}\n"));
    }

    TEST(SceneSerializerFuzzRegression, SceneIsSequence)
    {
        EXPECT_FALSE(DeserializeBytes("Scene: [a, b]\n"));
    }

    TEST(SceneSerializerFuzzRegression, SceneIsNull)
    {
        EXPECT_FALSE(DeserializeBytes("Scene: ~\n"));
    }

    // -------------------------------------------------------------------------
    // Wrong-typed settings blocks
    // -------------------------------------------------------------------------

    TEST(SceneSerializerFuzzRegression, PostProcessSettingsIsScalar)
    {
        // Helper used to assume `PostProcessSettings` was a map — a scalar
        // here used to cascade into bad operator[] calls.
        constexpr std::string_view kYaml =
            "Scene: Untitled\n"
            "PostProcessSettings: just_a_string\n";
        // Should succeed with default post-process settings, not crash.
        EXPECT_TRUE(DeserializeBytes(kYaml));
    }

    TEST(SceneSerializerFuzzRegression, StreamingSettingsIsSequence)
    {
        constexpr std::string_view kYaml =
            "Scene: Untitled\n"
            "StreamingSettings: [1, 2]\n";
        EXPECT_TRUE(DeserializeBytes(kYaml));
    }

    // -------------------------------------------------------------------------
    // Malformed entities
    // -------------------------------------------------------------------------

    TEST(SceneSerializerFuzzRegression, EntitiesIsScalar)
    {
        constexpr std::string_view kYaml =
            "Scene: Untitled\n"
            "Entities: not_a_sequence\n";
        EXPECT_TRUE(DeserializeBytes(kYaml));
    }

    TEST(SceneSerializerFuzzRegression, EntitiesContainsScalar)
    {
        constexpr std::string_view kYaml =
            "Scene: Untitled\n"
            "Entities:\n"
            "  - just_a_scalar\n";
        // Entity entry not a map → skip; whole load should succeed.
        EXPECT_TRUE(DeserializeBytes(kYaml));
    }

    TEST(SceneSerializerFuzzRegression, EntityIdIsMap)
    {
        constexpr std::string_view kYaml =
            "Scene: Untitled\n"
            "Entities:\n"
            "  - Entity: {x: 1}\n"
            "    TagComponent:\n"
            "      Tag: Foo\n";
        // Entity id not a scalar → skip the entity. Load succeeds.
        EXPECT_TRUE(DeserializeBytes(kYaml));
    }

    TEST(SceneSerializerFuzzRegression, TagComponentIsScalar)
    {
        constexpr std::string_view kYaml =
            "Scene: Untitled\n"
            "Entities:\n"
            "  - Entity: 1234\n"
            "    TagComponent: just_a_string\n";
        // TagComponent not a map → skip the name, entity itself still loads.
        EXPECT_TRUE(DeserializeBytes(kYaml));
    }

    // -------------------------------------------------------------------------
    // Raw garbage / pathological inputs
    // -------------------------------------------------------------------------

    TEST(SceneSerializerFuzzRegression, BinaryGarbage)
    {
        EXPECT_FALSE(DeserializeBytes(std::string_view("\x00\x01\x02\xff\xfe\xfd", 6)));
    }

    TEST(SceneSerializerFuzzRegression, UnterminatedFlowMap)
    {
        EXPECT_FALSE(DeserializeBytes("Scene: {unterminated\n"));
    }

    TEST(SceneSerializerFuzzRegression, ParserThrowsOnAnchorOnlyDoc)
    {
        // `&anchor` with nothing after it is an error in yaml-cpp.
        EXPECT_FALSE(DeserializeBytes("&anchor\n"));
    }

    TEST(SceneSerializerFuzzRegression, SinglePrintableChar)
    {
        // One-byte inputs are a known fuzz pattern.
        EXPECT_FALSE(DeserializeBytes("a"));
        EXPECT_FALSE(DeserializeBytes("{"));
        EXPECT_FALSE(DeserializeBytes("["));
        EXPECT_FALSE(DeserializeBytes("'"));
        EXPECT_FALSE(DeserializeBytes("\""));
    }

} // namespace OloEngine::Tests
