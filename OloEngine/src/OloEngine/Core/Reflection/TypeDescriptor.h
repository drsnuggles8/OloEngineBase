#pragma once

#include "MemberList.h"
#include "StringUtils.h"
#include <array>
#include <iostream>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>

namespace OloEngine::Core::Reflection
{

    //==============================================================================
    /// Base tag for untagged descriptions
    struct DummyTag
    {
    };

    //==============================================================================
    /// Primary template for Description (specialized by macros)
    template<typename T, typename TTag = DummyTag>
    struct Description;

    //==============================================================================
    /// Check if type has specialized description
    /// Detects Description specializations by checking for MemberListType nested type
    template<typename T, typename TTag = DummyTag, typename = void>
    struct IsDescribed : std::false_type
    {
    };

    template<typename T, typename TTag>
    struct IsDescribed<T, TTag, std::void_t<typename Description<std::remove_cvref_t<T>, TTag>::MemberListType>> : std::true_type
    {
    };

    template<typename T, typename TTag = DummyTag>
    constexpr bool IsDescribed_v = IsDescribed<T, TTag>::value;

    //==============================================================================
    /// Base interface for descriptions
    template<typename TDescription, typename TClass, typename TTag, typename TMemberList>
    struct DescriptionInterface
    {
        using ClassType = TClass;
        using TagType = TTag;
        using MemberListType = TMemberList;

        template<typename TFunc>
        static constexpr decltype(auto) ApplyToStaticType(TFunc&& func)
        {
            return TMemberList::ApplyToStaticType(std::forward<TFunc>(func));
        }
    };

    //==============================================================================
    /// Description provider - wraps MemberList with runtime string access
    template<typename TDescription, typename TObjType>
    struct DescriptionProvider
    {
      private:
        using TList = typename TDescription::MemberListType;

      public:
        static constexpr sizet s_NumberOfMembers = TList::Count();
        static constexpr sizet s_INVALID_INDEX = sizet(-1);

        //==============================================================================
        /// Member name operations

        static constexpr std::optional<std::string_view> GetMemberName(sizet memberIndex)
        {
            return (memberIndex != s_INVALID_INDEX && memberIndex < s_NumberOfMembers) ? std::optional<std::string_view>(TDescription::s_MemberNames[memberIndex]) : std::nullopt;
        }

        template<sizet MemberIndex>
        static constexpr std::string_view GetMemberName()
        {
            static_assert(s_NumberOfMembers > MemberIndex);
            return TDescription::s_MemberNames[MemberIndex];
        }

        static constexpr sizet IndexOf(std::string_view memberName)
        {
            for (sizet i = 0; i < s_NumberOfMembers; ++i)
            {
                if (TDescription::s_MemberNames[i] == memberName)
                    return i;
            }
            return s_INVALID_INDEX;
        }

        //==============================================================================
        /// Member value operations by name

        template<typename TValue>
        static constexpr std::optional<TValue> GetMemberValueByName(std::string_view memberName, const TObjType& object)
        {
            const auto index = IndexOf(memberName);
            return (index != s_INVALID_INDEX && index < s_NumberOfMembers) ? TList::template GetMemberValueOfType<TValue>(index, object) : std::nullopt;
        }

        template<typename TValue>
        static constexpr bool SetMemberValueByName(std::string_view memberName, const TValue& value, TObjType& object)
        {
            const auto index = IndexOf(memberName);
            return (index != s_INVALID_INDEX && index < s_NumberOfMembers) ? TList::template SetMemberValue<TValue>(index, value, object) : false;
        }

        //==============================================================================
        /// Type information by name

        static constexpr std::optional<bool> IsFunctionByName(std::string_view memberName)
        {
            const auto index = IndexOf(memberName);
            return (index != s_INVALID_INDEX && index < s_NumberOfMembers) ? TList::IsFunction(index) : std::nullopt;
        }

        static constexpr std::optional<sizet> GetMemberSizeByName(std::string_view memberName)
        {
            const auto index = IndexOf(memberName);
            return (index != s_INVALID_INDEX && index < s_NumberOfMembers) ? TList::GetMemberSize(index) : std::nullopt;
        }

        //==============================================================================
        /// Debug/introspection utilities

        static void PrintInfo(std::ostream& stream)
        {
            stream << "Class Name: '" << TDescription::s_ClassName << '\'' << '\n';
            stream << "Namespace: '" << TDescription::s_Namespace << '\'' << '\n';
            stream << "Number of members: " << s_NumberOfMembers << '\n';
            stream << "Members:" << '\n';
            stream << "---" << '\n';

            for (sizet i = 0; i < s_NumberOfMembers; ++i)
            {
                if (auto name = GetMemberName(i))
                    stream << *name;
                stream << " (" << TList::GetMemberSize(i).value_or(0) << " bytes)";

                if (auto isFunc = TList::IsFunction(i); isFunc && *isFunc)
                    stream << " (function)";

                stream << '\n';
            }
            stream << "---" << '\n';
        }

