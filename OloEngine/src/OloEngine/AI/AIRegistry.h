#pragma once

#include "OloEngine/AI/BehaviorTree/BTNode.h"
#include "OloEngine/AI/FSM/State.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Core/Log.h"

#include <functional>
#include <string>
#include <unordered_map>

namespace OloEngine
{
    class BTNodeRegistry
    {
      public:
        using Factory = std::function<Ref<BTNode>()>;

        static void Register(const std::string& typeName, Factory factory)
        {
            auto& factories = GetFactories();
            if (factories.contains(typeName))
            {
                OLO_CORE_WARN("[BTNodeRegistry] Overwriting existing type: {}", typeName);
            }
            factories[typeName] = std::move(factory);
        }

        static Ref<BTNode> Create(const std::string& typeName)
        {
            auto& factories = GetFactories();
            auto it = factories.find(typeName);
            if (it != factories.end())
            {
                return it->second();
            }
            OLO_CORE_WARN("[BTNodeRegistry] Unknown node type: {}", typeName);
            return nullptr;
        }

        static bool IsRegistered(const std::string& typeName)
        {
            return GetFactories().contains(typeName);
        }

        static const std::unordered_map<std::string, Factory>& GetRegisteredTypes()
        {
            return GetFactories();
        }

      private:
        static std::unordered_map<std::string, Factory>& GetFactories()
        {
            static std::unordered_map<std::string, Factory> s_Factories;
            return s_Factories;
        }
    };

    class FSMStateRegistry
    {
      public:
        using Factory = std::function<Ref<FSMState>()>;

        static void Register(const std::string& typeName, Factory factory)
        {
            auto& factories = GetFactories();
            if (factories.contains(typeName))
            {
                OLO_CORE_WARN("[FSMStateRegistry] Overwriting existing type: {}", typeName);
            }
            factories[typeName] = std::move(factory);
        }

        static Ref<FSMState> Create(const std::string& typeName)
        {
            auto& factories = GetFactories();
            auto it = factories.find(typeName);
            if (it != factories.end())
            {
                return it->second();
            }
            OLO_CORE_WARN("[FSMStateRegistry] Unknown state type: {}", typeName);
            return nullptr;
        }

        static bool IsRegistered(const std::string& typeName)
        {
            return GetFactories().contains(typeName);
        }

        static const std::unordered_map<std::string, Factory>& GetRegisteredTypes()
        {
            return GetFactories();
        }

      private:
        static std::unordered_map<std::string, Factory>& GetFactories()
        {
            static std::unordered_map<std::string, Factory> s_Factories;
            return s_Factories;
        }
    };

    // Register built-in BT nodes and FSM states
    void RegisterBuiltInAITypes();
} // namespace OloEngine
