#include "OloCtl/EventsFollow.h"

#include "OloCtl/HelpText.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <optional>
#include <ostream>
#include <string>

namespace OloCtl
{
    namespace
    {
        using Json = nlohmann::json;

        // DiagnosticEvent::CategoryToString's tokens, in enum order.
        constexpr std::array<const char*, 12> kCategoryTokens{
            "scene_load",
            "play",
            "stop",
            "entity_spawn",
            "entity_destroy",
            "asset_reload",
            "script_error",
            "scene_save",
            "scene_dirty",
            "asset_import",
            "compile_finished",
            "command_completed",
        };

        // One poll waits at most this long, whatever --timeout allows: a follower
        // that is interrupted should notice within seconds, not half a minute.
        constexpr int kMaxWaitMs = 10000;
        // ...and at least this long, so the loop never degenerates into a busy poll.
        constexpr int kMinWaitMs = 1000;
        // The HTTP read timeout must exceed the long-poll, or every idle poll is
        // reported as the editor having gone away. waitMs is at most --timeout / 2.
        constexpr int kMinTimeoutMs = 2 * kMinWaitMs;
        // olo_events_wait's `count` maximum.
        constexpr unsigned long long kServerMaxCount = 500;
        // DiagnosticsEventLog::kCapacity: the ring the cursor can fall behind.
        constexpr unsigned long long kRingWindow = 512;

        struct FollowOptions
        {
            // Absent: omit sinceId and let the editor start at "now". Present,
            // including 0: sent verbatim (0 is the editor's "no lower bound").
            std::optional<unsigned long long> SinceId;
            std::vector<std::string> Categories;
            std::string Until;
            std::optional<unsigned long long> Count;
            std::optional<long long> ForSeconds;
        };

        bool IsCategoryToken(const std::string& token)
        {
            return std::find(kCategoryTokens.begin(), kCategoryTokens.end(), token) != kCategoryTokens.end();
        }

        std::string CategoryList()
        {
            std::string list;
            for (const char* token : kCategoryTokens)
            {
                if (!list.empty())
                    list += ", ";
                list += token;
            }
            return list;
        }

        // Decimal digits only: no sign, no whitespace, no suffix.
        bool ParseUnsigned(const std::string& text, unsigned long long& out)
        {
            if (text.empty() || !std::all_of(text.begin(), text.end(),
                                             [](unsigned char c)
                                             { return c >= '0' && c <= '9'; }))
            {
                return false;
            }
            errno = 0;
            char* end = nullptr;
            const unsigned long long value = std::strtoull(text.c_str(), &end, 10);
            if (end == text.c_str() || *end != '\0' || errno == ERANGE)
                return false;
            out = value;
            return true;
        }

        bool ParseFollowOptions(const std::vector<std::string>& flags, FollowOptions& out, std::string& outError)
        {
            const std::size_t flagCount = flags.size();
            for (std::size_t i = 0; i < flagCount; ++i)
            {
                const std::string& token = flags[i];
                if (token.size() < 3 || token.compare(0, 2, "--") != 0)
                {
                    outError = "`events follow` takes options only, got '" + token + "'.";
                    return false;
                }

                std::string name = token.substr(2);
                std::string value;
                bool hasValue = false;
                if (const std::size_t equals = name.find('='); equals != std::string::npos)
                {
                    value = name.substr(equals + 1);
                    name = name.substr(0, equals);
                    hasValue = true;
                }

                if (name != "since-id" && name != "category" && name != "until" && name != "count" && name != "for")
                {
                    outError = "unknown option --" + name + " for `events follow`.";
                    return false;
                }

                if (!hasValue)
                {
                    if (i + 1 >= flagCount)
                    {
                        outError = "--" + name + " expects a value.";
                        return false;
                    }
                    value = flags[++i];
                    // The same rule as a command argument: a value never begins with
                    // `--`, because swallowing the next option would run the loop with
                    // a wrong bound and still exit 0.
                    if (value.compare(0, 2, "--") == 0)
                    {
                        outError = "--" + name + " expects a value, got '" + value + "'. Use --" + name + "=" + value +
                                   " if the value really starts with two dashes.";
                        return false;
                    }
                }

                if (name == "since-id")
                {
                    unsigned long long parsed = 0;
                    if (!ParseUnsigned(value, parsed))
                    {
                        outError = "--since-id expects an event id (an integer >= 0), got '" + value + "'.";
                        return false;
                    }
                    out.SinceId = parsed;
                }
                else if (name == "category" || name == "until")
                {
                    if (!IsCategoryToken(value))
                    {
                        outError = "--" + name + ": '" + value + "' is not an event category. One of: " +
                                   CategoryList() + ".";
                        return false;
                    }
                    if (name == "category")
                        out.Categories.push_back(value);
                    else
                        out.Until = value;
                }
                else if (name == "count")
                {
                    unsigned long long parsed = 0;
                    if (!ParseUnsigned(value, parsed) || parsed == 0)
                    {
                        outError = "--count expects a number of events (an integer >= 1), got '" + value + "'.";
                        return false;
                    }
                    out.Count = parsed;
                }
                else // "for"
                {
                    unsigned long long parsed = 0;
                    // Bounded so the deadline arithmetic below cannot overflow.
                    if (!ParseUnsigned(value, parsed) || parsed == 0 || parsed > 86400ULL * 366ULL)
                    {
                        outError = "--for expects a number of seconds (an integer >= 1), got '" + value + "'.";
                        return false;
                    }
                    out.ForSeconds = static_cast<long long>(parsed);
                }
            }
            return true;
        }

