#pragma once

#include "OloEngine/Gameplay/Quest/Quest.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace OloEngine
{
    class QuestDatabase
    {
      public:
        static void LoadFromDirectory(const std::string& path);
        static void Register(const QuestDefinition& definition);
        static const QuestDefinition* Get(const std::string& questId);
        static std::vector<const QuestDefinition*> GetByCategory(const std::string& category);
        static std::vector<const QuestDefinition*> GetAll();
        static void Clear();

      private:
        static std::unordered_map<std::string, QuestDefinition>& GetQuests();
    };

} // namespace OloEngine
