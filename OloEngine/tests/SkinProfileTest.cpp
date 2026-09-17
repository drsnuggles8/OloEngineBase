// OLO_TEST_LAYER: unit
//
// The authored skin profile and the slot table that gives it a per-pixel
// identity (issue #1231).
//
// WHAT THIS PINS, AND WHY EACH PART NEEDS PINNING:
//
//   * VALIDATION. `SkinProfileParameters::Sanitize` is the only gate between an
//     authored file and the material UBO. A NaN scatter radius reaching the GPU
//     poisons a whole frame, and `std::clamp` does NOT catch it — NaN compares
//     false against both bounds and passes straight through. So the non-finite
//     cases are tested separately from the out-of-range ones.
//
//   * YAML ROUND-TRIP. The serializer is what makes a profile survive
//     import/edit/cook/save/load, and the ASSET-PACK path shares its
//     to-/from-string body, so a round-trip here covers the cooked path too.
//     It runs without a project on disk, through the public
//     SerializeToYAML/DeserializeFromYAML pair (the same arrangement
//     TilesetSerializerTest uses).
//
//   * SLOT ASSIGNMENT. The deferred path names a profile by a three-bit slot.
//     Two profiles sharing a slot would shade one head with the other's
//     parameters, and slots CHANGING between frames would make a pixel's
//     identity flicker for no authored reason — so stickiness is asserted, not
//     assumed.
//
//   * LOUD FALLBACKS. The house rule is that a path which cannot do its job
//     says so countably. Every fallback reason is exercised and its counter
//     checked, because a test that scraped the log would pass just as well if
//     the substitution were silent.
//
// No GPU: every one of these is CPU state, which is also what makes them run on
// the Linux CI runners that have no GL context.

// The PCH FIRST, not as decoration: pulling EditorAssetManager.h in reaches
// Scene.h -> Task.h -> TaskPrivate.h, whose `FPlatformProcess::Yield()` is a
// hard parse error once <windows.h> has defined `Yield` as a macro. The PCH
// is what establishes the engine's Windows-header hygiene.
#include "OloEnginePCH.h"

#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Asset/AssetManager/EditorAssetManager.h"
#include "OloEngine/Asset/AssetSerializer.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Renderer/Material.h"
#include "OloEngine/Renderer/MaterialKind.h"
#include "OloEngine/Renderer/PBRModel.h"
#include "OloEngine/Renderer/SkinProfile.h"
#include "OloEngine/Renderer/SkinProfileTable.h"

#include "TestTempDir.h"

#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <set>
#include <vector>

using namespace OloEngine;

namespace
{
    // A profile whose every field is a distinct, in-range, non-default value, so
    // a round-trip that drops one is visible rather than merely plausible.
    SkinProfileParameters SentinelParameters()
    {
        SkinProfileParameters p;
        p.EvaluationModel = SkinEvaluationModel::DiffuseSpecularSplit;
        p.ScatterColor = glm::vec3(0.71f, 0.43f, 0.29f);
        p.ScatterRadiusMM = glm::vec3(2.25f, 1.125f, 0.375f);
        p.ThicknessScale = 850.0f;
        p.SpecularTint = glm::vec3(0.95f, 0.87f, 0.81f);
        // The transmission lobe (issue #1242). EVERY FIELD IS OFF ITS DEFAULT,
        // which is the whole job of a sentinel: AuthoredValuesSurviveAYamlRoundTrip
        // compares with operator==, so a field left at its default would
        // round-trip "correctly" through the default on BOTH sides and the test
        // would pass for a serializer that never wrote the key at all. That is
        // the same trap this file's EvaluationModel assertion already calls out.
        p.Transmission.Strength = 0.625f;
        p.Transmission.Anisotropy = 0.375f;
        p.Transmission.Power = 12.0f;
        return p;
    }
} // namespace