        bool IsErrorResult(const Json& result)
        {
            const auto isError = result.find("isError");
            return isError != result.end() && isError->is_boolean() && isError->get<bool>();
        }

        // Every `text` block of the result's `content`, joined with newlines.
        std::string TextContent(const Json& result)
        {
            std::string text;
            const auto content = result.find("content");
            if (content == result.end() || !content->is_array())
                return text;
            for (const Json& block : *content)
            {
                if (!block.is_object())
                    continue;
                const auto blockText = block.find("text");
                if (blockText == block.end() || !blockText->is_string())
                    continue;
                if (!text.empty())
                    text += '\n';
                text += blockText->get<std::string>();
            }
            return text;
        }

        bool ReadUnsignedField(const Json& object, const char* key, unsigned long long& out)
        {
            const auto field = object.find(key);
            if (field == object.end())
                return false;
            if (field->is_number_unsigned())
            {
                out = field->get<unsigned long long>();
                return true;
            }
            if (field->is_number_integer() && field->get<long long>() >= 0)
            {
                out = static_cast<unsigned long long>(field->get<long long>());
                return true;
            }
            return false;
        }

        std::string CategoryOf(const Json& event)
        {
            if (!event.is_object())
                return {};
            const auto category = event.find("category");
            if (category == event.end() || !category->is_string())
                return {};
            return category->get<std::string>();
        }

