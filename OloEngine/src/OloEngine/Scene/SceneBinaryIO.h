#pragma once

// ============================================================================
// SceneBinaryIO — low-level typed binary I/O for the `.scenebin` sidecar cache
// (issue #525). Writers stream to a std::ostream; readers pull from an in-memory
// Reader (the whole sidecar is slurped into a buffer once, so reads are bounded
// memcpys and a corrupt/truncated file is caught by the bounds check rather than
// desyncing).
//
// These overloads are the leaf primitives the OloHeaderTool-generated per-component
// binary blocks (Scene/Generated/SceneBinary{Write,Read}Components.Generated.inl)
// call — one Write/Read pair per serializable scalar / string / glm / enum /
// AssetHandle type. Container / nested-struct / Ref framing is emitted inline by
// the generator (see EmitBinaryWriteFields / EmitBinaryReadFields in
// tools/OloHeaderTool/main.cpp), which bottoms out in these calls.
//
// All values are little-endian (the engine targets LE, like MeshBinaryFormat.h).
// Floats (and glm float types) are validated for finiteness on read: a NaN/Inf in
// the cache means corruption, so Read returns false and the whole fast-path load
// is abandoned in favour of the YAML source of truth.
// ============================================================================

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/UUID.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <cmath>
#include <cstring>
#include <ostream>
#include <string>
#include <string_view>
#include <type_traits>

namespace OloEngine::SceneBinIO
{
    // Upper bound on a container element count / string length read from the
    // cache, so a corrupt count can't trigger a huge allocation before the
    // bounds check would catch the short read.
    constexpr u32 MaxContainerElements = 100'000'000u;
    constexpr u32 MaxStringLength = 64u * 1024u * 1024u;

    // Stable on-disk ComponentId: FNV-1a-32 of the component type name. Used to
    // tag each binary component block. MUST stay bit-identical to
    // SceneBinComponentId in tools/OloHeaderTool/main.cpp (same seed / prime) so
    // the hand-written TransformComponent block and the generated blocks agree.
    constexpr u32 ComponentId(std::string_view name)
    {
        u32 hash = 2166136261u;
        for (char c : name)
        {
            hash ^= static_cast<u8>(c);
            hash *= 16777619u;
        }
        return hash;
    }

    // Non-owning cursor over the fully-buffered sidecar. Raw() is the single
    // bounds-checked primitive every Read bottoms out in.
    struct Reader
    {
        const u8* Data = nullptr;
        sizet Size = 0;
        sizet Cursor = 0;

        [[nodiscard]] bool Raw(void* dst, sizet n)
        {
            if (n > Size - Cursor) // Cursor <= Size is an invariant (Raw only advances on success)
            {
                return false;
            }
            std::memcpy(dst, Data + Cursor, n);
            Cursor += n;
            return true;
        }
    };

    // ── Fixed-width scalar helpers (used directly for counts / ids / handles) ──
    inline void WriteU32(std::ostream& out, u32 v)
    {
        out.write(reinterpret_cast<const char*>(&v), sizeof(v));
    }
    inline void WriteU64(std::ostream& out, u64 v)
    {
        out.write(reinterpret_cast<const char*>(&v), sizeof(v));
    }
    [[nodiscard]] inline bool ReadU32(Reader& r, u32& v)
    {
        return r.Raw(&v, sizeof(v));
    }
    [[nodiscard]] inline bool ReadU64(Reader& r, u64& v)
    {
        return r.Raw(&v, sizeof(v));
    }

    // ── Typed leaf Write/Read overloads (what the generated blocks call) ──

    // bool → 1 byte.
    inline void Write(std::ostream& out, bool v)
    {
        const u8 b = v ? 1u : 0u;
        out.write(reinterpret_cast<const char*>(&b), 1);
    }
    [[nodiscard]] inline bool Read(Reader& r, bool& v)
    {
        u8 b = 0;
        if (!r.Raw(&b, 1))
        {
            return false;
        }
        v = (b != 0);
        return true;
    }

    // Integer types (exact width, raw LE bytes).
#define OLO_SCENEBIN_INT(T)                                      \
    inline void Write(std::ostream& out, T v)                    \
    {                                                            \
        out.write(reinterpret_cast<const char*>(&v), sizeof(v)); \
    }                                                            \
    [[nodiscard]] inline bool Read(Reader& r, T& v)              \
    {                                                            \
        return r.Raw(&v, sizeof(v));                             \
    }
    OLO_SCENEBIN_INT(u8)
    OLO_SCENEBIN_INT(i8)
    OLO_SCENEBIN_INT(u16)
    OLO_SCENEBIN_INT(i16)
    OLO_SCENEBIN_INT(u32)
    OLO_SCENEBIN_INT(i32)
    OLO_SCENEBIN_INT(u64)
    OLO_SCENEBIN_INT(i64)
#undef OLO_SCENEBIN_INT

    // Floating point — finiteness-validated on read.
    inline void Write(std::ostream& out, f32 v)
    {
        out.write(reinterpret_cast<const char*>(&v), sizeof(v));
    }
    [[nodiscard]] inline bool Read(Reader& r, f32& v)
    {
        return r.Raw(&v, sizeof(v)) && std::isfinite(v);
    }
    inline void Write(std::ostream& out, f64 v)
    {
        out.write(reinterpret_cast<const char*>(&v), sizeof(v));
    }
    [[nodiscard]] inline bool Read(Reader& r, f64& v)
    {
        return r.Raw(&v, sizeof(v)) && std::isfinite(v);
    }

