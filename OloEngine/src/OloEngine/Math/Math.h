#pragma once

#include "OloEngine/Core/Base.h"

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
    //
    // Padding rule (issue #1019): the memcmp covers every byte of sizeof(T),
    // padding included, and padding is unspecified after member stores — a
    // constructor never writes it and an optimiser may re-materialise it
    // (seen on GCC 14 -O3). So a type compared as a WHOLE object here must
    // have no padding bytes: order members so alignment leaves no hole and
    // name any remaining tail bytes as explicit, always-initialised
    // `OLO_SERIALIZE(Skip) u8 Pad0 = 0;` members. Scalars and glm types are
    // padding-free already. The static_assert below cannot enforce this
    // (`std::has_unique_object_representations_v` is false for any type that
    // holds a float); OloEngine/tests/BitwiseEqualLayoutTest.cpp is the
    // mechanism — it lists every whole-object caller and fails on a padded
    // layout. Add a new `BitwiseEqual(*this, other)` type to that list.
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

    // Euclidean modulo — `%` in C++ (and GLSL) truncates toward zero, which
    // maps a negative value to a NEGATIVE result. Toroidal/wraparound
    // indexing (a fixed-size ring window, a cascade lattice) needs the
    // mathematical convention instead: the result is always in [0, modulus)
    // for any signed input. Shared by DDGI's cascade addressing
    // (Renderer/DDGI/DDGICommon.h — see DDGI::WrapIndex) and the terrain 3D
    // ring-buffer chunk window (Terrain/ChunkRingBuffer3D.h): both are the
    // same footgun (a negative storage index) on the same underlying scheme
    // — see docs/agent-rules/ddgi-probe-cascades-and-sparsity.md §2.
    [[nodiscard("the wrapped index is the only effect")]] inline i32 WrapIndex(i32 value, i32 modulus) noexcept
    {
        const i32 m = glm::max(modulus, 1);
        const i32 r = value % m;
        return (r < 0) ? (r + m) : r;
    }
} // namespace OloEngine::Math
