#pragma once

#include "OloEngine/Core/Base.h"

#include <optional>
#include <string>
#include <string_view>

namespace OloEngine::Env
{
    // @brief The engine's ONE environment-variable reader.
    //
    // Before this existed the engine called `std::getenv` from ~60 places, eight
    // of which were near-identical private `IsTruthyEnvironmentVariable` copies
    // that had already drifted in what they accepted as "true". Each site also
    // hand-rolled its own parse — `strcmp(v, "1") == 0`, `*v != '0'`,
    // `std::atoi`, `value[0] != 'f'` — so two variables documented the same way
    // behaved differently.
    //
    // Everything funnels through here now, which buys three things:
    //
    //   1. **One definition of truthiness.** See `IsTruthy`.
    //   2. **One suppression.** `getenv` is flagged by cpp:S990 and has no
    //      portable thread-safe replacement (Windows has GetEnvironmentVariableA,
    //      POSIX has nothing), so it needs a justified NOSONAR — once, here,
    //      rather than once per call site.
    //   3. **One place to change the mechanism.** Moving a knob off the
    //      environment and onto a config file or a command-line flag is now a
    //      change to its *caller*, not a hunt through the tree.
    //
    // **Read once, at startup, into your own state.** These functions are not
    // meant to be called per frame: `getenv` returns a pointer into shared
    // static storage, and while nothing in the engine writes the environment,
    // the test harness does (`OloEngineTest.cpp`'s `main` calls `_putenv_s`,
    // and `McpDispatchTest` sets and restores variables around a case). Reading
    // once during init keeps every consumer clear of that.

    // The raw value, if the variable is set AND non-empty. An empty variable is
    // reported as absent: every caller in this engine treated "" as unset, and
    // the MSVC CRT uses an empty value to mean "delete the variable".
    [[nodiscard]] std::optional<std::string> Get(const char* name);

    // The engine's definition of an on/off switch: set, non-empty, and not
    // starting with '0', 'f' or 'F'. So "1", "true", "yes", "on" are all on;
    // "0", "false", "False" and unset are all off.
    //
    // Deliberately lenient, because the failure it prevents is silent: a lever
    // typed as `FOO=true` when the code tested `strcmp(v, "1") == 0` reads as
    // "off" and the developer concludes the lever does not work.
    [[nodiscard]] bool IsTruthy(const char* name);

    // Exact, case-sensitive match. For the levers whose contract is "only this
    // one value does anything" — where a typo must NOT be interpreted as on.
    [[nodiscard]] bool IsExactly(const char* name, std::string_view expected);

    // Integer value, or nullopt when unset or unparseable. Unlike `std::atoi`
    // this does not silently return 0 for garbage, so a caller can tell "set to
    // zero" apart from "mistyped".
    [[nodiscard]] std::optional<i64> GetInt(const char* name);
} // namespace OloEngine::Env