        static void PrintInfoWithValues(std::ostream& stream, const TObjType& obj)
        {
            stream << "Class Name: '" << TDescription::s_ClassName << '\'' << '\n';
            stream << "Namespace: '" << TDescription::s_Namespace << '\'' << '\n';
            stream << "Number of members: " << s_NumberOfMembers << '\n';
            stream << "Members:" << '\n';
            stream << "---" << '\n';

            auto unwrapOuter = [&stream, &obj](auto... members)
            {
                sizet memberCounter = 0;
                auto unwrap = [&stream, &obj, memberCounter](auto memb, sizet index) mutable
                {
                    if (auto name = GetMemberName(index))
                        stream << *name;

                    if constexpr (!std::is_member_function_pointer_v<decltype(memb)>)
                    {
                        // Derive the member type directly from the member access expression
                        if constexpr (IsStreamable_v<decltype(obj.*memb)>)
                            stream << "{ " << (obj.*memb) << " }";
                    }

                    stream << " (" << TList::GetMemberSize(index).value_or(0) << " bytes)";

                    if (auto isFunc = TList::IsFunction(index); isFunc && *isFunc)
                        stream << " (function)";

                    stream << '\n';
                };
                (unwrap(members, memberCounter++), ...);
            };

            TList::ApplyToStaticType(unwrapOuter);
            stream << "---" << '\n';
        }
    };

} // namespace OloEngine::Core::Reflection

//==============================================================================
/// MACRO SYSTEM for creating descriptions

/**
 * Create a tagged description for a class with specified member pointers
 * @param Class - The class to describe
 * @param Tag - Tag type to distinguish different descriptions of the same class
 * @param ... - Variadic list of member pointers (&Class::member1, &Class::member2, ...)
 */
#define OLO_DESCRIBE_TAGGED(Class, Tag, ...)                                                                                            \
    template<>                                                                                                                          \
    struct OloEngine::Core::Reflection::Description<Class, Tag> : OloEngine::Core::Reflection::MemberList<__VA_ARGS__>,                 \
                                                                  OloEngine::Core::Reflection::DescriptionInterface<                    \
                                                                      OloEngine::Core::Reflection::Description<Class, Tag>,             \
                                                                      Class, Tag, OloEngine::Core::Reflection::MemberList<__VA_ARGS__>> \
    {                                                                                                                                   \
        using MemberListType = OloEngine::Core::Reflection::MemberList<__VA_ARGS__>;                                                    \
                                                                                                                                        \
      private:                                                                                                                          \
        static inline const std::string_view s_MemberStr{ __VA_OPT__(#__VA_ARGS__) __VA_OPT__(, ) "" };                                 \
        static inline const std::string_view s_ClassStr{ #Class };                                                                      \
        static inline const std::string_view s_Delimiter{ "," };                                                                        \
        static constexpr sizet s_MemberCount = MemberListType::Count();                                                                 \
                                                                                                                                        \
      public:                                                                                                                           \
        static inline const std::string_view s_Namespace =                                                                              \
            OloEngine::Core::Reflection::StringUtils::ExtractNamespace(s_ClassStr);                                                     \
        static inline const std::string_view s_ClassName =                                                                              \
            OloEngine::Core::Reflection::StringUtils::ExtractClassName(s_ClassStr);                                                     \
                                                                                                                                        \
        static inline const std::array<std::string_view, s_MemberCount> s_MemberNames =                                                 \
            s_MemberCount > 0 ? OloEngine::Core::Reflection::StringUtils::CleanMemberNames<s_MemberCount>(                              \
                                    OloEngine::Core::Reflection::StringUtils::RemoveNamespace<s_MemberCount>(                           \
                                        OloEngine::Core::Reflection::StringUtils::SplitString<s_MemberCount>(                           \
                                            s_MemberStr, s_Delimiter)))                                                                 \
                              : std::array<std::string_view, s_MemberCount>{};                                                          \
    };

/**
 * Create an untagged description for a class
 * @param Class - The class to describe
 * @param ... - Variadic list of member pointers (&Class::member1, &Class::member2, ...)
 */
#define OLO_DESCRIBE(Class, ...) OLO_DESCRIBE_TAGGED(Class, OloEngine::Core::Reflection::DummyTag, __VA_ARGS__)
