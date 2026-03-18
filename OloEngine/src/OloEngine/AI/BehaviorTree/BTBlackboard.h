#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/UUID.h"

#include <glm/glm.hpp>

#include <optional>
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

        [[nodiscard]] std::optional<Value> GetRaw(const std::string& key) const
        {
            auto it = m_Data.find(key);
            if (it != m_Data.end())
            {
                return it->second;
            }
            return std::nullopt;
        }

      private:
        std::unordered_map<std::string, Value> m_Data;
    };

    [[nodiscard]] inline std::string BlackboardValueToString(const BTBlackboard::Value& value)
    {
        return std::visit([](auto const& v) -> std::string
                          {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, bool>)
                return v ? "true" : "false";
            else if constexpr (std::is_same_v<T, i32>)
                return std::to_string(v);
            else if constexpr (std::is_same_v<T, f32>)
                return std::to_string(v);
            else if constexpr (std::is_same_v<T, std::string>)
                return v;
            else if constexpr (std::is_same_v<T, glm::vec3>)
                return "(" + std::to_string(v.x) + ", " + std::to_string(v.y) + ", " + std::to_string(v.z) + ")";
            else if constexpr (std::is_same_v<T, UUID>)
                return std::to_string(static_cast<u64>(v));
            else
                return "<unknown>"; }, value);
    }
} // namespace OloEngine
