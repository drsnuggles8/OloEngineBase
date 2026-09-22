// OLO_TEST_LAYER: unit
// Source-text fixtures exercise the production parser and recursive classifier.
// Filesystem enumeration and emitted-code compilation remain integration checks.
#include "ComponentClassifier.h"

#include <gtest/gtest.h>

#include <array>
#include <string_view>

namespace OloEngine::Tests
{
    namespace
    {
        auto Classify(std::string_view source)
        {
            const std::array sources{ source };
            return OloHeaderTool::ClassifyComponents(sources);
        }
        using OloHeaderTool::PropType;
    } // namespace

    TEST(OloHeaderToolClassifier, ClassifiesScalarsAndPreservesWireKeys)
    {
        const auto result = Classify(R"(
            enum class State : u8 { Idle, Active };
            struct ExampleComponent {
                OLO_PROPERTY() float m_Speed = 1.0f;
                std::string m_Label;
                glm::vec3 Position{};
                State Current = State::Idle;
                u8 Count = 0;
                UUID Target;
            };
        )");
        const auto& info = result.at("ExampleComponent");
        ASSERT_TRUE(info.trivial);
        ASSERT_EQ(info.fields.size(), 6u);
        EXPECT_EQ(info.fields[0].member, "m_Speed");
        EXPECT_EQ(info.fields[0].key, "Speed");
        EXPECT_EQ(info.fields[0].type, PropType::Float);
        EXPECT_EQ(info.fields[1].type, PropType::String);
        EXPECT_EQ(info.fields[2].type, PropType::Vec3);
        EXPECT_EQ(info.fields[3].type, PropType::Enum);
        EXPECT_EQ(info.fields[4].type, PropType::SmallUInt);
        EXPECT_EQ(info.fields[5].type, PropType::AssetHandle);
        EXPECT_EQ(info.fields[5].cppType, "UUID");
    }

    TEST(OloHeaderToolClassifier, ResolvesNestedStructsAcrossHeaderTexts)
    {
        const std::array<std::string_view, 3> sources{
            "struct PathComponent { std::vector<Waypoint> Points; Waypoint Current; };",
            "struct Waypoint { glm::vec3 Position; Mode State; };",
            "enum class Mode { Walk, Run };"
        };
        const auto result = OloHeaderTool::ClassifyComponents(sources);
        const auto& info = result.at("PathComponent");
        ASSERT_TRUE(info.trivial);
        ASSERT_EQ(info.fields.size(), 2u);
        EXPECT_TRUE(info.fields[0].isVector);
        EXPECT_EQ(info.fields[0].type, PropType::Struct);
        ASSERT_EQ(info.fields[0].subFields.size(), 2u);
        EXPECT_EQ(info.fields[0].subFields[1].type, PropType::Enum);
        EXPECT_FALSE(info.fields[1].isVector);
        EXPECT_EQ(info.fields[1].type, PropType::Struct);
    }

    TEST(OloHeaderToolClassifier, InlineMethodDoesNotSwallowPrivateLabel)
    {
        const auto result = Classify(R"(
            struct AccessComponent {
                float PublicValue = 1.0f;
                void Touch() { if (true) { } }
            private:
                float Hidden = 2.0f;
            public:
                int VisibleAgain = 3;
            };
        )");
        const auto& info = result.at("AccessComponent");
        EXPECT_FALSE(info.trivial);
        ASSERT_EQ(info.fields.size(), 2u);
        EXPECT_EQ(info.fields[0].member, "PublicValue");
        EXPECT_EQ(info.fields[1].member, "VisibleAgain");
    }

