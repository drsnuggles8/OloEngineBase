#pragma once

#include "OloEngine/Core/TransparentStringHash.h"
#include "OloEngine/Gameplay/Quest/Quest.h"

#include <string>
#include <string_view>
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
        static std::vector<const QuestDefinition*> GetByCategory(std::string_view category);
        static std::vector<const QuestDefinition*> GetAll();
        static void Clear();

      private:
        [[nodiscard("quests map must be used")]] static std::unordered_map<std::string, QuestDefinition, StringHash, StringEqual>& GetQuests();
    };

} // namespace OloEngine
