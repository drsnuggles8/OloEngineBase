#pragma once

#include <glm/glm.hpp>

#include <cmath>
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

    // Returns true iff every component is finite (no NaN, no ±Inf).
    //
    // Use to validate floats read from untrusted sources — scene YAML,
    // save-games, network packets — before they reach the GPU, physics, or
    // matrix math. A non-finite transform uploaded to the instance SSBO or fed
    // to `transpose(inverse(...))` yields undefined rendering / NaN normals;
    // a non-finite value handed to Jolt destabilises the simulation. This is
    // the predicate behind the cpp-coding-quality §2 rule that every float
    // crossing a serialization boundary must be `std::isfinite`-checked.
    [[nodiscard]] inline bool IsFinite(float v) noexcept
    {
        return std::isfinite(v);
    }
    
    [[nodiscard]] inline bool IsFinite(double v) noexcept
    {
        return std::isfinite(v);
    }

    [[nodiscard]] inline bool IsFinite(const glm::vec2& v) noexcept
    {
        return std::isfinite(v.x) && std::isfinite(v.y);
    }

    [[nodiscard]] inline bool IsFinite(const glm::vec3& v) noexcept
    {
        return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
    }

    [[nodiscard]] inline bool IsFinite(const glm::vec4& v) noexcept
    {
        return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) && std::isfinite(v.w);
    }

    [[nodiscard]] inline bool IsFinite(const glm::mat4& m) noexcept
    {
        for (glm::length_t c = 0; c < 4; ++c)
            for (glm::length_t r = 0; r < 4; ++r)
                if (!std::isfinite(m[c][r]))
                    return false;
        return true;
    }
} // namespace OloEngine::Math
