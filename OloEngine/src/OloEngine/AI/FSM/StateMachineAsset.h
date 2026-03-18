#pragma once

#include "OloEngine/Asset/Asset.h"
#include "OloEngine/Asset/AssetTypes.h"
#include "OloEngine/Core/UUID.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace OloEngine
{
    struct FSMStateData
    {
        std::string ID;
        std::string TypeName; // Registered state type, or empty for generic
        std::unordered_map<std::string, std::string> Properties;
    };

    struct FSMTransitionData
    {
        std::string FromState;
        std::string ToState;
        std::string ConditionExpression; // e.g. "Health < 20" — interpreted at runtime
    };

    class StateMachineAsset : public Asset
    {
      public:
        StateMachineAsset() = default;
        ~StateMachineAsset() override = default;

        static AssetType GetStaticType()
        {
            return AssetType::StateMachine;
        }
        AssetType GetAssetType() const override
        {
            return GetStaticType();
        }

        const std::vector<FSMStateData>& GetStates() const
        {
            return m_States;
        }
        std::vector<FSMStateData>& GetStates()
        {
            return m_States;
        }

        const std::vector<FSMTransitionData>& GetTransitions() const
        {
            return m_Transitions;
        }
        std::vector<FSMTransitionData>& GetTransitions()
        {
            return m_Transitions;
        }

        const std::string& GetInitialStateID() const
        {
            return m_InitialStateID;
        }
        void SetInitialStateID(const std::string& id)
        {
            m_InitialStateID = id;
        }

        void AddState(FSMStateData state)
        {
            m_States.push_back(std::move(state));
        }

        void AddTransition(FSMTransitionData transition)
        {
            m_Transitions.push_back(std::move(transition));
        }

      private:
        std::vector<FSMStateData> m_States;
        std::vector<FSMTransitionData> m_Transitions;
        std::string m_InitialStateID;
    };
} // namespace OloEngine
