#include "OloEnginePCH.h"
#include "VisualScriptTypes.h"

#include <array>
#include <charconv>
#include <cmath>
#include <sstream>
#include <system_error>

namespace OloEngine::VisualScript
{
    namespace
    {
        // Shortest representation that round-trips exactly through from_chars.
        // Deliberately NOT fmt/ostream with a fixed precision: a save → load →
        // save round trip must be byte-identical (AC#1) and a truncated decimal
        // is the classic way that quietly fails on the third save.
        std::string FloatToStorage(f32 value)
        {
            if (!std::isfinite(value))
            {
                // Non-finite never reaches storage — the writer sanitizes first.
                // Emitting "0" rather than "nan" keeps the file loadable by a
                // stricter parser.
                return "0";
            }
            std::array<char, 64> buffer{};
            const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
            if (result.ec != std::errc{})
            {
                return "0";
            }
            return std::string(buffer.data(), result.ptr);
        }

        f32 FloatFromStorage(std::string_view text)
        {
            f32 value = 0.0f;
            const auto* first = text.data();
            const auto* last = text.data() + text.size();
            while (first != last && (*first == ' ' || *first == '\t'))
            {
                ++first;
            }
            const auto result = std::from_chars(first, last, value);
            if (result.ec != std::errc{} || !std::isfinite(value))
            {
                return 0.0f;
            }
            return value;
        }

        i64 IntFromStorage(std::string_view text)
        {
            i64 value = 0;
            const auto* first = text.data();
            const auto* last = text.data() + text.size();
            while (first != last && (*first == ' ' || *first == '\t'))
            {
                ++first;
            }
            const auto result = std::from_chars(first, last, value);
            if (result.ec != std::errc{})
            {
                // Tolerate "3.7" landing in an Int pin — truncate rather than
                // silently producing 0, which reads as a deliberate value.
                return static_cast<i64>(FloatFromStorage(text));
            }
            return value;
        }

        u64 U64FromStorage(std::string_view text)
        {
            u64 value = 0;
            const auto* first = text.data();
            const auto* last = text.data() + text.size();
            while (first != last && (*first == ' ' || *first == '\t'))
            {
                ++first;
            }
            if (const auto result = std::from_chars(first, last, value); result.ec != std::errc{})
            {
                return 0;
            }
            return value;
        }

        // "1 2 3" -> up to 4 floats. Space-separated so the whole value stays one
        // YAML scalar and the storage form is identical for every arity.
        void ParseFloatList(std::string_view text, f32* out, sizet count)
        {
            for (sizet i = 0; i < count; ++i)
            {
                out[i] = 0.0f;
            }
            sizet index = 0;
            sizet cursor = 0;
            while (index < count && cursor < text.size())
            {
                while (cursor < text.size() && (text[cursor] == ' ' || text[cursor] == '\t'))
                {
                    ++cursor;
                }
                const sizet start = cursor;
                while (cursor < text.size() && text[cursor] != ' ' && text[cursor] != '\t')
                {
                    ++cursor;
                }
                if (cursor > start)
                {
                    out[index] = FloatFromStorage(text.substr(start, cursor - start));
                    ++index;
                }
            }
        }

        std::string JoinFloats(const f32* values, sizet count)
        {
            std::string result;
            for (sizet i = 0; i < count; ++i)
            {
                if (i != 0)
                {
                    result += ' ';
                }
                result += FloatToStorage(values[i]);
            }
            return result;
        }
    } // namespace

    const char* PinTypeToString(PinType type) noexcept
    {
        switch (type)
        {
            case PinType::Exec:
                return "Exec";
            case PinType::Bool:
                return "Bool";
            case PinType::Int:
                return "Int";
            case PinType::Float:
                return "Float";
            case PinType::Vec2:
                return "Vec2";
            case PinType::Vec3:
                return "Vec3";
            case PinType::Vec4:
                return "Vec4";
            case PinType::String:
                return "String";
            case PinType::Entity:
                return "Entity";
            case PinType::Asset:
                return "Asset";
            case PinType::Any:
                return "Any";
        }
        return "Exec";
    }

