#include "OloEnginePCH.h"

// OLO_TEST_LAYER: Functional
// =============================================================================
// SubtitlesFollowDialogueStateTest — Functional Test.
//
// Cross-subsystem seam under test:
//   Accessibility settings × SystemScheduler ("Subtitles" in the physics
//   shadow, After("Dialogue")) × DialogueStateComponent × the UI entity
//   hierarchy, driven by real Scene::OnUpdateRuntime ticks.
//
// This is the acceptance test for issue #458's first bullet — "dialogue / voice
// lines show subtitles that can be toggled". The three things a unit test on
// SubtitleSystem alone would NOT catch, and which each break the feature
// silently:
//
//  * The "Subtitles" node never being registered in the gameplay scheduler, so
//    SubtitleSystem::Update is never called from a real tick.
//  * Scene::InitDialogueSystem not constructing the system, so GetSubtitleSystem
//    returns null in every runtime session.
//  * The overlay entities not being created / torn down in step with the
//    caption, leaking UI entities across a Play → Stop → Play cycle.
//
// There is deliberately no assertion about audio: no voice-line playback path
// exists in the engine (DialogueNodeData carries no audio asset or duration), so
// captions are driven by dialogue node state and by explicit ShowCaption pushes.
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/Accessibility/AccessibilitySettings.h"
#include "OloEngine/Accessibility/SubtitleSystem.h"
#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Asset/AssetManager/EditorAssetManager.h"
#include "OloEngine/Dialogue/DialogueSystem.h"
#include "OloEngine/Dialogue/DialogueTreeAsset.h"
#include "OloEngine/Dialogue/DialogueTypes.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>

using namespace OloEngine;
using namespace OloEngine::Functional;

class SubtitlesFollowDialogueStateTest : public FunctionalTest
{
  protected:
    static constexpr const char* kLine = "The dragon stirs in its slumber...";
    static constexpr const char* kSpeaker = "Warden";

    void BuildScene() override
    {
        EnableAssetManager({});

        m_TreeAsset = Ref<DialogueTreeAsset>::Create();

        DialogueNodeData root;
        root.ID = OloEngine::UUID{ static_cast<u64>(0x200ULL) };
        root.Type = "dialogue";
        root.Name = "Opening";
        root.Properties.emplace("text", DialoguePropertyValue{ std::string(kLine) });
        root.Properties.emplace("speaker", DialoguePropertyValue{ std::string(kSpeaker) });
        m_TreeAsset->GetNodesWritable().push_back(std::move(root));
        m_TreeAsset->SetRootNodeID(OloEngine::UUID{ static_cast<u64>(0x200ULL) });
        m_TreeAsset->RebuildNodeIndex();

        m_TreeHandle = AssetManager::AddMemoryOnlyAsset<DialogueTreeAsset>(m_TreeAsset);
        ASSERT_NE(static_cast<u64>(m_TreeHandle), 0ULL);

        m_Speaker = GetScene().CreateEntity("Speaker");
        auto& dc = m_Speaker.AddComponent<DialogueComponent>();
        dc.m_DialogueTree = m_TreeHandle;
        m_Speaker.AddComponent<DialogueStateComponent>();

        EnableDialogue();
    }

    void TearDown() override
    {
        // The settings are process-global; leaving one on would leak into any
        // sibling case sharing this process under --gtest_filter.
        Accessibility::Reset();
        FunctionalTest::TearDown();
    }

    void EnableSubtitles(bool on)
    {
        AccessibilitySettings s = Accessibility::Get();
        s.SubtitlesEnabled = on;
        Accessibility::Set(s);
    }

    // Count the overlay entities the system owns, by name.
    [[nodiscard]] u32 CountOverlayEntities()
    {
        u32 count = 0;
        for (auto view = GetScene().GetAllEntitiesWith<TagComponent>(); auto e : view)
        {
            const auto& tag = view.template get<TagComponent>(e).Tag;
            if (tag == "SubtitleCanvas" || tag == "SubtitlePanel" || tag == "SubtitleText")
                ++count;
        }
        return count;
    }

