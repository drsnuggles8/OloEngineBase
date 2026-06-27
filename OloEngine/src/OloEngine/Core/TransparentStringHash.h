#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>

namespace OloEngine
{
    // Transparent hash/equal functors for heterogeneous lookup on associative
    // containers keyed by std::string. Pairing these (plus std::equal_to<>) with
    // an unordered container lets find/count/contains/erase accept std::string_view
    // and const char* without materialising a temporary std::string (SonarQube
    // cpp:S6045). The is_transparent marker is what enables the heterogeneous
    // overloads.
    struct StringHash
    {
        using is_transparent = void;
        [[nodiscard]] std::size_t operator()(std::string_view sv) const
        {
            return std::hash<std::string_view>{}(sv);
        }
    };

    struct StringEqual
    {
        using is_transparent = void;
        [[nodiscard]] bool operator()(std::string_view lhs, std::string_view rhs) const
        {
            return lhs == rhs;
        }
    };
} // namespace OloEngine
