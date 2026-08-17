#pragma once

#include "OloEngine/Core/Base.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace OloEngine::Levers
{
    // @brief The engine's debug/diagnostic levers, in one enumerable place.
    //
    // These are the switches you flip for ONE run of an already-built binary:
    // poison the transient pool, force the CPU terrain path, run the gameplay
    // schedule on one thread. The environment is the right *input* for that —
    // you are often attaching to something a script launched — so each lever
    // still seeds from its variable. What was missing is everything around it:
    //
    //   * **No list.** Twenty-one independent `Env::IsTruthy(...)` reads
    //     scattered across twelve TUs. "Which levers exist?" had no answer
    //     short of grepping, and "which are on right now?" had none at all.
    //   * **Not addressable from code.** A test or tool that wanted one on had
    //     to write the environment — which is exactly the `_putenv_s` the test
    //     harness carried until the flags change removed it.
    //
    // So: one table (`DebugLevers.inl`), accessors generated from it, a setter
    // for everything except the read-once text levers, and `Snapshot()` for the
    // startup log and the MCP tool. Add a lever to the .inl and nowhere else.
    //
    // **This is not a console-variable system.** There is no name-based lookup,
    // no editor console, no persistence, and a change does not propagate to a
    // subsystem that has already read its value. It is the registry a CVar
    // system would need underneath it, delivered on its own — issue #821 is
    // the layer above, and the reason a setter here does not always take
    // effect is precisely the change notification it would add.
    //
    // **Values are seeded once**, on first access, from the environment. A
    // setter overrides the seed whether or not it has happened yet, but a
    // subsystem that cached the value at init will not notice — see each
    // lever's consumer.

    enum class Tristate : u8
    {
        Unset, // leave the caller's own default alone
        Off,
        On
    };

    enum class LeverKind : u8
    {
        Toggle,   // lenient truthiness
        Exact,    // only the exact value "1"
        Tristate, // unset leaves a computed default alone
        Integer,
        Number,
        Text // read-only, no setter
    };

    // One row of `Snapshot()`. `Value` is rendered for display; `IsDefault`
    // says whether the lever is doing nothing, which is what the startup log
    // filters on.
    struct LeverInfo
    {
        std::string_view Name; // the environment variable
        std::string_view Help;
        LeverKind Kind = LeverKind::Toggle;
        std::string Value;
        bool IsDefault = true;
        bool FromEnvironment = false; // false once a setter has overridden it
    };

    // Every lever and its current value, in table order. A snapshot by value
    // rather than a span, because the values are mutable.
    [[nodiscard]] std::vector<LeverInfo> Snapshot();

    // The OLO_LEVER_NUMBER parse, exposed because it is the interesting
    // boundary and the only part of the registry testable without a second
    // process: nullptr, empty, trailing garbage ("2.0abc", "2.0 "), "inf" /
    // "nan" in any case, and out-of-range all yield nullopt rather than a
    // plausible number.
    //
    // Rejecting non-finite HERE is load-bearing, not tidiness. This value
    // reaches the task-graph worker budget, where inf passes a `>= 1.0f` guard
    // and the resulting ceil(workers * inf) cast to i32 is undefined behaviour.
    // Pure and side-effect free.
    [[nodiscard]] std::optional<f32> ParseNumberLever(const char* raw, f32 minValue, f32 maxValue);

    // The non-default levers as "NAME=value, NAME=value", or "" when every
    // lever is at its default. Cheap enough to call at startup.
    [[nodiscard]] std::string ActiveSummary();

    // Logs `ActiveSummary()` at INFO, or nothing when nothing is set. Called
    // once during engine init so a run that behaves oddly says why in its own
    // log rather than in someone's shell history.
    void LogActive();

// --- Generated from DebugLevers.inl -----------------------------------------
#define OLO_LEVER_TOGGLE(id, env, help) \
    [[nodiscard]] bool id();            \
    void Set##id(bool value);
#define OLO_LEVER_EXACT(id, env, help) \
    [[nodiscard]] bool id();           \
    void Set##id(bool value);
#define OLO_LEVER_TRISTATE(id, env, help) \
    [[nodiscard]] Tristate id();          \
    void Set##id(Tristate value);
#define OLO_LEVER_INT(id, env, minValue, help) \
    [[nodiscard]] std::optional<i64> id();     \
    void Set##id(std::optional<i64> value);
#define OLO_LEVER_NUMBER(id, env, minValue, maxValue, help) \
    [[nodiscard]] std::optional<f32> id();                  \
    void Set##id(std::optional<f32> value);
#define OLO_LEVER_TEXT(id, env, help) [[nodiscard]] std::optional<std::string> id();

#include "OloEngine/Core/DebugLevers.inl"

#undef OLO_LEVER_TOGGLE
#undef OLO_LEVER_EXACT
#undef OLO_LEVER_TRISTATE
#undef OLO_LEVER_INT
#undef OLO_LEVER_NUMBER
#undef OLO_LEVER_TEXT
} // namespace OloEngine::Levers
