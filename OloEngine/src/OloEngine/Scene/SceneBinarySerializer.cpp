#include "OloEnginePCH.h"
#include "OloEngine/Scene/SceneSerializer.h"
#include "OloEngine/Scene/SceneBinaryFormat.h"
#include "OloEngine/Scene/SceneBinaryIO.h"

#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Core/UUID.h"
#include "OloEngine/Debug/DiagnosticsEventLog.h"
#include "OloEngine/Debug/Instrumentor.h"

#include <entt.hpp>
#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

// ============================================================================
// Binary scene sidecar cache (issue #525) — the outer .scenebin container.
//
// Per-entity HYBRID storage: an entity whose every component is binary-covered
// (see SceneSerializer::CoveredComponentIds) is written as typed binary via
// SceneSerializer::WriteEntityComponentsBinary; any other entity is written as
// its SerializeEntity text and reloaded through the unchanged
// SceneSerializer::DeserializeEntity path. So every scene round-trips correctly;
// the binary encoding just provides the speedup for the simple-entity bulk.
//
// Layout, coverage, and invalidation rules live in SceneBinaryFormat.h.
// ============================================================================

namespace OloEngine
{
    namespace
    {
        std::filesystem::path SidecarPathFor(const std::filesystem::path& sourcePath)
        {
            std::filesystem::path sidecar = sourcePath;
            sidecar += ".scenebin";
            return sidecar;
        }

        // Staleness key (size + last-write-time) of the source scene file.
        bool ComputeSourceMeta(const std::filesystem::path& sourcePath, u64& outSize, u64& outTimestamp)
        {
            std::error_code ec;
            const auto size = std::filesystem::file_size(sourcePath, ec);
            if (ec)
                return false;
            const auto writeTime = std::filesystem::last_write_time(sourcePath, ec);
            if (ec)
                return false;
            outSize = static_cast<u64>(size);
            outTimestamp = static_cast<u64>(writeTime.time_since_epoch().count());
            return true;
        }
    } // namespace