    // glm float vectors / quat / matrices — raw bytes, every component finite.
    namespace detail
    {
        template<typename T>
        [[nodiscard]] inline bool ReadFloatBlock(Reader& r, T& v, int count)
        {
            if (!r.Raw(glm::value_ptr(v), sizeof(f32) * static_cast<sizet>(count)))
            {
                return false;
            }
            const f32* p = glm::value_ptr(v);
            for (int i = 0; i < count; ++i)
            {
                if (!std::isfinite(p[i]))
                {
                    return false;
                }
            }
            return true;
        }
    } // namespace detail

    inline void Write(std::ostream& out, const glm::vec2& v)
    {
        out.write(reinterpret_cast<const char*>(glm::value_ptr(v)), sizeof(f32) * 2);
    }
    inline void Write(std::ostream& out, const glm::vec3& v)
    {
        out.write(reinterpret_cast<const char*>(glm::value_ptr(v)), sizeof(f32) * 3);
    }
    inline void Write(std::ostream& out, const glm::vec4& v)
    {
        out.write(reinterpret_cast<const char*>(glm::value_ptr(v)), sizeof(f32) * 4);
    }
    inline void Write(std::ostream& out, const glm::quat& v)
    {
        out.write(reinterpret_cast<const char*>(glm::value_ptr(v)), sizeof(f32) * 4);
    }
    inline void Write(std::ostream& out, const glm::mat3& v)
    {
        out.write(reinterpret_cast<const char*>(glm::value_ptr(v)), sizeof(f32) * 9);
    }
    inline void Write(std::ostream& out, const glm::mat4& v)
    {
        out.write(reinterpret_cast<const char*>(glm::value_ptr(v)), sizeof(f32) * 16);
    }
    [[nodiscard]] inline bool Read(Reader& r, glm::vec2& v)
    {
        return detail::ReadFloatBlock(r, v, 2);
    }
    [[nodiscard]] inline bool Read(Reader& r, glm::vec3& v)
    {
        return detail::ReadFloatBlock(r, v, 3);
    }
    [[nodiscard]] inline bool Read(Reader& r, glm::vec4& v)
    {
        return detail::ReadFloatBlock(r, v, 4);
    }
    [[nodiscard]] inline bool Read(Reader& r, glm::quat& v)
    {
        return detail::ReadFloatBlock(r, v, 4);
    }
    [[nodiscard]] inline bool Read(Reader& r, glm::mat3& v)
    {
        return detail::ReadFloatBlock(r, v, 9);
    }
    [[nodiscard]] inline bool Read(Reader& r, glm::mat4& v)
    {
        return detail::ReadFloatBlock(r, v, 16);
    }

    // glm integer vectors — raw bytes, no finiteness check.
    inline void Write(std::ostream& out, const glm::ivec2& v)
    {
        out.write(reinterpret_cast<const char*>(glm::value_ptr(v)), sizeof(i32) * 2);
    }
    inline void Write(std::ostream& out, const glm::ivec3& v)
    {
        out.write(reinterpret_cast<const char*>(glm::value_ptr(v)), sizeof(i32) * 3);
    }
    inline void Write(std::ostream& out, const glm::ivec4& v)
    {
        out.write(reinterpret_cast<const char*>(glm::value_ptr(v)), sizeof(i32) * 4);
    }
    [[nodiscard]] inline bool Read(Reader& r, glm::ivec2& v)
    {
        return r.Raw(glm::value_ptr(v), sizeof(i32) * 2);
    }
    [[nodiscard]] inline bool Read(Reader& r, glm::ivec3& v)
    {
        return r.Raw(glm::value_ptr(v), sizeof(i32) * 3);
    }
    [[nodiscard]] inline bool Read(Reader& r, glm::ivec4& v)
    {
        return r.Raw(glm::value_ptr(v), sizeof(i32) * 4);
    }

    // std::string — u32 length + bytes.
    inline void Write(std::ostream& out, const std::string& v)
    {
        const auto len = static_cast<u32>(v.size());
        WriteU32(out, len);
        if (len > 0)
        {
            out.write(v.data(), static_cast<std::streamsize>(len));
        }
    }
    [[nodiscard]] inline bool Read(Reader& r, std::string& v)
    {
        u32 len = 0;
        if (!ReadU32(r, len) || len > MaxStringLength)
        {
            return false;
        }
        v.assign(len, '\0');
        return len == 0 || r.Raw(v.data(), len);
    }

    // UUID / AssetHandle (AssetHandle = UUID) — round-trips as a u64.
    inline void Write(std::ostream& out, const UUID& v)
    {
        WriteU64(out, static_cast<u64>(v));
    }
    [[nodiscard]] inline bool Read(Reader& r, UUID& v)
    {
        u64 raw = 0;
        if (!ReadU64(r, raw))
        {
            return false;
        }
        v = UUID(raw);
        return true;
    }

    // Enums — round-trip as their value widened to i32 (matches the scene YAML
    // serializer's int treatment). Constrained so it never shadows the exact
    // integer / bool overloads above.
    template<typename E>
        requires std::is_enum_v<E>
    inline void Write(std::ostream& out, E v)
    {
        Write(out, static_cast<i32>(v));
    }
    template<typename E>
        requires std::is_enum_v<E>
    [[nodiscard]] inline bool Read(Reader& r, E& v)
    {
        i32 raw = 0;
        if (!Read(r, raw))
        {
            return false;
        }
        v = static_cast<E>(raw);
        return true;
    }
} // namespace OloEngine::SceneBinIO
