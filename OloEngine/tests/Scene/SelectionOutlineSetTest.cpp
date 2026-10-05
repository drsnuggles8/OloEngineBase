// OLO_TEST_LAYER: unit
// =============================================================================
// SelectionOutlineSetTest.cpp -- issue #1533.
//
// The editor outlines a selection by entity ID, and the band is drawn wherever
// a covered pixel meets an uncovered one. A furred animal is three entities on
// screen -- the body, its eyes (children), its coat (a separate entity bound to
// the body) -- and outlining the body alone drew a band round every gap in the
// fur. These pin what a selection covers: its descendants and the coats bound
// to anything covered, each once, and nothing else.
//
// Headless, no GL: CollectSelectionOutlineIds walks the ECS only.
// =============================================================================

#include "OloEnginePCH.h"

#include <gtest/gtest.h>

#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/SelectionOutlineSet.h"

#include <algorithm>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        [[nodiscard]] i32 IdOf(Entity entity)
        {
            return static_cast<i32>(static_cast<u32>(entity));
        }

        [[nodiscard]] bool Covers(const std::vector<i32>& ids, Entity entity)
        {
            return std::ranges::find(ids, IdOf(entity)) != ids.end();
        }

        struct Animals
        {
            Ref<Scene> World;
            Entity Dog;
            Entity EyeL;
            Entity EyeR;
            Entity Coat;
            Entity Cat;
            Entity CatCoat;
        };

        [[nodiscard]] Animals MakeAnimals()
        {
            Animals a;
            a.World = Ref<Scene>::Create();
            a.Dog = a.World->CreateEntity("Dog");
            a.EyeL = a.World->CreateEntity("EyeL");
            a.EyeL.SetParent(a.Dog);
            a.EyeR = a.World->CreateEntity("EyeR");
            a.EyeR.SetParent(a.Dog);
            a.Coat = a.World->CreateEntity("DogCoat");
            auto& coat = a.Coat.AddComponent<GroomBindingComponent>();
            coat.m_TargetEntity = a.Dog.GetUUID();
            coat.m_Enabled = true;
            a.Cat = a.World->CreateEntity("Cat");
            a.CatCoat = a.World->CreateEntity("CatCoat");
            auto& catCoat = a.CatCoat.AddComponent<GroomBindingComponent>();
            catCoat.m_TargetEntity = a.Cat.GetUUID();
            catCoat.m_Enabled = true;
            return a;
        }
    } // namespace

    TEST(SelectionOutlineSet, AnAnimalIsOutlinedWithItsChildrenAndItsCoat)
    {
        Animals a = MakeAnimals();
        const std::vector<Entity> selected{ a.Dog };
        const std::vector<i32> ids = CollectSelectionOutlineIds(*a.World, selected);

        EXPECT_TRUE(Covers(ids, a.Dog));
        EXPECT_TRUE(Covers(ids, a.EyeL));
        EXPECT_TRUE(Covers(ids, a.EyeR));
        EXPECT_TRUE(Covers(ids, a.Coat)) << "the coat is part of the animal's silhouette";
        // The control: another animal's coat is bound to something that was not
        // selected, and must not join.
        EXPECT_FALSE(Covers(ids, a.Cat));
        EXPECT_FALSE(Covers(ids, a.CatCoat));
        EXPECT_EQ(ids.size(), 4u);
        EXPECT_EQ(ids.front(), IdOf(a.Dog)) << "the selection itself comes first";
    }

    TEST(SelectionOutlineSet, ACoatSelectedAloneAndADisabledBindingStayTheirOwn)
    {
        Animals a = MakeAnimals();

        // The coat alone is the coat alone: binding is one-way, body to coat.
        const std::vector<Entity> coatOnly{ a.Coat };
        const std::vector<i32> coatIds = CollectSelectionOutlineIds(*a.World, coatOnly);
        EXPECT_EQ(coatIds, std::vector<i32>{ IdOf(a.Coat) });

        // A disabled binding grows nothing on the body, so it is not its outline.
        a.Coat.GetComponent<GroomBindingComponent>().m_Enabled = false;
        const std::vector<Entity> dogOnly{ a.Dog };
        const std::vector<i32> dogIds = CollectSelectionOutlineIds(*a.World, dogOnly);
        EXPECT_FALSE(Covers(dogIds, a.Coat));
        EXPECT_EQ(dogIds.size(), 3u);
    }

    TEST(SelectionOutlineSet, EachEntityIsCoveredOnce)
    {
        Animals a = MakeAnimals();
        // An eye selected beside its parent, and the dog twice: the IDs go to a
        // fixed-size uniform array, so a repeat would crowd out a real entity.
        const std::vector<Entity> selected{ a.EyeL, a.Dog, a.Dog };
        const std::vector<i32> ids = CollectSelectionOutlineIds(*a.World, selected);
        std::vector<i32> sorted = ids;
        std::ranges::sort(sorted);
        EXPECT_EQ(std::ranges::adjacent_find(sorted), sorted.end()) << "an ID was listed twice";
        EXPECT_EQ(ids.size(), 4u);
    }
} // namespace OloEngine::Tests
