// httplib.h MUST be included before any header that pulls in <windows.h> so that
// <winsock2.h> wins the include race on Windows (windows.h would otherwise drag in
// the legacy <winsock.h> and cause redefinition errors). OloEditor has no PCH, so
// this single ordering rule is enough.
#include "OloEnginePCH.h"
#include <httplib.h>

#include "MCP/McpServer.h"
#include "MCP/McpEventStream.h"

#include "OloEngine/Core/Log.h"
#include "OloEngine/Debug/DiagnosticsEventLog.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Task/NamedThreads.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace OloEngine::MCP
{
    namespace
    {
        // ---- JSON-RPC 2.0 envelope helpers -------------------------------------
        constexpr int kParseError = -32700;
        constexpr int kInvalidRequest = -32600;
        constexpr int kMethodNotFound = -32601;
        constexpr int kInvalidParams = -32602;

        // DoS hardening (issue #306 item D): the largest request body POST /mcp will
        // buffer before dispatch. cpp-httplib defaults its payload cap to SIZE_MAX,
        // so without this a single request could buffer an arbitrarily large body
        // into memory. A real JSON-RPC call (or batch) is a few KB; 8 MiB is generous
        // headroom while still bounding the worst case.
        constexpr std::size_t kMaxRequestBytes = 8ull * 1024 * 1024;

        Json MakeResult(const Json& id, Json result)
        {
            return Json{ { "jsonrpc", "2.0" }, { "id", id }, { "result", std::move(result) } };
        }

        Json MakeError(const Json& id, int code, const std::string& message)
        {
            return Json{ { "jsonrpc", "2.0" },
                         { "id", id },
                         { "error", { { "code", code }, { "message", message } } } };
        }

        // ---- tools/call argument validation (issue #357 / #306 item D) ---------
        //
        // A deliberately small JSON-Schema-subset validator covering exactly what
        // the schema-builder DSL (McpSchemaBuilder.h) can emit — see
        // McpServer::ValidateArguments for the public contract. Everything here is
        // pure (Json + stdlib only) so the dispatch test binary exercises it
        // without an editor.

        // Render a JSON-Schema `type` token as an English noun phrase for messages.
        std::string SchemaTypeNoun(std::string_view type)
        {
            if (type == "integer")
                return "an integer";
            if (type == "number")
                return "a number";
            if (type == "string")
                return "a string";
            if (type == "boolean")
                return "a boolean";
            if (type == "array")
                return "an array";
            if (type == "object")
                return "an object";
            if (type == "null")
                return "null";
            return std::string(type);
        }

        // True if `value` satisfies the single JSON-Schema `type` token. "integer"
        // is strict (a JSON integer, not a 5.0-style float) — deliberate hardening:
        // every integer field the DSL declares (count / page / pageSize / bounds)
        // is sent as an integer by any sane client, and a strict check needs no
        // float-equality test (which SonarQube flags). "number" accepts either.
        bool ValueMatchesSchemaType(const Json& value, std::string_view type)
        {
            if (type == "object")
                return value.is_object();
            if (type == "array")
                return value.is_array();
            if (type == "string")
                return value.is_string();
            if (type == "boolean")
                return value.is_boolean();
            if (type == "number")
                return value.is_number(); // integer or float
            if (type == "integer")
                return value.is_number_integer();
            if (type == "null")
                return value.is_null();
            return true; // an unmodelled type keyword: don't reject what we can't check
        }

        // "'parent.child'"-style dotted path for nested fields; just "child" at the
        // top level (empty parent). Keeps an error pointing at the exact field.
        std::string JoinFieldPath(const std::string& parent, std::string_view name)
        {
            if (parent.empty())
                return std::string(name);
            return parent + "." + std::string(name);
        }

        // A quoted subject for a message: "'count'", or "value" for the unnamed
        // top-level object (which in practice always matches, so it rarely shows).
        std::string FieldSubject(const std::string& label)
        {
            return label.empty() ? std::string("value") : "'" + label + "'";
        }

        // Validate `value` against `schema` (`label` names `value` for messages),
        // returning the first error or nullopt. Forward-declared so the per-keyword
        // checks below can recurse into nested properties / array items. A non-object
        // schema is permissive (nothing modelled to enforce). Each keyword lives in
        // its own small checker so the orchestrator stays flat and easy to audit.
        std::optional<std::string> ValidateValueAgainstSchema(const Json& schema, const Json& value,
                                                              const std::string& label);

        // `type`: a single token, or an array of tokens (a union — match any one).
        std::optional<std::string> CheckType(const Json& schema, const Json& value, const std::string& label)
        {
            const auto it = schema.find("type");
            if (it == schema.end())
                return std::nullopt;

            if (it->is_string())
            {
                const std::string type = it->get<std::string>();
                if (!ValueMatchesSchemaType(value, type))
                    return FieldSubject(label) + " must be " + SchemaTypeNoun(type);
                return std::nullopt;
            }
            if (!it->is_array())
                return std::nullopt; // malformed `type` — not modelled

            bool matched = false;
            std::string nouns;
            for (const auto& entry : *it)
            {
                if (!entry.is_string())
                    continue;
                const std::string type = entry.get<std::string>();
                matched = matched || ValueMatchesSchemaType(value, type);
                if (!nouns.empty())
                    nouns += " or ";
                nouns += SchemaTypeNoun(type);
            }
            if (matched || nouns.empty())
                return std::nullopt;
            return FieldSubject(label) + " must be " + nouns;
        }

        // `enum`: the value must equal one of the allowed entries.
        std::optional<std::string> CheckEnum(const Json& schema, const Json& value, const std::string& label)
        {
            const auto it = schema.find("enum");
            if (it == schema.end() || !it->is_array())
                return std::nullopt;
            for (const auto& allowed : *it)
            {
                if (value == allowed)
                    return std::nullopt;
            }
            return FieldSubject(label) + " must be one of " + it->dump();
        }

        // `minimum` / `maximum` / `exclusiveMinimum` — only meaningful once the value
        // is a number. The comparisons are relational, not equality, so float-clean.
        std::optional<std::string> CheckNumericBounds(const Json& schema, const Json& value, const std::string& label)
        {
            if (!value.is_number())
                return std::nullopt;
            const double n = value.get<double>();
            // A non-finite number (NaN / ±Inf) slips past the relational bound checks
            // below — every comparison against NaN is false, and +Inf clears any
            // schema without a `maximum` — and is never valid input. Reject it up
            // front (the project rule to isfinite-validate floats from JSON/network).
            if (!std::isfinite(n))
                return FieldSubject(label) + " must be a finite number";
            if (const auto it = schema.find("minimum"); it != schema.end() && it->is_number() && n < it->get<double>())
                return FieldSubject(label) + " must be >= " + it->dump();
            if (const auto it = schema.find("maximum"); it != schema.end() && it->is_number() && n > it->get<double>())
                return FieldSubject(label) + " must be <= " + it->dump();
            if (const auto it = schema.find("exclusiveMinimum");
                it != schema.end() && it->is_number() && n <= it->get<double>())
                return FieldSubject(label) + " must be > " + it->dump();
            return std::nullopt;
        }

        // `required` + `additionalProperties` + per-property recursion.
        std::optional<std::string> CheckObjectConstraints(const Json& schema, const Json& value,
                                                          const std::string& label)
        {
            if (!value.is_object())
                return std::nullopt;

            const Json* properties = nullptr;
            if (const auto it = schema.find("properties"); it != schema.end() && it->is_object())
                properties = &(*it);

            if (const auto it = schema.find("required"); it != schema.end() && it->is_array())
            {
                for (const auto& entry : *it)
                {
                    if (!entry.is_string())
                        continue;
                    const std::string name = entry.get<std::string>();
                    if (!value.contains(name))
                        return "missing required property '" + JoinFieldPath(label, name) + "'";
                }
            }

            const auto addlIt = schema.find("additionalProperties");
            const bool closed = addlIt != schema.end() && addlIt->is_boolean() && !addlIt->get<bool>();
            const Json* addlSchema = (addlIt != schema.end() && addlIt->is_object()) ? &(*addlIt) : nullptr;

            for (const auto& [key, member] : value.items())
            {
                if (properties != nullptr && properties->contains(key))
                {
                    if (auto err = ValidateValueAgainstSchema((*properties)[key], member, JoinFieldPath(label, key)))
                        return err;
                }
                else if (closed)
                    return "unexpected property '" + JoinFieldPath(label, key) + "'";
                else if (addlSchema != nullptr)
                {
                    if (auto err = ValidateValueAgainstSchema(*addlSchema, member, JoinFieldPath(label, key)))
                        return err;
                }
            }
            return std::nullopt;
        }

        // `minItems` / `maxItems` + per-element `items` recursion.
        std::optional<std::string> CheckArrayConstraints(const Json& schema, const Json& value,
                                                         const std::string& label)
        {
            if (!value.is_array())
                return std::nullopt;

            const auto count = static_cast<std::int64_t>(value.size());
            if (const auto it = schema.find("minItems");
                it != schema.end() && it->is_number_integer() && count < it->get<std::int64_t>())
                return FieldSubject(label) + " must have at least " + it->dump() + " items";
            if (const auto it = schema.find("maxItems");
                it != schema.end() && it->is_number_integer() && count > it->get<std::int64_t>())
                return FieldSubject(label) + " must have at most " + it->dump() + " items";

            const auto it = schema.find("items");
            if (it == schema.end() || !it->is_object())
                return std::nullopt;
            for (std::size_t i = 0; i < value.size(); ++i)
            {
                const std::string elemLabel =
                    (label.empty() ? std::string("value") : label) + "[" + std::to_string(i) + "]";
                if (auto err = ValidateValueAgainstSchema(*it, value[i], elemLabel))
                    return err;
            }
            return std::nullopt;
        }

        std::optional<std::string> ValidateValueAgainstSchema(const Json& schema, const Json& value,
                                                              const std::string& label)
        {
            if (!schema.is_object())
                return std::nullopt;
            if (auto err = CheckType(schema, value, label))
                return err;
            if (auto err = CheckEnum(schema, value, label))
                return err;
            if (auto err = CheckNumericBounds(schema, value, label))
                return err;
            if (auto err = CheckObjectConstraints(schema, value, label))
                return err;
            return CheckArrayConstraints(schema, value, label);
        }

        // Reverse-DNS-namespaced `_meta` key carrying a tool's toolset (grouping
        // category) in tools/list / tools/search entries. `_meta` is the MCP-blessed
        // extension point (spec 2025-06-18), so surfacing the category there keeps
        // tools/list conformant — a strict client validating the Tool schema won't
        // reject an unknown top-level field.
        constexpr const char* kToolsetMetaKey = "io.oloengine/toolset";

        // ---- per-call progress/cancellation scope (issue #357 item B) ----------
        //
        // One tools/call executes synchronously on one dispatch thread, so a
        // thread_local scope gives EmitProgress / IsCurrentCallCancelled access to
        // the call's token, sink, and cancel flag without changing the ToolHandler
        // signature. Installed by HandleToolsCall around the handler; the sink is
        // installed by the ProcessRequestBody overload around the whole dispatch.
        // (A MarshalRead job runs on the game thread and thus sees a null scope —
        // progress is emitted from the handler thread by design.)
        struct ActiveCallScope
        {
            Json ProgressToken;                            // null => caller didn't opt in
            std::shared_ptr<std::atomic<bool>> CancelFlag; // shared with the in-flight registry
            f64 LastProgress = 0.0;                        // monotonicity guard
        };
        thread_local ActiveCallScope* t_ActiveCall = nullptr;
        thread_local const McpServer::NotificationSink* t_ActiveSink = nullptr;

        // RAII installer for the per-call scope.
        class CallScopeGuard
        {
          public:
            explicit CallScopeGuard(ActiveCallScope& scope)
            {
                t_ActiveCall = &scope;
            }
            ~CallScopeGuard()
            {
                t_ActiveCall = nullptr;
            }
            CallScopeGuard(const CallScopeGuard&) = delete;
            CallScopeGuard& operator=(const CallScopeGuard&) = delete;
        };

        // Canonical in-flight-registry key for a JSON-RPC id: the compact dump
        // distinguishes 5 from "5", so cancellation matches by exact value AND
        // type, as JSON-RPC id semantics require.
        std::string RequestIdKey(const Json& id)
        {
            return id.dump();
        }

        // Protocol revisions this server implements, newest first. 2025-11-25
        // (issue #357 P5b) is negotiable because every applicable delta vs
        // 2025-06-18 is covered: SEP-1303 input-validation-as-tool-error (see
        // HandleToolsCall), 403 on bad Origin (already), progress/cancellation
        // utilities (#357 item B), JSON Schema 2020-12-compatible tool schemas
        // (the builder emits a compatible subset), and the OAuth / elicitation /
        // sampling / tasks additions are optional capabilities we do not
        // advertise. Shared by HandleInitialize's negotiation and the transport's
        // MCP-Protocol-Version header check.
        constexpr std::array<std::string_view, 4> kSupportedProtocolVersions = {
            "2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05"
        };

        bool IsSupportedProtocolVersion(std::string_view version)
        {
            return std::find(kSupportedProtocolVersions.begin(), kSupportedProtocolVersions.end(), version) !=
                   kSupportedProtocolVersions.end();
        }

        // True when `body` is a single (non-batch) tools/call that opted into
        // progress via params._meta.progressToken (string or integer per spec) —
        // the gate for upgrading the POST response to an SSE stream. Parsing here
        // is bounded by the transport's kMaxRequestBytes cap; a malformed body
        // returns false and flows through the plain path's error handling.
        bool WantsProgressStream(const std::string& body)
        {
            const Json parsed = Json::parse(body, /*cb=*/nullptr, /*allow_exceptions=*/false);
            if (!parsed.is_object())
                return false;
            const auto methodIt = parsed.find("method");
            if (methodIt == parsed.end() || !methodIt->is_string() || methodIt->get<std::string>() != "tools/call")
                return false;
            const auto params = parsed.find("params");
            if (params == parsed.end() || !params->is_object())
                return false;
            const auto meta = params->find("_meta");
            if (meta == params->end() || !meta->is_object())
                return false;
            const auto token = meta->find("progressToken");
            return token != meta->end() && (token->is_string() || token->is_number());
        }

        // ASCII-lowercase a string for case-insensitive search/compare. The tool
        // surface is all ASCII identifiers and English prose, so a locale-independent
        // byte fold is correct and avoids std::tolower's locale baggage.
        std::string ToLowerAscii(std::string_view s)
        {
            std::string out(s);
            std::transform(out.begin(), out.end(), out.begin(),
                           [](unsigned char c)
                           { return static_cast<char>(std::tolower(c)); });
            return out;
        }

        // Serialize one registered tool into its MCP tools/list entry. Shared by
        // tools/list and the custom tools/search so both present byte-identical
        // entries; the only optional field beyond the spec basics is the toolset,
        // carried under `_meta` (omitted for uncategorized tools).
        Json BuildToolEntry(const ToolDef& tool)
        {
            Json entry;
            entry["name"] = tool.Name;
            // Top-level display title (spec 2025-06-18); omitted when unset so the
            // client falls back to the name.
            if (!tool.Title.empty())
                entry["title"] = tool.Title;
            entry["description"] = tool.Description;
            entry["inputSchema"] = tool.InputSchema.is_null()
                                       ? Json{ { "type", "object" } }
                                       : tool.InputSchema;
            // JSON Schema for the structured result (spec 2025-06-18); omitted unless
            // a non-empty object so text-only tools stay clean.
            if (tool.OutputSchema.is_object() && !tool.OutputSchema.empty())
                entry["outputSchema"] = tool.OutputSchema;
            // Behavioural hints (readOnlyHint, etc.); omitted unless a non-empty object.
            if (tool.Annotations.is_object() && !tool.Annotations.empty())
                entry["annotations"] = tool.Annotations;
            // Grouping category under the spec's `_meta` extension point; omitted for
            // uncategorized tools so their entry is unchanged from before toolsets.
            if (!tool.Toolset.empty())
                entry["_meta"] = Json{ { kToolsetMetaKey, tool.Toolset } };
            return entry;
        }

        // Random lowercase-hex string of `bytes` bytes (so 2*bytes characters).
        // Used for the auth token and session ids. std::random_device is seeded
        // per call; this is a localhost secret, not a cryptographic key exchange.
        std::string GenerateHexToken(std::size_t bytes)
        {
            std::random_device rd;
            std::mt19937_64 gen(((static_cast<u64>(rd()) << 32) ^ rd()) ^
                                static_cast<u64>(std::chrono::steady_clock::now().time_since_epoch().count()));
            std::uniform_int_distribution<u32> dist(0, 255);

            static constexpr char kHex[] = "0123456789abcdef";
            std::string out;
            out.reserve(bytes * 2);
            for (std::size_t i = 0; i < bytes; ++i)
            {
                const auto b = static_cast<u8>(dist(gen));
                out.push_back(kHex[b >> 4]);
                out.push_back(kHex[b & 0x0F]);
            }
            return out;
        }

        // Length-independent, content-constant-time string compare for the bearer
        // token, to avoid leaking the token length/prefix via response timing.
        bool ConstantTimeEquals(std::string_view a, std::string_view b)
        {
            // Fold length difference into the accumulator instead of early-out.
            u32 diff = static_cast<u32>(a.size() ^ b.size());
            const std::size_t n = std::min(a.size(), b.size());
            for (std::size_t i = 0; i < n; ++i)
                diff |= static_cast<u8>(a[i]) ^ static_cast<u8>(b[i]);
            return diff == 0;
        }

        // Write/remove the discovery file (host/port/token/url) used for attach
        // without copy-paste. Best-effort — failures are logged, never fatal.
        void WriteDiscoveryFile(const std::string& path, u16 port, const std::string& token)
        {
            if (path.empty())
                return;
            Json j;
            j["host"] = "127.0.0.1";
            j["port"] = port;
            j["token"] = token;
            j["url"] = "http://127.0.0.1:" + std::to_string(port) + "/mcp";

            std::ofstream out(path, std::ios::trunc | std::ios::binary);
            if (!out)
            {
                OLO_CORE_WARN("[MCP] Could not write discovery file: {}", path);
                return;
            }
            out << j.dump(2);
        }

        void RemoveDiscoveryFile(const std::string& path)
        {
            if (path.empty())
                return;
            std::error_code ec;
            std::filesystem::remove(std::filesystem::path(path), ec);
        }

        // Scrub absolute filesystem paths from text (Windows drive-letter paths and
        // POSIX /home//Users paths) so project layout / usernames don't leak when
        // redaction is enabled. Heuristic, best-effort.
        std::string RedactPathsInText(const std::string& text)
        {
            static const std::regex winPath(R"([A-Za-z]:[\\/][^\s"'<>|]*)");
            static const std::regex posixHome(R"(/(?:home|Users)/[^\s"'<>|]*)");
            std::string out = std::regex_replace(text, winPath, "<path>");
            out = std::regex_replace(out, posixHome, "<path>");
            return out;
        }

        // Apply redaction in place to every text content block of an MCP content array.
        void RedactContentArray(Json& content)
        {
            if (!content.is_array())
                return;
            for (auto& block : content)
            {
                if (block.is_object() && block.value("type", std::string{}) == "text" && block.contains("text") && block["text"].is_string())
                    block["text"] = RedactPathsInText(block["text"].get<std::string>());
            }
        }

        // Apply redaction in place to every string leaf of a structured-content
        // value, recursing through objects and arrays. The text mirror in `content`
        // is redacted by RedactContentArray; this keeps the same path-scrubbing
        // guarantee for the parallel `structuredContent` (e.g. asset paths embedded
        // in a serialized component dump) before it leaves the process.
        void RedactStructuredContent(Json& value)
        {
            if (value.is_string())
                value = RedactPathsInText(value.get<std::string>());
            else if (value.is_object() || value.is_array())
            {
                for (auto& child : value)
                    RedactStructuredContent(child);
            }
        }

        // ---- SSE server-push stream (issue #306 item B) -------------------------

        // Worst-case push latency: the content provider is invoked back-to-back by
        // httplib, so the stream loop sleeps this long each cycle to avoid busy-spin.
        // Imperceptible for a live-watch loop, and bounds how long after Stop() a
        // stream takes to notice the server is gone.
        constexpr std::chrono::milliseconds kStreamPollInterval{ 250 };
        // Idle keep-alive cadence: emit an SSE comment after this long with no event,
        // so intermediaries see traffic and a dead client is detected via a failed write.
        constexpr std::chrono::seconds kStreamHeartbeat{ 15 };

        // One service cycle of a GET /mcp push stream, run on an httplib worker
        // thread. Drains every diagnostics event newer than `cursor` from the
        // (mutex-guarded, lock-safe) ring buffer and writes each as an MCP
        // notification SSE frame, advances the cursor, then emits a keep-alive
        // heartbeat once the stream has been idle. Returns false when a write fails
        // (client gone) so the caller ends the stream. Reads only the lock-safe event
        // log — no main-thread marshal needed (mirrors olo_events_tail).
        [[nodiscard]] bool ServiceEventStream(httplib::DataSink& sink, u64& cursor,
                                              std::chrono::steady_clock::time_point& lastWrite)
        {
            DiagnosticEventQuery query;
            query.SinceId = cursor;
            query.MaxCount = 0; // deliver every new event — no newest-N cap on a live stream.
            const DiagnosticEventQueryResult snap = DiagnosticsEventLog::Get().QueryWithCursor(query);
            for (const auto& event : snap.Events)
            {
                const std::string frame = FormatSseEvent(event.Id, MakeEventNotification(event));
                if (!sink.write(frame.data(), frame.size()))
                    return false;
                lastWrite = std::chrono::steady_clock::now();
            }
            // Advance to the buffer head (past events that were filtered or none) so a
            // later cycle never rescans the same ids — same cursor semantics as
            // olo_events_tail's lastId.
            cursor = snap.LastId;

            if (std::chrono::steady_clock::now() - lastWrite >= kStreamHeartbeat)
            {
                const std::string hb = FormatSseComment("keep-alive");
                if (!sink.write(hb.data(), hb.size()))
                    return false;
                lastWrite = std::chrono::steady_clock::now();
            }
            return true;
        }

    } // namespace

    // ---- ToolResult ------------------------------------------------------------

    ToolResult ToolResult::Text(const std::string& text)
    {
        ToolResult r;
        r.Content = Json::array({ Json{ { "type", "text" }, { "text", text } } });
        r.IsError = false;
        return r;
    }

    ToolResult ToolResult::Error(const std::string& message)
    {
        ToolResult r;
        r.Content = Json::array({ Json{ { "type", "text" }, { "text", message } } });
        r.IsError = true;
        return r;
    }

    ToolResult ToolResult::Structured(const Json& data)
    {
        ToolResult r;
        // Mirror the structured object into a text block (spec: keep a back-compat
        // serialization in `content` for clients that don't parse structuredContent).
        r.Content = Json::array({ Json{ { "type", "text" }, { "text", data.dump(2) } } });
        r.StructuredContent = data;
        r.IsError = false;
        return r;
    }

    // ---- McpServer -------------------------------------------------------------

    McpServer::McpServer(EditorMcpContext context)
        : m_Context(std::move(context))
    {
    }

    McpServer::~McpServer()
    {
        Stop();
    }

    bool McpServer::IsValidToolName(std::string_view name)
    {
        // MCP places no hard length cap on tool names, but the broader spec uses
        // 1..128 for identifier-like fields and clients/UIs assume a bounded,
        // shell-safe character set. Enforce 1..128 chars of [A-Za-z0-9_.-].
        if (name.empty() || name.size() > 128)
            return false;
        for (const char c : name)
        {
            const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                            (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-';
            if (!ok)
                return false;
        }
        return true;
    }

    std::optional<std::string> McpServer::ValidateArguments(const Json& schema, const Json& args)
    {
        // An absent / non-object / empty schema declares no constraints, so there is
        // nothing to enforce — treat it as permissive (the caller also skips such
        // schemas, but guarding here keeps the helper safe to call unconditionally).
        if (!schema.is_object() || schema.empty())
            return std::nullopt;
        return ValidateValueAgainstSchema(schema, args, std::string{});
    }

    void McpServer::RegisterTool(ToolDef tool)
    {
        // Fail loudly at registration: every tool is registered in code at startup,
        // so a malformed name is a programmer error, not a runtime condition. Catch
        // it here instead of letting clients choke on the name later.
        OLO_CORE_VERIFY(IsValidToolName(tool.Name),
                        "[MCP] Invalid tool name '{}': must be 1-128 chars of [A-Za-z0-9_.-].", tool.Name);
        m_Tools.push_back(std::move(tool));
    }

    void McpServer::RegisterResource(ResourceDef resource)
    {
        m_Resources.push_back(std::move(resource));
    }

    void McpServer::RegisterPrompt(PromptDef prompt)
    {
        m_Prompts.push_back(std::move(prompt));
    }

    bool McpServer::Start(u16 port)
    {
        if (m_Running.load(std::memory_order_acquire))
            return false;

        // Clear any consent-abort latch left by a prior Stop() so this fresh session
        // can prompt again (the queue was already drained as the old workers unblocked).
        {
            std::lock_guard lock(m_ConsentMutex);
            m_ConsentAborting = false;
        }

        m_Token = GenerateHexToken(16);

        m_Http = CreateScope<httplib::Server>();

        // Bound the buffered request body so an oversized POST is rejected (413)
        // before any handler runs, instead of being read entirely into memory.
        m_Http->set_payload_max_length(kMaxRequestBytes);

        // Give httplib-generated error responses (notably the 413 from the cap above,
        // which short-circuits before HandlePost) a small JSON-RPC error body. Our
        // own handler errors already carry their envelope, so only fill an empty body.
        m_Http->set_error_handler([](const httplib::Request&, httplib::Response& res)
                                  {
            if (res.body.empty())
            {
                const Json body = MakeError(Json(nullptr), kInvalidRequest,
                                            "Request rejected (HTTP " + std::to_string(res.status) + ")");
                res.set_content(body.dump(), "application/json");
            } });

        m_Http->Post("/mcp", [this](const httplib::Request& req, httplib::Response& res)
                     { HandlePost(req, res); });

        // Streamable-HTTP GET opens a persistent server-push SSE stream: new
        // diagnostics events are pushed as MCP notifications (issue #306 item B).
        m_Http->Get("/mcp", [this](const httplib::Request& req, httplib::Response& res)
                    { HandleGetStream(req, res); });

        // Explicit session teardown.
        m_Http->Delete("/mcp", [this](const httplib::Request& req, httplib::Response& res)
                       {
            if (req.has_header("Mcp-Session-Id"))
            {
                const std::string sid = req.get_header_value("Mcp-Session-Id");
                std::lock_guard lock(m_SessionMutex);
                m_Sessions.erase(sid);
            }
            res.status = 200; });

        if (!m_Http->bind_to_port("127.0.0.1", static_cast<int>(port)))
        {
            OLO_CORE_ERROR("[MCP] Failed to bind 127.0.0.1:{} — is the port already in use?", port);
            m_Http.reset();
            m_Token.clear();
            return false;
        }

        m_Port = port;
        m_Running.store(true, std::memory_order_release);
        m_ListenThread = std::thread([this]
                                     {
            m_Http->listen_after_bind();
            m_Running.store(false, std::memory_order_release); });

        WriteDiscoveryFile(DiscoveryFilePath(m_Port), m_Port, m_Token);

        OLO_CORE_INFO("[MCP] Read-only diagnostics server listening on http://127.0.0.1:{}/mcp", port);
        return true;
    }

    void McpServer::Stop()
    {
        if (!m_Http)
            return;

        // Signal first so any handler blocked in MarshalRead aborts promptly
        // instead of deadlocking against this thread (Stop runs on the game thread).
        m_Running.store(false, std::memory_order_release);

        // Release any worker blocked in RequestConsent as a Deny BEFORE joining the
        // http worker pool below — otherwise the join would wait forever on a thread
        // parked on a consent that the (now-gone) editor UI can never resolve.
        {
            std::lock_guard lock(m_ConsentMutex);
            m_ConsentAborting = true;
        }
        m_ConsentCv.notify_all();

        m_Http->stop();
        if (m_ListenThread.joinable())
            m_ListenThread.join();
        // Destroying the Server joins its internal worker pool, so no handler is
        // running once this returns — safe to clear the token afterwards.
        m_Http.reset();

        RemoveDiscoveryFile(DiscoveryFilePath(m_Port));

        {
            std::lock_guard lock(m_SessionMutex);
            m_Sessions.clear();
        }
        m_Token.clear();

        // Drop any ephemeral sun-direction override an agent left active
        // (olo_scene_set_time_of_day / olo_scene_set_sun_angle, #316 Part 4) so the
        // editor returns to the authored procedural-sky sun once its agent is gone.
        // Stop() runs on the game thread, so touching renderer session state here is
        // safe; a no-op when no override is active.
        Renderer3D::ClearSunDirectionOverride();

        OLO_CORE_INFO("[MCP] Diagnostics server stopped");
    }

    std::string McpServer::DiscoveryFilePath(u16 port)
    {
        // An explicit override wins: the launching tool picks the exact path it will
        // read back, so parallel worktree editors never collide regardless of port.
        if (const char* overridePath = std::getenv("OLO_MCP_DISCOVERY_FILE");
            overridePath != nullptr && *overridePath != '\0')
            return overridePath;

        std::error_code ec;
        const std::filesystem::path dir = std::filesystem::temp_directory_path(ec);
        if (ec)
            return {};

        // Default port keeps the legacy single-file name (back-compat for the panel /
        // manual attach and the docs); any other port namespaces by port so two
        // editors on distinct ports don't overwrite each other's host/token.
        if (port == DefaultPort)
            return (dir / "oloengine-mcp.json").string();
        return (dir / ("oloengine-mcp-" + std::to_string(port) + ".json")).string();
    }

    Json McpServer::MarshalRead(const std::function<Json()>& readJob, std::chrono::milliseconds timeout)
    {
        auto promise = std::make_shared<std::promise<Json>>();
        std::future<Json> future = promise->get_future();
        const bool stillRunning = m_Running.load(std::memory_order_acquire);

        // Enqueue onto the game thread; it drains this at the next frame boundary
        // (Application::Run, before the scene is stepped) — a consistent snapshot.
        OloEngine::Tasks::EnqueueGameThreadTask(
            [promise, readJob]()
            {
                try
                {
                    promise->set_value(readJob());
                }
                catch (...)
                {
                    promise->set_exception(std::current_exception());
                }
            },
            "MCP_MainThreadRead");

        if (!stillRunning)
            throw std::runtime_error("MCP server is not running");

        const auto deadline = std::chrono::steady_clock::now() + timeout;
        for (;;)
        {
            if (future.wait_for(std::chrono::milliseconds(50)) == std::future_status::ready)
                return future.get();

            // If the server is being torn down, bail rather than block teardown.
            if (!m_Running.load(std::memory_order_acquire))
                throw std::runtime_error("MCP server stopping; main-thread read aborted");

            if (std::chrono::steady_clock::now() >= deadline)
                throw std::runtime_error("Timed out waiting for the editor main thread (is the editor responsive?)");
        }
    }

    namespace
    {
        // A compact, human-readable rendering of a tool's arguments for the consent
        // modal: one "key = value" per line (strings unquoted, everything else via a
        // compact JSON dump). Generic over any ProjectWrite tool's argument shape, so
        // the dialog needs no per-tool knowledge.
        std::string BuildConsentSummary(const Json& arguments)
        {
            if (!arguments.is_object() || arguments.empty())
                return "(no arguments)";

            std::string out;
            for (auto it = arguments.begin(); it != arguments.end(); ++it)
            {
                out += it.key();
                out += " = ";
                const Json& value = it.value();
                out += value.is_string() ? value.get<std::string>() : value.dump();
                out += '\n';
            }
            if (!out.empty() && out.back() == '\n')
                out.pop_back();
            return out;
        }
    } // namespace

    void McpServer::SetWriteConsentMode(WriteConsentMode mode)
    {
        m_ConsentMode.store(mode);

        // Drain any in-flight prompts to match the new mode: AllowSession approves
        // them (the human just said "allow everything"), Disabled denies them. Prompt
        // leaves them awaiting a per-action decision. Either way, wake the waiters.
        if (mode == WriteConsentMode::Prompt)
            return;

        const ConsentDecision resolution =
            (mode == WriteConsentMode::AllowSession) ? ConsentDecision::Approve : ConsentDecision::Deny;
        {
            std::lock_guard lock(m_ConsentMutex);
            for (ConsentEntry& entry : m_ConsentQueue)
            {
                if (entry.Decision == ConsentDecision::Pending)
                    entry.Decision = resolution;
            }
        }
        m_ConsentCv.notify_all();
    }

    std::vector<McpServer::PendingConsent> McpServer::PendingConsents() const
    {
        std::vector<PendingConsent> pending;
        std::lock_guard lock(m_ConsentMutex);
        for (const ConsentEntry& entry : m_ConsentQueue)
        {
            if (entry.Decision != ConsentDecision::Pending)
                continue; // already resolved, waiting to be reaped by its handler thread
            pending.push_back(PendingConsent{ entry.Id, entry.ToolName, entry.ToolTitle, entry.Summary });
        }
        return pending;
    }

    void McpServer::ResolveConsent(u64 id, ConsentDecision decision)
    {
        // ApproveAll flips the whole session to auto-approve, which also resolves this
        // prompt and every other pending one — route it through SetWriteConsentMode so
        // the mode change and the drain happen atomically together. But only when `id`
        // still names a live pending prompt: a stale/missing id is a no-op (same
        // contract as the Approve/Deny path), so a late click on an already-reaped
        // request can't silently disable consent for the rest of the session.
        if (decision == ConsentDecision::ApproveAll)
        {
            {
                std::lock_guard lock(m_ConsentMutex);
                const bool present = std::any_of(m_ConsentQueue.begin(), m_ConsentQueue.end(),
                                                 [id](const ConsentEntry& entry)
                                                 { return entry.Id == id && entry.Decision == ConsentDecision::Pending; });
                if (!present)
                    return;
            }
            SetWriteConsentMode(WriteConsentMode::AllowSession);
            return;
        }

        {
            std::lock_guard lock(m_ConsentMutex);
            for (ConsentEntry& entry : m_ConsentQueue)
            {
                if (entry.Id == id && entry.Decision == ConsentDecision::Pending)
                {
                    entry.Decision = decision;
                    break;
                }
            }
        }
        m_ConsentCv.notify_all();
    }

    ConsentDecision McpServer::RequestConsent(const ToolDef& tool, const Json& arguments)
    {
        const auto timeout = std::chrono::milliseconds(m_ConsentTimeoutMs.load());

        u64 myId = 0;
        {
            std::unique_lock lock(m_ConsentMutex);
            if (m_ConsentAborting)
                return ConsentDecision::Deny;
            // A concurrent mode change may have already settled the outcome before we
            // enqueue anything — honour it without prompting.
            const WriteConsentMode mode = m_ConsentMode.load();
            if (mode == WriteConsentMode::AllowSession)
                return ConsentDecision::Approve;
            if (mode == WriteConsentMode::Disabled)
                return ConsentDecision::Deny;

            myId = m_NextConsentId++;
            ConsentEntry entry;
            entry.Id = myId;
            entry.ToolName = tool.Name;
            entry.ToolTitle = tool.Title.empty() ? tool.Name : tool.Title;
            entry.Summary = BuildConsentSummary(arguments);
            m_ConsentQueue.push_back(std::move(entry));

            const bool resolved = m_ConsentCv.wait_for(lock, timeout, [this, myId]
                                                       {
                                                           if (m_ConsentAborting)
                                                               return true;
                                                           for (const ConsentEntry& e : m_ConsentQueue)
                                                           {
                                                               if (e.Id == myId)
                                                                   return e.Decision != ConsentDecision::Pending;
                                                           }
                                                           return true; // entry vanished => treat as resolved
                                                       });

            // Reap our entry and read its decision under the same lock.
            ConsentDecision decision = ConsentDecision::Pending;
            for (auto it = m_ConsentQueue.begin(); it != m_ConsentQueue.end(); ++it)
            {
                if (it->Id == myId)
                {
                    decision = it->Decision;
                    m_ConsentQueue.erase(it);
                    break;
                }
            }

            if (m_ConsentAborting)
                return ConsentDecision::Deny;
            if (!resolved || decision == ConsentDecision::Pending)
                return ConsentDecision::Timeout;
            return decision;
        }
    }

    void McpServer::HandlePost(const httplib::Request& req, httplib::Response& res)
    {
        const auto sendJson = [&res](const Json& body, int status)
        {
            res.status = status;
            res.set_content(body.dump(), "application/json");
        };

        // 1. Origin check (DNS-rebinding defence).
        if (req.has_header("Origin") && !IsOriginAllowed(req.get_header_value("Origin")))
        {
            res.status = 403;
            sendJson(MakeError(Json(nullptr), kInvalidRequest, "Origin not allowed"), 403);
            return;
        }

        // 2. Bearer-token auth.
        if (!CheckAuth(req))
        {
            res.set_header("WWW-Authenticate", "Bearer");
            sendJson(MakeError(Json(nullptr), kInvalidRequest, "Unauthorized"), 401);
            return;
        }

        // 3. Session validation (only when the client presents one).
        if (req.has_header("Mcp-Session-Id"))
        {
            const std::string sid = req.get_header_value("Mcp-Session-Id");
            std::lock_guard lock(m_SessionMutex);
            if (!m_Sessions.contains(sid))
            {
                // Unknown/expired session — tell the client to re-initialize.
                sendJson(MakeError(Json(nullptr), kInvalidRequest, "Unknown session"), 404);
                return;
            }
        }

        // 3b. Protocol-version header (Streamable HTTP, spec 2025-06-18+): when
        // the client stamps MCP-Protocol-Version on a post-initialize request,
        // reject a version we do not support with 400, per spec. An absent
        // header is fine (the spec says assume an older revision).
        if (req.has_header("MCP-Protocol-Version") &&
            !IsSupportedProtocolVersion(req.get_header_value("MCP-Protocol-Version")))
        {
            sendJson(MakeError(Json(nullptr), kInvalidRequest,
                               "Unsupported MCP-Protocol-Version header"),
                     400);
            return;
        }

        // 4. Streamable-HTTP upgrade (issue #357 item B): a single tools/call that
        // opts into progress (params._meta.progressToken) and accepts SSE gets its
        // response as a text/event-stream — progress frames as they happen, then
        // the final response frame. Everything else keeps the plain-JSON path.
        if (req.get_header_value("Accept").find("text/event-stream") != std::string::npos &&
            WantsProgressStream(req.body))
        {
            HandleStreamingPost(req.body, res);
            return;
        }

        // 4-5. Parse + route the JSON-RPC body. All framing (parse error, batch
        // handling, notification suppression, the initialize session-id side
        // effect) lives in the transport-agnostic seam so it can be unit tested.
        const FramedResponse framed = ProcessRequestBody(req.body);

        // Echo the freshly minted session id on a successful initialize so
        // subsequent requests can be correlated (and old sessions invalidated
        // across server restarts).
        if (!framed.SessionId.empty())
            res.set_header("Mcp-Session-Id", framed.SessionId);

        if (framed.Body.is_null())
        {
            res.status = framed.Status; // 202 — notification / all-notification batch
            return;
        }
        sendJson(framed.Body, framed.Status);
    }

    void McpServer::HandleGetStream(const httplib::Request& req, httplib::Response& res)
    {
        // Same gates as HandlePost: Origin (DNS-rebinding defence), bearer auth, and
        // session validation when the client presents an Mcp-Session-Id.
        if (req.has_header("Origin") && !IsOriginAllowed(req.get_header_value("Origin")))
        {
            res.status = 403;
            return;
        }
        if (!CheckAuth(req))
        {
            res.set_header("WWW-Authenticate", "Bearer");
            res.status = 401;
            return;
        }
        if (req.has_header("Mcp-Session-Id"))
        {
            const std::string sid = req.get_header_value("Mcp-Session-Id");
            std::lock_guard lock(m_SessionMutex);
            if (!m_Sessions.contains(sid))
            {
                res.status = 404;
                return;
            }
        }
        if (req.has_header("MCP-Protocol-Version") &&
            !IsSupportedProtocolVersion(req.get_header_value("MCP-Protocol-Version")))
        {
            res.status = 400;
            return;
        }

        // Where to resume: SSE reconnection replays the last id via Last-Event-ID, so
        // honour it to resume without gaps. Otherwise start at the current head so a
        // fresh subscriber receives only NEW events — not a backlog flood (the ring
        // holds 512). This is the per-connection cursor the plan calls for.
        u64 startCursor = DiagnosticsEventLog::Get().LastId();
        if (req.has_header("Last-Event-ID"))
        {
            // Resume only on a cleanly-parsed cursor. from_chars rejects a leading sign
            // and trailing junk, unlike std::stoull which would accept "123abc" as 123
            // or "-1" as a wrapped ULLONG_MAX (silently starving the stream). Requiring
            // full-string consumption means a malformed header falls back to the head.
            const std::string lastEventId = req.get_header_value("Last-Event-ID");
            u64 parsed = 0;
            const char* const first = lastEventId.data();
            const char* const last = first + lastEventId.size();
            if (const auto [ptr, ec] = std::from_chars(first, last, parsed); ec == std::errc{} && ptr == last)
                startCursor = parsed;
        }

        res.set_header("Cache-Control", "no-cache");
        // Conventional SSE hint: tell any intermediary not to buffer the stream.
        res.set_header("X-Accel-Buffering", "no");

        res.set_chunked_content_provider(
            "text/event-stream",
            [this, cursor = startCursor, lastWrite = std::chrono::steady_clock::now(), greeted = false](
                std::size_t /*offset*/, httplib::DataSink& sink) mutable -> bool
            {
                // Server tearing down: end the stream gracefully so the worker thread
                // is free to be joined by Stop().
                if (!m_Running.load(std::memory_order_acquire))
                {
                    sink.done();
                    return true;
                }
                if (!sink.is_writable())
                    return false; // client gone

                // One-time greeting comment so the client sees the stream is open
                // before the first event, and as an immediate disconnect probe.
                if (!greeted)
                {
                    const std::string hello = FormatSseComment("olo-mcp event stream connected");
                    if (!sink.write(hello.data(), hello.size()))
                        return false;
                    greeted = true;
                    lastWrite = std::chrono::steady_clock::now();
                }

                if (!ServiceEventStream(sink, cursor, lastWrite))
                    return false;

                // Pace the loop (httplib calls the provider back-to-back).
                std::this_thread::sleep_for(kStreamPollInterval);
                return true;
            },
            [this](bool /*success*/)
            {
                m_ActiveStreams.fetch_sub(1, std::memory_order_relaxed);
            });
        // Count the stream only once the provider + its releaser are registered: if
        // set_chunked_content_provider had thrown, the releaser would never run, so an
        // increment before it could leak. The provider/releaser run later (during
        // response writing, after this handler returns), so the matching decrement
        // can't race ahead of this increment.
        m_ActiveStreams.fetch_add(1, std::memory_order_relaxed);
    }

    void McpServer::EmitProgress(f64 progress, f64 total, const std::string& message) const
    {
        ActiveCallScope* scope = t_ActiveCall;
        const NotificationSink* sink = t_ActiveSink;
        if (scope == nullptr || scope->ProgressToken.is_null() || sink == nullptr || !(*sink))
            return; // caller didn't opt in, or the transport has nowhere to put it.

        // The spec requires progress to increase with each notification; clamp a
        // misbehaving emitter forward rather than sending a non-monotonic value.
        if (progress <= scope->LastProgress)
            progress = scope->LastProgress + 1.0;
        scope->LastProgress = progress;

        Json params{ { "progressToken", scope->ProgressToken }, { "progress", progress } };
        if (total >= 0.0)
            params["total"] = total;
        if (!message.empty())
            params["message"] = message;
        (*sink)(Json{ { "jsonrpc", "2.0" }, { "method", "notifications/progress" }, { "params", std::move(params) } });
    }

    bool McpServer::IsCurrentCallCancelled() const
    {
        const ActiveCallScope* scope = t_ActiveCall;
        return scope != nullptr && scope->CancelFlag && scope->CancelFlag->load(std::memory_order_acquire);
    }

    void McpServer::HandleStreamingPost(const std::string& body, httplib::Response& res)
    {
        res.set_header("Cache-Control", "no-cache");
        res.set_header("X-Accel-Buffering", "no");
        res.set_chunked_content_provider(
            "text/event-stream",
            [this, body](std::size_t /*offset*/, httplib::DataSink& sink) -> bool
            {
                // Single invocation does the whole call: dispatch synchronously on
                // THIS worker thread (so the handler's MarshalRead contract is
                // unchanged), writing each progress notification as an SSE frame
                // the moment the tool emits it, then the final response frame.
                const NotificationSink notifier = [&sink](const Json& notification)
                {
                    const std::string frame = FormatSseData(notification);
                    (void)sink.write(frame.data(), frame.size()); // best-effort: a gone client just drops frames
                };
                const FramedResponse framed = ProcessRequestBody(body, notifier);

                // A cancelled call gets NO response frame (spec: the server
                // SHOULD NOT respond to a cancelled request) — the stream simply
                // ends. Everything else (result or error) is the final frame.
                const bool cancelled = framed.Body.is_object() && framed.Body.contains("error") &&
                                       framed.Body["error"].is_object() &&
                                       framed.Body["error"].value("code", 0) == kRequestCancelledCode;
                if (!framed.Body.is_null() && !cancelled)
                {
                    const std::string frame = FormatSseData(framed.Body);
                    (void)sink.write(frame.data(), frame.size());
                }
                sink.done();
                return true;
            });
    }

    Json McpServer::HandleMessage(const Json& message)
    {
        return DispatchRpc(message);
    }

    McpServer::FramedResponse McpServer::ProcessRequestBody(const std::string& body, const NotificationSink& sink)
    {
        // Install the sink for the duration of the dispatch; HandleToolsCall's
        // per-call scope picks it up (thread_local — dispatch is synchronous on
        // this thread). RAII so an escaping exception still restores the previous
        // value (nesting then degrades sanely too).
        struct SinkGuard
        {
            const NotificationSink* Previous;
            explicit SinkGuard(const NotificationSink& s)
                : Previous(t_ActiveSink)
            {
                t_ActiveSink = &s;
            }
            ~SinkGuard()
            {
                t_ActiveSink = Previous;
            }
            SinkGuard(const SinkGuard&) = delete;
            SinkGuard& operator=(const SinkGuard&) = delete;
        };
        const SinkGuard guard(sink);
        return ProcessRequestBody(body);
    }

    McpServer::FramedResponse McpServer::ProcessRequestBody(const std::string& body)
    {
        FramedResponse out;

        // Parse the JSON-RPC body.
        Json parsed;
        try
        {
            parsed = Json::parse(body);
        }
        catch (const std::exception&)
        {
            out.Body = MakeError(Json(nullptr), kParseError, "Parse error");
            return out;
        }

        // Batch: array of messages → array of responses (notifications drop out).
        if (parsed.is_array())
        {
            // An empty batch is itself an invalid JSON-RPC request (spec §6).
            if (parsed.empty())
            {
                out.Body = MakeError(Json(nullptr), kInvalidRequest, "Invalid Request");
                return out;
            }
            Json responses = Json::array();
            for (const auto& message : parsed)
            {
                Json response = DispatchRpc(message);
                if (!response.is_null())
                    responses.push_back(std::move(response));
            }
            if (responses.empty())
            {
                out.Status = 202; // all notifications — nothing to return
                return out;
            }
            out.Body = std::move(responses);
            return out;
        }

        // Single message.
        // Detect a successful initialize for the session-id side effect. Read
        // "method" defensively (see DispatchRpc): value() would throw on a
        // non-string method.
        std::string method;
        if (parsed.is_object() && parsed.contains("method") && parsed["method"].is_string())
            method = parsed["method"].get<std::string>();
        Json response = DispatchRpc(parsed);

        // A successful initialize mints + registers a session id; the transport
        // surfaces it in the Mcp-Session-Id header.
        if (method == "initialize" && response.contains("result"))
        {
            std::string sid = GenerateHexToken(16);
            {
                std::lock_guard lock(m_SessionMutex);
                m_Sessions.insert(sid);
            }
            out.SessionId = std::move(sid);
        }

        if (response.is_null())
        {
            out.Status = 202; // notification — nothing to return
            return out;
        }
        out.Body = std::move(response);
        return out;
    }

    Json McpServer::DispatchRpc(const Json& request)
    {
        if (!request.is_object())
            return MakeError(Json(nullptr), kInvalidRequest, "Invalid Request");

        const bool hasId = request.contains("id");
        const Json id = hasId ? request["id"] : Json(nullptr);

        // Read "method" defensively: nlohmann's value() throws type_error.302 when
        // the key is present but not a string, so a malformed `"method": 123` would
        // escape as an exception instead of a clean JSON-RPC error. Treat any
        // non-string (or absent) method as missing — handled as Invalid Request below.
        std::string method;
        if (request.contains("method") && request["method"].is_string())
            method = request["method"].get<std::string>();

        // Notifications (no id) get no response. `notifications/cancelled` is the
        // one notification with a side effect (issue #357 item B): flag the named
        // in-flight tools/call so its handler can stop cooperatively. An unknown /
        // already-finished requestId is a spec-sanctioned no-op (the race is
        // inherent — cancellation "MAY arrive after processing completes").
        // Matching is by exact id value (see RequestIdKey), and only tools/call
        // ids are ever registered, so an `initialize` id can never match — the
        // spec's "MUST NOT cancel initialize" holds by construction.
        if (!hasId)
        {
            if (method == "notifications/cancelled" && request.contains("params") && request["params"].is_object() &&
                request["params"].contains("requestId"))
            {
                std::shared_ptr<std::atomic<bool>> flag;
                {
                    std::lock_guard lock(m_InFlightMutex);
                    const auto it = m_InFlightCalls.find(RequestIdKey(request["params"]["requestId"]));
                    if (it != m_InFlightCalls.end())
                        flag = it->second;
                }
                if (flag)
                    flag->store(true, std::memory_order_release);
            }
            return Json(nullptr);
        }

        if (method.empty())
            return MakeError(id, kInvalidRequest, "Invalid Request: missing method");

        if (method == "initialize")
            return HandleInitialize(id, request.value("params", Json::object()));
        if (method == "ping")
            return MakeResult(id, Json::object());
        if (method == "tools/list")
            return HandleToolsList(id);
        if (method == "tools/search")
            return HandleToolsSearch(id, request.value("params", Json::object()));
        if (method == "tools/call")
            return HandleToolsCall(id, request.value("params", Json::object()));
        if (method == "resources/list")
            return HandleResourcesList(id);
        if (method == "resources/read")
            return HandleResourcesRead(id, request.value("params", Json::object()));
        if (method == "prompts/list")
            return HandlePromptsList(id);
        if (method == "prompts/get")
            return HandlePromptsGet(id, request.value("params", Json::object()));

        return MakeError(id, kMethodNotFound, "Method not found: " + method);
    }

    Json McpServer::HandleInitialize(const Json& id, const Json& params)
    {
        // Echo the client's protocol version when we recognise it, else advertise
        // our latest (2025-11-25 — issue #357 P5b; the applicable spec deltas are
        // covered, see kSupportedProtocolVersions). Transport framing is identical
        // across these revisions.
        std::string version{ kSupportedProtocolVersions.front() };
        if (params.contains("protocolVersion") && params["protocolVersion"].is_string())
        {
            const std::string requested = params["protocolVersion"].get<std::string>();
            if (IsSupportedProtocolVersion(requested))
                version = requested;
        }

        Json result;
        result["protocolVersion"] = version;
        // `logging` is advertised because the GET /mcp SSE stream pushes diagnostics
        // events as `notifications/message` log notifications (issue #306 item B).
        result["capabilities"] = Json{ { "tools", { { "listChanged", false } } },
                                       { "resources", { { "subscribe", false }, { "listChanged", false } } },
                                       { "prompts", { { "listChanged", false } } },
                                       { "logging", Json::object() } };
        // `description` on Implementation is a 2025-11-25 addition (aligns with
        // the MCP registry's server.json shape); older clients ignore it.
        result["serverInfo"] = Json{ { "name", "OloEditor" },
                                     { "title", "OloEngine Editor Diagnostics" },
                                     { "description",
                                       "Read-only diagnostics for a running OloEngine editor session: logs, "
                                       "scene/ECS state, performance, shaders, assets, physics, screenshots, "
                                       "and opt-in consented editor writes." },
                                     { "version", "0.0.1" } };
        result["instructions"] =
            "Read-only diagnostics for a running OloEngine editor session. Use olo_log_tail "
            "to see the most recent engine log messages, olo_events_tail for a 'what just "
            "happened?' timeline (scene load, play/stop, entity spawn/destroy, asset reload, "
            "script error — poll incrementally with sinceId), and olo_scene_summary to inspect "
            "the active scene. Everything exposed here is read-only — no tool mutates the project.";
        return MakeResult(id, result);
    }

    Json McpServer::HandleToolsList(const Json& id) const
    {
        Json tools = Json::array();
        for (const auto& tool : m_Tools)
            tools.push_back(BuildToolEntry(tool));
        return MakeResult(id, Json{ { "tools", std::move(tools) } });
    }

    Json McpServer::HandleToolsSearch(const Json& id, const Json& params) const
    {
        // A non-object params (e.g. [] or "x") would otherwise skip both optional
        // filters below and silently return the unfiltered catalogue. Reject it as a
        // client error — matching the strictness of tools/call's `name` check. (An
        // absent params is dispatched as an empty object, so the no-filter "search
        // everything" call still passes here.)
        if (!params.is_object())
            return MakeError(id, kInvalidParams, "Invalid params: 'params' must be an object");

        // Both filters are optional, but a present-and-non-string filter is a client
        // error (invalid params) rather than a silent no-op — matching the strictness
        // of tools/call's `name` check.
        if (params.contains("query") && !params["query"].is_string())
            return MakeError(id, kInvalidParams, "Invalid params: 'query' must be a string");
        if (params.contains("toolset") && !params["toolset"].is_string())
            return MakeError(id, kInvalidParams, "Invalid params: 'toolset' must be a string");

        const std::string toolsetFilter = params.contains("toolset")
                                              ? ToLowerAscii(params["toolset"].get<std::string>())
                                              : std::string{};

        // Split the free-text query into whitespace-separated terms; a tool matches
        // only when EVERY term appears (case-insensitive substring) somewhere in its
        // searchable text. A missing / whitespace-only query matches everything
        // (subject to the toolset filter), so tools/search with no useful query mirrors
        // tools/list while still returning the toolset catalogue.
        std::vector<std::string> terms;
        if (params.contains("query"))
        {
            std::istringstream stream(ToLowerAscii(params["query"].get<std::string>()));
            std::string term;
            while (stream >> term)
                terms.push_back(std::move(term));
        }

        Json matched = Json::array();
        std::map<std::string, std::size_t> toolsetCounts; // sorted by canonical (lowercased) name
        for (const auto& tool : m_Tools)
        {
            // Case-fold each tool's toolset once and reuse it for both the catalogue
            // key and the filter compare. This keeps the two in lockstep: a mixed-case
            // value (e.g. "Render" alongside "render") collapses into one catalogue
            // entry instead of splitting into two that a single case-insensitive filter
            // would still match — and it avoids re-lowercasing the toolset per tool.
            const std::string toolsetKey = ToLowerAscii(tool.Toolset);

            // Count every categorized tool for the catalogue before applying filters,
            // so the catalogue always describes the full surface, not the matches.
            if (!toolsetKey.empty())
                ++toolsetCounts[toolsetKey];

            if (!toolsetFilter.empty() && toolsetKey != toolsetFilter)
                continue;

            if (!terms.empty())
            {
                const std::string haystack =
                    ToLowerAscii(tool.Name + ' ' + tool.Title + ' ' + tool.Description + ' ' + tool.Toolset);
                const bool allTermsMatch = std::all_of(terms.begin(), terms.end(),
                                                       [&haystack](const std::string& t)
                                                       { return haystack.find(t) != std::string::npos; });
                if (!allTermsMatch)
                    continue;
            }

            Json entry = BuildToolEntry(tool);
            // Friendly top-level field on this custom method (we own its shape) so an
            // agent reading search results doesn't have to dig into `_meta`.
            if (!tool.Toolset.empty())
                entry["toolset"] = tool.Toolset;
            matched.push_back(std::move(entry));
        }

        Json toolsets = Json::array();
        for (const auto& [name, count] : toolsetCounts)
            toolsets.push_back(Json{ { "name", name }, { "count", count } });

        return MakeResult(id, Json{ { "tools", std::move(matched) }, { "toolsets", std::move(toolsets) } });
    }

    Json McpServer::HandleToolsCall(const Json& id, const Json& params)
    {
        if (!params.contains("name") || !params["name"].is_string())
            return MakeError(id, kInvalidParams, "Invalid params: 'name' is required");

        const std::string name = params["name"].get<std::string>();
        const ToolDef* tool = FindTool(name);
        if (tool == nullptr)
            return MakeError(id, kInvalidParams, "Unknown tool: " + name);

        // `arguments` is optional, but when present it MUST be an object (MCP spec).
        // A present-but-non-object payload is malformed: coercing it to {} would
        // validate against an empty object and hide the mismatch, so reject it. Only
        // a truly-absent field defaults to {}.
        if (params.contains("arguments") && !params["arguments"].is_object())
            return MakeError(id, kInvalidParams, "Invalid params: 'arguments' must be an object");
        const Json arguments = params.contains("arguments") ? params["arguments"] : Json::object();

        // Enforce the tool's declared inputSchema BEFORE anything else user-visible,
        // so a malformed call fails with a clean, field-naming message instead of
        // depending on whatever ad-hoc checks that one handler happens to do (issue
        // #357 conformance / #306 item D hardening). A permissive (empty / non-object)
        // schema validates nothing. This must precede the consent gate below: a
        // malformed write should never raise the per-action consent modal (the human
        // would be asked to approve a call that can't run) — validate first, prompt only
        // for a well-formed mutation.
        //
        // The failure is a TOOL EXECUTION error (isError:true), not a protocol
        // error: SEP-1303 (spec 2025-11-25) clarified input-validation failures
        // should flow back to the MODEL so it can self-correct the arguments —
        // a protocol error is often swallowed by the client shim instead.
        // Protocol errors remain for a malformed ENVELOPE (missing name,
        // non-object arguments, unknown tool — the checks above).
        if (const auto error = ValidateArguments(tool->InputSchema, arguments))
        {
            const ToolResult invalid =
                ToolResult::Error("Invalid arguments for tool '" + name + "': " + *error);
            return MakeResult(id, Json{ { "content", invalid.Content }, { "isError", true } });
        }

        // Session write consent (issue #306 item C): a project-mutating tool is gated
        // by the WriteConsentMode the user set in the MCP panel (default Disabled,
        // never persisted). This stacks on top of the enabled + bearer-token gate:
        // even an authenticated agent stays read-only w.r.t. the project until the
        // human opts in for the session. Read-only / ephemeral editor-state tools
        // (camera / viewport / render overrides) are not ProjectWrite, so no mode
        // affects them.
        //
        //   Disabled     -> refuse outright.
        //   Prompt       -> block this worker thread on RequestConsent until the human
        //                   approves/denies the per-action modal (or it times out).
        //   AllowSession -> proceed (the human already approved everything).
        if (tool->ProjectWrite)
        {
            const WriteConsentMode mode = m_ConsentMode.load();
            if (mode == WriteConsentMode::Disabled)
                return MakeError(id, kInvalidParams,
                                 "Write tools are disabled. Set writes to \"Prompt\" or \"Allow all\" in the "
                                 "editor's MCP Server panel to permit this mutation (they are off by default).");
            if (mode == WriteConsentMode::Prompt)
            {
                switch (RequestConsent(*tool, arguments))
                {
                    case ConsentDecision::Approve:
                    case ConsentDecision::ApproveAll:
                        break; // human approved — fall through to the handler.
                    case ConsentDecision::Timeout:
                        return MakeError(id, kInvalidParams,
                                         "Write consent request timed out with no response from the editor user.");
                    case ConsentDecision::Deny:
                    case ConsentDecision::Pending:
                    default:
                        return MakeError(id, kInvalidParams,
                                         "The editor user denied this write. Ask them to Approve it (or switch writes "
                                         "to \"Allow all\") in the editor's MCP Server panel.");
                }
            }
        }

        // ---- per-call progress/cancellation scope (issue #357 item B) ----------
        // Register this call in the in-flight registry so a concurrently-arriving
        // `notifications/cancelled` (matched by exact id value) can flag it, and
        // install the thread-local scope EmitProgress / IsCurrentCallCancelled
        // read. RAII: the registry entry lives exactly as long as the dispatch.
        ActiveCallScope scope;
        if (params.contains("_meta") && params["_meta"].is_object() && params["_meta"].contains("progressToken"))
        {
            const Json& token = params["_meta"]["progressToken"];
            if (token.is_string() || token.is_number())
                scope.ProgressToken = token;
        }
        scope.CancelFlag = std::make_shared<std::atomic<bool>>(false);
        const std::string idKey = RequestIdKey(id);
        {
            std::lock_guard lock(m_InFlightMutex);
            m_InFlightCalls[idKey] = scope.CancelFlag;
        }
        struct InFlightGuard
        {
            McpServer& Server;
            const std::string& Key;
            ~InFlightGuard()
            {
                std::lock_guard lock(Server.m_InFlightMutex);
                Server.m_InFlightCalls.erase(Key);
            }
        } inFlightGuard{ *this, idKey };
        const CallScopeGuard scopeGuard(scope);

        ToolResult result;
        try
        {
            result = tool->Handler(*this, arguments);
        }
        catch (const std::exception& e)
        {
            result = ToolResult::Error(std::string("Tool failed: ") + e.what());
        }

        // A cancelled call's result is discarded per spec ("SHOULD NOT send a
        // response"): the SSE transport suppresses the frame entirely; the
        // plain-JSON path must return SOME HTTP body, so it carries the
        // conventional kRequestCancelledCode error, which clients ignore. This
        // also covers the benign race where the tool completed just as the
        // cancellation arrived — the client has already stopped listening.
        if (scope.CancelFlag->load(std::memory_order_acquire))
            return MakeError(id, kRequestCancelledCode, "Request cancelled");

        if (RedactPaths())
        {
            RedactContentArray(result.Content);
            if (!result.StructuredContent.is_null())
                RedactStructuredContent(result.StructuredContent);
        }

        Json resultObj = Json{ { "content", std::move(result.Content) }, { "isError", result.IsError } };
        // Typed result alongside the text mirror (spec 2025-06-18); omitted for
        // text-only tools so their result shape is unchanged.
        if (!result.StructuredContent.is_null())
            resultObj["structuredContent"] = std::move(result.StructuredContent);
        return MakeResult(id, std::move(resultObj));
    }

    const ToolDef* McpServer::FindTool(const std::string& name) const
    {
        for (const auto& tool : m_Tools)
        {
            if (tool.Name == name)
                return &tool;
        }
        return nullptr;
    }

    Json McpServer::HandleResourcesList(const Json& id) const
    {
        Json resources = Json::array();
        for (const auto& resource : m_Resources)
        {
            resources.push_back(Json{ { "uri", resource.Uri },
                                      { "name", resource.Name },
                                      { "description", resource.Description },
                                      { "mimeType", resource.MimeType } });
        }
        return MakeResult(id, Json{ { "resources", std::move(resources) } });
    }

    Json McpServer::HandleResourcesRead(const Json& id, const Json& params)
    {
        if (!params.contains("uri") || !params["uri"].is_string())
            return MakeError(id, kInvalidParams, "Invalid params: 'uri' is required");

        const std::string uri = params["uri"].get<std::string>();
        const ResourceDef* resource = FindResource(uri);
        if (resource == nullptr)
            return MakeError(id, kInvalidParams, "Unknown resource: " + uri);

        std::string text;
        try
        {
            text = resource->Reader(*this);
        }
        catch (const std::exception& e)
        {
            return MakeError(id, kInvalidRequest, std::string("Failed to read resource: ") + e.what());
        }

        if (RedactPaths())
            text = RedactPathsInText(text);

        return MakeResult(id, Json{ { "contents",
                                      Json::array({ Json{ { "uri", uri },
                                                          { "mimeType", resource->MimeType },
                                                          { "text", std::move(text) } } }) } });
    }

    const ResourceDef* McpServer::FindResource(const std::string& uri) const
    {
        for (const auto& resource : m_Resources)
        {
            if (resource.Uri == uri)
                return &resource;
        }
        return nullptr;
    }

    Json McpServer::HandlePromptsList(const Json& id) const
    {
        Json prompts = Json::array();
        for (const auto& prompt : m_Prompts)
        {
            prompts.push_back(Json{ { "name", prompt.Name },
                                    { "title", prompt.Title },
                                    { "description", prompt.Description } });
        }
        return MakeResult(id, Json{ { "prompts", std::move(prompts) } });
    }

    Json McpServer::HandlePromptsGet(const Json& id, const Json& params) const
    {
        if (!params.contains("name") || !params["name"].is_string())
            return MakeError(id, kInvalidParams, "Invalid params: 'name' is required");

        const std::string name = params["name"].get<std::string>();
        const PromptDef* prompt = FindPrompt(name);
        if (prompt == nullptr)
            return MakeError(id, kInvalidParams, "Unknown prompt: " + name);

        Json messages = Json::array({ Json{ { "role", "user" },
                                            { "content", { { "type", "text" }, { "text", prompt->Text } } } } });
        return MakeResult(id, Json{ { "description", prompt->Description }, { "messages", std::move(messages) } });
    }

    const PromptDef* McpServer::FindPrompt(const std::string& name) const
    {
        for (const auto& prompt : m_Prompts)
        {
            if (prompt.Name == name)
                return &prompt;
        }
        return nullptr;
    }

    bool McpServer::CheckAuth(const httplib::Request& req) const
    {
        if (!req.has_header("Authorization"))
            return false;
        return CheckBearerAuth(req.get_header_value("Authorization"), m_Token);
    }

    bool McpServer::CheckBearerAuth(std::string_view authorizationHeader, std::string_view expectedToken)
    {
        // An empty expected token means the server isn't running (no token has been
        // generated) — reject everything, including an empty presented token.
        if (expectedToken.empty())
            return false;

        constexpr std::string_view kPrefix = "Bearer ";
        if (authorizationHeader.size() <= kPrefix.size() || authorizationHeader.substr(0, kPrefix.size()) != kPrefix)
            return false;

        return ConstantTimeEquals(authorizationHeader.substr(kPrefix.size()), expectedToken);
    }

    bool McpServer::IsOriginAllowed(std::string_view origin)
    {
        // A browser-originated request carries an Origin; non-browser agents
        // (Claude Code/Desktop) send none — accept those. When present, the host
        // must be loopback.
        if (origin.empty() || origin == "null")
            return true;
        const auto schemeEnd = origin.find("://");
        if (schemeEnd == std::string_view::npos)
            return false;
        const auto hostStart = schemeEnd + 3;
        if (hostStart >= origin.size())
            return false;

        std::string_view host;
        if (origin[hostStart] == '[')
        {
            // Bracketed IPv6 literal (e.g. http://[::1]:7345). The address itself
            // is full of ':' separators, so the host runs to the closing ']', not
            // the first ':'. Keep the brackets so it matches the allowlist form.
            const auto bracketEnd = origin.find(']', hostStart);
            if (bracketEnd == std::string_view::npos)
                return false;
            host = origin.substr(hostStart, bracketEnd - hostStart + 1);
        }
        else
        {
            const auto hostEnd = origin.find_first_of(":/", hostStart);
            host = origin.substr(hostStart, hostEnd == std::string_view::npos ? std::string_view::npos : hostEnd - hostStart);
        }
        return host == "127.0.0.1" || host == "localhost" || host == "[::1]" || host == "::1";
    }
} // namespace OloEngine::MCP