    Ref<DialogueTreeAsset> m_TreeAsset;
    AssetHandle m_TreeHandle{};
    Entity m_Speaker;
};

TEST_F(SubtitlesFollowDialogueStateTest, SubtitleSystemIsConstructedWithTheDialogueSystem)
{
    EXPECT_NE(GetScene().GetSubtitleSystem(), nullptr)
        << "Scene::InitDialogueSystem did not allocate the subtitle system — the "
           "caption overlay would be dead in every runtime session.";
}

TEST_F(SubtitlesFollowDialogueStateTest, DisabledSubtitlesCreateNoEntitiesAndShowNothing)
{
    EnableSubtitles(false);

    auto* dialogue = GetScene().GetDialogueSystem();
    ASSERT_NE(dialogue, nullptr);
    dialogue->StartDialogue(m_Speaker);
    TickFor(0.5f);

    auto* subtitles = GetScene().GetSubtitleSystem();
    ASSERT_NE(subtitles, nullptr);
    EXPECT_FALSE(subtitles->IsVisible());
    EXPECT_TRUE(subtitles->GetVisibleText().empty());
    // The "pays nothing when unused" guarantee: the overlay is created lazily,
    // so a project that never enables captions gets no extra entities at all.
    EXPECT_EQ(CountOverlayEntities(), 0u)
        << "the overlay must be created lazily — a project that never shows a "
           "caption should not carry three extra UI entities";
}

TEST_F(SubtitlesFollowDialogueStateTest, ActiveDialogueShowsTheLineThroughARealSceneTick)
{
    EnableSubtitles(true);

    auto* dialogue = GetScene().GetDialogueSystem();
    ASSERT_NE(dialogue, nullptr);
    dialogue->StartDialogue(m_Speaker);

    ASSERT_EQ(m_Speaker.GetComponent<DialogueStateComponent>().m_State, DialogueState::Displaying);

    TickFor(0.2f);

    auto* subtitles = GetScene().GetSubtitleSystem();
    ASSERT_NE(subtitles, nullptr);
    EXPECT_TRUE(subtitles->IsVisible())
        << "the Subtitles scheduler node did not run — check Scene::GetGameplayScheduler";
    EXPECT_NE(subtitles->GetVisibleText().find(kLine), std::string::npos)
        << "the caption did not carry the dialogue line";
    EXPECT_NE(subtitles->GetVisibleText().find(kSpeaker), std::string::npos)
        << "speaker attribution is on by default and should be prefixed";
}

TEST_F(SubtitlesFollowDialogueStateTest, CaptionShowsTheWholeLineNotTheTypewriterPrefix)
{
    // A caption exists so the line can be read at the reader's own pace. Gating
    // it on m_TextRevealProgress would make the accessibility path SLOWER than
    // the content it stands in for — the dialogue box already typewriters.
    EnableSubtitles(true);

    auto* dialogue = GetScene().GetDialogueSystem();
    ASSERT_NE(dialogue, nullptr);
    dialogue->StartDialogue(m_Speaker);

    // One short tick: the reveal is nowhere near complete at 30 chars/sec.
    RunFrames(2);

    const auto& state = m_Speaker.GetComponent<DialogueStateComponent>();
    ASSERT_LT(state.m_TextRevealProgress, 1.0f) << "the reveal finished too early for this test to mean anything";

    auto* subtitles = GetScene().GetSubtitleSystem();
    ASSERT_NE(subtitles, nullptr);
    EXPECT_NE(subtitles->GetVisibleText().find(kLine), std::string::npos)
        << "the caption showed a partial line — captions must not be typewriter-gated";
}

