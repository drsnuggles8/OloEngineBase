#include "OloEnginePCH.h"
#include "OloEngine/Core/DebugLevers.h"

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Environment.h"
#include "OloEngine/Core/Log.h"

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <string>
#include <vector>

namespace OloEngine::Levers
{
    namespace
    {
        // Sentinels. An unset Integer/Number must stay distinguishable from a
        // set-to-zero one — that distinction is the whole reason the accessors
        // return optional rather than a value with a magic default.
        constexpr i64 kUnsetInt = (std::numeric_limits<i64>::min)();
        constexpr f32 kUnsetNumber = std::numeric_limits<f32>::quiet_NaN();

        [[nodiscard]] bool IsUnsetNumber(f32 v)
        {
            return std::isnan(v);
        }

        // Storage. One member per lever, generated from the same table as the
        // accessors so the two cannot drift.
        //
        // Relaxed atomics: a lever orders no other data, and the ones read
        // per-frame (transient poison at materialize time, terrain LOD once per
        // terrain per frame) sit on paths where an acquire would be pure cost.
        // Text levers are seeded once and never written after, so they need no
        // atomic at all.
#define OLO_LEVER_TOGGLE(id, env, help) std::atomic<u8> id{ 0 };
#define OLO_LEVER_EXACT(id, env, help) std::atomic<u8> id{ 0 };
#define OLO_LEVER_TRISTATE(id, env, help) std::atomic<u8> id{ static_cast<u8>(Tristate::Unset) };
#define OLO_LEVER_INT(id, env, minValue, help) std::atomic<i64> id{ kUnsetInt };
#define OLO_LEVER_NUMBER(id, env, minValue, maxValue, help) std::atomic<f32> id{ kUnsetNumber };
#define OLO_LEVER_TEXT(id, env, help) std::optional<std::string> id;

        struct Storage
        {
#include "OloEngine/Core/DebugLevers.inl"
        };

#undef OLO_LEVER_TOGGLE
#undef OLO_LEVER_EXACT
#undef OLO_LEVER_TRISTATE
#undef OLO_LEVER_INT
#undef OLO_LEVER_NUMBER
#undef OLO_LEVER_TEXT

        // True for a lever a setter has written, so the seed leaves it alone
        // and Snapshot() can report where the value came from.
#define OLO_LEVER_TOGGLE(id, env, help) std::atomic<bool> id{ false };
#define OLO_LEVER_EXACT(id, env, help) std::atomic<bool> id{ false };
#define OLO_LEVER_TRISTATE(id, env, help) std::atomic<bool> id{ false };
#define OLO_LEVER_INT(id, env, minValue, help) std::atomic<bool> id{ false };
#define OLO_LEVER_NUMBER(id, env, minValue, maxValue, help) std::atomic<bool> id{ false };
#define OLO_LEVER_TEXT(id, env, help)

        struct Overridden
        {
#include "OloEngine/Core/DebugLevers.inl"
        };

#undef OLO_LEVER_TOGGLE
#undef OLO_LEVER_EXACT
#undef OLO_LEVER_TRISTATE
#undef OLO_LEVER_INT
#undef OLO_LEVER_NUMBER
#undef OLO_LEVER_TEXT

        // All three are constant-initialized, so there is no static-init order
        // question between them and no lever can be read before its storage
        // exists.
        Storage s_Values;
        Overridden s_Overridden;
        std::once_flag s_SeedOnce;

        // Seeding is LAZY — first access — which can be arbitrarily early,
        // possibly before Log::Initialize(). So a malformed value is recorded
        // here and reported by LogActive() once the logger is definitely up,
        // rather than logged from inside the seed.
        std::vector<std::string> s_SeedWarnings;

        // "0"/"false" off, "1"/"true" on, anything else leaves the caller's own
        // computed default alone. Deliberately NOT Env::IsTruthy: for these the
        // caller derives a default from the hardware, and a garbage value must
        // not silently replace it.
        [[nodiscard]] Tristate ReadTristate(const char* name)
        {
            const std::optional<std::string> value = Env::Get(name);
            if (!value)
            {
                return Tristate::Unset;
            }
            if (*value == "0" || *value == "false")
            {
                return Tristate::Off;
            }
            if (*value == "1" || *value == "true")
            {
                return Tristate::On;
            }
            return Tristate::Unset;
        }

        [[nodiscard]] f32 ReadNumber(const char* name, f32 minValue, f32 maxValue)
        {
            const std::optional<std::string> raw = Env::Get(name);
            if (!raw)
            {
                return kUnsetNumber;
            }
            if (const std::optional<f32> parsed = ParseNumberLever(raw->c_str(), minValue, maxValue))
            {
                return *parsed;
            }
            s_SeedWarnings.push_back(std::string(name) + "='" + *raw + "' ignored (must be finite and within [" +
                                     std::to_string(minValue) + ", " + std::to_string(maxValue) + "])");
            return kUnsetNumber;
        }

