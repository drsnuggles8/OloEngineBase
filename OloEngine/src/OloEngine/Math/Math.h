#pragma once

#include <glm/glm.hpp>

#include <cstring>
#include <type_traits>

namespace OloEngine::Math
{
    bool DecomposeTransform(const glm::mat4& transform, glm::vec3& translation, glm::vec3& rotation, glm::vec3& scale);

    // Bit-exact comparison of two trivially-copyable values.
    //
    // Use for change detection (undo/redo, component equality, sentinel checks)
    // where every bit pattern must match — including NaN == NaN. Operator `==`
    // on float / glm::vec / glm::mat is forbidden by cpp-coding-quality §2a;
    // this is the canonical replacement for the bit-exact case (epsilon
    // comparison is for tolerance-based predicates).
    //
    // Equivalent to `std::memcmp(&a, &b, sizeof(T)) == 0` but documents intent
    // and prevents accidental size/type mismatches.
    template<typename T>
    [[nodiscard]] inline bool BitwiseEqual(const T& a, const T& b) noexcept
    {
        static_assert(std::is_trivially_copyable_v<T>,
                      "Math::BitwiseEqual requires a trivially-copyable type");
        return std::memcmp(&a, &b, sizeof(T)) == 0;
    }
} // namespace OloEngine::Math
