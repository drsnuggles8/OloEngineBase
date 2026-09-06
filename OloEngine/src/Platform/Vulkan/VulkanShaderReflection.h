#pragma once

#include "OloEngine/Core/Base.h"

#if OLO_WITH_VULKAN

// =============================================================================
// VulkanShaderReflection.h — the reflection helpers BOTH reflection sites share.
//
// There are two of them — VulkanShader::ReflectStage (graphics) and
// VulkanComputeShader's own `append` — and they are the classic two-mirrors
// shape: the compute one was written by copying the graphics one, and every
// field added to one since has had to be remembered in the other. #1078 was
// found because the array length was added to only one of them, which is
// exactly the drift this header exists to stop. A field that decides how a
// binding is MAPPED belongs here, called from both.
// =============================================================================

#include <spirv_cross/spirv_cross.hpp>

#include <algorithm>
#include <format>
#include <stdexcept>

namespace OloEngine::VulkanReflection
{
    // The ceiling on a declared array length this backend will map. The root
    // struct carries one u32 image index (and, for a combined sampler, one u32
    // sampler index) per element, so a declaration of N costs 8N bytes of root
    // data — and an unchecked N overflows the u32 offset arithmetic in
    // VulkanRootDataLayout::Build long before it runs out of heap. 256 is far
    // above the only real tenant (Renderer2D_Quad's 32) and far below trouble.
    inline constexpr u32 kMaxBindingArrayCount = 256u;

    // The declared array length of a binding, as the root-data layout needs it
    // (#1078). 1 for a scalar declaration.
    //
    // THROWS on an array shape this backend cannot map, and the shader build
    // FAILS. Returning 1 and carrying on was the first attempt and it is wrong:
    // a count of 1 selects the SCALAR mapping, which resolves array element i
    // by heap-slot ADJACENCY — so the shader would load, render, and sample an
    // unrelated descriptor for every element past [0]. That is #1078 itself,
    // silently reintroduced for exactly the declarations we could not read. A
    // shader that cannot be mapped correctly must not run.
    //
    // Both callers already restore their previous committed state and destroy
    // the modules they built, so the throw is the sanctioned failure path
    // rather than a new one.
    //
    //   * a runtime-sized (unsized) array — descriptor indexing, which this
    //     backend does not implement;
    //   * a spec-constant-sized array — spirv-cross reports the constant's ID
    //     in `array`, NOT a length, and `array_size_literal[i]` is how you tell;
    //   * a length past kMaxBindingArrayCount.
    [[nodiscard]] inline u32 ReflectBindingArrayCount(const spirv_cross::Compiler& compiler,
                                                      const spirv_cross::Resource& resource,
                                                      const char* shaderName)
    {
        const spirv_cross::SPIRType& type = compiler.get_type(resource.type_id);
        if (type.array.empty())
        {
            return 1u;
        }

        u64 count = 1u;
        for (sizet dimension = 0; dimension < type.array.size(); ++dimension)
        {
            const bool isLiteral =
                dimension < type.array_size_literal.size() && type.array_size_literal[dimension];
            const u32 extent = type.array[dimension];
            if (!isLiteral || extent == 0u)
            {
                throw std::runtime_error(
                    std::format("'{}' binding '{}' declares a {} array; this backend maps only literal-sized "
                                "arrays. Give it a literal size.",
                                shaderName, resource.name,
                                extent == 0u ? "runtime-sized" : "specialization-constant-sized"));
            }
            count *= extent;
        }

        if (count > kMaxBindingArrayCount)
        {
            throw std::runtime_error(std::format(
                "'{}' binding '{}' declares an array of {}; this backend maps at most {}. Raise "
                "kMaxBindingArrayCount if this is legitimate.",
                shaderName, resource.name, count, kMaxBindingArrayCount));
        }
        return static_cast<u32>(count);
    }
} // namespace OloEngine::VulkanReflection

#endif // OLO_WITH_VULKAN