        void Seed()
        {
            std::call_once(s_SeedOnce,
                           [] {
#define OLO_LEVER_TOGGLE(id, env, help)                   \
    if (!s_Overridden.id.load(std::memory_order_relaxed)) \
        s_Values.id.store(Env::IsTruthy(env) ? u8{ 1 } : u8{ 0 }, std::memory_order_relaxed);
#define OLO_LEVER_EXACT(id, env, help)                    \
    if (!s_Overridden.id.load(std::memory_order_relaxed)) \
        s_Values.id.store(Env::IsExactly(env, "1") ? u8{ 1 } : u8{ 0 }, std::memory_order_relaxed);
#define OLO_LEVER_TRISTATE(id, env, help)                 \
    if (!s_Overridden.id.load(std::memory_order_relaxed)) \
        s_Values.id.store(static_cast<u8>(ReadTristate(env)), std::memory_order_relaxed);
#define OLO_LEVER_INT(id, env, minValue, help)                                                               \
    if (!s_Overridden.id.load(std::memory_order_relaxed))                                                    \
    {                                                                                                        \
        const std::optional<i64> parsed = Env::GetInt(env);                                                  \
        s_Values.id.store(parsed && *parsed >= (minValue) ? *parsed : kUnsetInt, std::memory_order_relaxed); \
    }
#define OLO_LEVER_NUMBER(id, env, minValue, maxValue, help) \
    if (!s_Overridden.id.load(std::memory_order_relaxed))   \
        s_Values.id.store(ReadNumber(env, (minValue), (maxValue)), std::memory_order_relaxed);
#define OLO_LEVER_TEXT(id, env, help) s_Values.id = Env::Get(env);

#include "OloEngine/Core/DebugLevers.inl"

#undef OLO_LEVER_TOGGLE
#undef OLO_LEVER_EXACT
#undef OLO_LEVER_TRISTATE
#undef OLO_LEVER_INT
#undef OLO_LEVER_NUMBER
#undef OLO_LEVER_TEXT
                           });
        }
    } // namespace

// --- Accessors and setters --------------------------------------------------
// A setter marks the lever overridden BEFORE seeding, then writes. That order
// is load-bearing: a setter called before anything has read the lever must
// survive the lazy seed a later first read triggers, or the environment would
// quietly win and the caller would never find out.
#define OLO_LEVER_TOGGLE(id, env, help)                                          \
    bool id()                                                                    \
    {                                                                            \
        Seed();                                                                  \
        return s_Values.id.load(std::memory_order_relaxed) != 0;                 \
    }                                                                            \
    void Set##id(bool value)                                                     \
    {                                                                            \
        s_Overridden.id.store(true, std::memory_order_relaxed);                  \
        Seed();                                                                  \
        s_Values.id.store(value ? u8{ 1 } : u8{ 0 }, std::memory_order_relaxed); \
    }
#define OLO_LEVER_EXACT(id, env, help) OLO_LEVER_TOGGLE(id, env, help)
#define OLO_LEVER_TRISTATE(id, env, help)                                          \
    Tristate id()                                                                  \
    {                                                                              \
        Seed();                                                                    \
        return static_cast<Tristate>(s_Values.id.load(std::memory_order_relaxed)); \
    }                                                                              \
    void Set##id(Tristate value)                                                   \
    {                                                                              \
        s_Overridden.id.store(true, std::memory_order_relaxed);                    \
        Seed();                                                                    \
        s_Values.id.store(static_cast<u8>(value), std::memory_order_relaxed);      \
    }
#define OLO_LEVER_INT(id, env, minValue, help)                                    \
    std::optional<i64> id()                                                       \
    {                                                                             \
        Seed();                                                                   \
        const i64 v = s_Values.id.load(std::memory_order_relaxed);                \
        return v == kUnsetInt ? std::nullopt : std::optional<i64>{ v };           \
    }                                                                             \
    void Set##id(std::optional<i64> value)                                        \
    {                                                                             \
        s_Overridden.id.store(true, std::memory_order_relaxed);                   \
        Seed();                                                                   \
        s_Values.id.store(value ? *value : kUnsetInt, std::memory_order_relaxed); \
    }
#define OLO_LEVER_NUMBER(id, env, minValue, maxValue, help)                          \
    std::optional<f32> id()                                                          \
    {                                                                                \
        Seed();                                                                      \
        const f32 v = s_Values.id.load(std::memory_order_relaxed);                   \
        return IsUnsetNumber(v) ? std::nullopt : std::optional<f32>{ v };            \
    }                                                                                \
    void Set##id(std::optional<f32> value)                                           \
    {                                                                                \
        s_Overridden.id.store(true, std::memory_order_relaxed);                      \
        Seed();                                                                      \
        s_Values.id.store(value ? *value : kUnsetNumber, std::memory_order_relaxed); \
    }
#define OLO_LEVER_TEXT(id, env, help) \
    std::optional<std::string> id()   \
    {                                 \
        Seed();                       \
        return s_Values.id;           \
    }

#include "OloEngine/Core/DebugLevers.inl"

#undef OLO_LEVER_TOGGLE
#undef OLO_LEVER_EXACT
#undef OLO_LEVER_TRISTATE
#undef OLO_LEVER_INT
#undef OLO_LEVER_NUMBER
#undef OLO_LEVER_TEXT