    bool SceneSerializer::TryLoadBinarySidecar(const std::filesystem::path& sourcePath)
    {
        OLO_PROFILE_FUNCTION();

        const std::filesystem::path sidecar = SidecarPathFor(sourcePath);

        std::error_code ec;
        if (!std::filesystem::exists(sidecar, ec) || ec)
            return false;

        // Slurp the whole sidecar into memory once — reads become bounded memcpys
        // and a truncated file is caught by the bounds check, not a desync.
        std::vector<u8> buffer;
        {
            std::ifstream in(sidecar, std::ios::binary | std::ios::ate);
            if (!in)
                return false;
            const std::streamoff end = in.tellg();
            if (end < static_cast<std::streamoff>(sizeof(OSceneFormat::FileHeader)))
                return false;
            buffer.resize(static_cast<sizet>(end));
            in.seekg(0);
            if (!in.read(reinterpret_cast<char*>(buffer.data()), end))
                return false;
        }

        OSceneFormat::FileHeader header{};
        std::memcpy(&header, buffer.data(), sizeof(header));

        // ── Header validity + version range (reject anything newer than this
        //    build understands; see docs/agent-rules/binary-format-versioning.md) ──
        if (header.Magic != OSceneFormat::MagicNumber)
            return false;
        if (header.Version < OSceneFormat::MinSupportedVersion || header.Version > OSceneFormat::CurrentVersion)
            return false;
        // A scene YAML-schema bump re-derives the cache rather than migrating bytes.
        if (header.SceneSchemaVersion != SceneSerializer::CurrentVersion)
            return false;
        if (header.EntityCount > OSceneFormat::MaxEntityCount)
            return false;

        // ── Staleness: source `.olo` must be the same file the sidecar came from ──
        u64 srcSize = 0;
        u64 srcTimestamp = 0;
        if (!ComputeSourceMeta(sourcePath, srcSize, srcTimestamp))
            return false;
        if (header.SourceFileSize != srcSize || header.SourceTimestamp != srcTimestamp)
            return false;

        SceneBinIO::Reader reader{ buffer.data(), buffer.size(), sizeof(OSceneFormat::FileHeader) };

        std::string sceneName;
        std::string settingsYaml;
        if (!SceneBinIO::Read(reader, sceneName) || !SceneBinIO::Read(reader, settingsYaml))
            return false;

        // Parse the tiny settings blob before touching the scene.
        YAML::Node settings;
        try
        {
            settings = YAML::Load(settingsYaml);
        }
        catch (const std::exception&)
        {
            return false;
        }
        if (settings && !settings.IsMap())
            return false;

        // Pre-size the pools every entity gets (issue #525 cheap-wins slice).
        const u32 entityCount = header.EntityCount;
        m_Scene->m_Registry.storage<entt::entity>().reserve(entityCount);
        m_Scene->m_Registry.storage<IDComponent>().reserve(entityCount);
        m_Scene->m_Registry.storage<TransformComponent>().reserve(entityCount);
        m_Scene->m_Registry.storage<TagComponent>().reserve(entityCount);

        // Decode entities, tracking every one created so the whole attempt can be
        // rolled back cleanly on any failure (leaving the scene pristine for the
        // YAML fallback). Settings are applied only after a fully-successful decode.
        std::vector<Entity> created;
        created.reserve(entityCount);
        bool ok = true;
        {
            DiagnosticsEventLog::SuppressScope suppressSpawnFlood;

            for (u32 i = 0; i < entityCount && ok; ++i)
            {
                u8 kind = 0;
                u64 uuid = 0;
                std::string tag;
                if (!SceneBinIO::Read(reader, kind) || !SceneBinIO::Read(reader, uuid) ||
                    !SceneBinIO::Read(reader, tag))
                {
                    ok = false;
                    break;
                }

                if (kind == OSceneFormat::kBinary)
                {
                    Entity entity = m_Scene->CreateEntityWithUUID(UUID(uuid), tag);
                    created.push_back(entity);
                    if (!ReadEntityComponentsBinary(reader, entity))
                    {
                        ok = false;
                        break;
                    }
                }
                else if (kind == OSceneFormat::kYaml)
                {
                    std::string entityYaml;
                    if (!SceneBinIO::Read(reader, entityYaml))
                    {
                        ok = false;
                        break;
                    }
                    try
                    {
                        YAML::Node node = YAML::Load(entityYaml);
                        if (!node || !node.IsMap())
                        {
                            ok = false;
                            break;
                        }
                        // DeserializeEntity destroys its own half-built entity and
                        // rethrows on failure, so a throw here never leaves a stray.
                        created.push_back(DeserializeEntity(uuid, tag, node));
                    }
                    catch (const std::exception&)
                    {
                        ok = false;
                        break;
                    }
                }
                else
                {
                    ok = false; // unknown kind
                    break;
                }
            }
        }

        if (!ok)
        {
            DiagnosticsEventLog::SuppressScope suppressSpawnFlood;
            for (Entity entity : created)
                m_Scene->DestroyEntity(entity);
            OLO_CORE_WARN("SceneSerializer: binary sidecar '{}' failed to decode; falling back to YAML", sidecar.string());
            return false;
        }

        // ─────────────────── commit ───────────────────
        if (settings && settings.IsMap())
            ApplySceneSettings(*m_Scene, settings);

        // Match the YAML path: the on-disk filename is the authoritative scene name.
        m_Scene->SetName(sourcePath.filename().string());

        OLO_CORE_INFO("SceneSerializer: loaded scene from binary sidecar '{}' ({} entities)",
                      sidecar.string(), entityCount);
        return true;
    }

