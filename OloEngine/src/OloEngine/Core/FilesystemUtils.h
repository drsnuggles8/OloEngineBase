#pragma once

#include <filesystem>
#include <spdlog/fmt/fmt.h>
#include "OloEngine/Core/UUID.h"

// Custom formatter for std::filesystem::path for spdlog/fmt
template<>
struct fmt::formatter<std::filesystem::path> : formatter<std::string> 
{
    // Parse is inherited from formatter<std::string>
    
    // Format std::filesystem::path using the provided context
    template<typename FormatContext>
    auto format(const std::filesystem::path& p, FormatContext& ctx) const 
    {
        return formatter<std::string>::format(p.string(), ctx);
    }
};

// Custom formatter for OloEngine::UUID for spdlog/fmt
template<>
struct fmt::formatter<OloEngine::UUID> : formatter<u64> 
{
    // Parse is inherited from formatter<u64>
    
    // Format UUID using the provided context
    template<typename FormatContext>
    auto format(const OloEngine::UUID& uuid, FormatContext& ctx) const 
    {
        return formatter<u64>::format(static_cast<u64>(uuid), ctx);
    }
};