    PinType PinTypeFromString(std::string_view name) noexcept
    {
        if (name == "Exec")
            return PinType::Exec;
        if (name == "Bool")
            return PinType::Bool;
        if (name == "Int")
            return PinType::Int;
        if (name == "Float")
            return PinType::Float;
        if (name == "Vec2")
            return PinType::Vec2;
        if (name == "Vec3")
            return PinType::Vec3;
        if (name == "Vec4")
            return PinType::Vec4;
        if (name == "String")
            return PinType::String;
        if (name == "Entity")
            return PinType::Entity;
        if (name == "Asset")
            return PinType::Asset;
        if (name == "Any")
            return PinType::Any;
        return PinType::Exec;
    }

    //==============================================================================
    // PinValue
    //==============================================================================

    PinValue PinValue::MakeExec()
    {
        PinValue value;
        value.m_Type = PinType::Exec;
        return value;
    }

    PinValue PinValue::MakeBool(bool v)
    {
        PinValue value;
        value.m_Type = PinType::Bool;
        value.m_Storage = v;
        return value;
    }

    PinValue PinValue::MakeInt(i64 v)
    {
        PinValue value;
        value.m_Type = PinType::Int;
        value.m_Storage = v;
        return value;
    }

    PinValue PinValue::MakeFloat(f32 v)
    {
        PinValue value;
        value.m_Type = PinType::Float;
        value.m_Storage = v;
        return value;
    }

    PinValue PinValue::MakeVec2(const glm::vec2& v)
    {
        PinValue value;
        value.m_Type = PinType::Vec2;
        value.m_Storage = v;
        return value;
    }

    PinValue PinValue::MakeVec3(const glm::vec3& v)
    {
        PinValue value;
        value.m_Type = PinType::Vec3;
        value.m_Storage = v;
        return value;
    }

    PinValue PinValue::MakeVec4(const glm::vec4& v)
    {
        PinValue value;
        value.m_Type = PinType::Vec4;
        value.m_Storage = v;
        return value;
    }

    PinValue PinValue::MakeString(std::string v)
    {
        PinValue value;
        value.m_Type = PinType::String;
        value.m_Storage = std::move(v);
        return value;
    }

    PinValue PinValue::MakeEntity(UUID v)
    {
        PinValue value;
        value.m_Type = PinType::Entity;
        value.m_Storage = static_cast<u64>(v);
        return value;
    }

    PinValue PinValue::MakeAsset(UUID v)
    {
        PinValue value;
        value.m_Type = PinType::Asset;
        value.m_Storage = static_cast<u64>(v);
        return value;
    }

    PinValue PinValue::DefaultFor(PinType type)
    {
        switch (type)
        {
            case PinType::Bool:
                return MakeBool(false);
            case PinType::Int:
                return MakeInt(0);
            case PinType::Float:
                return MakeFloat(0.0f);
            case PinType::Vec2:
                return MakeVec2(glm::vec2(0.0f));
            case PinType::Vec3:
                return MakeVec3(glm::vec3(0.0f));
            case PinType::Vec4:
                return MakeVec4(glm::vec4(0.0f));
            case PinType::String:
                return MakeString({});
            case PinType::Entity:
                return MakeEntity(UUID(0));
            case PinType::Asset:
                return MakeAsset(UUID(0));
            case PinType::Any:
            {
                PinValue value;
                value.m_Type = PinType::Any;
                return value;
            }
            case PinType::Exec:
                break;
        }
        return MakeExec();
    }

    bool PinValue::AsBool() const noexcept
    {
        switch (m_Type)
        {
            case PinType::Bool:
                return std::get<bool>(m_Storage);
            case PinType::Int:
                return std::get<i64>(m_Storage) != 0;
            case PinType::Float:
                // Not `!= 0.0f` — a NaN would answer "true" under an equality
                // test's negation, which is the wrong side for a Branch guard.
                return std::isfinite(std::get<f32>(m_Storage)) && std::fabs(std::get<f32>(m_Storage)) > 0.0f;
            case PinType::String:
            {
                const auto& text = std::get<std::string>(m_Storage);
                return text == "true" || text == "True" || text == "1";
            }
            case PinType::Entity:
            case PinType::Asset:
                return std::get<u64>(m_Storage) != 0;
            default:
                return false;
        }
    }