TEST_F(SubtitlesFollowDialogueStateTest, ToggleHidesAndRestoresTheCaptionWithoutRestartingDialogue)
{
    // The acceptance criterion is "subtitles ... can be toggled", which means
    // mid-line too, not only before a conversation starts.
    EnableSubtitles(true);

    auto* dialogue = GetScene().GetDialogueSystem();
    ASSERT_NE(dialogue, nullptr);
    dialogue->StartDialogue(m_Speaker);
    TickFor(0.2f);

    auto* subtitles = GetScene().GetSubtitleSystem();
    ASSERT_NE(subtitles, nullptr);
    ASSERT_TRUE(subtitles->IsVisible());

    EnableSubtitles(false);
    RunFrames(2);
    EXPECT_FALSE(subtitles->IsVisible());
    EXPECT_TRUE(subtitles->GetVisibleText().empty());

    EnableSubtitles(true);
    RunFrames(2);
    EXPECT_TRUE(subtitles->IsVisible()) << "re-enabling mid-line must bring the caption back";
    EXPECT_NE(subtitles->GetVisibleText().find(kLine), std::string::npos);
}

TEST_F(SubtitlesFollowDialogueStateTest, SpeakerPrefixIsSuppressedWhenTheSettingIsOff)
{
    AccessibilitySettings s = Accessibility::Get();
    s.SubtitlesEnabled = true;
    s.SubtitleShowSpeaker = false;
    Accessibility::Set(s);

    auto* dialogue = GetScene().GetDialogueSystem();
    ASSERT_NE(dialogue, nullptr);
    dialogue->StartDialogue(m_Speaker);
    TickFor(0.2f);

    auto* subtitles = GetScene().GetSubtitleSystem();
    ASSERT_NE(subtitles, nullptr);
    EXPECT_EQ(subtitles->GetVisibleText(), std::string(kLine));
}

TEST_F(SubtitlesFollowDialogueStateTest, PushedCaptionOutranksDialogueAndExpiresOnItsOwn)
{
    // ShowCaption is the seam a voice-line / cinematic / barks path would use;
    // it must win over the dialogue source while live and release it on expiry.
    EnableSubtitles(true);

    auto* dialogue = GetScene().GetDialogueSystem();
    ASSERT_NE(dialogue, nullptr);
    dialogue->StartDialogue(m_Speaker);
    TickFor(0.2f);

    auto* subtitles = GetScene().GetSubtitleSystem();
    ASSERT_NE(subtitles, nullptr);

    subtitles->ShowCaption("Incoming transmission", "Radio", 0.5f);
    RunFrames(2);
    EXPECT_NE(subtitles->GetVisibleText().find("Incoming transmission"), std::string::npos)
        << "a pushed caption must outrank the dialogue source while it is live";

    TickFor(1.0f);
    EXPECT_NE(subtitles->GetVisibleText().find(kLine), std::string::npos)
        << "an expired pushed caption must fall back to the still-active dialogue line";
}

TEST_F(SubtitlesFollowDialogueStateTest, PushedCaptionWithABadDurationIsRejectedRatherThanPinned)
{
    EnableSubtitles(true);

    auto* subtitles = GetScene().GetSubtitleSystem();
    ASSERT_NE(subtitles, nullptr);

    subtitles->ShowCaption("Never shown", "", -1.0f);
    RunFrames(2);
    EXPECT_TRUE(subtitles->GetVisibleText().empty())
        << "a non-positive duration must clear, not pin a caption on screen forever";

    subtitles->ShowCaption("Also never shown", "", std::numeric_limits<f32>::quiet_NaN());
    RunFrames(2);
    EXPECT_TRUE(subtitles->GetVisibleText().empty());
}

TEST_F(SubtitlesFollowDialogueStateTest, CaptionClearsWhenTheDialogueEnds)
{
    EnableSubtitles(true);

    auto* dialogue = GetScene().GetDialogueSystem();
    ASSERT_NE(dialogue, nullptr);
    dialogue->StartDialogue(m_Speaker);
    TickFor(0.2f);

    auto* subtitles = GetScene().GetSubtitleSystem();
    ASSERT_NE(subtitles, nullptr);
    ASSERT_TRUE(subtitles->IsVisible());

    dialogue->EndDialogue(m_Speaker);
    RunFrames(2);

    EXPECT_FALSE(subtitles->IsVisible());
    EXPECT_TRUE(subtitles->GetVisibleText().empty());
}

