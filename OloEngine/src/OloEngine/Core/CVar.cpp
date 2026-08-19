#include "OloEnginePCH.h"
#include "OloEngine/Core/CVar.h"

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/DebugLevers.h"
#include "OloEngine/Core/Log.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <format>
#include <limits>
#include <mutex>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace OloEngine::CVars
{
    namespace
    {
        struct Entry
        {
            CVarBinding Binding;
            // The rendered value as of the last dispatch. The change test is a
            // comparison against THIS rather than a dirty flag, so a write that
            // does not move the value — or a pair of writes that cancel within
            // one frame — notifies nobody, and a write from any path at all
            // (typed setter, SetFromString, --set) is caught the same way.
            std::string LastNotified;
            std::vector<std::pair<u64, ChangeCallback>> Callbacks;
        };

        struct Registry
        {
            std::mutex Mutex;
            // deque, not vector: references into it must survive a later
            // Register(), because dispatch copies a callback list out of an
            // entry while other threads may still be registering.
            std::deque<Entry> Entries;
            std::vector<CVarHandle> Pending;
            u64 NextCallbackId = 1;
        };

        // Meyers singleton so a CVar<T> at namespace scope can register during
        // static initialization without depending on this file's own init order.
        [[nodiscard]] Registry& Reg()
        {
            static Registry registry;
            return registry;
        }

        [[nodiscard]] char LowerAscii(char c)
        {
            return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }

        [[nodiscard]] bool EqualsIgnoreCase(std::string_view a, std::string_view b)
        {
            return a.size() == b.size() &&
                   std::equal(a.begin(), a.end(), b.begin(),
                              [](char x, char y)
                              { return LowerAscii(x) == LowerAscii(y); });
        }

        [[nodiscard]] bool StartsWithIgnoreCase(std::string_view text, std::string_view prefix)
        {
            return text.size() >= prefix.size() &&
                   std::equal(prefix.begin(), prefix.end(), text.begin(),
                              [](char x, char y)
                              { return LowerAscii(x) == LowerAscii(y); });
        }

        [[nodiscard]] bool ContainsIgnoreCase(std::string_view text, std::string_view needle)
        {
            if (needle.empty())
            {
                return true;
            }
            if (needle.size() > text.size())
            {
                return false;
            }
            for (sizet i = 0; i + needle.size() <= text.size(); ++i)
            {
                if (StartsWithIgnoreCase(text.substr(i), needle))
                {
                    return true;
                }
            }
            return false;
        }

        // Thin alias: the definition is the public CVars::TrimText below, so
        // there is exactly one whitespace rule in the engine.
        [[nodiscard]] std::string_view Trim(std::string_view text)
        {
            return TrimText(text);
        }

        // The lever rows are not registered from a static initializer — they
        // live in another TU, and tying two static-init orders together for no
        // gain is how this kind of table ends up half-populated. Instead every
        // name-based entry point funnels through here first.
        //
        // This must NOT be called from Register() or MarkChanged(): both are
        // reachable *from* Levers::RegisterCVars() and from a lever setter, and
        // re-entering call_once from inside its own callback deadlocks.
        void EnsureBuiltinsRegistered()
        {
            static std::once_flag once;
            std::call_once(once, []
                           { Levers::RegisterCVars(); });
        }

        [[nodiscard]] u32 IndexOf(CVarHandle handle)
        {
            return static_cast<u32>(handle);
        }

        // Caller holds the lock.
        [[nodiscard]] Entry* FindEntryLocked(Registry& registry, std::string_view name)
        {
            for (Entry& entry : registry.Entries)
            {
                if (EqualsIgnoreCase(entry.Binding.Name, name))
                {
                    return &entry;
                }
            }
            return nullptr;
        }

        // Caller holds the lock. `rendered` is passed in because every caller
        // has already paid for it.
        [[nodiscard]] CVarInfo MakeInfo(const Entry& entry, u32 index, std::string rendered)
        {
            CVarInfo info;
            info.Name = entry.Binding.Name;
            info.Help = entry.Binding.Help;
            info.Type = entry.Binding.Type;
            info.Value = std::move(rendered);
            info.IsDefault = entry.Binding.IsDefault != nullptr ? entry.Binding.IsDefault(entry.Binding.Context) : true;
            info.ReadOnly = entry.Binding.ReadOnly;
            info.Handle = static_cast<CVarHandle>(index);
            return info;
        }
    } // namespace

    // --- Parse helpers ------------------------------------------------------

    std::string_view TrimText(std::string_view text)
    {
        const auto isSpace = [](char c)
        { return std::isspace(static_cast<unsigned char>(c)) != 0; };
        while (!text.empty() && isSpace(text.front()))
        {
            text.remove_prefix(1);
        }
        while (!text.empty() && isSpace(text.back()))
        {
            text.remove_suffix(1);
        }
        return text;
    }

    std::string_view CVarTypeName(CVarType type)
    {
        switch (type)
        {
            case CVarType::Bool:
                return "bool";
            case CVarType::Tristate:
                return "tristate";
            case CVarType::Int:
                return "int";
            case CVarType::Float:
                return "float";
            case CVarType::String:
                return "string";
            default:
                // Not a missing enumerator - every one is handled above. This is
                // the "read through a stale/raw cast" case.
                return "unknown";
        }
    }

    std::optional<bool> ParseBoolText(std::string_view raw)
    {
        const std::string_view text = Trim(raw);
        if (EqualsIgnoreCase(text, "1") || EqualsIgnoreCase(text, "true") || EqualsIgnoreCase(text, "yes") ||
            EqualsIgnoreCase(text, "on"))
        {
            return true;
        }
        if (EqualsIgnoreCase(text, "0") || EqualsIgnoreCase(text, "false") || EqualsIgnoreCase(text, "no") ||
            EqualsIgnoreCase(text, "off"))
        {
            return false;
        }
        return std::nullopt;
    }

    bool IsUnsetText(std::string_view raw)
    {
        const std::string_view text = Trim(raw);
        return text.empty() || EqualsIgnoreCase(text, "unset") || EqualsIgnoreCase(text, "none");
    }

    std::optional<i64> ParseIntText(std::string_view raw)
    {
        const std::string_view text = Trim(raw);
        if (text.empty())
        {
            return std::nullopt;
        }
        // strtoll needs a null-terminated buffer, and the trailing-garbage check
        // is the whole point: "8x" must be an error, not 8.
        const std::string buffer(text);
        char* end = nullptr;
        errno = 0;
        const long long value = std::strtoll(buffer.c_str(), &end, 10);
        if (end == buffer.c_str() || (end != nullptr && *end != '\0') || errno == ERANGE)
        {
            return std::nullopt;
        }
        return static_cast<i64>(value);
    }

    // --- Registry -----------------------------------------------------------

    CVarHandle Register(const CVarBinding& binding)
    {
        OLO_CORE_ASSERT(!binding.Name.empty(), "a cvar needs a name");
        OLO_CORE_ASSERT(binding.Render != nullptr, "a cvar needs a Render function");

        Registry& registry = Reg();
        const std::lock_guard lock(registry.Mutex);

        if (const Entry* existing = FindEntryLocked(registry, binding.Name); existing != nullptr)
        {
            // Registering the same name twice would give the console two
            // answers for one question. Loud, because it is always a bug.
            OLO_CORE_ERROR("[CVar] duplicate registration of '{}' ignored", binding.Name);
            return CVarHandle::Invalid;
        }

        Entry& entry = registry.Entries.emplace_back();
        entry.Binding = binding;
        entry.LastNotified = binding.Render(binding.Context);
        return static_cast<CVarHandle>(static_cast<u32>(registry.Entries.size() - 1));
    }

    void MarkChanged(CVarHandle handle)
    {
        if (handle == CVarHandle::Invalid)
        {
            return;
        }
        Registry& registry = Reg();
        const std::lock_guard lock(registry.Mutex);
        if (IndexOf(handle) >= registry.Entries.size())
        {
            return;
        }
        // Duplicates are fine — dispatch de-duplicates and the list is drained
        // every frame, so it cannot grow without bound.
        registry.Pending.push_back(handle);
    }

    std::vector<CVarInfo> Snapshot()
    {
        EnsureBuiltinsRegistered();
        Registry& registry = Reg();
        const std::lock_guard lock(registry.Mutex);

        std::vector<CVarInfo> out;
        out.reserve(registry.Entries.size());
        for (u32 i = 0; i < static_cast<u32>(registry.Entries.size()); ++i)
        {
            const Entry& entry = registry.Entries[i];
            out.push_back(MakeInfo(entry, i, entry.Binding.Render(entry.Binding.Context)));
        }
        return out;
    }

    std::optional<CVarInfo> Find(std::string_view name)
    {
        EnsureBuiltinsRegistered();
        // Trimmed, exactly as SetFromString trims. They must agree: a caller
        // that passes the same padded name to both — which olo_cvar_set does,
        // straight off a JSON string — would otherwise get a successful write
        // and then no metadata for it, which reads as the write not landing.
        const std::string_view trimmedName = Trim(name);
        Registry& registry = Reg();
        const std::lock_guard lock(registry.Mutex);

        for (u32 i = 0; i < static_cast<u32>(registry.Entries.size()); ++i)
        {
            const Entry& entry = registry.Entries[i];
            if (EqualsIgnoreCase(entry.Binding.Name, trimmedName))
            {
                return MakeInfo(entry, i, entry.Binding.Render(entry.Binding.Context));
            }
        }
        return std::nullopt;
    }

    SetResult SetFromString(std::string_view name, std::string_view value)
    {
        EnsureBuiltinsRegistered();

        const std::string_view trimmedName = Trim(name);
        SetResult result;

        // The binding is copied out and the lock released before Parse runs: a
        // parser calls a typed setter, which calls MarkChanged, which takes this
        // same mutex.
        CVarBinding binding;
        bool found = false;
        std::vector<std::string> similar;
        {
            Registry& registry = Reg();
            const std::lock_guard lock(registry.Mutex);
            if (const Entry* entry = FindEntryLocked(registry, trimmedName); entry != nullptr)
            {
                binding = entry->Binding;
                found = true;
            }
            else
            {
                // A "did you mean" that only fires on a substring of the WHOLE
                // typed name is nearly useless: the names are underscore-joined
                // compounds, and the realistic mistake is remembering some of
                // the words rather than mistyping one character. So match on any
                // word of the input — "OLO_POISON" finds
                // OLO_RG_POISON_TRANSIENTS, which a substring test does not.
                for (const Entry& candidate : registry.Entries)
                {
                    if (similar.size() >= 5)
                    {
                        break;
                    }
                    for (std::string_view rest = trimmedName; !rest.empty();)
                    {
                        const sizet separator = rest.find('_');
                        const std::string_view word = rest.substr(0, separator);
                        rest = separator == std::string_view::npos ? std::string_view{} : rest.substr(separator + 1);

                        // "OLO" is in every name, so it distinguishes nothing;
                        // very short fragments match half the table.
                        if (word.size() < 4)
                        {
                            continue;
                        }
                        if (ContainsIgnoreCase(candidate.Binding.Name, word))
                        {
                            similar.emplace_back(candidate.Binding.Name);
                            break;
                        }
                    }
                }
            }
        }

        if (!found)
        {
            result.Error = "unknown console variable '" + std::string(trimmedName) + "'";
            if (!similar.empty())
            {
                result.Error += " - did you mean";
                for (const std::string& candidate : similar)
                {
                    result.Error += " " + candidate;
                }
                result.Error += "?";
            }
            return result;
        }

        result.OldValue = binding.Render(binding.Context);
        result.NewValue = result.OldValue;

        if (binding.ReadOnly || binding.Parse == nullptr)
        {
            result.Error = std::string(binding.Name) + " is read-only (it is consumed once at init, so a later "
                                                       "write would be silently ignored rather than take effect)";
            return result;
        }

        std::string error;
        if (!binding.Parse(binding.Context, value, error))
        {
            result.Error = std::string(binding.Name) + ": " + error;
            return result;
        }

        result.Ok = true;
        result.NewValue = binding.Render(binding.Context);
        result.Changed = result.NewValue != result.OldValue;
        return result;
    }

    std::vector<std::string_view> Complete(std::string_view prefix, sizet maxResults)
    {
        EnsureBuiltinsRegistered();
        const std::string_view trimmed = Trim(prefix);

        Registry& registry = Reg();
        const std::lock_guard lock(registry.Mutex);

        std::vector<std::string_view> out;
        for (const Entry& entry : registry.Entries)
        {
            if (out.size() >= maxResults)
            {
                break;
            }
            if (StartsWithIgnoreCase(entry.Binding.Name, trimmed))
            {
                out.push_back(entry.Binding.Name);
            }
        }
        return out;
    }

    std::string LongestCompletion(std::string_view prefix)
    {
        // No cap: the answer is only correct over ALL matches.
        const std::vector<std::string_view> matches = Complete(prefix, (std::numeric_limits<sizet>::max)());
        if (matches.empty())
        {
            return std::string(prefix);
        }

        std::string common(matches.front());
        for (const std::string_view candidate : matches)
        {
            sizet i = 0;
            while (i < common.size() && i < candidate.size() && LowerAscii(common[i]) == LowerAscii(candidate[i]))
            {
                ++i;
            }
            common.resize(i);
        }
        return common;
    }

    // --- Change notification ------------------------------------------------

    CallbackHandle AddChangeCallback(std::string_view name, ChangeCallback callback, bool invokeNow)
    {
        EnsureBuiltinsRegistered();
        if (!callback)
        {
            return {};
        }

        CallbackHandle handle;
        CVarInfo info;
        {
            Registry& registry = Reg();
            const std::lock_guard lock(registry.Mutex);

            Entry* entry = FindEntryLocked(registry, Trim(name));
            if (entry == nullptr)
            {
                OLO_CORE_WARN("[CVar] AddChangeCallback for unknown console variable '{}'", name);
                return {};
            }

            handle.Id = registry.NextCallbackId++;
            entry->Callbacks.emplace_back(handle.Id, callback);

            if (invokeNow)
            {
                u32 index = 0;
                for (u32 i = 0; i < static_cast<u32>(registry.Entries.size()); ++i)
                {
                    if (&registry.Entries[i] == entry)
                    {
                        index = i;
                        break;
                    }
                }
                info = MakeInfo(*entry, index, entry->Binding.Render(entry->Binding.Context));
            }
        }

        // Outside the lock: a callback typically touches renderer state and may
        // itself read the registry.
        if (invokeNow)
        {
            callback(info);
        }
        return handle;
    }

    bool RemoveChangeCallback(CallbackHandle handle)
    {
        if (!handle.IsValid())
        {
            return false;
        }
        Registry& registry = Reg();
        const std::lock_guard lock(registry.Mutex);
        for (Entry& entry : registry.Entries)
        {
            const auto it = std::ranges::find_if(entry.Callbacks,
                                                 [&](const auto& pair)
                                                 { return pair.first == handle.Id; });
            if (it != entry.Callbacks.end())
            {
                entry.Callbacks.erase(it);
                return true;
            }
        }
        return false;
    }

    void DispatchPendingChanges()
    {
        Registry& registry = Reg();

        std::vector<CVarHandle> pending;
        {
            const std::lock_guard lock(registry.Mutex);
            if (registry.Pending.empty())
            {
                return; // the common case: one uncontended lock per frame
            }
            pending.swap(registry.Pending);
        }

        std::ranges::sort(pending);
        pending.erase(std::ranges::unique(pending).begin(), pending.end());

        for (const CVarHandle handle : pending)
        {
            CVarInfo info;
            std::vector<ChangeCallback> callbacks;
            {
                const std::lock_guard lock(registry.Mutex);
                const u32 index = IndexOf(handle);
                if (index >= registry.Entries.size())
                {
                    continue;
                }
                Entry& entry = registry.Entries[index];
                std::string rendered = entry.Binding.Render(entry.Binding.Context);
                if (rendered == entry.LastNotified)
                {
                    // Marked but not actually moved — a set to the value it
                    // already had, or two writes that cancelled this frame.
                    continue;
                }
                entry.LastNotified = rendered;
                if (entry.Callbacks.empty())
                {
                    continue;
                }
                info = MakeInfo(entry, index, std::move(rendered));
                callbacks.reserve(entry.Callbacks.size());
                for (const auto& [id, callback] : entry.Callbacks)
                {
                    callbacks.push_back(callback);
                }
            }

            for (const ChangeCallback& callback : callbacks)
            {
                callback(info);
            }
        }
    }

    // --- Command line -------------------------------------------------------

    std::optional<std::pair<std::string, std::string>> ParseAssignment(std::string_view argument)
    {
        const sizet equals = argument.find('=');
        if (equals == std::string_view::npos)
        {
            return std::nullopt;
        }
        const std::string_view name = Trim(argument.substr(0, equals));
        if (name.empty())
        {
            return std::nullopt;
        }
        // The value is NOT trimmed of interior content and NOT split on further
        // '=' — "A=B=C" sets A to "B=C", which is what a string-valued cvar
        // needs and what every shell does.
        return std::pair<std::string, std::string>{ std::string(name), std::string(Trim(argument.substr(equals + 1))) };
    }

    CommandLineResult ApplyCommandLine(int argc, char** argv)
    {
        CommandLineResult result;
        if (argv == nullptr)
        {
            return result;
        }

        constexpr std::string_view kFlag = "--set";
        constexpr std::string_view kFlagEquals = "--set=";

        for (int i = 1; i < argc; ++i)
        {
            if (argv[i] == nullptr)
            {
                continue;
            }
            const std::string_view argument(argv[i]);

            std::string_view assignment;
            if (argument == kFlag)
            {
                if (i + 1 >= argc || argv[i + 1] == nullptr)
                {
                    result.Errors.emplace_back("--set needs an argument of the form NAME=VALUE");
                    break;
                }
                assignment = argv[++i];
            }
            else if (argument.starts_with(kFlagEquals))
            {
                assignment = argument.substr(kFlagEquals.size());
            }
            else
            {
                continue;
            }

            const std::optional<std::pair<std::string, std::string>> parsed = ParseAssignment(assignment);
            if (!parsed)
            {
                result.Errors.push_back("--set '" + std::string(assignment) + "' is not of the form NAME=VALUE");
                continue;
            }

            const SetResult setResult = SetFromString(parsed->first, parsed->second);
            if (!setResult.Ok)
            {
                result.Errors.push_back("--set " + std::string(assignment) + ": " + setResult.Error);
                continue;
            }
            ++result.Applied;
        }

        return result;
    }

    // --- CVar<T> ------------------------------------------------------------

    namespace
    {
        // std::string is not atomic, so a string cvar's value is guarded. One
        // shared mutex for every string cvar: they are rare, written from a
        // console, and read at init.
        [[nodiscard]] std::mutex& StringValueMutex()
        {
            static std::mutex mutex;
            return mutex;
        }

        [[nodiscard]] std::string RenderBool(bool value)
        {
            return value ? "on" : "off";
        }
    } // namespace

    template<typename T>
    CVar<T>::CVar(std::string_view name, T defaultValue, std::string_view help)
        : m_Name(name), m_Help(help), m_Default(defaultValue), m_Value(defaultValue)
    {
        CVarBinding binding;
        binding.Name = m_Name;
        binding.Help = m_Help;
        binding.ReadOnly = false;
        binding.Context = this;

        if constexpr (std::is_same_v<T, bool>)
        {
            binding.Type = CVarType::Bool;
            binding.Render = [](void* context)
            { return RenderBool(static_cast<CVar<T>*>(context)->Get()); };
            binding.Parse = [](void* context, std::string_view raw, std::string& error)
            {
                const std::optional<bool> parsed = ParseBoolText(raw);
                if (!parsed)
                {
                    error = "expected on/off (also accepts 1/0, true/false, yes/no)";
                    return false;
                }
                static_cast<CVar<T>*>(context)->Set(*parsed);
                return true;
            };
        }
        else if constexpr (std::is_same_v<T, i64>)
        {
            binding.Type = CVarType::Int;
            binding.Render = [](void* context)
            { return std::to_string(static_cast<CVar<T>*>(context)->Get()); };
            binding.Parse = [](void* context, std::string_view raw, std::string& error)
            {
                const std::optional<i64> parsed = ParseIntText(raw);
                if (!parsed)
                {
                    error = "expected a whole number";
                    return false;
                }
                static_cast<CVar<T>*>(context)->Set(*parsed);
                return true;
            };
        }
        else if constexpr (std::is_same_v<T, f32>)
        {
            binding.Type = CVarType::Float;
            // Shortest round-trip, not std::to_string's fixed six decimals: the
            // registry decides "did this change?" by comparing rendered
            // strings, so a float rendering that loses precision silently
            // swallows small-but-real changes.
            binding.Render = [](void* context)
            { return std::format("{}", static_cast<CVar<T>*>(context)->Get()); };
            binding.Parse = [](void* context, std::string_view raw, std::string& error)
            {
                // Reuses the levers' parser so "inf"/"nan"/trailing garbage are
                // rejected here rather than reaching a consumer that divides by
                // it. The bounds are the whole finite f32 range for a plain
                // CVar<f32>; a lever narrows them per row.
                const std::string buffer(Trim(raw));
                const std::optional<f32> parsed = Levers::ParseNumberLever(
                    buffer.c_str(), -(std::numeric_limits<f32>::max)(), (std::numeric_limits<f32>::max)());
                if (!parsed)
                {
                    error = "expected a finite number";
                    return false;
                }
                static_cast<CVar<T>*>(context)->Set(*parsed);
                return true;
            };
        }
        else
        {
            binding.Type = CVarType::String;
            binding.Render = [](void* context)
            {
                std::string value = static_cast<CVar<T>*>(context)->Get();
                // Render must never be empty — an empty cell reads as a broken
                // binding rather than an empty string.
                return value.empty() ? std::string("unset") : value;
            };
            binding.Parse = [](void* context, std::string_view raw, std::string&)
            {
                static_cast<CVar<T>*>(context)->Set(std::string(Trim(raw)));
                return true;
            };
        }

        binding.IsDefault = [](void* context)
        {
            const CVar<T>* self = static_cast<const CVar<T>*>(context);
            if constexpr (std::is_same_v<T, f32>)
            {
                // Bit-identical, not epsilon: "is this cvar doing nothing?" is a
                // provenance question, not a numeric one.
                const f32 value = self->Get();
                const f32 fallback = self->Default();
                return std::memcmp(&value, &fallback, sizeof(f32)) == 0;
            }
            else
            {
                return self->Get() == self->Default();
            }
        };

        m_Handle = Register(binding);
    }

    template<typename T>
    T CVar<T>::Get() const
    {
        if constexpr (std::is_same_v<T, std::string>)
        {
            const std::lock_guard lock(StringValueMutex());
            return m_Value;
        }
        else
        {
            return m_Value.load(std::memory_order_relaxed);
        }
    }

    template<typename T>
    void CVar<T>::Set(T value)
    {
        if constexpr (std::is_same_v<T, std::string>)
        {
            {
                const std::lock_guard lock(StringValueMutex());
                m_Value = std::move(value);
            }
        }
        else
        {
            m_Value.store(value, std::memory_order_relaxed);
        }
        MarkChanged(m_Handle);
    }

    template class CVar<bool>;
    template class CVar<i64>;
    template class CVar<f32>;
    template class CVar<std::string>;
} // namespace OloEngine::CVars
