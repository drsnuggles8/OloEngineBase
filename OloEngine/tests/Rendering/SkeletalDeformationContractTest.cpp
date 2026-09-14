// =============================================================================
// SkeletalDeformationContractTest.cpp
//
// Criterion 1 of issue #1226: a skeletal subject must render with matching
// positions and normals in colour, depth and every available raster shadow
// technique. That is a statement about seven shaders agreeing, and the only way
// to make it hold is to stop having seven implementations of it — so what these
// tests assert is structural: every skinned consumer obtains its deformed
// vertex from the one shared producer,
// OloEditor/assets/shaders/include/SkeletalDeformation.glsl, and none of them
// still accumulates a bone palette of its own.
//
// A numeric comparison would be the weaker test here. Sampling positions from
// each pass proves the copies agree for the vertices sampled, on the driver
// that ran; it says nothing about the next edit. Before #1226 the copies HAD
// drifted and a numeric spot-check would very likely have missed it: the three
// shadow-path shaders indexed the bone palette with no bounds test and had no
// zero-weight guard, so they differed from the colour pass only for vertices
// with no bone influence or a bone ID past the palette — a vertex class a
// well-formed test mesh does not contain, and a real import routinely does.
//
// Pure text analysis of the shipped shader sources. No GL context.
//
// OLO_TEST_LAYER: shaderpipe
// =============================================================================

#include "OloEnginePCH.h"

