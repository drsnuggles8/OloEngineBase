// httplib.h MUST be included before any header that pulls in <windows.h> so that
// <winsock2.h> wins the include race on Windows (windows.h would otherwise drag in
// the legacy <winsock.h> and cause redefinition errors). OloEditor has no PCH, so
// this single ordering rule is enough.
#include "OloEnginePCH.h"
#include "OloEngine/Core/Environment.h"
#include <httplib.h>

#include "MCP/McpServer.h"
#include "Automation/AutomationSchemaValidation.h"
#include "MCP/McpAudienceReport.h"
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
#include <limits>
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
#include <unordered_map>
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

        // DoS hardening (issue #306): the largest request body POST /mcp will
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

        // Reverse-DNS-namespaced `_meta` key carrying a tool's toolset (grouping
        // category) in tools/list / tools/search entries. `_meta` is the MCP-blessed
        // extension point (spec 2025-06-18), so surfacing the category there keeps
        // tools/list conformant — a strict client validating the Tool schema won't
        // reject an unknown top-level field.
        constexpr const char* kToolsetMetaKey = "io.oloengine/toolset";

        // ---- per-call progress/cancellation scope (issue #357) ----------
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
            Json ProgressToken;                                    // null => caller didn't opt in
            std::shared_ptr<std::atomic<bool>> CancelFlag;         // shared with the in-flight registry
            f64 LastProgress = std::numeric_limits<f64>::lowest(); // monotonicity guard
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
        // utilities (#357), JSON Schema 2020-12-compatible tool schemas
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

        // ---- cacheable list results (spec 2026-07-28, server/utilities/caching) ----
        //
        // tools/list, prompts/list, resources/list and resources/read attach two
        // optional freshness hints so a client can avoid re-fetching a result it
        // already holds:
        //   * ttlMs      — integer milliseconds (MUST be >= 0) the client MAY treat
        //                  the result as fresh; semantics mirror HTTP
        //                  Cache-Control: max-age (0 == immediately stale).
        //   * cacheScope — "public" (no user-specific data; any cache/proxy MAY share
        //                  it) or "private" (per-authorization-context; MUST NOT be
        //                  shared across auth contexts).
        //
        // This is ADDITIVE and version-NEUTRAL: we do NOT advertise the 2026-07-28
        // protocol (that requires the whole stateless core — server/discover, _meta
        // identity, routing headers, resultType — explicitly out of scope for
        // issue #777). The spec only *requires* these hints on results tagged
        // resultType: "complete", but emitting them unconditionally is spec-compatible
        // and harmless: older clients ignore both unknown fields, so every existing
        // result shape is unchanged. The two fields sit at the top level of the result
        // object, beside `tools` / `prompts` / `resources` / `contents`.
        void AddCacheHints(Json& result, i64 ttlMs, const char* cacheScope)
        {
            result["ttlMs"] = ttlMs;
            result["cacheScope"] = cacheScope;
        }

        // tools/list: the catalogue is static for a server run except for two things,
        // and BOTH fire notifications/tools/list_changed on every live SSE stream —
        // olo_script_tools_reload (issue #607), which rescans <project>/McpTools, and
        // SetExposurePolicy (issue #1124), which changes how much of the registry is
        // listed. Because that push is the authoritative invalidation
        // (capabilities.tools.listChanged is true), a generous TTL is honest — it only
        // bridges the gap between changes.
        // 5 min matches the spec's own worked example.
        constexpr i64 kToolsListTtlMs = 300000;

        // resources/list: the base catalogue is static, but capture tools publish and
        // evict ephemeral resources while serving (issue #673), also pushed via
        // notifications/resources/list_changed. Shorter than tools/list because that
        // ephemeral churn is more frequent; the push again bounds staleness.
        constexpr i64 kResourcesListTtlMs = 60000;

        // prompts/list: prompts are compiled in and FIXED for the whole server run
        // (capabilities.prompts.listChanged is false — there is no push invalidation),
        // so the TTL is the only freshness signal. A long TTL is fully honest precisely
        // because the list is immutable within a run; a reconnecting client
        // re-initializes into a fresh process.
        constexpr i64 kPromptsListTtlMs = 3600000;

        // resources/read: a read reflects LIVE editor state (current frame / scene /
        // log) and may be path-redacted per the session's redaction setting, so caching
        // it is NOT honest — ttlMs 0 tells the client to treat it as immediately stale
        // and re-read on demand.
        constexpr i64 kResourcesReadTtlMs = 0;

        // The three catalogues carry no user-specific data and are identical for every
        // caller, so they are "public". A resources/read payload can depend on the
        // redaction setting and on host-local paths, so it is "private" — it MUST NOT
        // be shared across authorization contexts.
        constexpr const char* kCacheScopePublic = "public";
        constexpr const char* kCacheScopePrivate = "private";

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
        // tools/list, the custom tools/search and the discovery gateway (#1124) so
        // all three present byte-identical entries; the only optional field beyond
        // the spec basics is the toolset, carried under `_meta` (omitted for
        // uncategorized tools). Exposed to the gateway's own TU as the public static
        // McpServer::BuildToolEntry, which forwards here.
        Json BuildToolEntryImpl(const ToolDef& tool)
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
            // Display icons (SEP-973, spec 2025-11-25). Emitted ONLY when the array is
            // non-empty: the spec models `icons` as an optional field, and an empty
            // array would advertise "this tool has icons" while carrying none, which a
            // client may render as a broken/blank slot. RegisterTool already rejected a
            // malformed value, so a present array is well-formed here.
            if (tool.Icons.is_array() && !tool.Icons.empty())
                entry["icons"] = tool.Icons;
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
        //
        // The drive-path match requires a non-scheme character (or start of
        // string) BEFORE the drive letter: without that guard, the trailing
        // "o://..." of a URI like "olo://capture/1/shot.png" (#673 resource
        // links) parses as drive "o:" + path and redaction corrupts the URI to
        // "ol<path>". std::regex has no lookbehind, so the guard is a captured
        // prefix restored via $1.
        std::string RedactPathsInText(const std::string& text)
        {
            static const std::regex winPath(R"((^|[^A-Za-z0-9+.\-])([A-Za-z]:[\\/][^\s"'<>|]*))");
            static const std::regex posixHome(R"(/(?:home|Users)/[^\s"'<>|]*)");
            std::string out = std::regex_replace(text, winPath, "$1<path>");
            out = std::regex_replace(out, posixHome, "<path>");
            return out;
        }

        // Apply redaction in place to every text content block of an MCP content
        // array — plus the human-readable fields of resource_link blocks (#673
        // Tier 1), whose name/description could carry a path (e.g. a golden's
        // relative location); the olo:// uri never does, but scrubbing it too
        // keeps the guarantee unconditional.
        void RedactContentArray(Json& content)
        {
            if (!content.is_array())
                return;
            for (auto& block : content)
            {
                if (!block.is_object())
                    continue;
                const std::string type = block.value("type", std::string{});
                if (type == "text" && block.contains("text") && block["text"].is_string())
                    block["text"] = RedactPathsInText(block["text"].get<std::string>());
                else if (type == "resource_link")
                {
                    for (const char* field : { "name", "description", "uri" })
                    {
                        if (block.contains(field) && block[field].is_string())
                            block[field] = RedactPathsInText(block[field].get<std::string>());
                    }
                }
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

        // ---- SSE server-push stream (issue #306) -------------------------

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

    // ---- McpServer -------------------------------------------------------------

    McpServer::McpServer(EditorMcpContext context)
        : m_Context(std::move(context))
    {
        // The registry can be replaced wholesale (script live reload, a bridged
        // connection coming or going) by callers that know nothing about MCP. Every
        // such swap is exactly what tools/list_changed exists to announce, so the
        // announcement is wired here rather than at each call site — a new replace
        // path then cannot forget it.
        m_Registry.SetChangeListener([this]
                                     { NotifyToolsListChanged(); });
    }

    McpServer::~McpServer()
    {
        Stop();
    }

    // The name, icons and argument rules belong to the COMMAND, so they live with
    // the registry now (issue #1123). These stay as the spellings the transport,
    // the Lua registration path and the tests already use.
    bool McpServer::IsValidToolName(std::string_view name)
    {
        return AutomationRegistry::IsValidName(name);
    }

    std::optional<std::string> McpServer::ValidateArguments(const Json& schema, const Json& args)
    {
        return Automation::AutomationSchema::ValidateArguments(schema, args);
    }

    bool McpServer::IsValidIcons(const Json& icons)
    {
        return AutomationRegistry::IsValidIcons(icons);
    }

    void McpServer::RegisterTool(ToolDef tool)
    {
        m_Registry.Register(std::move(tool));
    }

    void McpServer::ReplaceScriptTools(std::vector<ToolDef> scriptTools)
    {
        m_Registry.ReplaceScriptCommands(std::move(scriptTools));
    }

    void McpServer::UnregisterScriptTools()
    {
        m_Registry.UnregisterScriptCommands();
    }

    bool McpServer::IsValidClientAlias(std::string_view alias)
    {
        return AutomationRegistry::IsValidClientAlias(alias);
    }

    std::string McpServer::ClientToolPrefix(const std::string& alias)
    {
        return AutomationRegistry::ClientPrefix(alias);
    }

    sizet McpServer::ReplaceClientTools(const std::string& alias, std::vector<ToolDef> clientTools)
    {
        return m_Registry.ReplaceClientCommands(alias, std::move(clientTools));
    }

    void McpServer::NotifyToolsListChanged()
    {
        // Bump FIRST: an SSE stream that polls the generation must never observe a
        // stale generation alongside a fresh tool list (it would skip the notify).
        m_ToolsGeneration.fetch_add(1, std::memory_order_release);

        std::vector<NotificationSink> sinks;
        {
            std::lock_guard lock(m_ListenerMutex);
            sinks.reserve(m_Listeners.size());
            for (const auto& [token, sink] : m_Listeners)
                sinks.push_back(sink);
        }
        // Invoke OUTSIDE the lock: a sink that re-enters the server (or removes
        // itself) must not deadlock on m_ListenerMutex.
        const Json notification{ { "jsonrpc", "2.0" }, { "method", "notifications/tools/list_changed" } };
        for (const NotificationSink& sink : sinks)
        {
            if (sink)
                sink(notification);
        }
    }

    u64 McpServer::AddNotificationListener(NotificationSink sink)
    {
        std::lock_guard lock(m_ListenerMutex);
        const u64 token = m_NextListenerToken++;
        m_Listeners.emplace_back(token, std::move(sink));
        return token;
    }

    void McpServer::RemoveNotificationListener(u64 token)
    {
        std::lock_guard lock(m_ListenerMutex);
        std::erase_if(m_Listeners, [token](const std::pair<u64, NotificationSink>& entry)
                      { return entry.first == token; });
    }

    void McpServer::SetServerIcons(Json icons)
    {
        OLO_CORE_VERIFY(IsValidIcons(icons),
                        "[MCP] Invalid serverInfo icons: expected a non-empty array of {{ src, mimeType?, sizes? }}.");
        std::lock_guard lock(m_ServerIconsMutex);
        m_ServerIcons = std::move(icons);
    }

    void McpServer::RegisterResource(ResourceDef resource)
    {
        // Copy-on-write publish, mirroring RegisterTool: build the new vector,
        // swap it in atomically. A duplicate URI REPLACES the existing entry
        // instead of silently shadowing it (FindResource returns the first match).
        std::shared_ptr<ResourceList> next;
        {
            std::lock_guard writeLock(m_ResourcesWriteMutex);
            next = std::make_shared<ResourceList>(*ResourcesSnapshot());
            std::erase_if(*next, [&resource](const ResourceDef& existing)
                          { return existing.Uri == resource.Uri; });
            next->push_back(std::move(resource));
            m_Resources.store(std::shared_ptr<const ResourceList>(next), std::memory_order_release);
        }
        NotifyResourcesListChanged();
    }

    void McpServer::RegisterEphemeralResource(ResourceDef resource)
    {
        resource.Ephemeral = true;
        {
            std::lock_guard writeLock(m_ResourcesWriteMutex);
            auto next = std::make_shared<ResourceList>(*ResourcesSnapshot());
            std::erase_if(*next, [&resource](const ResourceDef& existing)
                          { return existing.Uri == resource.Uri; });
            next->push_back(std::move(resource));

            // Evict OLDEST ephemerals (registration order) until both bounds
            // hold. The just-published entry is only evicted if it alone busts a
            // bound (an oversized capture with maxCount >= 1 still displaces
            // everything older first).
            const auto ephemeralCount = [&next]
            {
                sizet count = 0;
                for (const ResourceDef& entry : *next)
                    count += entry.Ephemeral ? 1 : 0;
                return count;
            };
            const auto ephemeralBytes = [&next]
            {
                u64 bytes = 0;
                for (const ResourceDef& entry : *next)
                    bytes += entry.Ephemeral ? entry.SizeBytes : 0;
                return bytes;
            };
            while (ephemeralCount() > m_EphemeralMaxCount ||
                   (ephemeralBytes() > m_EphemeralMaxBytes && ephemeralCount() > 1))
            {
                const auto oldest = std::find_if(next->begin(), next->end(),
                                                 [](const ResourceDef& entry)
                                                 { return entry.Ephemeral; });
                if (oldest == next->end())
                    break;
                next->erase(oldest);
            }
            m_Resources.store(std::shared_ptr<const ResourceList>(std::move(next)), std::memory_order_release);
        }
        NotifyResourcesListChanged();
    }

    bool McpServer::PublishArtifact(Automation::AutomationArtifact artifact)
    {
        ResourceDef resource;
        resource.Uri = std::move(artifact.Uri);
        resource.Name = std::move(artifact.Name);
        resource.Description = std::move(artifact.Description);
        resource.MimeType = std::move(artifact.MimeType);
        resource.SizeBytes = static_cast<u64>(artifact.Bytes.size());
        // The bytes are stashed in the closure at publish time; resources/read hands
        // them back base64-encoded. Capture by value and move in — the caller has
        // already given up ownership.
        resource.BlobReader = [bytes = std::move(artifact.Bytes)](McpServer&)
        { return bytes; };
        RegisterEphemeralResource(std::move(resource));
        return true;
    }

    void McpServer::ClearEphemeralResources()
    {
        bool changed = false;
        {
            std::lock_guard writeLock(m_ResourcesWriteMutex);
            const ResourceSnapshot current = ResourcesSnapshot();
            auto next = std::make_shared<ResourceList>();
            next->reserve(current->size());
            for (const ResourceDef& entry : *current)
            {
                if (!entry.Ephemeral)
                    next->push_back(entry);
            }
            changed = next->size() != current->size();
            if (changed)
                m_Resources.store(std::shared_ptr<const ResourceList>(std::move(next)), std::memory_order_release);
        }
        if (changed)
            NotifyResourcesListChanged();
    }

    void McpServer::SetEphemeralResourceLimits(sizet maxCount, u64 maxBytes)
    {
        std::lock_guard writeLock(m_ResourcesWriteMutex);
        m_EphemeralMaxCount = maxCount;
        m_EphemeralMaxBytes = maxBytes;
    }

    void McpServer::NotifyResourcesListChanged()
    {
        // Bump FIRST — same ordering contract as NotifyToolsListChanged: an SSE
        // stream that polls the generation must never observe a stale generation
        // alongside a fresh resource list.
        m_ResourcesGeneration.fetch_add(1, std::memory_order_release);

        std::vector<NotificationSink> sinks;
        {
            std::lock_guard lock(m_ListenerMutex);
            sinks.reserve(m_Listeners.size());
            for (const auto& [token, sink] : m_Listeners)
                sinks.push_back(sink);
        }
        const Json notification{ { "jsonrpc", "2.0" }, { "method", "notifications/resources/list_changed" } };
        for (const NotificationSink& sink : sinks)
        {
            if (sink)
                sink(notification);
        }
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

        // Own the port EXCLUSIVELY. httplib's default socket options set
        // SO_REUSEADDR (Windows) / SO_REUSEPORT (Linux), both of which let a SECOND
        // process bind the same 127.0.0.1:port — after which the OS hands incoming
        // connections to either listener non-deterministically. For a token-
        // authenticated diagnostics server that means a client can reach the wrong
        // instance and be rejected with a 401 (each instance mints its own token),
        // and two editors would silently share a port. It also makes the parallel
        // HTTP tests flaky: colliding per-process ports cross-wire, so a request
        // authenticated for one server lands on another. Demand exclusive ownership
        // so a second bind to a live port fails cleanly instead (surfaced as the
        // "port already in use" error below), which also makes the tests' bind-sweep
        // reliably land on a truly-free port.
        m_Http->set_socket_options([](auto sock)
                                   {
#ifdef _WIN32
                                       httplib::set_socket_opt(sock, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, 1);
#else
                                       // Keep SO_REUSEADDR (NOT SO_REUSEPORT) so a quick restart isn't blocked by
                                       // a lingering TIME_WAIT socket; on POSIX that flag alone does not permit a
                                       // second live listener on the port, so it grants no cross-wiring.
                                       httplib::set_socket_opt(sock, SOL_SOCKET, SO_REUSEADDR, 1);
#endif
                                   });

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
        // diagnostics events are pushed as MCP notifications (issue #306).
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

        // What this session's catalogue actually costs (issue #1124). Logged once at
        // Start rather than left to be re-measured by hand in a year: the number grew
        // 15k -> 66k tokens between #673 and #1124 with nothing recording it, which is
        // how the regression stayed invisible. The byte counts are exact; the token
        // figure is bytes/4 and says so.
        {
            const ToolRegistryMetrics metrics = ComputeRegistryMetrics();
            OLO_CORE_INFO("[MCP] tools/list profile '{}': {} of {} tools, {} bytes (~{} tokens); "
                          "full surface {} bytes (~{} tokens). Hidden tools stay callable by name.",
                          ToStringView(metrics.Profile), metrics.ListedTools, metrics.TotalTools,
                          metrics.ListedBytes, metrics.ApproxListedTokens(), metrics.FullBytes,
                          metrics.ApproxFullTokens());
        }
        return true;
    }

    void McpServer::Stop()
    {
        if (!m_Http)
        {
            // Never started (or already stopped) — but outbound client
            // connections can exist without a running HTTP server (tests, a
            // panel connect before Start): tear those down regardless.
            ShutdownClients();
            return;
        }

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

        // Same reasoning for workers blocked on an outbound child's response
        // (issue #673): fail every pending bridged call and join the
        // client reader threads BEFORE the pool join below.
        ShutdownClients();

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
        // Resource subscriptions die with the session that made them: the streams
        // that would carry the updates are gone, and a later Start() must not
        // inherit a subscription no live client asked for.
        {
            std::lock_guard lock(m_SubscriptionMutex);
            m_ResourceSubscriptions.clear();
        }
        m_Token.clear();

        // Drop the session's ephemeral capture resources so a later Start()
        // begins with a clean registry (their URIs would otherwise be stale
        // advertisements to a newly attached agent). After m_Http.reset() no
        // worker is left mid-read; any listener sink still fires harmlessly.
        ClearEphemeralResources();

        // (The ephemeral sun-direction override clear that used to live here was
        // retired by issue #633 — olo_scene_set_time_of_day now edits the
        // serialized TimeOfDayComponent instead of session-global renderer state,
        // so there is nothing to restore on server stop.)

        OLO_CORE_INFO("[MCP] Diagnostics server stopped");
    }

    std::string McpServer::DiscoveryFilePath(u16 port)
    {
        // An explicit override wins: the launching tool picks the exact path it will
        // read back, so parallel worktree editors never collide regardless of port.
        if (const std::optional<std::string> overridePath = Env::Get("OLO_MCP_DISCOVERY_FILE"))
            return *overridePath;

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

    Json McpServer::MarshalReadOnMainThread(const std::function<Json()>& readJob,
                                            std::chrono::milliseconds timeout)
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
            pending.push_back(PendingConsent{ entry.Id, entry.ToolName, entry.ToolTitle, entry.Summary, entry.External });
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

    ConsentDecision McpServer::RequestConsent(const ToolDef& tool, const Json& arguments,
                                              const std::shared_ptr<std::atomic<bool>>& cancelFlag)
    {
        const auto timeout = std::chrono::milliseconds(m_ConsentTimeoutMs.load());

        // Predicate helper: a call cancelled via notifications/cancelled while parked
        // here must return promptly (issue #610). The cancellation path stores the
        // flag then synchronizes on m_ConsentMutex before notifying, so a read taken
        // under this lock cannot miss a store the notifier has already published.
        const auto cancelled = [&cancelFlag]
        { return cancelFlag && cancelFlag->load(std::memory_order_acquire); };

        u64 myId = 0;
        {
            std::unique_lock lock(m_ConsentMutex);
            if (m_ConsentAborting)
                return ConsentDecision::Deny;
            if (cancelled())
                return ConsentDecision::Cancel;
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
            entry.External = !tool.ClientAlias.empty();
            m_ConsentQueue.push_back(std::move(entry));

            const bool resolved = m_ConsentCv.wait_for(lock, timeout, [this, myId, &cancelled]
                                                       {
                                                           if (m_ConsentAborting || cancelled())
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
            // Cancellation wins over a racing human decision: even if the modal was
            // just approved, an arrived notifications/cancelled means the write must
            // not run and the response is discarded per spec.
            if (cancelled())
                return ConsentDecision::Cancel;
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

        // 4. Streamable-HTTP upgrade (issue #357): a single tools/call that
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
            [this, cursor = startCursor, lastWrite = std::chrono::steady_clock::now(), greeted = false,
             toolsGeneration = ToolsGeneration(), resourcesGeneration = ResourcesGeneration(),
             subscriptionTokens = std::unordered_map<std::string, u64>{}](
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

                // Tool-list changes (script-tool live reload, #607). Polled off the
                // monotonic generation counter rather than pushed from the reloading
                // thread: this DataSink may only be written from the stream's own
                // worker thread, so a cross-thread push would be a data race. A
                // 250 ms-granularity notify is exactly right for a "re-list your
                // tools" hint. Advertised via capabilities.tools.listChanged.
                if (const u64 generation = ToolsGeneration(); generation != toolsGeneration)
                {
                    const std::string frame = FormatSseData(
                        Json{ { "jsonrpc", "2.0" }, { "method", "notifications/tools/list_changed" } });
                    if (!sink.write(frame.data(), frame.size()))
                        return false;
                    toolsGeneration = generation;
                    lastWrite = std::chrono::steady_clock::now();
                }

                // Resource-list changes (ephemeral capture publishes/evictions,
                // #673) — same polled-generation delivery as the tools
                // block above, same single-writer-thread constraint. Advertised
                // via capabilities.resources.listChanged.
                if (const u64 generation = ResourcesGeneration(); generation != resourcesGeneration)
                {
                    const std::string frame = FormatSseData(
                        Json{ { "jsonrpc", "2.0" }, { "method", "notifications/resources/list_changed" } });
                    if (!sink.write(frame.data(), frame.size()))
                        return false;
                    resourcesGeneration = generation;
                    lastWrite = std::chrono::steady_clock::now();
                }

                // Resource subscriptions (#777 — the `logging` offramp carrier).
                // Same polled-generation delivery as the two blocks above, for the
                // same single-writer-thread reason, but per SUBSCRIBED URI: each
                // subscribable resource exposes a monotonic ChangeToken and we emit
                // `notifications/resources/updated` when it advances.
                //
                // A URI this stream has not seen before starts from the
                // SUBSCRIBE-TIME baseline, never from the token as of now. Seeding
                // from "now" would swallow every change between the subscribe and
                // this cycle — including everything that happened before the client
                // opened its stream at all — which is precisely the
                // never-fires failure the ChangeToken gate exists to prevent.
                // Unsubscribing drops the URI from both maps, so a resubscribe
                // re-baselines rather than replaying.
                //
                // Updates COALESCE by construction: the token is sampled once per
                // cycle, so a burst inside one poll interval yields one
                // notification. That is the correct semantics — the notification
                // means "re-read this resource", not "one thing happened" — but a
                // consumer must read from its own cursor, not count notifications.
                {
                    const ResourceSnapshot resources = ResourcesSnapshot();
                    std::unordered_map<std::string, u64> stillSubscribed;
                    for (const auto& [uri, baseline] : SubscriptionBaselines())
                    {
                        const ResourceDef* resource = FindResource(*resources, uri);
                        if (resource == nullptr || !resource->ChangeToken)
                            continue; // evicted or replaced by a non-subscribable def
                        const u64 token = resource->ChangeToken();
                        const auto seen = subscriptionTokens.find(uri);
                        const u64 lastSeen = seen != subscriptionTokens.end() ? seen->second : baseline;
                        if (lastSeen != token)
                        {
                            const std::string frame = FormatSseData(
                                Json{ { "jsonrpc", "2.0" },
                                      { "method", "notifications/resources/updated" },
                                      { "params", Json{ { "uri", uri } } } });
                            if (!sink.write(frame.data(), frame.size()))
                                return false;
                            lastWrite = std::chrono::steady_clock::now();
                        }
                        stillSubscribed.emplace(uri, token);
                    }
                    subscriptionTokens = std::move(stillSubscribed);
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

    void McpServer::EmitProgressUpdate(f64 progress, f64 total, const std::string& message) const
    {
        ActiveCallScope* scope = t_ActiveCall;
        const NotificationSink* sink = t_ActiveSink;
        if (scope == nullptr || scope->ProgressToken.is_null() || sink == nullptr || !(*sink))
            return; // caller didn't opt in, or the transport has nowhere to put it.

        if (!std::isfinite(progress) || !std::isfinite(total))
            return;

        // The spec requires progress to increase with each notification. Drop a
        // duplicate/regression instead of inventing a value that may exceed total.
        if (progress <= scope->LastProgress)
            return;
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
        // one notification with a side effect (issue #357): flag the named
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
                {
                    flag->store(true, std::memory_order_release);
                    // The flagged call may be parked in RequestConsent on the consent
                    // modal (issue #610). Wake it: a bare atomic store racing the
                    // waiter's predicate check could be lost, so take m_ConsentMutex
                    // (never nested with m_InFlightMutex above — released already) to
                    // serialize with the waiter before notifying. A call in its handler
                    // instead just polls the flag, so this notify is a harmless no-op
                    // for it. m_ConsentMutex is not held across the notify — a spurious
                    // wake of unrelated waiters is cheap and they re-check and re-sleep.
                    {
                        std::lock_guard consentLock(m_ConsentMutex);
                    }
                    m_ConsentCv.notify_all();
                }
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
        if (method == "resources/subscribe")
            return HandleResourcesSubscribe(id, request.value("params", Json::object()));
        if (method == "resources/unsubscribe")
            return HandleResourcesUnsubscribe(id, request.value("params", Json::object()));
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
        // events as `notifications/message` log notifications (issue #306).
        // It is DEPRECATED as of spec 2026-07-28 (SEP-2577, ≥12-month offramp), so
        // it now has a successor running beside it rather than replacing it: the
        // `olo://events/recent` resource plus `resources.subscribe` below. Both
        // carriers stay live for the whole offramp — dropping `logging` early would
        // break every client that speaks a 2025-* revision, which today is all of
        // them. See docs/agent-rules/mcp-protocol-eras.md.
        //
        // `tools.listChanged` is TRUE since the script-tool live-reload item (#607):
        // olo_script_tools_reload (and the MCP panel's "Reload script tools" button)
        // rescan <project assets>/McpTools while the server is serving and swap the
        // registry, then emit `notifications/tools/list_changed` on every live SSE
        // stream. `resources.listChanged` is TRUE since the resource-link work
        // (#673): capture tools publish ephemeral resources while serving
        // (RegisterEphemeralResource), announced the same generation-polled way.
        // Prompts are still fixed for a server run.
        //
        // `resources.subscribe` is TRUE since the `logging` offramp (#777): a
        // client can subscribe to `olo://events/recent` and receive
        // `notifications/resources/updated` whenever the diagnostics ring advances.
        // Only resources carrying a ResourceDef::ChangeToken accept a subscription;
        // the rest are rejected by name (HandleResourcesSubscribe).
        result["capabilities"] = Json{ { "tools", { { "listChanged", true } } },
                                       { "resources", { { "subscribe", true }, { "listChanged", true } } },
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
        // serverInfo.icons (SEP-973): emitted only when the host supplied some, so a
        // default session's serverInfo is byte-identical to before.
        {
            std::lock_guard lock(m_ServerIconsMutex);
            if (m_ServerIcons.is_array() && !m_ServerIcons.empty())
                result["serverInfo"]["icons"] = m_ServerIcons;
        }
        result["instructions"] =
            "Read-only diagnostics for a running OloEngine editor session. Use olo_log_tail "
            "to see the most recent engine log messages, olo_events_tail for a 'what just "
            "happened?' timeline (scene load, play/stop, entity spawn/destroy, asset reload, "
            "script error — poll incrementally with sinceId), and olo_scene_summary to inspect "
            "the active scene. Everything exposed here is read-only — no tool mutates the project. "
            "tools/list shows a curated core set by default, not the whole surface: there are far "
            "more tools (rendering, physics, shaders, perf, assets, scripting, the editor panels) "
            "and they are all still callable by name. Call olo_capability first to see what exists, "
            "olo_tool_search to find one, and olo_tool_describe for its input schema.";
        return MakeResult(id, result);
    }

    Json McpServer::BuildToolEntry(const ToolDef& tool)
    {
        return BuildToolEntryImpl(tool);
    }

    void McpServer::SetExposurePolicy(ExposurePolicy policy)
    {
        m_ExposurePolicy.store(std::make_shared<const ExposurePolicy>(std::move(policy)),
                               std::memory_order_release);
        // The catalogue a client sees just changed, which is exactly what
        // tools/list_changed exists to announce — a connected agent that cached the
        // narrowed list would otherwise never learn the host widened it.
        NotifyToolsListChanged();
    }

    ToolExposureFacts McpServer::ExposureFactsOf(const ToolDef& tool)
    {
        // A tool is "user-provided" when it exists only because this user configured
        // it: a project Lua script tool (ScriptOwned) or one bridged from an outbound
        // client connection (ClientAlias). Those stay listed under every profile.
        // Public and single-sited on purpose — McpToolsGateway's per-hit `listed` flag
        // calls this too, so the gateway can never disagree with tools/list about what
        // is listed.
        return ToolExposureFacts{ tool.Name, tool.Toolset, tool.ScriptOwned || !tool.ClientAlias.empty() };
    }

    Json McpServer::HandleToolsList(const Json& id) const
    {
        const ToolSnapshot snapshot = ToolsSnapshot();
        const std::shared_ptr<const ExposurePolicy> policy = m_ExposurePolicy.load(std::memory_order_acquire);
        Json tools = Json::array();
        for (const auto& tool : *snapshot)
        {
            // A tool this host cannot serve is not listed AND not callable — see
            // AutomationCommand::IsAvailable. Distinct from exposure below, and
            // checked first: advertising something that cannot run is worse than
            // hiding something that can. No builtin declares one, so this changes
            // nothing observable today.
            if (!tool.AvailableOn(*this))
                continue;
            // Exposure is a LISTING filter only (issue #1124): a tool skipped here is
            // still resolvable by HandleToolsCall, still returned by tools/search, and
            // still describable through the gateway. Nothing is unregistered.
            if (policy->ShouldList(ExposureFactsOf(tool)))
                tools.push_back(BuildToolEntry(tool));
        }
        Json result{ { "tools", std::move(tools) } };
        AddCacheHints(result, kToolsListTtlMs, kCacheScopePublic);
        return MakeResult(id, std::move(result));
    }

    ToolRegistryMetrics McpServer::ComputeRegistryMetrics() const
    {
        const ToolSnapshot snapshot = ToolsSnapshot();
        const std::shared_ptr<const ExposurePolicy> policy = m_ExposurePolicy.load(std::memory_order_acquire);
        return ComputeRegistryMetrics(snapshot, *policy);
    }

    ToolRegistryMetrics McpServer::ComputeRegistryMetrics(const ToolSnapshot& snapshot,
                                                          const ExposurePolicy& policy) const
    {
        ToolRegistryMetrics metrics;
        metrics.Profile = policy.Profile;
        metrics.TotalTools = snapshot->size();

        // Measure the two catalogues the way a client actually receives them: the
        // `tools` array of a tools/list result, dumped compactly (the transport does
        // not pretty-print). Anything else — summing per-entry sizes, estimating from
        // schema lengths — drifts from the payload it claims to describe, which is the
        // failure mode this metric exists to prevent.
        Json full = Json::array();
        Json listed = Json::array();
        for (const auto& tool : *snapshot)
        {
            Json entry = BuildToolEntry(tool);
            if (policy.ShouldList(ExposureFactsOf(tool)))
            {
                ++metrics.ListedTools;
                listed.push_back(entry);
            }
            full.push_back(std::move(entry));
        }
        metrics.FullBytes = Json{ { "tools", std::move(full) } }.dump().size();
        metrics.ListedBytes = Json{ { "tools", std::move(listed) } }.dump().size();
        return metrics;
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

        const ToolSnapshot snapshot = ToolsSnapshot();
        Json matched = Json::array();
        std::map<std::string, std::size_t> toolsetCounts; // sorted by canonical (lowercased) name
        for (const auto& tool : *snapshot)
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

    std::optional<std::string> McpServer::RewriteGatewayExecuteParams(const Json& params, Json& rewritten)
    {
        const Json arguments = params.contains("arguments") ? params["arguments"] : Json::object();
        if (!arguments.is_object())
            return "Invalid arguments for '" + std::string(kGatewayExecuteTool) + "': 'arguments' must be an object.";

        if (!arguments.contains("tool") || !arguments["tool"].is_string())
            return "Invalid arguments for '" + std::string(kGatewayExecuteTool) +
                   "': 'tool' is required and must be the name of the tool to run (e.g. \"olo_shader_errors\"). "
                   "Use olo_tool_search to find one.";

        const std::string target = arguments["tool"].get<std::string>();
        if (target.empty())
            return "Invalid arguments for '" + std::string(kGatewayExecuteTool) + "': 'tool' must not be empty.";
        // No nesting. Allowing it would buy nothing and would turn a client bug into
        // unbounded recursion through HandleToolsCall.
        if (target == std::string(kGatewayExecuteTool))
            return "'" + std::string(kGatewayExecuteTool) + "' cannot execute itself; pass the target tool's name.";

        // Mirror tools/call's own contract: an absent `arguments` means {}, a
        // present-but-non-object one is malformed rather than coerced.
        if (arguments.contains("arguments") && !arguments["arguments"].is_object())
            return "Invalid arguments for '" + std::string(kGatewayExecuteTool) +
                   "': the nested 'arguments' must be an object.";

        rewritten = Json::object();
        rewritten["name"] = target;
        rewritten["arguments"] = arguments.contains("arguments") ? arguments["arguments"] : Json::object();
        // Carry the request's `_meta` through untouched: it is where a progressToken
        // lives, so dropping it would silently disable progress notifications for
        // every call made through the gateway.
        if (params.contains("_meta"))
            rewritten["_meta"] = params["_meta"];
        return std::nullopt;
    }

    Json McpServer::HandleToolsCall(const Json& id, const Json& params)
    {
        if (!params.contains("name") || !params["name"].is_string())
            return MakeError(id, kInvalidParams, "Invalid params: 'name' is required");

        // ---- the gateway's execute verb (issue #1124) -------------------------
        //
        // olo_tool_execute is an ALIAS for tools/call, not a tool with a handler: it
        // rewrites its `{tool, arguments}` payload into a plain tools/call envelope
        // and re-enters here. Doing it as a rewrite rather than as a handler that
        // invokes the target is the whole point — the target then passes through the
        // SAME inputSchema validation, the SAME write-consent gate and the SAME
        // progress/cancellation scope as a direct call, instead of a second
        // implementation of all three that could disagree with this one. (Its
        // registered ToolDef carries a handler that can never run; see
        // McpToolsGateway.cpp for why it is registered at all.)
        //
        // Recursion is bounded at one level: the rewrite rejects an inner name equal
        // to the gateway's own, so the re-entered call cannot take this branch again.
        if (params["name"].get<std::string>() == std::string(kGatewayExecuteTool))
        {
            Json rewritten;
            if (const std::optional<std::string> error = RewriteGatewayExecuteParams(params, rewritten))
            {
                // A tool-execution error, not a protocol one: the model chose these
                // arguments and can correct them (SEP-1303), the same reasoning
                // ValidateArguments' failure path below is written against.
                const ToolResult invalid = ToolResult::Error(*error);
                return MakeResult(id, Json{ { "content", invalid.Content }, { "isError", true } });
            }
            return HandleToolsCall(id, rewritten);
        }

        // Pin the tool snapshot for the WHOLE call (see the ToolsSnapshot contract):
        // a concurrent script-tool reload may swap the registry mid-dispatch, and
        // `tool` must stay valid — as must the Lua state its handler closes over.
        const ToolSnapshot snapshot = ToolsSnapshot();

        const std::string name = params["name"].get<std::string>();
        const ToolDef* tool = FindTool(*snapshot, name);
        if (tool == nullptr)
            return MakeError(id, kInvalidParams, "Unknown tool: " + name);

        // A tool whose availability predicate says no on this host is not callable,
        // and reports the same way an unregistered one does: from the client's side
        // the two are the same fact, and a second error shape would only invite a
        // client to retry. No builtin declares one (see AutomationCommand::IsAvailable).
        if (!tool->AvailableOn(*this))
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
        // #357 conformance / #306 hardening). A permissive (empty / non-object)
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

        // ---- per-call progress/cancellation scope (issue #357) ----------
        // Register this call in the in-flight registry so a concurrently-arriving
        // `notifications/cancelled` (matched by exact id value) can flag it, and
        // install the thread-local scope EmitProgress / IsCurrentCallCancelled
        // read. RAII: the registry entry lives exactly as long as the dispatch.
        //
        // This MUST precede the consent gate below (issue #610): a ProjectWrite tool
        // parked in RequestConsent on the Prompt-mode modal is otherwise invisible to
        // cancellation until it starts running. Registering first means the flag is
        // reachable while the call waits, and RequestConsent consults it — so a
        // notifications/cancelled aborts the wait promptly instead of running to the
        // human's decision or the consent timeout. The RAII guards below also ensure
        // every consent-gate early-return path still erases the in-flight entry.
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

        // Session write consent (issue #306): a project-mutating tool is gated
        // by the WriteConsentMode the user set in the MCP panel (default Disabled,
        // never persisted). This stacks on top of the enabled + bearer-token gate:
        // even an authenticated agent stays read-only w.r.t. the project until the
        // human opts in for the session. Read-only / ephemeral editor-state tools
        // (camera / viewport / render overrides) are not ProjectWrite, so no mode
        // affects them.
        //
        //   Disabled     -> refuse outright.
        //   Prompt       -> block this worker thread on RequestConsent until the human
        //                   approves/denies the per-action modal (or it times out, or a
        //                   notifications/cancelled aborts the wait — issue #610).
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
                switch (RequestConsent(*tool, arguments, scope.CancelFlag))
                {
                    case ConsentDecision::Approve:
                    case ConsentDecision::ApproveAll:
                        break; // human approved — fall through to the handler.
                    case ConsentDecision::Timeout:
                        return MakeError(id, kInvalidParams,
                                         "Write consent request timed out with no response from the editor user.");
                    case ConsentDecision::Cancel:
                        // notifications/cancelled reached the call while it was parked
                        // on the modal (issue #610). The write never ran; respond as a
                        // cancelled request (clients ignore this body per spec), same as
                        // the post-handler cancellation check below.
                        return MakeError(id, kRequestCancelledCode, "Request cancelled");
                    case ConsentDecision::Deny:
                    case ConsentDecision::Pending:
                    default:
                        return MakeError(id, kInvalidParams,
                                         "The editor user denied this write. Ask them to Approve it (or switch writes "
                                         "to \"Allow all\") in the editor's MCP Server panel.");
                }
            }
        }

        // Linearization point for the consent→handler transition (issue #610 review):
        // a notifications/cancelled observed by now — after consent was granted but
        // before the write starts — prevents the handler from running at all, instead
        // of executing the (undoable) write and discarding its result at the
        // post-handler check below. The CancelFlag is a single atomic, so this load is
        // ordered strictly before or after the cancel store in the flag's modification
        // order: load == true ⇒ the cancel is "before" write initiation ⇒ don't run;
        // load == false ⇒ execution has started and any later cancel is handled by the
        // handler's cooperative IsCurrentCallCancelled() polling + the post-handler
        // discard. (A CAS state machine would add no further guarantee here — a
        // mid-run cancel can always reach an opaque handler.)
        if (scope.CancelFlag->load(std::memory_order_acquire))
            return MakeError(id, kRequestCancelledCode, "Request cancelled");

        // Through the registry, so a handler is entered in exactly one place and the
        // adapter cannot drift from the no-transport path on what running a command
        // means (exception capture included).
        ToolResult result = AutomationRegistry::RunHandler(*tool, *this, arguments);

        // A cancelled call's result is discarded per spec ("SHOULD NOT send a
        // response"): the SSE transport suppresses the frame entirely; the
        // plain-JSON path must return SOME HTTP body, so it carries the
        // conventional kRequestCancelledCode error, which clients ignore. This
        // also covers the benign race where the tool completed just as the
        // cancellation arrived — the client has already stopped listening.
        if (scope.CancelFlag->load(std::memory_order_acquire))
            return MakeError(id, kRequestCancelledCode, "Request cancelled");

        // Audience-tagged content blocks for a tool that declared them (#673 Tier
        // 2). Rebuilding from StructuredContent — rather than annotating what the
        // handler returned — is what makes the machine block compact; the
        // single-block guard means a handler that appended its own extra block (a
        // resource_link) or already emitted the pair itself is left alone, so this
        // is idempotent and never drops content. Runs BEFORE redaction so the
        // human report is scrubbed on exactly the same terms as the JSON mirror.
        if (tool->DualAudienceContent && !result.IsError && !result.StructuredContent.is_null() &&
            result.Content.is_array() && result.Content.size() == 1)
        {
            result = ToolResult::StructuredDualAudience(result.StructuredContent,
                                                        tool->Title.empty() ? tool->Name : tool->Title);
        }

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

    const ToolDef* McpServer::FindTool(const ToolList& tools, const std::string& name)
    {
        return AutomationRegistry::Find(tools, name);
    }

    Json McpServer::HandleResourcesList(const Json& id) const
    {
        const ResourceSnapshot snapshot = ResourcesSnapshot();
        Json resources = Json::array();
        for (const auto& resource : *snapshot)
        {
            Json entry{ { "uri", resource.Uri },
                        { "name", resource.Name },
                        { "description", resource.Description },
                        { "mimeType", resource.MimeType } };
            // `size` (spec 2025-06-18) lets a client budget a read up front;
            // 0 means unknown, so omit it then.
            if (resource.SizeBytes > 0)
                entry["size"] = resource.SizeBytes;
            resources.push_back(std::move(entry));
        }
        Json result{ { "resources", std::move(resources) } };
        AddCacheHints(result, kResourcesListTtlMs, kCacheScopePublic);
        return MakeResult(id, std::move(result));
    }

    Json McpServer::HandleResourcesRead(const Json& id, const Json& params)
    {
        if (!params.contains("uri") || !params["uri"].is_string())
            return MakeError(id, kInvalidParams, "Invalid params: 'uri' is required");

        const std::string uri = params["uri"].get<std::string>();
        // Pin the snapshot for the WHOLE read — the Reader may block on a
        // MarshalRead for seconds, and a concurrent publish/eviction must not
        // dangle the ResourceDef (or free a capture's bytes) under it.
        const ResourceSnapshot snapshot = ResourcesSnapshot();
        const ResourceDef* resource = FindResource(*snapshot, uri);
        if (resource == nullptr)
            return MakeError(id, kInvalidParams, "Unknown resource: " + uri);

        // Binary resource: emit the spec's base64 `blob` contents variant.
        // (No path redaction — the payload is binary, not prose.)
        if (resource->BlobReader)
        {
            std::vector<u8> bytes;
            try
            {
                bytes = resource->BlobReader(*this);
            }
            catch (const std::exception& e)
            {
                return MakeError(id, kInvalidRequest, std::string("Failed to read resource: ") + e.what());
            }
            Json result{ { "contents",
                           Json::array({ Json{ { "uri", uri },
                                               { "mimeType", resource->MimeType },
                                               { "blob", Base64Encode(bytes) } } }) } };
            AddCacheHints(result, kResourcesReadTtlMs, kCacheScopePrivate);
            return MakeResult(id, std::move(result));
        }

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

        Json result{ { "contents",
                       Json::array({ Json{ { "uri", uri },
                                           { "mimeType", resource->MimeType },
                                           { "text", std::move(text) } } }) } };
        AddCacheHints(result, kResourcesReadTtlMs, kCacheScopePrivate);
        return MakeResult(id, std::move(result));
    }

    Json McpServer::HandleResourcesSubscribe(const Json& id, const Json& params)
    {
        if (!params.contains("uri") || !params["uri"].is_string())
            return MakeError(id, kInvalidParams, "Invalid params: 'uri' is required");

        const std::string uri = params["uri"].get<std::string>();
        const ResourceSnapshot snapshot = ResourcesSnapshot();
        const ResourceDef* resource = FindResource(*snapshot, uri);
        if (resource == nullptr)
            return MakeError(id, kInvalidParams, "Unknown resource: " + uri);

        // Refuse a subscription we could never fire. Without a ChangeToken there is
        // no cheap way to know the resource changed, so accepting would promise
        // updates that never arrive — the exact dishonesty the diagnostics server's
        // "report unknown rather than fabricate" rule forbids. Name the ones that
        // do work so the client can recover in one step.
        if (!resource->ChangeToken)
        {
            std::string subscribable;
            for (const ResourceDef& candidate : *snapshot)
            {
                if (!candidate.ChangeToken)
                    continue;
                if (!subscribable.empty())
                    subscribable += ", ";
                subscribable += candidate.Uri;
            }
            return MakeError(id, kInvalidParams,
                             "Resource is not subscribable: " + uri +
                                 (subscribable.empty() ? " (no resource on this server supports subscriptions)"
                                                       : " (subscribable resources: " + subscribable + ")"));
        }

        // Baseline the token HERE, not on the stream's first poll: a change landing
        // between this call and the next poll cycle — or before the client even
        // opens its stream — must still be reported, or the subscription silently
        // eats it. `emplace` keeps an existing subscription's baseline, so a
        // repeated subscribe is idempotent and cannot discard a pending update;
        // unsubscribe erases, so a resubscribe deliberately re-baselines.
        {
            std::lock_guard lock(m_SubscriptionMutex);
            m_ResourceSubscriptions.emplace(uri, resource->ChangeToken());
        }
        return MakeResult(id, Json::object());
    }

    Json McpServer::HandleResourcesUnsubscribe(const Json& id, const Json& params)
    {
        if (!params.contains("uri") || !params["uri"].is_string())
            return MakeError(id, kInvalidParams, "Invalid params: 'uri' is required");

        // Idempotent by design: unsubscribing something never subscribed is a
        // no-op success, so a client tearing down after a reconnect cannot be
        // tripped by a subscription the server already forgot.
        {
            std::lock_guard lock(m_SubscriptionMutex);
            m_ResourceSubscriptions.erase(params["uri"].get<std::string>());
        }
        return MakeResult(id, Json::object());
    }

    std::vector<std::string> McpServer::SubscribedUris() const
    {
        std::vector<std::string> uris;
        {
            std::lock_guard lock(m_SubscriptionMutex);
            uris.reserve(m_ResourceSubscriptions.size());
            for (const auto& [uri, baseline] : m_ResourceSubscriptions)
                uris.push_back(uri);
        }
        // Sorted so the observable is deterministic — an unordered_map's iteration
        // order is not, and a test asserting on it would be a latent flake.
        std::sort(uris.begin(), uris.end());
        return uris;
    }

    std::unordered_map<std::string, u64> McpServer::SubscriptionBaselines() const
    {
        std::lock_guard lock(m_SubscriptionMutex);
        return m_ResourceSubscriptions;
    }

    const ResourceDef* McpServer::FindResource(const ResourceList& resources, const std::string& uri)
    {
        for (const auto& resource : resources)
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
        Json result{ { "prompts", std::move(prompts) } };
        AddCacheHints(result, kPromptsListTtlMs, kCacheScopePublic);
        return MakeResult(id, std::move(result));
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
