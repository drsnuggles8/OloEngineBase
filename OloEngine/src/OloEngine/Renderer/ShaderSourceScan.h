#pragma once

// =============================================================================
// ShaderSourceScan.h — the two questions a backend asks of GLSL TEXT, in one
// place because both backends now ask them.
//
// A shader's compile route is decided from its source before anything compiles
// it, and each backend decides its own: the GL backend asks whether the source
// spells `OLO_BINDLESS` (the raw-GLSL route, ADR 0011 amendments (19)/(24)), and
// the Vulkan backend asks whether it spells `OLO_MATERIAL_VULKAN_HEAP_READER`
// (the material heap arm, amendment (96)). Both questions are "does this token
// occur as a whole identifier outside every comment", and answering them with
// two copies of the scanner is how the two backends would come to disagree about
// what a shader is — the failure this file exists to prevent.
//
// GLSL has no raw strings and no character literals, so comments are the only
// span that has to be skipped.
// =============================================================================

#include "OloEngine/Core/Base.h"

#include <algorithm>
#include <string_view>

namespace OloEngine::ShaderSourceScan
{
    // GLSL identifiers are ASCII by definition, so classify them directly rather
    // than through <cctype>. Three reasons, all of which SonarCloud flags
    // separately on the same expression: std::isalnum returns an INT, so using it
    // as a `&&`/`||` operand is a bool-conversion bug (S867); it is
    // locale-sensitive, so a build under a non-C locale can classify differently
    // (M23_404); and it needs an unsigned-char cast at every call site to avoid UB
    // on negative values (S810). A four-line predicate has none of those
    // properties and says what it means.
    [[nodiscard]] constexpr bool IsIdentifierChar(char c) noexcept
    {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
    }

    [[nodiscard]] constexpr bool IsAsciiSpace(char c) noexcept
    {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f';
    }

    // True when `token` occurs in `source` as a WHOLE IDENTIFIER outside every
    // // and /* */ comment.
    //
    // THE WHOLE-IDENTIFIER REQUIREMENT IS LOAD-BEARING, not tidiness.
    // include/BindlessHeap.glsl guards itself with `#ifndef
    // OLO_BINDLESS_HEAP_GLSL`, and a plain substring search for "OLO_BINDLESS"
    // matches that guard — every shader that merely INCLUDES the header would
    // then take the raw-GLSL route whether or not it converted anything, and
    // taking the route is what makes the heap seam stop issuing real binds (§5c).
    //
    // THE COMMENT SKIP IS EQUALLY LOAD-BEARING, and amendment (96) is why it now
    // matters on both backends: a converted material shader explains its Vulkan
    // arm in prose that names the OTHER arm's token, so a scan that read comments
    // would put a Vulkan-converted shader on the GL bindless route and vice versa.
    [[nodiscard]] constexpr bool MentionsOutsideComments(std::string_view source, std::string_view token) noexcept
    {
        for (sizet i = 0; i < source.size();)
        {
            if (source.compare(i, 2, "//") == 0)
            {
                i = std::min(source.find('\n', i), source.size());
                continue;
            }
            if (source.compare(i, 2, "/*") == 0)
            {
                const sizet end = source.find("*/", i + 2u);
                i = (end == std::string_view::npos) ? source.size() : end + 2u;
                continue;
            }
            if (source.compare(i, token.size(), token) == 0)
            {
                const bool leftOk = (i == 0) || !IsIdentifierChar(source[i - 1u]);
                const sizet after = i + token.size();
                const bool rightOk = (after >= source.size()) || !IsIdentifierChar(source[after]);
                if (leftOk && rightOk)
                {
                    return true;
                }
            }
            ++i;
        }
        return false;
    }

    // The token a material shader defines to take the Vulkan heap arm (ADR 0011
    // amendment (96)). Named here rather than spelled at each site because THREE
    // places must agree on it and they live in different layers: the shader
    // defines it, PBRCommon.glsl's OLO_MAT_* wrappers switch on it, and
    // VulkanShader scans for it to decide that the program reads per-material
    // offsets — which is what makes CommandDispatch skip the five material binds.
    // A typo in any one of them is a silently wrong image, not a build error.
    inline constexpr std::string_view kVulkanMaterialHeapReaderToken = "OLO_MATERIAL_VULKAN_HEAP_READER";
} // namespace OloEngine::ShaderSourceScan