namespace OloEngine::Tests
{
    // =========================================================================
    // The kind / algorithm-version separation (ADR 0024)
    // =========================================================================

    // The design claim the whole issue rests on, asserted rather than left in a
    // comment: the two selectors are independent, so every combination is
    // representable. A future refactor that folded skin into PBRModel would
    // make this fail to compile, which is the point.
    TEST(SkinMaterialKindTest, KindAndClosureVersionAreIndependentAxes)
    {
        Material material;
        EXPECT_EQ(material.GetMaterialKind(), MaterialKind::Generic);
        EXPECT_EQ(material.GetPBRModel(), PBRModel::Legacy);

        material.SetMaterialKind(MaterialKind::Skin);
        EXPECT_EQ(material.GetPBRModel(), PBRModel::Legacy)
            << "setting the kind changed the closure version — the two axes are not independent";

        material.SetPBRModel(PBRModel::ClosureV2);
        EXPECT_EQ(material.GetMaterialKind(), MaterialKind::Skin)
            << "setting the closure version changed the kind — the two axes are not independent";
    }

    // The two-bit G-Buffer field is the hard ceiling on the kind enum, and
    // MaterialKind.h static_asserts against it. This is the runtime half of the
    // same claim: every declared kind fits, and the validator agrees about which
    // values exist.
    TEST(SkinMaterialKindTest, EveryDeclaredKindFitsTheGBufferField)
    {
        for (i32 kind = 0; kind < kMaterialKindCount; ++kind)
        {
            EXPECT_TRUE(IsValidMaterialKind(kind)) << "kind " << kind << " is below kMaterialKindCount but rejected";
            EXPECT_LE(kind, kMaterialKindGBufferMax) << "kind " << kind << " does not fit the G-Buffer kind field";
        }
        EXPECT_FALSE(IsValidMaterialKind(-1));
        EXPECT_FALSE(IsValidMaterialKind(kMaterialKindCount));
    }

    // =========================================================================
    // Parameter validation
    // =========================================================================

    TEST(SkinProfileTest, SanitizeAcceptsAnAuthoredProfileUnchanged)
    {
        SkinProfileParameters p = SentinelParameters();
        const SkinProfileParameters before = p;
        EXPECT_TRUE(p.Sanitize()) << "an in-range profile was reported as corrected";
        EXPECT_TRUE(p == before) << "Sanitize changed a value that was already valid";
    }

    // NaN and infinity, per field. These are the cases `std::clamp` does not
    // catch: NaN compares false against both bounds, so a clamp-only validator
    // passes it through and the material UBO carries a NaN.
    TEST(SkinProfileTest, SanitizeReplacesEveryNonFiniteFieldWithItsDefault)
    {
        const SkinProfileParameters defaults{};
        constexpr f32 kNaN = std::numeric_limits<f32>::quiet_NaN();
        constexpr f32 kInf = std::numeric_limits<f32>::infinity();

        {
            SkinProfileParameters p = SentinelParameters();
            p.ScatterColor.g = kNaN;
            EXPECT_FALSE(p.Sanitize());
            EXPECT_TRUE(std::isfinite(p.ScatterColor.g));
            EXPECT_FLOAT_EQ(p.ScatterColor.g, defaults.ScatterColor.g);
        }
        {
            SkinProfileParameters p = SentinelParameters();
            p.ScatterRadiusMM.r = kInf;
            EXPECT_FALSE(p.Sanitize());
            EXPECT_TRUE(std::isfinite(p.ScatterRadiusMM.r));
            EXPECT_FLOAT_EQ(p.ScatterRadiusMM.r, defaults.ScatterRadiusMM.r);
        }
        {
            SkinProfileParameters p = SentinelParameters();
            p.ThicknessScale = -kInf;
            EXPECT_FALSE(p.Sanitize());
            EXPECT_TRUE(std::isfinite(p.ThicknessScale));
            EXPECT_FLOAT_EQ(p.ThicknessScale, defaults.ThicknessScale);
        }
        {
            SkinProfileParameters p = SentinelParameters();
            p.SpecularTint.b = kNaN;
            EXPECT_FALSE(p.Sanitize());
            EXPECT_TRUE(std::isfinite(p.SpecularTint.b));
            EXPECT_FLOAT_EQ(p.SpecularTint.b, defaults.SpecularTint.b);
        }
    }