    i64 PinValue::AsInt() const noexcept
    {
        switch (m_Type)
        {
            case PinType::Bool:
                return std::get<bool>(m_Storage) ? 1 : 0;
            case PinType::Int:
                return std::get<i64>(m_Storage);
            case PinType::Float:
            {
                const f32 raw = std::get<f32>(m_Storage);
                return std::isfinite(raw) ? static_cast<i64>(raw) : 0;
            }
            case PinType::String:
                return IntFromStorage(std::get<std::string>(m_Storage));
            case PinType::Entity:
            case PinType::Asset:
                return static_cast<i64>(std::get<u64>(m_Storage));
            default:
                return 0;
        }
    }

    f32 PinValue::AsFloat() const noexcept
    {
        switch (m_Type)
        {
            case PinType::Bool:
                return std::get<bool>(m_Storage) ? 1.0f : 0.0f;
            case PinType::Int:
                return static_cast<f32>(std::get<i64>(m_Storage));
            case PinType::Float:
            {
                const f32 raw = std::get<f32>(m_Storage);
                return std::isfinite(raw) ? raw : 0.0f;
            }
            case PinType::Vec2:
                return std::get<glm::vec2>(m_Storage).x;
            case PinType::Vec3:
                return std::get<glm::vec3>(m_Storage).x;
            case PinType::Vec4:
                return std::get<glm::vec4>(m_Storage).x;
            case PinType::String:
                return FloatFromStorage(std::get<std::string>(m_Storage));
            default:
                return 0.0f;
        }
    }

    glm::vec2 PinValue::AsVec2() const noexcept
    {
        switch (m_Type)
        {
            case PinType::Vec2:
                return std::get<glm::vec2>(m_Storage);
            case PinType::Vec3:
                return glm::vec2(std::get<glm::vec3>(m_Storage));
            case PinType::Vec4:
                return glm::vec2(std::get<glm::vec4>(m_Storage));
            case PinType::Bool:
            case PinType::Int:
            case PinType::Float:
                return glm::vec2(AsFloat());
            case PinType::String:
            {
                glm::vec2 out(0.0f);
                ParseFloatList(std::get<std::string>(m_Storage), &out.x, 2);
                return out;
            }
            default:
                return glm::vec2(0.0f);
        }
    }

    glm::vec3 PinValue::AsVec3() const noexcept
    {
        switch (m_Type)
        {
            case PinType::Vec2:
            {
                const glm::vec2 v = std::get<glm::vec2>(m_Storage);
                return glm::vec3(v.x, v.y, 0.0f);
            }
            case PinType::Vec3:
                return std::get<glm::vec3>(m_Storage);
            case PinType::Vec4:
                return glm::vec3(std::get<glm::vec4>(m_Storage));
            case PinType::Bool:
            case PinType::Int:
            case PinType::Float:
                return glm::vec3(AsFloat());
            case PinType::String:
            {
                glm::vec3 out(0.0f);
                ParseFloatList(std::get<std::string>(m_Storage), &out.x, 3);
                return out;
            }
            default:
                return glm::vec3(0.0f);
        }
    }

    glm::vec4 PinValue::AsVec4() const noexcept
    {
        switch (m_Type)
        {
            case PinType::Vec2:
            {
                const glm::vec2 v = std::get<glm::vec2>(m_Storage);
                return glm::vec4(v.x, v.y, 0.0f, 0.0f);
            }
            case PinType::Vec3:
            {
                const glm::vec3 v = std::get<glm::vec3>(m_Storage);
                return glm::vec4(v.x, v.y, v.z, 0.0f);
            }
            case PinType::Vec4:
                return std::get<glm::vec4>(m_Storage);
            case PinType::Bool:
            case PinType::Int:
            case PinType::Float:
                return glm::vec4(AsFloat());
            case PinType::String:
            {
                glm::vec4 out(0.0f);
                ParseFloatList(std::get<std::string>(m_Storage), &out.x, 4);
                return out;
            }
            default:
                return glm::vec4(0.0f);
        }
    }

    std::string PinValue::AsString() const
    {
        if (m_Type == PinType::String)
        {
            return std::get<std::string>(m_Storage);
        }
        if (m_Type == PinType::Bool)
        {
            return std::get<bool>(m_Storage) ? "true" : "false";
        }
        return ToStorageString();
    }

