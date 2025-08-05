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
struct fmt::formatter<OloEngine::UUID> : formatter<uint64_t> 
{
    // Parse is inherited from formatter<uint64_t>
    
    // Format UUID using the provided context
    template<typename FormatContext>
    auto format(const OloEngine::UUID& uuid, FormatContext& ctx) const 
    {
        return formatter<uint64_t>::format(static_cast<uint64_t>(uuid), ctx);
    }
};
