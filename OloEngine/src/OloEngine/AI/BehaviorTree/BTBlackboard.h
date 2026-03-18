#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/UUID.h"

#include <glm/glm.hpp>

#include <string>
#include <unordered_map>
#include <variant>

namespace OloEngine
{
    class BTBlackboard
    {
      public:
        using Value = std::variant<bool, i32, f32, std::string, glm::vec3, UUID>;

        void Set(const std::string& key, Value value)
        {
            m_Data[key] = std::move(value);
        }

        template<typename T>
        [[nodiscard]] T Get(const std::string& key, T defaultValue = {}) const
        {
            auto it = m_Data.find(key);
            if (it == m_Data.end())
            {
                return defaultValue;
            }
            if (auto* val = std::get_if<T>(&it->second))
            {
                return *val;
            }
            return defaultValue;
        }

        [[nodiscard]] bool Has(const std::string& key) const
        {
            return m_Data.contains(key);
        }

        void Remove(const std::string& key)
        {
            m_Data.erase(key);
        }

        void Clear()
        {
            m_Data.clear();
        }

        [[nodiscard]] const std::unordered_map<std::string, Value>& GetAll() const
        {
            return m_Data;
        }

        [[nodiscard]] Value GetRaw(const std::string& key) const
        {
            auto it = m_Data.find(key);
            return (it != m_Data.end()) ? it->second : Value{};
        }

      private:
        std::unordered_map<std::string, Value> m_Data;
    };
} // namespace OloEngine