    UUID PinValue::AsEntity() const noexcept
    {
        switch (m_Type)
        {
            case PinType::Entity:
            case PinType::Asset:
                return UUID(std::get<u64>(m_Storage));
            case PinType::Int:
                return UUID(static_cast<u64>(std::get<i64>(m_Storage)));
            case PinType::String:
                return UUID(U64FromStorage(std::get<std::string>(m_Storage)));
            default:
                return UUID(0);
        }
    }

    UUID PinValue::AsAsset() const noexcept
    {
        return AsEntity();
    }

    PinValue PinValue::ConvertTo(PinType type) const
    {
        if (type == m_Type || type == PinType::Any)
        {
            return *this;
        }
        switch (type)
        {
            case PinType::Bool:
                return MakeBool(AsBool());
            case PinType::Int:
                return MakeInt(AsInt());
            case PinType::Float:
                return MakeFloat(AsFloat());
            case PinType::Vec2:
                return MakeVec2(AsVec2());
            case PinType::Vec3:
                return MakeVec3(AsVec3());
            case PinType::Vec4:
                return MakeVec4(AsVec4());
            case PinType::String:
                return MakeString(AsString());
            case PinType::Entity:
                return MakeEntity(AsEntity());
            case PinType::Asset:
                return MakeAsset(AsAsset());
            case PinType::Exec:
            case PinType::Any:
                break;
        }
        return DefaultFor(type);
    }

    std::string PinValue::ToStorageString() const
    {
        switch (m_Type)
        {
            case PinType::Bool:
                return std::get<bool>(m_Storage) ? "true" : "false";
            case PinType::Int:
                return std::to_string(std::get<i64>(m_Storage));
            case PinType::Float:
                return FloatToStorage(std::get<f32>(m_Storage));
            case PinType::Vec2:
            {
                const glm::vec2 v = std::get<glm::vec2>(m_Storage);
                return JoinFloats(&v.x, 2);
            }
            case PinType::Vec3:
            {
                const glm::vec3 v = std::get<glm::vec3>(m_Storage);
                return JoinFloats(&v.x, 3);
            }
            case PinType::Vec4:
            {
                const glm::vec4 v = std::get<glm::vec4>(m_Storage);
                return JoinFloats(&v.x, 4);
            }
            case PinType::String:
                return std::get<std::string>(m_Storage);
            case PinType::Entity:
            case PinType::Asset:
                return std::to_string(std::get<u64>(m_Storage));
            case PinType::Exec:
            case PinType::Any:
                break;
        }
        return {};
    }

    PinValue PinValue::FromStorageString(PinType type, std::string_view text)
    {
        switch (type)
        {
            case PinType::Bool:
                return MakeBool(text == "true" || text == "True" || text == "1");
            case PinType::Int:
                return MakeInt(IntFromStorage(text));
            case PinType::Float:
                return MakeFloat(FloatFromStorage(text));
            case PinType::Vec2:
            {
                glm::vec2 out(0.0f);
                ParseFloatList(text, &out.x, 2);
                return MakeVec2(out);
            }
            case PinType::Vec3:
            {
                glm::vec3 out(0.0f);
                ParseFloatList(text, &out.x, 3);
                return MakeVec3(out);
            }
            case PinType::Vec4:
            {
                glm::vec4 out(0.0f);
                ParseFloatList(text, &out.x, 4);
                return MakeVec4(out);
            }
            case PinType::String:
                return MakeString(std::string(text));
            case PinType::Entity:
                return MakeEntity(UUID(U64FromStorage(text)));
            case PinType::Asset:
                return MakeAsset(UUID(U64FromStorage(text)));
            case PinType::Exec:
            case PinType::Any:
                break;
        }
        return DefaultFor(type);
    }