    TEST(OloHeaderToolClassifier, NestedPrivateMembersKeepOnlyPartialPublicFields)
    {
        const auto result = Classify(R"(
            class Settings {
                int DefaultPrivate;
            public:
                float Amount;
                void Update() {}
            private:
                float Hidden;
            };
            struct SettingsComponent { Settings Parameters; };
        )");
        const auto& info = result.at("SettingsComponent");
        EXPECT_FALSE(info.trivial);
        ASSERT_EQ(info.fields.size(), 1u);
        EXPECT_TRUE(info.fields[0].structPartial);
        ASSERT_EQ(info.fields[0].subFields.size(), 1u);
        EXPECT_EQ(info.fields[0].subFields[0].member, "Amount");
    }

    TEST(OloHeaderToolClassifier, UnknownMembersFailClosedWithoutLosingEditableFields)
    {
        const auto result = Classify(R"(
            struct RuntimeComponent { float Amount; UnknownType Runtime; };
        )");
        const auto& info = result.at("RuntimeComponent");
        EXPECT_FALSE(info.trivial);
        ASSERT_EQ(info.fields.size(), 1u);
        EXPECT_EQ(info.fields[0].member, "Amount");
    }

    TEST(OloHeaderToolClassifier, AssetRefsFollowTransitiveInheritance)
    {
        const std::array<std::string_view, 2> sources{
            R"(struct AssetComponent { Ref<Material> Surface; };
               struct RuntimeComponent { Ref<Worker> Runtime; };)",
            R"(class Asset {}; class RendererResource : public Asset {};
               class Material : public RendererResource {}; class Worker {}; )"
        };
        const auto result = OloHeaderTool::ClassifyComponents(sources);
        const auto& info = result.at("AssetComponent");
        ASSERT_TRUE(info.trivial);
        ASSERT_EQ(info.fields.size(), 1u);
        EXPECT_EQ(info.fields[0].type, PropType::Ref);
        EXPECT_EQ(info.fields[0].refType, "Material");
        EXPECT_FALSE(result.at("RuntimeComponent").trivial);
        EXPECT_TRUE(result.at("RuntimeComponent").fields.empty());
    }

    TEST(OloHeaderToolClassifier, SerializeAnnotationsPreserveSkipAndRangeSemantics)
    {
        const auto result = Classify(R"(
            struct RangedComponent {
                OLO_SERIALIZE(Clamp, Min=0, Max=10) float Amount;
                OLO_SERIALIZE(Reject, Min=1) int Count;
            private:
                OLO_SERIALIZE(Skip) UnknownType Runtime;
            };
            struct InvalidComponent {
                OLO_SERIALIZE(Clamp, Reject, Min=0) float Amount;
            };
            struct UnsupportedComponent {
                OLO_SERIALIZE(Clamp, Min=0) std::string Label;
            };
        )");
        const auto& info = result.at("RangedComponent");
        ASSERT_TRUE(info.trivial);
        ASSERT_EQ(info.fields.size(), 2u);
        EXPECT_TRUE(info.fields[0].hasClamp);
        EXPECT_FALSE(info.fields[0].hasReject);
        EXPECT_EQ(info.fields[0].clampMin, "0");
        EXPECT_EQ(info.fields[0].clampMax, "10");
        EXPECT_TRUE(info.fields[1].hasReject);
        EXPECT_EQ(info.fields[1].clampMin, "1");
        EXPECT_FALSE(result.at("InvalidComponent").trivial);
        EXPECT_FALSE(result.at("UnsupportedComponent").trivial);
    }

    TEST(OloHeaderToolClassifier, RecursiveTypesTerminateAndFailClosed)
    {
        const auto result = Classify(R"(
            struct Recursive { Recursive Next; };
            struct CycleComponent { Recursive Root; };
        )");
        EXPECT_FALSE(result.at("CycleComponent").trivial);
        EXPECT_TRUE(result.at("CycleComponent").fields.empty());
    }

    TEST(OloHeaderToolClassifier, CommentsForwardDeclarationsAndEnumClassesAreNotRecords)
    {
        const auto result = Classify(R"(
            // struct CommentComponent { int Value; };
            /* struct BlockComponent { int Value; }; */
            struct ForwardComponent;
            enum class EnumComponent { Value };
            class PrivateComponent { public: int Value; };
            struct RealComponent { int Value; };
        )");
        ASSERT_EQ(result.size(), 1u);
        EXPECT_TRUE(result.at("RealComponent").trivial);
    }

    TEST(OloHeaderToolClassifier, StructDefinitionWinsOverClassAndFirstStructWins)
    {
        const std::array<std::string_view, 3> sources{
            "class Settings { int Hidden; };",
            "struct Settings { float First; }; struct TestComponent { Settings Value; };",
            "struct Settings { int Second; };"
        };
        const auto result = OloHeaderTool::ClassifyComponents(sources);
        const auto& info = result.at("TestComponent");
        ASSERT_TRUE(info.trivial);
        ASSERT_EQ(info.fields.size(), 1u);
        ASSERT_EQ(info.fields[0].subFields.size(), 1u);
        EXPECT_EQ(info.fields[0].subFields[0].member, "First");
    }

    TEST(OloHeaderToolClassifier, TArrayAndVectorShareWireShapeButKeepTheirStorageApis)
    {
        const auto result = Classify(R"(
            struct ArraysComponent { TArray<glm::vec3> Points; std::vector<glm::vec3> LegacyPoints; };
        )");
        const auto& info = result.at("ArraysComponent");
        ASSERT_TRUE(info.trivial);
        ASSERT_EQ(info.fields.size(), 2u);
        EXPECT_TRUE(info.fields[0].isVector);
        EXPECT_TRUE(info.fields[1].isVector);
        EXPECT_TRUE(info.fields[0].isTArray);
        EXPECT_FALSE(info.fields[1].isTArray);
        EXPECT_EQ(info.fields[0].type, PropType::Vec3);
        EXPECT_EQ(info.fields[1].type, PropType::Vec3);
    }

    TEST(OloHeaderToolClassifier, FStringAndTArrayClassificationRecursesThroughNestedStructs)
    {
        const auto result = Classify(R"(
            struct Choice { FString Text; TArray<FString> Tags; };
            struct Conversation { TArray<Choice> Choices; };
            struct DialogueComponent { Conversation Data; FString Name; std::string BindingName; };
        )");
        const auto& info = result.at("DialogueComponent");
        ASSERT_TRUE(info.trivial);
        ASSERT_EQ(info.fields.size(), 3u);
        EXPECT_EQ(info.fields[1].type, PropType::FString);
        EXPECT_EQ(info.fields[2].type, PropType::String);
        ASSERT_EQ(info.fields[0].subFields.size(), 1u);
        const auto& choices = info.fields[0].subFields[0];
        EXPECT_TRUE(choices.isTArray);
        ASSERT_EQ(choices.subFields.size(), 2u);
        EXPECT_EQ(choices.subFields[0].type, PropType::FString);
        EXPECT_TRUE(choices.subFields[1].isTArray);
        EXPECT_EQ(choices.subFields[1].type, PropType::FString);
    }

    TEST(OloHeaderToolClassifier, UnsupportedArrayAllocatorsAndMalformedDeclarationsFailClosed)
    {
        for (const std::string_view declaration : {
                 "TArray<int, CustomAllocator> Values;",
                 "TArray<int, Alloc::Mode> Values;",
                 "TArray<int, TInlineAllocator<4>> Values;",
                 "TArray<TArray<int>> Values;",
                 "TArray<Ref<Texture>> Values;",
                 "TArray<int Values;",
                 "TArray<> Values;" })
        {
            SCOPED_TRACE(declaration);
            const auto result = Classify("enum class Mode { One }; class Texture : public Asset {}; "
                                         "struct InvalidComponent { " +
                                         std::string(declaration) + " };");
            EXPECT_FALSE(result.at("InvalidComponent").trivial);
            EXPECT_TRUE(result.at("InvalidComponent").fields.empty());
        }
    }
} // namespace OloEngine::Tests
