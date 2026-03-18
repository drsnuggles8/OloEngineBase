#include "OloEnginePCH.h"
#include "AIRegistry.h"
#include "OloEngine/AI/BehaviorTree/BTComposites.h"
#include "OloEngine/AI/BehaviorTree/BTDecorators.h"
#include "OloEngine/AI/BehaviorTree/BTTasks.h"

namespace OloEngine
{
    void RegisterBuiltInAITypes()
    {
        OLO_PROFILE_FUNCTION();
        // Composite nodes
        BTNodeRegistry::Register("Sequence", []()
                                 { return Ref<BTSequence>(new BTSequence()); });
        BTNodeRegistry::Register("Selector", []()
                                 { return Ref<BTSelector>(new BTSelector()); });
        BTNodeRegistry::Register("Parallel", []()
                                 { return Ref<BTParallel>(new BTParallel()); });

        // Decorator nodes
        BTNodeRegistry::Register("Inverter", []()
                                 { return Ref<BTInverter>(new BTInverter()); });
        BTNodeRegistry::Register("Repeater", []()
                                 { return Ref<BTRepeater>(new BTRepeater()); });
        BTNodeRegistry::Register("Cooldown", []()
                                 { return Ref<BTCooldown>(new BTCooldown()); });
        BTNodeRegistry::Register("ConditionalGuard", []()
                                 { return Ref<BTConditionalGuard>(new BTConditionalGuard()); });

        // Task nodes
        BTNodeRegistry::Register("Wait", []()
                                 { return Ref<BTWait>(new BTWait()); });
        BTNodeRegistry::Register("SetBlackboardValue", []()
                                 { return Ref<BTSetBlackboardValue>(new BTSetBlackboardValue()); });
        BTNodeRegistry::Register("Log", []()
                                 { return Ref<BTLog>(new BTLog()); });
        BTNodeRegistry::Register("CheckBlackboardKey", []()
                                 { return Ref<BTCheckBlackboardKey>(new BTCheckBlackboardKey()); });
        BTNodeRegistry::Register("MoveTo", []()
                                 { return Ref<BTMoveTo>(new BTMoveTo()); });
        BTNodeRegistry::Register("PlayAnimation", []()
                                 { return Ref<BTPlayAnimation>(new BTPlayAnimation()); });
    }
} // namespace OloEngine