    std::optional<f32> ParseNumberLever(const char* raw, f32 minValue, f32 maxValue)
    {
        if (raw == nullptr)
        {
            return std::nullopt;
        }
        // strtof, not atof: atof has no error reporting and is undefined on
        // overflow, whereas strtof reports "not a number at all" via endPtr and
        // overflow via ±HUGE_VALF (== ±inf). "inf"/"nan" likewise parse to
        // non-finite values, so the isfinite guard catches all three.
        char* end = nullptr;
        const f32 value = std::strtof(raw, &end);
        // end == raw means nothing parsed; *end != '\0' means trailing garbage
        // ("2.0abc", "2.0 "), which must be rejected rather than truncated.
        if (end == raw || *end != '\0')
        {
            return std::nullopt;
        }
        if (!std::isfinite(value) || value < minValue || value > maxValue)
        {
            return std::nullopt;
        }
        return value;
    }

    std::vector<LeverInfo> Snapshot()
    {
        Seed();
        std::vector<LeverInfo> out;

#define OLO_LEVER_BOOLROW(id, env, help, kind)                                        \
    {                                                                                 \
        const bool on = s_Values.id.load(std::memory_order_relaxed) != 0;             \
        out.push_back(LeverInfo{ env, help, kind, on ? "on" : "off", !on,             \
                                 !s_Overridden.id.load(std::memory_order_relaxed) }); \
    }
#define OLO_LEVER_TOGGLE(id, env, help) OLO_LEVER_BOOLROW(id, env, help, LeverKind::Toggle)
#define OLO_LEVER_EXACT(id, env, help) OLO_LEVER_BOOLROW(id, env, help, LeverKind::Exact)
#define OLO_LEVER_TRISTATE(id, env, help)                                                                   \
    {                                                                                                       \
        const auto t = static_cast<Tristate>(s_Values.id.load(std::memory_order_relaxed));                  \
        out.push_back(LeverInfo{ env, help, LeverKind::Tristate,                                            \
                                 t == Tristate::Unset ? "unset" : (t == Tristate::On ? "on" : "off"),       \
                                 t == Tristate::Unset, !s_Overridden.id.load(std::memory_order_relaxed) }); \
    }
#define OLO_LEVER_INT(id, env, minValue, help)                                                              \
    {                                                                                                       \
        const i64 v = s_Values.id.load(std::memory_order_relaxed);                                          \
        out.push_back(LeverInfo{ env, help, LeverKind::Integer,                                             \
                                 v == kUnsetInt ? std::string("unset") : std::to_string(v), v == kUnsetInt, \
                                 !s_Overridden.id.load(std::memory_order_relaxed) });                       \
    }
#define OLO_LEVER_NUMBER(id, env, minValue, maxValue, help)                                                     \
    {                                                                                                           \
        const f32 v = s_Values.id.load(std::memory_order_relaxed);                                              \
        out.push_back(LeverInfo{ env, help, LeverKind::Number,                                                  \
                                 IsUnsetNumber(v) ? std::string("unset") : std::to_string(v), IsUnsetNumber(v), \
                                 !s_Overridden.id.load(std::memory_order_relaxed) });                           \
    }
#define OLO_LEVER_TEXT(id, env, help)                                                                             \
    out.push_back(LeverInfo{ env, help, LeverKind::Text, s_Values.id.value_or("unset"), !s_Values.id.has_value(), \
                             true });

#include "OloEngine/Core/DebugLevers.inl"

#undef OLO_LEVER_TOGGLE
#undef OLO_LEVER_EXACT
#undef OLO_LEVER_TRISTATE
#undef OLO_LEVER_INT
#undef OLO_LEVER_NUMBER
#undef OLO_LEVER_TEXT
#undef OLO_LEVER_BOOLROW

        return out;
    }

    std::string ActiveSummary()
    {
        std::string summary;
        for (const LeverInfo& lever : Snapshot())
        {
            if (lever.IsDefault)
            {
                continue;
            }
            if (!summary.empty())
            {
                summary += ", ";
            }
            summary += lever.Name;
            summary += '=';
            summary += lever.Value;
            if (!lever.FromEnvironment)
            {
                summary += " (set in code)";
            }
        }
        return summary;
    }

    void LogActive()
    {
        Seed();
        // Deferred from the seed, which can run before the logger exists.
        for (const std::string& warning : s_SeedWarnings)
        {
            OLO_CORE_WARN("[Levers] {}", warning);
        }
        s_SeedWarnings.clear();

        if (const std::string summary = ActiveSummary(); !summary.empty())
        {
            OLO_CORE_INFO("[Levers] active: {}", summary);
        }
    }
} // namespace OloEngine::Levers
