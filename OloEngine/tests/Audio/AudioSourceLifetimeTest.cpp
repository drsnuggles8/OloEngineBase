// OLO_TEST_LAYER: unit
//
// An AudioSource whose sound never initialised must destruct cleanly (#1203).
//
// Every headless test deserialises scenes with no audio engine up, so every
// AudioSourceComponent in them holds a source whose ma_sound_init_from_file
// failed. The destructor then called ma_sound_uninit on a zeroed ma_sound,
// which dereferences its null engine pointer: UBSan failed
// AssetSceneLoad.AllSandboxScenesDeserialiseThroughEditorAssetManager on the
// GPU UBSan nightly on exactly that line, while the ASan nightly (which does
// not diagnose a null member access) passed. This pins the contract directly,
// in a process where the engine is deliberately absent, so a sanitizer run
// sees it without a GPU.
#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Audio/AudioEngine.h"
#include "OloEngine/Audio/AudioSource.h"
#include "OloEngine/Core/Ref.h"

TEST(AudioSourceLifetime, ASourceThatFailedToInitialiseDestructsWithoutTouchingTheEngine)
{
    ASSERT_EQ(OloEngine::AudioEngine::GetEngine(), nullptr) << "this test needs the audio engine NOT to be up";

    // A path that cannot load, and no engine to load it into: the constructor
    // logs the failure and the object must still be safe to own and drop.
    auto source = OloEngine::Ref<OloEngine::AudioSource>::Create("does-not-exist/nothing.wav");
    ASSERT_TRUE(source);
    EXPECT_STREQ(source->GetPath(), "does-not-exist/nothing.wav");
    source.Reset(); // the destructor is the subject; under UBSan this is the assertion
    SUCCEED();
}