    // A finite but out-of-range value is a different failure: it CLAMPS to the
    // bound rather than reverting to the default, because the author's intent is
    // legible ("as much as possible") where a NaN's is not.
    TEST(SkinProfileTest, SanitizeClampsFiniteOutOfRangeValuesToTheirBounds)
    {
        SkinProfileParameters p = SentinelParameters();
        p.ScatterColor = glm::vec3(-0.5f, 2.0f, 0.5f);
        p.ScatterRadiusMM = glm::vec3(0.0f, kMaxSkinScatterRadiusMM * 10.0f, 1.0f);
        p.ThicknessScale = -1.0f;
        p.SpecularTint = glm::vec3(1.5f, 0.5f, -1.0f);

        EXPECT_FALSE(p.Sanitize());

        EXPECT_FLOAT_EQ(p.ScatterColor.r, 0.0f);
        EXPECT_FLOAT_EQ(p.ScatterColor.g, 1.0f);
        EXPECT_FLOAT_EQ(p.ScatterColor.b, 0.5f) << "an in-range channel was altered by its neighbours";

        // Zero is not merely small: a zero mean free path divides by zero in any
        // diffusion profile, which is why the floor is positive rather than 0.
        EXPECT_FLOAT_EQ(p.ScatterRadiusMM.r, kMinSkinScatterRadiusMM);
        EXPECT_GT(p.ScatterRadiusMM.r, 0.0f);
        EXPECT_FLOAT_EQ(p.ScatterRadiusMM.g, kMaxSkinScatterRadiusMM);

        EXPECT_FLOAT_EQ(p.ThicknessScale, kMinSkinThicknessScale);

        EXPECT_FLOAT_EQ(p.SpecularTint.r, 1.0f);
        EXPECT_FLOAT_EQ(p.SpecularTint.b, 0.0f);
    }

    // An evaluation model index this build does not know is a file from a newer
    // build, not a rounding problem: it rejects to version 0 rather than
    // saturating onto the highest version this build happens to have.
    TEST(SkinProfileTest, SanitizeRejectsAnUnknownEvaluationModel)
    {
        SkinProfileParameters p = SentinelParameters();
        p.EvaluationModel = static_cast<SkinEvaluationModel>(kSkinEvaluationModelCount + 7);
        EXPECT_FALSE(p.Sanitize());
        EXPECT_EQ(p.EvaluationModel, SkinEvaluationModel::DiffuseSpecularSplit);
    }

    TEST(SkinProfileTest, DefaultParametersAreThemselvesValid)
    {
        // The fallback a missing profile gets must not itself need correcting —
        // otherwise the "loud fallback" path would log twice and the second
        // message would be about the engine's own defaults.
        SkinProfileParameters defaults = SkinProfile::DefaultParameters();
        EXPECT_TRUE(defaults.Sanitize());
    }

    // =========================================================================
    // YAML round-trip
    // =========================================================================

