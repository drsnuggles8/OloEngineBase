#include "OloEnginePCH.h"

// OLO_TEST_LAYER: unit
// =============================================================================
// ParticleSystemComponentUndoEqualityTest — unit test (headless, no GL).
//
// Pins what ParticleSystemComponent's hand-written operator== treats as an edit
// (issue #1412). The editor's DrawComponent<T> compares a snapshot of the
// component against the live one every frame the inspector is open and pushes a
// "Property Change" undo entry when they differ. Scene::OnUpdateEditor simulates
// particle systems every Edit-mode frame for the preview, so the comparison has
// to be blind to simulation state. Two failure modes, one per direction:
//
//   1. It sees simulation state: every preview frame pushes a phantom undo entry
//      and marks the scene dirty. Pinned by SimulationIsNotAnEdit.
//   2. It misses an authored setting: that edit silently cannot be undone,
//      which is the bug #1412 fixed. Pinned by AuthoredSettingsAreEdits.
//
// The static half (the component reaches DrawComponent's operator== tier at
// all) lives in OwnedContainerInvariants.EveryConvertedComponentKeepsAnEditorUndoTier.
// =============================================================================

#include <gtest/gtest.h>

#include "OloEngine/Scene/Components.h"

using namespace OloEngine;

namespace
{
    ParticleSystemComponent MakeEmittingComponent()
    {
        ParticleSystemComponent psc;
        psc.System.Playing = true;
        psc.System.Looping = false;
        psc.System.Duration = 0.2f;
        psc.System.Emitter.RateOverTime = 200.0f;
        psc.System.Emitter.LifetimeMin = 5.0f;
        psc.System.Emitter.LifetimeMax = 5.0f;
        psc.System.SeedRandom(1412);
        return psc;
    }
} // namespace

TEST(ParticleSystemComponentUndoEquality, SimulationIsNotAnEdit)
{
    ParticleSystemComponent psc = MakeEmittingComponent();
    const ParticleSystemComponent snapshot = psc;

    // Run past Duration so the non-looping system also clears Playing itself,
    // the one public field the simulation writes.
    for (int frame = 0; frame < 10; ++frame)
    {
        psc.System.Update(0.05f, glm::vec3(0.0f));
    }

    ASSERT_GT(psc.System.GetAliveCount(), 0u) << "the system never emitted, so this test proves nothing";
    ASSERT_FALSE(psc.System.Playing) << "the non-looping system should have stopped itself";
    EXPECT_TRUE(snapshot == psc) << "simulating the preview would push a phantom undo entry every frame";
}

TEST(ParticleSystemComponentUndoEquality, AuthoredSettingsAreEdits)
{
    const ParticleSystemComponent snapshot = MakeEmittingComponent();

    auto expectEdit = [&snapshot](const char* what, auto&& edit)
    {
        ParticleSystemComponent edited = snapshot;
        ASSERT_TRUE(snapshot == edited) << "a copy must compare equal before editing " << what;
        edit(edited);
        EXPECT_FALSE(snapshot == edited) << "editing " << what << " would not be undoable";
    };

    expectEdit("Looping", [](ParticleSystemComponent& c)
               { c.System.Looping = true; });
    expectEdit("Duration", [](ParticleSystemComponent& c)
               { c.System.Duration = 3.0f; });
    expectEdit("the emitter rate", [](ParticleSystemComponent& c)
               { c.System.Emitter.RateOverTime = 5.0f; });
    expectEdit("the emission shape", [](ParticleSystemComponent& c)
               { c.System.Emitter.Shape = EmitSphere{ 2.0f }; });
    expectEdit("a burst", [](ParticleSystemComponent& c)
               { c.System.Emitter.Bursts.Add(BurstEntry{}); });
    expectEdit("a size-curve key", [](ParticleSystemComponent& c)
               { c.System.SizeModule.SizeCurve.Keys[1].Value = 0.5f; });
    expectEdit("a force field", [](ParticleSystemComponent& c)
               { c.System.ForceFields.Add(ModuleForceField{}); });
    expectEdit("a sub-emitter entry", [](ParticleSystemComponent& c)
               { c.System.SubEmitterModule.Entries.Add(SubEmitterEntry{}); });
    expectEdit("the trail colour", [](ParticleSystemComponent& c)
               { c.System.TrailModule.ColorEnd = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f); });
    expectEdit("Max Particles", [](ParticleSystemComponent& c)
               { c.System.SetMaxParticles(c.System.GetMaxParticles() + 1); });
    expectEdit("a child system's settings", [](ParticleSystemComponent& c)
               { c.ChildSystems.Add(ParticleSystem{}); });
    expectEdit("a child texture slot", [](ParticleSystemComponent& c)
               { c.ChildTextures.Add(nullptr); });
}

TEST(ParticleSystemComponentUndoEquality, ChildSystemSimulationIsNotAnEdit)
{
    ParticleSystemComponent psc = MakeEmittingComponent();
    psc.ChildSystems.Add(MakeEmittingComponent().System);
    const ParticleSystemComponent snapshot = psc;

    for (int frame = 0; frame < 5; ++frame)
    {
        psc.ChildSystems[0].Update(0.05f, glm::vec3(0.0f));
    }

    ASSERT_GT(psc.ChildSystems[0].GetAliveCount(), 0u);
    EXPECT_TRUE(snapshot == psc);
}

TEST(DialogueAndDiscoveryUndoEquality, IdentifiersAreCompared)
{
    DiscoveredSetComponent discovered;
    discovered.m_Discovered.Add(UUID(7));
    DiscoveredSetComponent otherDiscovered = discovered;
    EXPECT_TRUE(discovered == otherDiscovered);
    otherDiscovered.m_Discovered[0] = UUID(8);
    EXPECT_FALSE(discovered == otherDiscovered);

    DialogueStateComponent dialogue;
    dialogue.m_AvailableChoices.Add(DialogueChoice{ "Leave", UUID(3), "" });
    DialogueStateComponent otherDialogue = dialogue;
    EXPECT_TRUE(dialogue == otherDialogue);
    otherDialogue.m_AvailableChoices[0].TargetNodeID = UUID(4);
    EXPECT_FALSE(dialogue == otherDialogue);
    otherDialogue = dialogue;
    otherDialogue.m_TextRevealSpeed = 60.0f;
    EXPECT_FALSE(dialogue == otherDialogue);
}
