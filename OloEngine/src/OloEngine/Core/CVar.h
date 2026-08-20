#pragma once

#include "OloEngine/Core/Base.h"

#include <atomic>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace OloEngine::CVars
{
    // @brief The console-variable layer: name-based lookup, `--set`, an editor
    // console, and — the part that costs time without it — a CHANGE
    // NOTIFICATION so a subsystem that cached a value at init can react.
    //
    // `Core/DebugLevers.h` is the layer underneath. It already solved
    // registration, generated typed accessors, setters and enumeration, and it
    // keeps doing all of that: the lever rows are *bound* into this registry
    // rather than reimplemented on top of it, so all 21 rows keep their generated
    // accessors, and every call site, `Levers::LogActive()` and
    // `olo_debug_levers` are untouched. What this file adds is everything the
    // levers could not do:
    //
    //   * **Name-based.** `SetFromString("OLO_RG_POISON_TRANSIENTS", "on")` —
    //     which is what a console line, a `--set` argument and an MCP call all
    //     reduce to. The generated accessors stay the fast path.
    //   * **`--set name=value`** applied in `main()` before `CreateApplication`,
    //     so a value is in place before any subsystem can cache it.
    //   * **Change notification.** See below — this is the subtle part.
    //
    // ## The three questions a change notification has to answer
    //
    // **Who observes?** A subsystem registers a callback against ONE cvar by
    // name (`AddChangeCallback`). There is no global "something changed" hook,
    // because every real consumer cares about one knob.
    //
    // **When does it fire, relative to the frame?** Not at the write. A write
    // can arrive from the ImGui thread (the console), an httplib worker (MCP) or
    // a test, and a subsystem's reaction is usually "recreate a GPU resource" —
    // which must not run on those threads. So a write only MARKS the cvar, and
    // `DispatchPendingChanges()` drains the marks at the TOP OF THE FRAME on the
    // game thread, before any layer update. The whole frame then runs on one
    // consistent value. A write that lands mid-frame is seen by the next frame.
    //
    // **What if a subsystem registers a callback after the value changed?**
    // Nothing special happens, and that is deliberate: `AddChangeCallback`
    // invokes the callback once at registration by default (`invokeNow`). The
    // contract is therefore **"apply the current value"**, never "handle a
    // delta" — an idempotent callback makes the registration-vs-change ordering
    // irrelevant, which is the only version of this that is not a race. The same
    // contract is why several writes in one frame COALESCE into one call, and
    // why a value that goes A -> B -> A within a frame notifies nobody: the
    // registry compares the rendered value against what it last notified, so
    // DISPATCH delivers exactly once per *observed* change and never for a
    // set-to-the-value-it-already-had.
    //
    // `invokeNow` sits OUTSIDE that accounting, deliberately. Registering while
    // a change is already pending therefore calls the new callback twice: once
    // immediately, once at the next dispatch. That is the correct trade — the
    // alternative is advancing the shared last-notified value at registration,
    // which would swallow the pending notification for every callback ALREADY
    // registered on that cvar, turning a harmless repeat into a lost update.
    // Idempotence is what makes the repeat harmless, which is exactly why the
    // contract demands it.
    //
    // ## Thread safety
    //
    // Reads and writes of a cvar's VALUE are exactly as atomic as the binding
    // underneath — for every lever that is the relaxed atomic it already used,
    // so a console write cannot tear a render-thread read, and nothing stronger
    // is promised (a lever orders no other data). The registry's own structure —
    // the binding table, the callback lists, the pending-change set — is guarded
    // by a mutex, so registration, lookup, marking and dispatch are all safe
    // from any thread. Callbacks run on whichever thread calls
    // `DispatchPendingChanges()`, which the engine calls only from the game
    // thread.

    enum class CVarType : u8
    {
        Bool,
        Tristate, // on / off / unset — "unset" means "leave the computed default alone"
        Int,
        Float,
        String
    };

    // Index into the registry. Stable for the process lifetime; a cvar is never
    // unregistered.
    enum class CVarHandle : u32
    {
        Invalid = 0xFFFFFFFFu
    };

    // A cvar as the name-based side sees it. `Value` is rendered for display and
    // is also the string the registry compares to decide a change happened, so
    // it must be a faithful, stable rendering of the value.
    struct CVarInfo
    {
        std::string_view Name;
        std::string_view Help;
        CVarType Type = CVarType::Bool;
        std::string Value;
        bool IsDefault = true;
        bool ReadOnly = false;
        CVarHandle Handle = CVarHandle::Invalid;
    };

    // How the registry reaches storage it does not own. Captureless function
    // pointers plus an opaque context, so a binding costs one table row and no
    // allocation, and the fast typed accessor stays whatever it already was.
    //
    // `Name` and `Help` must have static lifetime — a string literal, or the
    // literals `DebugLevers.inl` already carries.
    struct CVarBinding
    {
        std::string_view Name;
        std::string_view Help;
        CVarType Type = CVarType::Bool;
        bool ReadOnly = false;
        void* Context = nullptr;

        // Current value, rendered. Must never be empty.
        std::string (*Render)(void* context) = nullptr;
        // True when the cvar is doing nothing — what the startup log filters on.
        bool (*IsDefault)(void* context) = nullptr;
        // Parse `raw` and store it. Returns false and fills `error` with a
        // one-line, user-facing reason on bad input. Null when ReadOnly.
        // Implementations route through their own typed setter, which is what
        // calls MarkChanged.
        bool (*Parse)(void* context, std::string_view raw, std::string& error) = nullptr;
    };

    // Adds a binding and returns its handle. Safe to call during static
    // initialization (the registry's storage is function-local).
    CVarHandle Register(const CVarBinding& binding);

    // Announce that a cvar's value may have changed. Cheap: records the handle
    // and returns. The comparison and the callbacks happen in
    // DispatchPendingChanges(). A typed setter calls this; callers going through
    // SetFromString do not need to.
    void MarkChanged(CVarHandle handle);

    // Every registered cvar, in registration order.
    [[nodiscard]] std::vector<CVarInfo> Snapshot();

    // Case-insensitive exact lookup.
    [[nodiscard]] std::optional<CVarInfo> Find(std::string_view name);

    struct SetResult
    {
        bool Ok = false;
        bool Changed = false; // the rendered value actually moved
        std::string Error;    // one line, user-facing; empty when Ok
        std::string OldValue;
        std::string NewValue;
    };

    // The one write path every front end reduces to. Case-insensitive name.
    [[nodiscard]] SetResult SetFromString(std::string_view name, std::string_view value);

    // Registered names starting with `prefix` (case-insensitive), in
    // registration order. Feeds the console's tab completion.
    [[nodiscard]] std::vector<std::string_view> Complete(std::string_view prefix, sizet maxResults = 64);

    // The longest common prefix of the completions of `prefix`, for the "tab
    // extends as far as it unambiguously can" half of completion. Returns
    // `prefix` itself when nothing matches.
    [[nodiscard]] std::string LongestCompletion(std::string_view prefix);

    // --- Change notification ------------------------------------------------

    using ChangeCallback = std::function<void(const CVarInfo&)>;

    struct CallbackHandle
    {
        u64 Id = 0;
        [[nodiscard]] bool IsValid() const noexcept
        {
            return Id != 0;
        }
    };

    // Observe one cvar. `invokeNow` calls the callback immediately with the
    // current value, which is what makes the callback's "apply the current
    // value" contract safe regardless of whether the value already moved. It is
    // an EXTRA call, not part of the once-per-change accounting — see the
    // header comment. An unknown name registers nothing and returns an invalid
    // handle.
    CallbackHandle AddChangeCallback(std::string_view name, ChangeCallback callback, bool invokeNow = true);
    bool RemoveChangeCallback(CallbackHandle handle);

    // Drain the marks and fire callbacks. Called once per frame from
    // Application's loops, at the top, before layers update. Tests call it
    // directly. Cheap when nothing is pending (the common case) — one
    // uncontended lock and return.
    void DispatchPendingChanges();

    // --- Command line -------------------------------------------------------

    // Splits "NAME=VALUE". Rejects an empty name and a missing '='; an empty
    // VALUE is allowed and reaches the cvar's own parser, which is where "" is
    // meaningful for some types and an error for others. Pure; exposed because
    // it is the interesting boundary and testable without a second process.
    [[nodiscard]] std::optional<std::pair<std::string, std::string>> ParseAssignment(std::string_view argument);

    struct CommandLineResult
    {
        u32 Applied = 0;
        std::vector<std::string> Errors; // one line per rejected `--set`
    };

    // Applies every `--set NAME=VALUE` / `--set=NAME=VALUE` in argv. Called from
    // main() before the Application exists, so a value is in place before any
    // subsystem can cache it — otherwise `--set` reinvents the exact bug the
    // change notification exists to fix. Malformed arguments are collected, not
    // fatal: the caller logs them.
    [[nodiscard]] CommandLineResult ApplyCommandLine(int argc, char** argv);

    // --- Typed CVar<T> ------------------------------------------------------

    // A cvar that owns its own storage, for code with no lever behind it.
    //
    // Intended lifetime is static — the registry keeps a pointer to the object
    // and cvars are never unregistered, so a stack-local one is a dangling
    // binding. Non-copyable and non-movable for the same reason. `name` and
    // `help` must be string literals.
    //
    // `Get()` on an arithmetic cvar is a relaxed atomic load of an inline
    // member: no map lookup, no lock, nothing the name-based side touches. That
    // is the point — the fast path stays fast and the registry is the slow,
    // name-keyed side.
    template<typename T>
    class CVar
    {
        static_assert(std::is_same_v<T, bool> || std::is_same_v<T, i64> || std::is_same_v<T, f32> ||
                          std::is_same_v<T, std::string>,
                      "CVar supports bool, i64, f32 and std::string");

      public:
        CVar(std::string_view name, T defaultValue, std::string_view help);

        CVar(const CVar&) = delete;
        CVar& operator=(const CVar&) = delete;
        CVar(CVar&&) = delete;
        CVar& operator=(CVar&&) = delete;
        ~CVar() = default;

        [[nodiscard]] T Get() const;
        void Set(T value);

        [[nodiscard]] std::string_view Name() const noexcept
        {
            return m_Name;
        }
        [[nodiscard]] std::string_view Help() const noexcept
        {
            return m_Help;
        }
        [[nodiscard]] CVarHandle Handle() const noexcept
        {
            return m_Handle;
        }
        [[nodiscard]] const T& Default() const noexcept
        {
            return m_Default;
        }

        [[nodiscard]] explicit operator T() const
        {
            return Get();
        }

      private:
        // Arithmetic values live in an atomic so a console write cannot tear a
        // worker's read; a string cannot, so it takes a registry-owned lock
        // through the helpers in the .cpp.
        using Storage = std::conditional_t<std::is_same_v<T, std::string>, std::string, std::atomic<T>>;

        std::string_view m_Name;
        std::string_view m_Help;
        T m_Default;
        Storage m_Value;
        CVarHandle m_Handle = CVarHandle::Invalid;
    };

    extern template class CVar<bool>;
    extern template class CVar<i64>;
    extern template class CVar<f32>;
    extern template class CVar<std::string>;

    // --- Shared parse helpers ----------------------------------------------
    // Public because the lever bindings in DebugLevers.cpp use them, and because
    // "what exactly does the console accept for a boolean?" is worth being able
    // to test directly.

    // Outer whitespace only. Exported for the same reason the parsers below
    // are: this rule decides which console input is accepted, and a second copy
    // of it somewhere else is free to drift from this one.
    [[nodiscard]] std::string_view TrimText(std::string_view text);

    // The wire/display name of a cvar type. Exported because the MCP tool's
    // schema `enum` and the editor console's display must use the same
    // vocabulary — two local copies would let a new type name be added to one
    // and not the other, and the disagreement would only surface as a confused
    // reader.
    [[nodiscard]] std::string_view CVarTypeName(CVarType type);

    // Console truthiness, deliberately STRICTER than `Env::IsTruthy`: exactly
    // 1/true/yes/on and 0/false/no/off, case-insensitive, and anything else is
    // an error. The environment's lenient rule exists so `FOO=true` is not
    // silently off; at a console prompt the opposite matters — a typo must come
    // back as "I don't know what that means" rather than quietly reading as on.
    [[nodiscard]] std::optional<bool> ParseBoolText(std::string_view raw);

    // "unset" / "none" / "" clear an optional-valued cvar. Integer and float
    // levers distinguish "set to zero" from "nobody set it", so a console needs
    // a way to say the second.
    [[nodiscard]] bool IsUnsetText(std::string_view raw);

    // Whole-string integer parse: no trailing garbage, no silent 0 for junk.
    [[nodiscard]] std::optional<i64> ParseIntText(std::string_view raw);
} // namespace OloEngine::CVars