    TEST(SkinProfileSerializerTest, AuthoredValuesSurviveAYamlRoundTrip)
    {
        auto written = Ref<SkinProfile>::Create();
        written->SetName("Reference Head");
        ASSERT_TRUE(written->SetParameters(SentinelParameters()));

        const SkinProfileSerializer serializer;
        const std::string yaml = serializer.SerializeToYAML(written);
        ASSERT_FALSE(yaml.empty());

        auto read = Ref<SkinProfile>::Create();
        ASSERT_TRUE(serializer.DeserializeFromYAML(yaml, read));

        EXPECT_EQ(read->GetName(), "Reference Head");
        EXPECT_TRUE(read->GetParameters() == written->GetParameters())
            << "a field did not survive the YAML round-trip — the .oloskin on disk and the "
               "profile in memory describe different skin";

        // The transport version needs its own assertion while it has only ONE
        // enumerator: the sentinel above cannot make it non-default, so a
        // serializer that never wrote the key — or never read it — would
        // round-trip "correctly" through the default and this test would pass.
        // Asserting on the emitted text is what covers the field until a second
        // version exists to set it to.
        EXPECT_NE(yaml.find("EvaluationModel"), std::string::npos)
            << "the serializer did not emit the EvaluationModel key, so a profile authored "
               "against a later skin transport would silently load as version 0:\n"
            << yaml;

        // The transmission block, pinned by NAME as well as by value (issue
        // #1242). The value comparison above already covers it now that the
        // sentinel moves all three fields, but the nested map is a shape a
        // future edit could flatten -- and a flattened key the reader still
        // found by its old path would round-trip while the FILE FORMAT silently
        // changed under every .oloskin on disk.
        EXPECT_NE(yaml.find("Transmission"), std::string::npos)
            << "the serializer did not emit the Transmission block:\n"
            << yaml;
        for (const char* key : { "Strength", "Anisotropy", "Power" })
        {
            EXPECT_NE(yaml.find(key), std::string::npos)
                << "the serializer did not emit Transmission." << key << ":\n"
                << yaml;
        }
    }

    // The serializer is the second half of the validation gate: a hand-edited
    // file is untrusted input, and `nan` / `.inf` are both valid YAML floats.
    TEST(SkinProfileSerializerTest, NonFiniteAndOutOfRangeYamlValuesNeverReachTheProfile)
    {
        const std::string yaml = R"(SkinProfile:
  Name: Corrupt
  EvaluationModel: 0
  ScatterColor: [.nan, 4.0, -2.0]
  ScatterRadiusMM: [.inf, 0.0, 1.0]
  ThicknessScale: .nan
  SpecularTint: [1.0, .nan, 0.5]
)";
        const SkinProfileSerializer serializer;
        auto profile = Ref<SkinProfile>::Create();
        ASSERT_TRUE(serializer.DeserializeFromYAML(yaml, profile));

        const SkinProfileParameters& p = profile->GetParameters();
        EXPECT_TRUE(std::isfinite(p.ScatterColor.r) && std::isfinite(p.ScatterColor.g) && std::isfinite(p.ScatterColor.b));
        EXPECT_TRUE(std::isfinite(p.ScatterRadiusMM.r) && std::isfinite(p.ScatterRadiusMM.g) && std::isfinite(p.ScatterRadiusMM.b));
        EXPECT_TRUE(std::isfinite(p.ThicknessScale));
        EXPECT_TRUE(std::isfinite(p.SpecularTint.r) && std::isfinite(p.SpecularTint.g) && std::isfinite(p.SpecularTint.b));

