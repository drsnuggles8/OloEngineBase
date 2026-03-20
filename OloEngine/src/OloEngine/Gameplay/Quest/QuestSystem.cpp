#include "OloEnginePCH.h"
#include "OloEngine/Gameplay/Quest/QuestSystem.h"
#include "OloEngine/Gameplay/Quest/QuestComponents.h"
#include "OloEngine/Gameplay/Quest/QuestDatabase.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"

namespace OloEngine
{
    void QuestSystem::OnUpdate(Scene* scene, f32 dt)
    {
        OLO_PROFILE_FUNCTION();

        auto journalView = scene->GetAllEntitiesWith<QuestJournalComponent>();
        for (auto e : journalView)
        {
            Entity entity = { e, scene };
            auto& jc = entity.GetComponent<QuestJournalComponent>();

            // Update timers for timed quests
            jc.Journal.UpdateTimers(dt);
        }
    }

} // namespace OloEngine