#include "OloEngine/Animation/SkeletalDeformation.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef OLO_TEST_EDITOR_ROOT
#error "OLO_TEST_EDITOR_ROOT must be defined by the test target's CMake — see OloEngine/tests/CMakeLists.txt"
#endif

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;

        [[nodiscard]] fs::path ShaderRoot()
        {
            return fs::path{ OLO_TEST_EDITOR_ROOT } / "assets" / "shaders";
        }

        [[nodiscard]] std::string ReadFile(const fs::path& path)
        {
            std::ifstream file(path, std::ios::binary);
            if (!file)
                return {};
            std::ostringstream contents;
            contents << file.rdbuf();
            return contents.str();
        }

        // Every shader that deforms a skinned vertex. Adding a skinned pass
        // without adding it here is the drift this file exists to prevent, so
        // the list is closed and a separate test checks that nothing outside it
        // touches the bone palette.
        constexpr std::array kSkinnedConsumers = {
            std::string_view{ "DepthPrepass_Skinned.glsl" },
            std::string_view{ "DepthPrepass_MaskSkinned.glsl" },
            std::string_view{ "PBR_GBuffer_Skinned.glsl" },
            std::string_view{ "PBR_MultiLight_Skinned.glsl" },
            std::string_view{ "ShadowDepthSkinned.glsl" },
            std::string_view{ "VSM_DepthSkinned.glsl" },
            std::string_view{ "VSM_DepthLocalSkinned.glsl" },
        };

        constexpr std::string_view kProducerInclude = "include/SkeletalDeformation.glsl";

        // The producer's own entry points. A consumer must reach the palette
        // through one of these.
        constexpr std::array kProducerEntryPoints = {
            std::string_view{ "OloDeformSkinnedVertex" },
            std::string_view{ "OloDeformSkinnedPosition" },
        };
    } // namespace

    TEST(SkeletalDeformationContract, ProducerIncludeExists)
    {
        const fs::path producer = ShaderRoot() / "include" / "SkeletalDeformation.glsl";
        ASSERT_TRUE(fs::exists(producer))
            << "the shared deformation producer is missing at " << producer.string();
    }

    TEST(SkeletalDeformationContract, EverySkinnedConsumerIncludesTheSharedProducer)
    {
        for (const std::string_view consumer : kSkinnedConsumers)
        {
            const fs::path path = ShaderRoot() / consumer;
            const std::string source = ReadFile(path);
            ASSERT_FALSE(source.empty()) << "could not read " << path.string();

            EXPECT_NE(source.find(kProducerInclude), std::string::npos)
                << consumer << " does not include " << kProducerInclude
                << " — a skinned pass that reaches the bone palette on its own is free to "
                   "drift from the colour pass it is depth-tested and shadow-matched against";
        }
    }

    TEST(SkeletalDeformationContract, EverySkinnedConsumerCallsAProducerEntryPoint)
    {
        for (const std::string_view consumer : kSkinnedConsumers)
        {
            const std::string source = ReadFile(ShaderRoot() / consumer);
            ASSERT_FALSE(source.empty()) << consumer;

            const bool callsProducer = std::ranges::any_of(
                kProducerEntryPoints,
                [&source](std::string_view entry)
                { return source.find(entry) != std::string::npos; });

            EXPECT_TRUE(callsProducer)
                << consumer << " includes the producer but never calls it — including the header "
                               "without using it is exactly as divergent as not including it";
        }
    }

    TEST(SkeletalDeformationContract, NoSkinnedConsumerAccumulatesItsOwnBonePalette)
    {
        // The accumulation signature: multiplying a bone-palette element by a
        // weight. Both historical spellings are covered — the colour/depth
        // group wrote `u_BoneTransforms[...] * a_BoneWeights[i]` and the shadow
        // group wrote `u_BoneMatrices[...] * a_BoneWeights[...]`.
        const std::regex inlineAccumulation(
            R"(u_(?:BoneTransforms|BoneMatrices|PrevBoneTransforms)\s*\[[^\]]*\]\s*\*)");

        for (const std::string_view consumer : kSkinnedConsumers)
        {
            const std::string source = ReadFile(ShaderRoot() / consumer);
            ASSERT_FALSE(source.empty()) << consumer;

            EXPECT_FALSE(std::regex_search(source, inlineAccumulation))
                << consumer << " still blends the bone palette inline. That is the "
                               "seven-copies state #1226 removed: the three shadow-path copies had "
                               "already drifted from the colour copy, losing both the bone-ID bounds "
                               "test and the zero-weight guard, so an unweighted vertex collapsed onto "
                               "the model origin in shadows while rendering at its rest position in "
                               "colour";
        }
    }

    TEST(SkeletalDeformationContract, OnlyTheProducerDeclaresTheBonePalettes)
    {
        // A second declaration of binding 4 or 31 in a skinned consumer would
        // shadow the producer's and quietly reintroduce a private palette.
        const std::regex paletteDeclaration(
            R"(layout\s*\(\s*std140\s*,\s*binding\s*=\s*(?:4|31)\s*\)\s*uniform)");

        for (const std::string_view consumer : kSkinnedConsumers)
        {
            const std::string source = ReadFile(ShaderRoot() / consumer);
            ASSERT_FALSE(source.empty()) << consumer;

            EXPECT_FALSE(std::regex_search(source, paletteDeclaration))
                << consumer << " declares a bone-palette uniform block of its own; the "
                               "producer include owns bindings 4 and 31";
        }

        const std::string producer = ReadFile(ShaderRoot() / "include" / "SkeletalDeformation.glsl");
        ASSERT_FALSE(producer.empty());
        EXPECT_TRUE(std::regex_search(producer, paletteDeclaration))
            << "the producer no longer declares the bone palette — the consumers now have "
               "no palette at all";
    }

    TEST(SkeletalDeformationContract, ProducerBoneLimitMatchesTheEngineConstant)
    {
        // OLO_MAX_BONES bounds every palette index the producer emits, and the
        // C++ side clamps its upload to AnimationConstants::MAX_BONES. If the
        // two ever part company the bounds test stops bounding the buffer that
        // is actually uploaded, which is an out-of-range uniform read on both
        // backends and a device-fault risk on Vulkan.
        const std::string producer = ReadFile(ShaderRoot() / "include" / "SkeletalDeformation.glsl");
        ASSERT_FALSE(producer.empty());

        const std::regex maxBones(R"(#define\s+OLO_MAX_BONES\s+(\d+))");
        std::smatch match;
        ASSERT_TRUE(std::regex_search(producer, match, maxBones))
            << "the producer does not define OLO_MAX_BONES";

        EXPECT_EQ(std::stoul(match[1].str()),
                  static_cast<unsigned long>(UBOStructures::AnimationConstants::MAX_BONES))
            << "OLO_MAX_BONES in SkeletalDeformation.glsl disagrees with "
               "AnimationConstants::MAX_BONES";
    }

    TEST(SkeletalDeformationContract, ProducerGuardsBoneIdsAndZeroWeightVertices)
    {
        const std::string producer = ReadFile(ShaderRoot() / "include" / "SkeletalDeformation.glsl");
        ASSERT_FALSE(producer.empty());

        // The bounds test. Its absence in the shadow group is the defect #1226
        // fixed; asserting on the producer is what stops it coming back for all
        // seven consumers at once.
        EXPECT_NE(producer.find("boneID >= 0 && boneID < OLO_MAX_BONES"), std::string::npos)
            << "the producer no longer bounds the bone ID before indexing the palette";

        // The zero-weight guard. Without it the accumulation of an unweighted
        // vertex is the zero matrix, which maps the vertex onto the model
        // origin rather than leaving it where it is.
        EXPECT_NE(producer.find("OLO_MIN_TOTAL_BONE_WEIGHT"), std::string::npos)
            << "the producer no longer guards zero-weight vertices";
    }

    TEST(SkeletalDeformationContract, OnlyVelocityConsumersOptIntoThePreviousPose)
    {
        // The previous-pose palette at binding 31 is opt-in because a declared
        // but unbound descriptor is a Vulkan validation error, and only the two
        // colour passes emit velocity. Depth and shadow passes must not opt in.
        constexpr std::array kVelocityConsumers = {
            std::string_view{ "PBR_GBuffer_Skinned.glsl" },
            std::string_view{ "PBR_MultiLight_Skinned.glsl" },
        };
        constexpr std::string_view kOptIn = "OLO_DEFORM_WANT_PREV";

        for (const std::string_view consumer : kSkinnedConsumers)
        {
            const std::string source = ReadFile(ShaderRoot() / consumer);
            ASSERT_FALSE(source.empty()) << consumer;

            const bool emitsVelocity = std::ranges::find(kVelocityConsumers, consumer) != kVelocityConsumers.end();
            const bool optsIn = source.find(kOptIn) != std::string::npos;

            if (emitsVelocity)
            {
                EXPECT_TRUE(optsIn) << consumer
                                    << " emits velocity but does not opt into the previous pose, so "
                                       "its motion vectors would carry entity motion only and a "
                                       "stationary animating character would report none";
            }
            else
            {
                EXPECT_FALSE(optsIn)
                    << consumer
                    << " opts into the previous-pose palette but never emits velocity — that "
                       "declares descriptor binding 31 on a pipeline nothing binds it for";
            }
        }
    }
    // Every read of the previous-pose palette must be gated on HasBoneHistory().
    //
    // This exists because the gate was first added to Renderer3D's animated-mesh
    // submission, which has NO CALLERS, while the path that actually renders --
    // Scene's own animated-mesh loop -- kept reading the palette raw. The flag was
    // therefore still decorative on everything that draws, which is precisely the
    // defect the flag had been introduced to fix. A guard applied to one of N call
    // sites is the failure mode here, so the test is over call sites, not behaviour.
    TEST(SkeletalDeformationContract, EveryPreviousPaletteReadIsGatedOnHasBoneHistory)
    {
        const std::array kSubmissionSources = {
            std::string_view{ "OloEngine/src/OloEngine/Scene/Scene.cpp" },
            std::string_view{ "OloEngine/src/OloEngine/Renderer/Renderer3DMeshSubmission.cpp" },
        };
        const fs::path repoRoot = fs::path{ OLO_TEST_EDITOR_ROOT }.parent_path();

        for (const std::string_view relative : kSubmissionSources)
        {
            const fs::path path = repoRoot / relative;
            const std::string source = ReadFile(path);
            ASSERT_FALSE(source.empty()) << "could not read " << path.string();

            constexpr char TheNewline = 0x0A;
            std::vector<std::string> lines;
            for (std::size_t start = 0; start <= source.size();)
            {
                const std::size_t end = source.find(TheNewline, start);
                lines.push_back(source.substr(start, end == std::string::npos ? std::string::npos : end - start));
                if (end == std::string::npos)
                    break;
                start = end + 1;
            }

            for (std::size_t i = 0; i < lines.size(); ++i)
            {
                if (lines[i].find("m_PrevFinalBoneMatrices") == std::string::npos)
                    continue;

                // The guard reads within a few lines of the access in every
                // current spelling (a ternary above it, or an if just before).
                const std::size_t lo = i >= 8 ? i - 8 : 0;
                const std::size_t hi = std::min(lines.size(), i + 9);
                bool guarded = false;
                for (std::size_t j = lo; j < hi && !guarded; ++j)
                    guarded = lines[j].find("HasBoneHistory") != std::string::npos;

                EXPECT_TRUE(guarded)
                    << relative << ":" << (i + 1)
                    << " reads m_PrevFinalBoneMatrices without a nearby HasBoneHistory() gate. "
                       "An ungated read hands the shaders a previous pose after a discontinuity, "
                       "which emits a velocity across the seam that TAA and motion blur smear.";
            }
        }
    }

    // Every frame-boundary bone-history advance is paired with a morph-history
    // advance, at the same call site.
    //
    // Same shape of risk as EveryPreviousPaletteReadIsGatedOnHasBoneHistory above,
    // and the same counter-move. The morph half of the surface (#1227) has its own
    // per-entity history, and it is only correct if it is advanced at EVERY frame
    // entry point the bone half is advanced at: Scene has four
    // (OnUpdateRuntime, OnUpdateRuntimeFixed, OnUpdateSimulation, OnUpdateEditor),
    // and a morph advance wired into three of them fails in exactly one mode,
    // silently, as a morphing character that smears under TAA in edit-mode preview
    // but not in Play. Nothing asserts and no numeric test distinguishes the two.
    //
    // The pairing is asserted over CALL SITES rather than over behaviour, because
    // #1226 shipped a guard on a function with no callers while the live path went
    // unguarded, and two review passes read it as done.
    TEST(SkeletalDeformationContract, EveryBoneHistoryAdvanceIsPairedWithAMorphAdvance)
    {
        const fs::path repoRoot = fs::path{ OLO_TEST_EDITOR_ROOT }.parent_path();
        const fs::path path = repoRoot / "OloEngine/src/OloEngine/Scene/Scene.cpp";
        const std::string source = ReadFile(path);
        ASSERT_FALSE(source.empty()) << "could not read " << path.string();

        constexpr char TheNewline = 0x0A;
        std::vector<std::string> lines;
        for (std::size_t start = 0; start <= source.size();)
        {
            const std::size_t end = source.find(TheNewline, start);
            lines.push_back(source.substr(start, end == std::string::npos ? std::string::npos : end - start));
            if (end == std::string::npos)
                break;
            start = end + 1;
        }

        // Pairs of (bone call, the morph call that must accompany it).
        const std::array<std::pair<std::string_view, std::string_view>, 2> kPairs = {
            std::pair{ std::string_view{ "SkeletalDeformationSystem::AdvanceHistory(this)" },
                       std::string_view{ "MorphDeformationSystem::AdvanceHistory(this)" } },
            std::pair{ std::string_view{ "SkeletalDeformationSystem::ResetHistory(" },
                       std::string_view{ "MorphDeformationSystem::ResetHistory(" } },
        };

        u32 pairedAdvances = 0;
        for (const auto& [boneCall, morphCall] : kPairs)
        {
            for (std::size_t i = 0; i < lines.size(); ++i)
            {
                if (lines[i].find(boneCall) == std::string::npos)
                    continue;

                // The morph call sits within a few lines — immediately after, or
                // after the short comment that explains the pairing.
                const std::size_t lo = i >= 4 ? i - 4 : 0;
                const std::size_t hi = std::min(lines.size(), i + 12);
                bool paired = false;
                for (std::size_t j = lo; j < hi && !paired; ++j)
                    paired = lines[j].find(morphCall) != std::string::npos;

                EXPECT_TRUE(paired)
                    << "Scene.cpp:" << (i + 1) << " calls " << boneCall
                    << " with no nearby " << morphCall
                    << ". The two halves of the shared animated surface must advance and reset "
                       "together at every frame entry point; a morph history advanced at only "
                       "some of them freezes the morph delta in exactly the modes it was left "
                       "out of, and nothing logs.";
                if (paired && boneCall.find("AdvanceHistory") != std::string_view::npos)
                    ++pairedAdvances;
            }
        }

        // A guard nobody calls is the failure this file exists to catch, so the
        // scan also has to fail when the call sites themselves disappear.
        EXPECT_GE(pairedAdvances, 4u)
            << "fewer than four paired frame-boundary advances found in Scene.cpp — either a "
               "frame entry point stopped advancing deformation history, or this scan stopped "
               "matching the call and is now passing vacuously";
    }

    // The morph half's reset causes exist and are nameable.
    //
    // Every reset is attributed so a discontinuity can be told apart from "nothing
    // moved" from outside a debugging session (#1226's counters, extended by
    // #1227). A cause that falls through ToString to "Unknown" reads, in the
    // editor panel and in olo_skeletal_deformation_stats, as a bug in the engine
    // rather than as a missing switch arm.
    TEST(SkeletalDeformationContract, EveryMorphResetCauseHasAName)
    {
        using Animation::DeformationHistoryResetCause;
        constexpr std::array kMorphCauses = {
            DeformationHistoryResetCause::MorphSurfaceChanged,
            DeformationHistoryResetCause::MorphSetChanged,
            DeformationHistoryResetCause::MeshTopologyChanged,
        };

        for (const auto cause : kMorphCauses)
        {
            EXPECT_NE(Animation::ToString(cause), "Unknown")
                << "a deformation history reset cause has no name, so every reset attributed to "
                   "it is indistinguishable from an engine bug in the statistics panel";
        }
    }

} // namespace OloEngine::Tests