    bool PinValue::IsFinite() const noexcept
    {
        switch (m_Type)
        {
            case PinType::Float:
                return std::isfinite(std::get<f32>(m_Storage));
            case PinType::Vec2:
            {
                const glm::vec2 v = std::get<glm::vec2>(m_Storage);
                return std::isfinite(v.x) && std::isfinite(v.y);
            }
            case PinType::Vec3:
            {
                const glm::vec3 v = std::get<glm::vec3>(m_Storage);
                return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
            }
            case PinType::Vec4:
            {
                const glm::vec4 v = std::get<glm::vec4>(m_Storage);
                return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) && std::isfinite(v.w);
            }
            default:
                return true;
        }
    }

    bool PinValue::SanitizeNonFinite()
    {
        if (IsFinite())
        {
            return false;
        }
        switch (m_Type)
        {
            case PinType::Float:
                m_Storage = 0.0f;
                break;
            case PinType::Vec2:
            {
                glm::vec2 v = std::get<glm::vec2>(m_Storage);
                for (glm::length_t i = 0; i < 2; ++i)
                {
                    if (!std::isfinite(v[i]))
                    {
                        v[i] = 0.0f;
                    }
                }
                m_Storage = v;
                break;
            }
            case PinType::Vec3:
            {
                glm::vec3 v = std::get<glm::vec3>(m_Storage);
                for (glm::length_t i = 0; i < 3; ++i)
                {
                    if (!std::isfinite(v[i]))
                    {
                        v[i] = 0.0f;
                    }
                }
                m_Storage = v;
                break;
            }
            case PinType::Vec4:
            {
                glm::vec4 v = std::get<glm::vec4>(m_Storage);
                for (glm::length_t i = 0; i < 4; ++i)
                {
                    if (!std::isfinite(v[i]))
                    {
                        v[i] = 0.0f;
                    }
                }
                m_Storage = v;
                break;
            }
            default:
                break;
        }
        return true;
    }

    bool PinValue::operator==(const PinValue& other) const
    {
        if (m_Type != other.m_Type)
        {
            return false;
        }
        switch (m_Type)
        {
            case PinType::Float:
                return Math::BitwiseEqual(std::get<f32>(m_Storage), std::get<f32>(other.m_Storage));
            case PinType::Vec2:
                return Math::BitwiseEqual(std::get<glm::vec2>(m_Storage), std::get<glm::vec2>(other.m_Storage));
            case PinType::Vec3:
                return Math::BitwiseEqual(std::get<glm::vec3>(m_Storage), std::get<glm::vec3>(other.m_Storage));
            case PinType::Vec4:
                return Math::BitwiseEqual(std::get<glm::vec4>(m_Storage), std::get<glm::vec4>(other.m_Storage));
            case PinType::Exec:
            case PinType::Any:
                return true;
            default:
                return m_Storage == other.m_Storage;
        }
    }

    LinkCompatibility CheckLinkCompatibility(PinType source, PinType target) noexcept
    {
        // Exec pairs with exec and nothing else. This is the single check that
        // keeps control flow and dataflow from ever crossing.
        if (IsExecPin(source) != IsExecPin(target))
        {
            return LinkCompatibility::Incompatible;
        }
        if (IsExecPin(source))
        {
            return LinkCompatibility::Exact;
        }
        if (source == target)
        {
            return LinkCompatibility::Exact;
        }
        if (source == PinType::Any || target == PinType::Any)
        {
            return LinkCompatibility::Coerced;
        }

        const auto isNumeric = [](PinType t)
        {
            return t == PinType::Bool || t == PinType::Int || t == PinType::Float;
        };
        const auto isVector = [](PinType t)
        {
            return t == PinType::Vec2 || t == PinType::Vec3 || t == PinType::Vec4;
        };

        // Anything renders as a String, so String is a universal sink (Print).
        if (target == PinType::String)
        {
            return LinkCompatibility::Coerced;
        }
        if (isNumeric(source) && isNumeric(target))
        {
            return LinkCompatibility::Coerced;
        }
        if (isVector(source) && isVector(target))
        {
            return LinkCompatibility::Coerced;
        }
        // A scalar broadcasts into a vector (the usual "* 2" convenience).
        if (isNumeric(source) && isVector(target))
        {
            return LinkCompatibility::Coerced;
        }
        // Entity and Asset are both UUIDs but mean different things; refuse the
        // cross-wire rather than silently handing an asset id to a spawn node.
        return LinkCompatibility::Incompatible;
    }

} // namespace OloEngine::VisualScript
