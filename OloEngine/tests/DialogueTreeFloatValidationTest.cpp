// OLO_TEST_LAYER: unit
//
// Guards DialogueTreeSerializer's YAML float reads against non-finite values in a
// hand-edited or corrupt .olodiag file. A ".nan"/".inf" EditorPosition breaks graph-UI
// layout, and a "nan"/"inf" float property (parsed via std::stof, which accepts both)
// would inject a non-finite value into dialogue condition/branch logic. See
// cpp-coding-quality.md §2b.
#include "OloEnginePCH.h"
#include <gtest/gtest.h>
#include "OloEngine/Scene/Scene.h" // Required: AssetSerializer.h uses Ref<Scene> inline
#include "OloEngine/Dialogue/DialogueTreeAsset.h"
#include "OloEngine/Dialogue/DialogueTreeSerializer.h"

#include <cmath>
#include <string>
#include <variant>

using namespace OloEngine;

namespace
{
    class DialogueTreeFloatValidationTest : public ::testing::Test
    {
      protected:
        DialogueTreeSerializer serializer;
    };

    TEST_F(DialogueTreeFloatValidationTest, NonFiniteEditorPositionIsClampedToOrigin)
    {
        const std::string yaml =
            "DialogueTree:\n"
            "  RootNodeID: 1001\n"
            "  Nodes:\n"
            "    - ID: 1001\n"
            "      Type: dialogue\n"
            "      Name: Root\n"
            "      EditorPosition: [.nan, .inf]\n"
            "  Connections: []\n";

        auto asset = Ref<DialogueTreeAsset>::Create();
        ASSERT_TRUE(serializer.TestDeserializeFromYAML(yaml, asset));

        const auto* node = asset->FindNode(UUID(1001));
        ASSERT_NE(node, nullptr);
        EXPECT_TRUE(std::isfinite(node->EditorPosition.x));
        EXPECT_TRUE(std::isfinite(node->EditorPosition.y));
        EXPECT_FLOAT_EQ(node->EditorPosition.x, 0.0f);
        EXPECT_FLOAT_EQ(node->EditorPosition.y, 0.0f);
    }

    TEST_F(DialogueTreeFloatValidationTest, NonFiniteFloatPropertyFallsThroughToString)
    {
        const std::string yaml =
            "DialogueTree:\n"
            "  RootNodeID: 1001\n"
            "  Nodes:\n"
            "    - ID: 1001\n"
            "      Type: condition\n"
            "      Name: Gate\n"
            "      EditorPosition: [0.0, 0.0]\n"
            "      Properties:\n"
            "        threshold:\n"
            "          type: float\n"
            "          value: \"nan\"\n"
            "  Connections: []\n";

        auto asset = Ref<DialogueTreeAsset>::Create();
        ASSERT_TRUE(serializer.TestDeserializeFromYAML(yaml, asset));

        const auto* node = asset->FindNode(UUID(1001));
        ASSERT_NE(node, nullptr);
        auto it = node->Properties.find("threshold");
        ASSERT_NE(it, node->Properties.end());
        // Non-finite float must NOT be stored as an f32 — it falls through to the raw string.
        EXPECT_FALSE(std::holds_alternative<f32>(it->second));
        const auto* asString = std::get_if<std::string>(&it->second);
        ASSERT_NE(asString, nullptr);
        EXPECT_EQ(*asString, "nan");
    }

    TEST_F(DialogueTreeFloatValidationTest, ValidFloatsAreParsedAndKept)
    {
        const std::string yaml =
            "DialogueTree:\n"
            "  RootNodeID: 2001\n"
            "  Nodes:\n"
            "    - ID: 2001\n"
            "      Type: condition\n"
            "      Name: Gate\n"
            "      EditorPosition: [100.0, 200.0]\n"
            "      Properties:\n"
            "        threshold:\n"
            "          type: float\n"
            "          value: \"1.5\"\n"
            "  Connections: []\n";

        auto asset = Ref<DialogueTreeAsset>::Create();
        ASSERT_TRUE(serializer.TestDeserializeFromYAML(yaml, asset));

        const auto* node = asset->FindNode(UUID(2001));
        ASSERT_NE(node, nullptr);
        EXPECT_FLOAT_EQ(node->EditorPosition.x, 100.0f);
        EXPECT_FLOAT_EQ(node->EditorPosition.y, 200.0f);

        auto it = node->Properties.find("threshold");
        ASSERT_NE(it, node->Properties.end());
        const auto* asFloat = std::get_if<f32>(&it->second);
        ASSERT_NE(asFloat, nullptr);
        EXPECT_FLOAT_EQ(*asFloat, 1.5f);
    }
} // namespace