        int FollowEvents(const CliOptions& options, const FollowOptions& follow, ICommandSource& source,
                         std::ostream& out, std::ostream& err)
        {
            if (options.TimeoutMs < kMinTimeoutMs)
            {
                err << "oloctl: `events follow` needs a read timeout of at least " << kMinTimeoutMs
                    << " ms (--timeout " << options.TimeoutMs
                    << " was given): each poll long-polls the editor for up to half of it.\n";
                return ExitCode::Usage;
            }

            using Clock = std::chrono::steady_clock;
            std::optional<Clock::time_point> deadline;
            if (follow.ForSeconds)
                deadline = Clock::now() + std::chrono::seconds(*follow.ForSeconds);

            std::optional<unsigned long long> cursor = follow.SinceId;
            unsigned long long printed = 0;

            if (options.Verbose)
            {
                err << "oloctl: following events from ";
                if (cursor)
                    err << "id " << *cursor << (*cursor == 0 ? " (the oldest the editor still holds)" : "");
                else
                    err << "now (the editor's current last id; new events only)";
                err << " via " << kEventsWaitCommand << '\n';
            }

            for (;;)
            {
                int waitMs = std::min(kMaxWaitMs, std::max(kMinWaitMs, options.TimeoutMs / 2));
                if (deadline)
                {
                    const long long remainingMs =
                        std::chrono::duration_cast<std::chrono::milliseconds>(*deadline - Clock::now()).count();
                    if (remainingMs <= 0)
                        return ExitCode::Ok;
                    waitMs = static_cast<int>(std::min<long long>(waitMs, remainingMs));
                }

                Json arguments{ { "waitMs", waitMs } };
                if (cursor)
                    arguments["sinceId"] = *cursor;
                if (!follow.Categories.empty())
                    arguments["categories"] = follow.Categories;
                if (follow.Count)
                    arguments["count"] = std::min<unsigned long long>(*follow.Count - printed, kServerMaxCount);

                const ICommandSource::InvokeOutcome outcome = source.Invoke(kEventsWaitCommand, arguments);
                if (!outcome.Ok)
                {
                    err << "oloctl: " << outcome.Error << '\n';
                    return ExitCode::Connection;
                }

                if (IsErrorResult(outcome.Result))
                {
                    const std::string text = TextContent(outcome.Result);
                    err << "oloctl: " << kEventsWaitCommand << " failed: "
                        << (text.empty() ? outcome.Result.dump() : text) << '\n';
                    if (text.find("Unknown tool") != std::string::npos)
                    {
                        err << "oloctl: this editor predates the automation event bus (#1131) and has no "
                            << kEventsWaitCommand << ". Rebuild the editor from a tree that has it.\n";
                    }
                    return ExitCode::CommandError;
                }

                // A result without the contract's shape is not a quiet "no events":
                // continuing without a cursor would re-poll from "now" and skip
                // whatever the editor recorded meanwhile, silently.
                const auto structured = outcome.Result.find("structuredContent");
                unsigned long long lastId = 0;
                const Json* events = nullptr;
                if (structured != outcome.Result.end() && structured->is_object() &&
                    ReadUnsignedField(*structured, "lastId", lastId))
                {
                    if (const auto found = structured->find("events"); found != structured->end() && found->is_array())
                        events = &*found;
                }
                if (events == nullptr)
                {
                    err << "oloctl: " << kEventsWaitCommand
                        << " returned no usable payload (expected structuredContent with `events` and `lastId`): "
                        << outcome.Result.dump() << '\n';
                    return ExitCode::NoStructured;
                }

                if (unsigned long long dropped = 0; ReadUnsignedField(*structured, "dropped", dropped) && dropped > 0)
                {
                    unsigned long long before = lastId;
                    if (unsigned long long firstId = 0; !events->empty() && ReadUnsignedField(events->front(), "id", firstId))
                        before = firstId;
                    err << "oloctl: " << dropped << " event(s) were dropped before id " << before
                        << ": the cursor fell behind the editor's " << kRingWindow << "-record window.\n";
                }

                for (const Json& event : *events)
                {
                    out << event.dump() << '\n';
                    ++printed;
                    if (!follow.Until.empty() && CategoryOf(event) == follow.Until)
                    {
                        out.flush();
                        return ExitCode::Ok;
                    }
                    if (follow.Count && printed >= *follow.Count)
                    {
                        out.flush();
                        return ExitCode::Ok;
                    }
                }
                // A follower is read live: a line must not sit in a buffer for the
                // length of the next long-poll.
                out.flush();
                cursor = lastId;
            }
        }
    } // namespace

    std::span<const char* const> EventCategoryTokens()
    {
        return kCategoryTokens;
    }

    int RunEventsVerb(const CliOptions& options, const std::vector<std::string>& rest, ICommandSource& source,
                      std::ostream& out, std::ostream& err)
    {
        // rest[0] is "events".
        if (rest.size() >= 2 && rest[1] != "follow")
        {
            err << "oloctl: `events` has one sub-verb, `follow`; got '" << rest[1] << "'. Run `oloctl events --help`.\n";
            return ExitCode::Usage;
        }
        if (rest.size() < 2 || options.Help)
        {
            out << RenderEventsHelp();
            return ExitCode::Ok;
        }

        FollowOptions follow;
        std::string error;
        if (!ParseFollowOptions({ rest.begin() + 2, rest.end() }, follow, error))
        {
            err << "oloctl: " << error << "\nRun `oloctl events --help`.\n";
            return ExitCode::Usage;
        }
        return FollowEvents(options, follow, source, out, err);
    }
} // namespace OloCtl
