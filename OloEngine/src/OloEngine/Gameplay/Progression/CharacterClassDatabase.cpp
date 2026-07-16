#include "OloEnginePCH.h"
#include "OloEngine/Gameplay/Progression/CharacterClassDatabase.h"

#include <unordered_set>

namespace OloEngine
{
    const CharacterClassDefinition* CharacterClassDatabase::FindClass(std::string_view classId) const
    {
        if (auto it = m_ClassIndex.find(classId); it != m_ClassIndex.end() && it->second < m_Classes.size())
        {
            return &m_Classes[it->second];
        }
        return nullptr;
    }

    void CharacterClassDatabase::RebuildIndex()
    {
        m_ClassIndex.clear();
        m_ClassIndex.reserve(m_Classes.size());
        auto classCount = m_Classes.size();
        for (sizet i = 0; i < classCount; ++i)
        {
            m_ClassIndex[m_Classes[i].ClassID] = i;
        }
    }

    bool CharacterClassDatabase::Validate(std::string* outError) const
    {
        auto fail = [outError](std::string message)
        {
            if (outError)
            {
                *outError = std::move(message);
            }
            return false;
        };

        std::unordered_set<std::string, StringHash, StringEqual> seenIds;
        seenIds.reserve(m_Classes.size());
        for (const auto& classDef : m_Classes)
        {
            if (classDef.ClassID.empty())
            {
                return fail("class with empty ClassID");
            }
            if (!seenIds.insert(classDef.ClassID).second)
            {
                return fail("duplicate ClassID '" + classDef.ClassID + "'");
            }
            if (classDef.AttributePointsPerLevel < 0 || classDef.SkillPointsPerLevel < 0)
            {
                return fail("class '" + classDef.ClassID + "' has negative per-level point grants");
            }
            if (classDef.LevelCap < 0)
            {
                return fail("class '" + classDef.ClassID + "' has negative LevelCap");
            }
            for (const auto& spec : classDef.Attributes)
            {
                if (spec.Attribute.empty())
                {
                    return fail("class '" + classDef.ClassID + "' has an attribute spec with empty name");
                }
            }
        }

        return true;
    }
} // namespace OloEngine
