#pragma once

#include <glm/glm.hpp>
#include <spdlog/fmt/fmt.h>
#include <string>

// Custom formatter for glm::vec3 for spdlog/fmt
template<>
struct fmt::formatter<glm::vec3> : formatter<std::string>
{
    // Parse is inherited from formatter<std::string>

    // Format glm::vec3 using the provided context
    template<typename FormatContext>
    auto format(const glm::vec3& v, FormatContext& ctx) const
    {
        std::string output = "(" + std::to_string(v.x) + ", " +
                             std::to_string(v.y) + ", " +
                             std::to_string(v.z) + ")";
        return formatter<std::string>::format(output, ctx);
    }
};