TEST_F(SubtitlesFollowDialogueStateTest, OverlayEntitiesAreCreatedOnceAndReusedAcrossLines)
{
    // Churning three entities per line would be a structural registry change
    // every time a character speaks — the reason hiding is done by sort order
    // and alpha rather than by destroying the hierarchy.
    EnableSubtitles(true);

    auto* subtitles = GetScene().GetSubtitleSystem();
    ASSERT_NE(subtitles, nullptr);

    subtitles->ShowCaption("First", "", 5.0f);
    RunFrames(2);
    ASSERT_EQ(CountOverlayEntities(), 3u);
    // OloEngine::UUID, spelled out: <rpcdce.h> (pulled in transitively by the
    // Windows headers) defines a ::UUID struct, so the bare name is ambiguous.
    const OloEngine::UUID firstTextEntity = subtitles->GetTextEntity();
    ASSERT_NE(static_cast<u64>(firstTextEntity), 0ULL);

    subtitles->ClearCaption();
    RunFrames(2);
    EXPECT_FALSE(subtitles->IsVisible());
    EXPECT_EQ(CountOverlayEntities(), 3u) << "hiding must not destroy the overlay";

    subtitles->ShowCaption("Second", "", 5.0f);
    RunFrames(2);
    EXPECT_EQ(CountOverlayEntities(), 3u);
    EXPECT_EQ(static_cast<u64>(subtitles->GetTextEntity()), static_cast<u64>(firstTextEntity))
        << "the overlay must be reused, not rebuilt, between lines";
}

TEST_F(SubtitlesFollowDialogueStateTest, ShutdownTearsTheOverlayDownAndALaterCaptionRebuildsIt)
{
    // The third failure mode this file's header names, and the one that was
    // previously unasserted: the overlay must be torn down with the session, or
    // every Play -> Stop -> Play cycle leaks three UI entities.
    //
    // Driven through SubtitleSystem::Shutdown — the exact call
    // Scene::OnRuntimeStop makes. Calling OnRuntimeStop itself is NOT possible
    // from this harness: it tears down streaming, scripting and physics
    // subsystems that FunctionalTest wires by hand and never starts, and it
    // access-violates (verified, SEH 0xC0000005). The companion test below
    // covers the call site instead.
    EnableSubtitles(true);

    auto* subtitles = GetScene().GetSubtitleSystem();
    ASSERT_NE(subtitles, nullptr);
    subtitles->ShowCaption("Session one", "", 5.0f);
    RunFrames(2);
    ASSERT_EQ(CountOverlayEntities(), 3u) << "the overlay was never created, so teardown proves nothing";

    subtitles->Shutdown(GetScene());

    EXPECT_EQ(CountOverlayEntities(), 0u)
        << "Shutdown left the caption overlay behind — a Play/Stop cycle would leak three UI entities";
    EXPECT_FALSE(subtitles->IsVisible());
    EXPECT_TRUE(subtitles->GetVisibleText().empty());

    // ...and the system is reusable afterwards, which is the "Play again" half.
    subtitles->ShowCaption("Session two", "", 5.0f);
    RunFrames(2);
    EXPECT_EQ(CountOverlayEntities(), 3u) << "the overlay was not rebuilt for the next session";
    EXPECT_NE(subtitles->GetVisibleText().find("Session two"), std::string::npos);
}