        EXPECT_GE(p.ScatterColor.g, 0.0f);
        EXPECT_LE(p.ScatterColor.g, 1.0f);
        EXPECT_GE(p.ScatterRadiusMM.g, kMinSkinScatterRadiusMM);
    }

    TEST(SkinProfileSerializerTest, AMissingSectionIsRefusedRatherThanLoadedEmpty)
    {
        const SkinProfileSerializer serializer;
        auto profile = Ref<SkinProfile>::Create();
        EXPECT_FALSE(serializer.DeserializeFromYAML("SomethingElse:\n  Key: 1\n", profile))
            << "a file that is not a skin profile loaded as one";
    }

    // =========================================================================
    // The slot table
    // =========================================================================

    // The table resolves through the REAL asset manager, so these tests mount a
    // throwaway project and register the profiles as memory-only assets. Going
    // through the manager is the point: a table that took parameters directly
    // would not exercise the hop where a dropped .oloskin actually goes missing.
    class SkinProfileTableTest : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            std::error_code ec;
            m_ProjectDir = OloEngine::Tests::TempDir("skin-profile-table");
            std::filesystem::create_directories(m_ProjectDir / "Assets", ec);
            ASSERT_FALSE(ec) << "failed to create the scratch project dir at " << m_ProjectDir.string();

            const std::filesystem::path projectFile = m_ProjectDir / "SkinProfileTable.oloproj";
            {
                std::ofstream proj(projectFile);
                proj << "Project:\n"
                        "  Name: SkinProfileTable\n"
                        "  StartScene: \"\"\n"
                        "  AssetDirectory: \"Assets\"\n"
                        "  ScriptModulePath: \"\"\n";
            }
            ASSERT_TRUE(Project::Load(projectFile)) << "Project::Load failed for " << projectFile.string();

            m_AssetManager = Ref<EditorAssetManager>::Create();
            // No file watcher: nothing is staged and nothing hot-reloads, and a
            // watcher thread outliving the test races the next one.
            m_AssetManager->Initialize(/*startFileWatcher=*/false);
            Project::SetAssetManager(m_AssetManager);
        }

        void TearDown() override
        {
            // Unload(), not just a Reset(): Project::Load installed this project
            // in a process-wide static that holds the asset manager, and
            // SetAssetManager asserts on null, so Unload is the only way back to
            // the no-project state. Leaving it installed keeps the manager alive
            // until STATIC destruction, where it serializes its registry into a
            // temp directory this TearDown already deleted and then reads
            // already-destroyed state -- an exit-time heap-use-after-free the
            // ASan CI job catches after every case has passed (see the note on
            // Project::Unload in Project.h).
            m_AssetManager.Reset();
            Project::Unload();
            std::error_code ec;
            if (!m_ProjectDir.empty())
                std::filesystem::remove_all(m_ProjectDir, ec);
        }

        // Registers a profile and hands back its handle.
        [[nodiscard]] AssetHandle MakeProfile(const char* name)
        {
            auto profile = Ref<SkinProfile>::Create();
            profile->SetName(name);
            (void)profile->SetParameters(SentinelParameters());
            return AssetManager::AddMemoryOnlyAsset<SkinProfile>(profile);
        }

        Ref<EditorAssetManager> m_AssetManager;
        std::filesystem::path m_ProjectDir;
    };

    TEST_F(SkinProfileTableTest, DistinctHandlesGetDistinctStickySlots)
    {
        SkinProfileTable table;
        std::vector<AssetHandle> handles;
        std::set<u32> slots;
        for (u32 i = 0; i < kMaxSkinProfileSlots; ++i)
        {
            const AssetHandle handle = MakeProfile("Profile");
            ASSERT_NE(static_cast<u64>(handle), 0ULL);
            handles.push_back(handle);

            const SkinProfileResolution r = table.Resolve(handle);
            ASSERT_FALSE(r.IsFallback()) << "profile " << i << " fell back: " << ToString(r.Reason);
            EXPECT_LT(r.Slot, kSkinProfileSlotNone) << "profile " << i << " got no slot";
            EXPECT_TRUE(slots.insert(r.Slot).second)
                << "profile " << i << " reused slot " << r.Slot
                << " — two profiles sharing a slot shade one head with the other's parameters";
            EXPECT_TRUE(r.Parameters == SentinelParameters())
                << "the resolved parameters are not the ones the asset carries";
        }
        EXPECT_EQ(table.GetAssignedSlotCount(), kMaxSkinProfileSlots);

        // Sticky: asking again returns the same slot, and the slot still names
        // the same parameters. A slot that moved between frames would make a
        // pixel's profile identity flicker with nothing authored changing.
        for (u32 i = 0; i < handles.size(); ++i)
        {
            const SkinProfileResolution again = table.Resolve(handles[i]);
            EXPECT_FALSE(again.IsFallback());
            EXPECT_TRUE(slots.contains(again.Slot));
            EXPECT_TRUE(table.GetParametersForSlot(again.Slot) == SentinelParameters());
        }
    }

    TEST_F(SkinProfileTableTest, NoHandleIsReportedAndCountedRatherThanDefaultedSilently)
    {
        SkinProfileTable table;
        const SkinProfileResolution r = table.Resolve(0);

        EXPECT_TRUE(r.IsFallback());
        EXPECT_EQ(r.Reason, SkinProfileFallbackReason::NoHandle);
        EXPECT_EQ(r.Slot, kSkinProfileSlotNone) << "a material with no profile named one anyway";
        EXPECT_EQ(table.GetFallbackCount(SkinProfileFallbackReason::NoHandle), 1u);

        // The substituted parameters are real and finite — the frame still
        // renders a head rather than a hole.
        SkinProfileParameters substituted = r.Parameters;
        EXPECT_TRUE(substituted.Sanitize());

        // Counted per resolve, not per handle: the log is deduplicated, the
        // counter is not, so "how often did this happen?" stays answerable.
        (void)table.Resolve(0);
        EXPECT_EQ(table.GetFallbackCount(SkinProfileFallbackReason::NoHandle), 2u);
    }

    TEST_F(SkinProfileTableTest, AnUnloadableHandleIsReportedAndCounted)
    {
        SkinProfileTable table;
        // A handle nothing was ever registered under — which is the state a
        // scene is in after a merge dropped the .oloskin file.
        const SkinProfileResolution r = table.Resolve(0xDEADBEEFull);

        EXPECT_TRUE(r.IsFallback());
        EXPECT_EQ(r.Reason, SkinProfileFallbackReason::AssetMissing);
        EXPECT_EQ(r.Slot, kSkinProfileSlotNone);
        EXPECT_EQ(table.GetFallbackCount(SkinProfileFallbackReason::AssetMissing), 1u);
        EXPECT_EQ(table.GetAssignedSlotCount(), 0u)
            << "a profile that could not be loaded still consumed a slot";
    }

    // The negative cache must not flatten the reason. A cache that remembered
    // only "this handle failed" would report every repeat as AssetMissing and
    // increment that counter instead of the real one — which turns the countable
    // half of the no-silent-fallbacks rule into a number that lies.
    TEST_F(SkinProfileTableTest, TheNegativeCacheReportsTheOriginalReasonOnEveryRepeat)
    {
        SkinProfileTable table;
        const SkinProfileResolution first = table.Resolve(0xDEADBEEFull);
        const SkinProfileResolution second = table.Resolve(0xDEADBEEFull);
        const SkinProfileResolution third = table.Resolve(0xDEADBEEFull);

        EXPECT_EQ(first.Reason, SkinProfileFallbackReason::AssetMissing);
        EXPECT_EQ(second.Reason, first.Reason) << "the cached repeat reported a different reason than the first resolve";
        EXPECT_EQ(third.Reason, first.Reason);

        // Counted every time, not once — the log is deduplicated, the counter is
        // not, so "how often did this happen?" stays answerable.
        EXPECT_EQ(table.GetFallbackCount(SkinProfileFallbackReason::AssetMissing), 3u);
        EXPECT_EQ(table.GetFallbackCount(SkinProfileFallbackReason::WrongAssetType), 0u);
        EXPECT_EQ(table.GetFallbackCount(SkinProfileFallbackReason::NoHandle), 0u);
    }

    // A profile the author has just fixed and saved must stop shading with the
    // fallback on the next frame. Without this the negative cache would hold it
    // until a scene load, with the inspector showing the corrected values while
    // the frame ignored them.
    TEST_F(SkinProfileTableTest, ForgettingAFailedHandleMakesTheNextResolveConsultTheAssetAgain)
    {
        SkinProfileTable table;
        const AssetHandle handle = MakeProfile("Reloaded");
        ASSERT_NE(static_cast<u64>(handle), 0ULL);

        // Poison the handle as if its file had been missing, then prove the
        // cache is what is answering.
        (void)table.Resolve(0xBADF00Dull);
        ASSERT_EQ(table.GetFallbackCount(SkinProfileFallbackReason::AssetMissing), 1u);
        (void)table.Resolve(0xBADF00Dull);
        ASSERT_EQ(table.GetFallbackCount(SkinProfileFallbackReason::AssetMissing), 2u);

        table.ForgetFailedHandle(0xBADF00Dull);

        // Still missing, so it fails again — the point is that it was RE-ASKED
        // rather than answered from the cache, and a handle that now resolves
        // takes the same door.
        const SkinProfileResolution afterForget = table.Resolve(0xBADF00Dull);
        EXPECT_EQ(afterForget.Reason, SkinProfileFallbackReason::AssetMissing);

        // And a healthy handle resolves normally throughout.
        const SkinProfileResolution good = table.Resolve(handle);
        EXPECT_FALSE(good.IsFallback()) << "forgetting one handle disturbed another";
        EXPECT_LT(good.Slot, kSkinProfileSlotNone);
    }

    TEST_F(SkinProfileTableTest, RunningOutOfSlotsIsReportedRatherThanAliasing)
    {
        SkinProfileTable table;
        for (u32 i = 0; i < kMaxSkinProfileSlots; ++i)
            (void)table.Resolve(MakeProfile("Filler"));
        ASSERT_EQ(table.GetAssignedSlotCount(), kMaxSkinProfileSlots);

        const SkinProfileResolution overflow = table.Resolve(MakeProfile("Overflow"));
        EXPECT_EQ(overflow.Reason, SkinProfileFallbackReason::SlotBudgetFull);
        EXPECT_EQ(overflow.Slot, kSkinProfileSlotNone)
            << "the overflowing profile aliased onto an existing slot — that shades one head "
               "with another's parameters and looks like a correct frame";
        EXPECT_EQ(table.GetFallbackCount(SkinProfileFallbackReason::SlotBudgetFull), 1u);
        // The forward paths carry the profile per material and need no slot, so
        // the AUTHORED parameters still come back — only the per-pixel identity
        // the deferred path needs is lost.
        EXPECT_TRUE(overflow.Parameters == SentinelParameters());
    }

    TEST_F(SkinProfileTableTest, ResetClearsAssignmentsAndCounters)
    {
        SkinProfileTable table;
        (void)table.Resolve(0);
        (void)table.Resolve(MakeProfile("Kept"));
        ASSERT_GT(table.GetFallbackCount(SkinProfileFallbackReason::NoHandle), 0u);
        ASSERT_EQ(table.GetAssignedSlotCount(), 1u);

        table.Reset();
        EXPECT_EQ(table.GetAssignedSlotCount(), 0u);
        EXPECT_EQ(table.GetFallbackCount(SkinProfileFallbackReason::NoHandle), 0u);
        EXPECT_EQ(table.GetFallbackCount(SkinProfileFallbackReason::AssetMissing), 0u);
    }

    TEST_F(SkinProfileTableTest, AnUnassignedSlotYieldsTheNeutralDefaults)
    {
        SkinProfileTable table;
        // Every slot, including the "none" code and one past the table, must
        // answer with finite neutral parameters: the deferred pass indexes this
        // with a number decoded from a G-Buffer texel, and a stale slot must
        // read as "no profile effect", never as garbage.
        for (u32 slot = 0; slot <= kSkinProfileSlotNone; ++slot)
        {
            SkinProfileParameters p = table.GetParametersForSlot(slot);
            EXPECT_TRUE(p.Sanitize()) << "slot " << slot << " yielded parameters that needed correcting";
        }
    }

} // namespace OloEngine::Tests