    void SceneSerializer::WriteBinarySidecar(const std::filesystem::path& sourcePath, const YAML::Node& settingsData) const
    {
        OLO_PROFILE_FUNCTION();

        auto& registry = m_Scene->m_Registry;
        const std::unordered_set<entt::id_type>& covered = CoveredComponentIds();

        // Fast scene-level test: if no populated component pool is outside the
        // covered set, every entity is binary — skip the per-entity check entirely
        // (keeps a 100k transform-only scene at O(pools), not O(entities×pools)).
        bool sceneAllBinary = true;
        for (auto [id, pool] : registry.storage())
        {
            if (pool.size() > 0 && !covered.contains(pool.info().hash()))
            {
                sceneAllBinary = false;
                break;
            }
        }

        // Per-entity test (only consulted for mixed scenes): is every component
        // this entity carries binary-covered? If not, it is stored as YAML text.
        auto entityIsBinary = [&](entt::entity handle)
        {
            if (sceneAllBinary)
                return true;
            for (auto [id, pool] : registry.storage())
            {
                if (pool.contains(handle) && !covered.contains(pool.info().hash()))
                    return false;
            }
            return true;
        };

        u64 srcSize = 0;
        u64 srcTimestamp = 0;
        if (!ComputeSourceMeta(sourcePath, srcSize, srcTimestamp))
            return;

        // Snapshot scene-level settings: the scene document minus its (huge)
        // Entities sequence, re-emitted. Re-emitting the shared sub-nodes copies no
        // scalar text, so no float precision is lost versus the original YAML.
        std::string settingsYaml;
        {
            YAML::Node settingsOnly(YAML::NodeType::Map);
            if (settingsData && settingsData.IsMap())
            {
                for (const auto& kv : settingsData)
                {
                    const auto key = kv.first.as<std::string>(std::string{});
                    if (key != "Entities")
                        settingsOnly[key] = kv.second;
                }
            }
            YAML::Emitter emitter;
            emitter << settingsOnly;
            if (emitter.good() && emitter.c_str())
                settingsYaml = emitter.c_str();
        }

        // Collect entities sorted by UUID — deterministic, matching the YAML
        // serialize order so re-saves are byte-stable.
        std::vector<entt::entity> sorted;
        registry.view<entt::entity>().each([&sorted](auto entityID)
                                           { sorted.push_back(entityID); });
        std::ranges::sort(sorted, [&registry](entt::entity a, entt::entity b)
                          {
                              const u64 ua = static_cast<u64>(registry.get<IDComponent>(a).ID);
                              const u64 ub = static_cast<u64>(registry.get<IDComponent>(b).ID);
                              return ua < ub; });

        // Write to a temp file, then atomically rename, so a crash mid-write never
        // leaves a half-written file that reads as valid.
        const std::filesystem::path sidecar = SidecarPathFor(sourcePath);
        std::filesystem::path tmp = sidecar;
        tmp += ".tmp";

        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            if (!out)
                return;

            OSceneFormat::FileHeader header;
            header.SceneSchemaVersion = SceneSerializer::CurrentVersion;
            header.SourceFileSize = srcSize;
            header.SourceTimestamp = srcTimestamp;
            header.EntityCount = static_cast<u32>(sorted.size());
            out.write(reinterpret_cast<const char*>(&header), sizeof(header));

            SceneBinIO::Write(out, m_Scene->GetName());
            SceneBinIO::Write(out, settingsYaml);

            for (entt::entity handle : sorted)
            {
                Entity entity{ handle, m_Scene.get() };
                const u64 uuid = static_cast<u64>(registry.get<IDComponent>(handle).ID);
                const std::string& tag = registry.get<TagComponent>(handle).Tag;
                const bool binary = entityIsBinary(handle);

                SceneBinIO::Write(out, static_cast<u8>(binary ? OSceneFormat::kBinary : OSceneFormat::kYaml));
                SceneBinIO::Write(out, uuid);
                SceneBinIO::Write(out, tag);

                if (binary)
                {
                    WriteEntityComponentsBinary(out, entity);
                }
                else
                {
                    YAML::Emitter em;
                    SerializeEntity(em, entity);
                    SceneBinIO::Write(out, std::string(em.good() && em.c_str() ? em.c_str() : ""));
                }
            }

            out.flush();
            if (!out.good())
            {
                out.close();
                std::error_code ec;
                std::filesystem::remove(tmp, ec);
                OLO_CORE_WARN("SceneSerializer: failed to write binary sidecar '{}'", sidecar.string());
                return;
            }
        }

        std::error_code ec;
        std::filesystem::rename(tmp, sidecar, ec);
        if (ec)
        {
            std::filesystem::remove(tmp, ec);
            OLO_CORE_WARN("SceneSerializer: failed to finalize binary sidecar '{}': {}", sidecar.string(), ec.message());
            return;
        }

        OLO_CORE_TRACE("SceneSerializer: wrote binary sidecar '{}' ({} entities)", sidecar.string(), sorted.size());
    }

} // namespace OloEngine