TEST_F(SubtitlesFollowDialogueStateTest, RuntimeStopIsWiredToTearTheSubtitleSystemDown)
{
    // The companion to the test above. Shutdown works; this asserts it is
    // actually CALLED when a session ends. Losing that one line in
    // Scene::OnRuntimeStop is a silent per-session entity leak, and the
    // behavioural test cannot see it because the harness cannot run
    // OnRuntimeStop (see above).
    //
    // A source scan, like AccessibilitySettingsTest's UIRenderer draw-site
    // check: not elegant, but it fails loudly on the one edit that would
    // reintroduce the leak. Resolved from the compile-time root, never the CWD.
    const auto scenePath = std::filesystem::path{ OLO_TEST_EDITOR_ROOT }.parent_path() /
                           "OloEngine" / "src" / "OloEngine" / "Scene" / "Scene.cpp";
    std::ifstream file(scenePath);
    ASSERT_TRUE(file.is_open()) << "Scene.cpp not found at " << scenePath.string();
    const std::string source((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

    const auto stopAt = source.find("void Scene::OnRuntimeStop()");
    ASSERT_NE(stopAt, std::string::npos) << "Scene::OnRuntimeStop not found — has it been renamed?";

    // Bound the search to OnRuntimeStop's OWN body. Searching to end-of-file
    // would let a call in any LATER method satisfy this guard after the one in
    // OnRuntimeStop was removed — which would make this test worse than no test
    // at all, because it would report the leak as guarded. The needle is a
    // newline + four-space closing brace: the end of a namespace-scope method
    // at this file's clang-format-enforced indentation.
    const auto stopEndAt = source.find("\n    }\n", stopAt);
    ASSERT_NE(stopEndAt, std::string::npos) << "could not find the end of Scene::OnRuntimeStop";

    const auto shutdownAt = source.find("m_SubtitleSystem->Shutdown(*this)", stopAt);
    EXPECT_TRUE(shutdownAt != std::string::npos && shutdownAt < stopEndAt)
        << "Scene::OnRuntimeStop no longer calls m_SubtitleSystem->Shutdown(*this) — every "
           "Play/Stop cycle now leaks the caption overlay's three UI entities, and no "
           "behavioural test can catch it because the functional harness cannot drive "
           "OnRuntimeStop.";
}

TEST_F(SubtitlesFollowDialogueStateTest, CaptionFontSizeTracksTheSettingAndStaysAuthorAuthored)
{
    // The caption writes the AUTHORED size; UIRenderer multiplies the global
    // text scale on top at draw time. If the system pre-multiplied here, the two
    // accessibility settings would compound and a 2x scale would render 4x.
    AccessibilitySettings s = Accessibility::Get();
    s.SubtitlesEnabled = true;
    s.SubtitleFontSize = 40.0f;
    s.UITextScale = 2.0f;
    Accessibility::Set(s);

    auto* subtitles = GetScene().GetSubtitleSystem();
    ASSERT_NE(subtitles, nullptr);
    subtitles->ShowCaption("Sized", "", 5.0f);
    RunFrames(2);

    Entity textEntity = GetScene().GetEntityByUUID(subtitles->GetTextEntity());
    ASSERT_TRUE(static_cast<bool>(textEntity));
    ASSERT_TRUE(textEntity.HasComponent<UITextComponent>());

    EXPECT_FLOAT_EQ(textEntity.GetComponent<UITextComponent>().m_FontSize, 40.0f)
        << "the component must carry the authored size; the scale is applied at draw time";
    // …and the draw-time resolution is where the scale lands.
    EXPECT_FLOAT_EQ(Accessibility::ResolveFontSize(40.0f), 80.0f);
}

TEST_F(SubtitlesFollowDialogueStateTest, ComposeCaptionPassesLiteralsThroughAndFormatsTheSpeaker)
{
    // Pure formatting contract, no Scene needed. A line that is not a "@key:"
    // localization key must survive verbatim — the caption path routes through
    // LocalizationManager::ResolveLocalizedText rather than a parallel string
    // system, and that helper is pass-through for literals.
    EXPECT_EQ(SubtitleSystem::ComposeCaption("Hello", "Guard", true), "Guard: Hello");
    EXPECT_EQ(SubtitleSystem::ComposeCaption("Hello", "Guard", false), "Hello");
    EXPECT_EQ(SubtitleSystem::ComposeCaption("Hello", "", true), "Hello");
    EXPECT_TRUE(SubtitleSystem::ComposeCaption("", "Guard", true).empty())
        << "an empty line must produce no caption even when a speaker is set";
}
