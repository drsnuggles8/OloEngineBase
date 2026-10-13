#include "OloEnginePCH.h"
#include "MCP/McpToolsCommon.h"

#include "OloEngine/Asset/AssetSystem/RepresentationStreamingDiagnostics.h"
#include "OloEngine/Renderer/Passes/GroomRenderPass.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Terrain/Foliage/FoliageRenderer.h"

namespace OloEngine::MCP
{
    void RegisterStreamingResidencyTools(AutomationRegistry& registry)
    {
        ToolDef stats;
        stats.Name = "olo_representation_streaming_stats";
        stats.Toolset = "render";
        stats.Title = "Groom and vegetation representation residency";
        stats.Description = "Report optional detail admission, pinned drawable fallback, CPU staging, cancellation and preparation/upload costs. GPU totals are attribution, not device residency; use olo_memory_report for physical backing and heap residency.";
        stats.Annotations = ReadOnlyAnnotations();
        stats.InputSchema = Schema::EmptyObject();
        stats.OutputSchema = Schema::Object();
        stats.MainMarshaled = true;
        stats.Handler = [](IAutomationHost& host, const Json&) -> ToolResult
        {
            return ToolResult::Structured(host.MarshalRead([&host]() -> Json
                                                           {
                Json result = RepresentationStreaming::ToJson(RepresentationStreaming::Get().GetStats());
                result["vegetation"] = Json::array();
                if (const auto* pass = Renderer3D::GetGroomRenderPass())
                {
                    const auto& s = pass->GetStreamingStats();
                    result["groom"] = { { "requested", s.Requested }, { "detailDraws", s.DetailDraws },
                                        { "pendingRequests", s.Pending }, { "fallbackDraws", s.FallbackDraws },
                                        { "cancelled", s.Cancelled }, { "evictions", s.Evictions },
                                        { "residentCpuBytes", s.BaseCpuBytes }, { "pinnedGpuBytes", s.FloorGpuBytes },
                                        { "optionalGpuBytes", s.OptionalGpuBytes } };
                }
                else
                    result["groom"] = nullptr;
                if (host.Context().GetActiveScene)
                {
                    if (Ref<Scene> scene = host.Context().GetActiveScene())
                    {
                        auto view = scene->GetAllEntitiesWith<IDComponent, FoliageComponent>();
                        for (auto entity : view)
                        {
                            const auto& foliage = view.get<FoliageComponent>(entity);
                            if (!foliage.m_Renderer)
                                continue;
                            const auto s = foliage.m_Renderer->GetStreamingStats();
                            const auto& t = foliage.m_Renderer->GetRebuildTimings();
                            const auto& registry = foliage.m_Renderer->GetInstanceRegistry();
                            const auto& census = registry.GetCensus();
                            result["vegetation"].push_back({ { "entity", static_cast<u64>(view.get<IDComponent>(entity).ID) },
                                                             { "canonicalInstances", census.m_CanonicalInstances },
                                                             { "generation", registry.GetGeneration() },
                                                             { "recordsEpoch", registry.GetRecordsEpoch() },
                                                             { "unsupportedVariants", census.m_UnsupportedVariants },
                                                             { "needsRebuild", foliage.m_NeedsRebuild },
                                                             { "isRebuilding", foliage.m_Renderer->IsRebuilding() },
                                                             { "publishedRebuildSerial", foliage.m_Renderer->GetPublishedRebuildSerial() },
                                                             { "lastRebuildLatencyMs", foliage.m_Renderer->GetRebuildLatencyMs() },
                                                             { "rebuildKnownArrayBytes", foliage.m_Renderer->GetRebuildStagingCpuBytes() },
                                                             { "rebuildCpuWallMs", {
                                                                 { "total", t.TotalMs }, { "terrainSnapshot", t.TerrainSnapshotMs },
                                                                 { "geometryMaterial", t.GeometryMaterialMs }, { "placement", t.PlacementMs },
                                                                 { "registry", t.RegistryMs }, { "instanceUpload", t.InstanceUploadMs },
                                                                 { "impostor", t.ImpostorMs }, { "alphaCoverage", t.AlphaCoverageMs },
                                                                 { "bounds", t.BoundsMs }, { "cullPreparationSinceRebuild", t.CullPreparationMs } } },
                                                             { "canonicalCpuBytes", s.CanonicalCpuBytes },
                                                             { "pinnedGpuBytes", s.PinnedGpuBytes }, { "optionalGpuBytes", s.OptionalGpuBytes },
                                                             { "pendingCpuBytes", s.PendingCpuBytes }, { "residentLayers", s.ResidentLayers },
                                                             { "pendingLayers", s.PendingLayers }, { "fallbackLayers", s.FallbackLayers },
                                                             { "evictions", s.Evictions }, { "reloads", s.Reloads } });
                        }
                    }
                }
                return result; }));
        };
        registry.Register(std::move(stats));

        ToolDef rebuild;
        rebuild.Name = "olo_foliage_rebuild";
        rebuild.Toolset = "render";
        rebuild.Title = "Request foliage regeneration";
        rebuild.Description = "Mark a terrain entity's foliage for regeneration. The scene update consumes the request when terrain data is available, using the same path as an inspector edit. Does not change authored layers or save the scene. Read olo_representation_streaming_stats for completion and CPU stage costs.";
        rebuild.Annotations = MutatingAnnotations(/*idempotent*/ true);
        rebuild.ProjectWrite = true;
        rebuild.InputSchema = Schema::Object().Prop("entity", Schema::String()).Required({ "entity" }).NoAdditional();
        rebuild.OutputSchema = Schema::Object().Prop("ok", Schema::Bool()).Required({ "ok" });
        rebuild.MainMarshaled = true;
        rebuild.Handler = [](IAutomationHost& host, const Json& arguments) -> ToolResult
        {
            u64 uuid = 0;
            if (!arguments.contains("entity") || !ParseUuid(arguments.at("entity"), uuid))
                return ToolResult::Error("Expected a terrain entity UUID.");
            return ToolResult::Structured(host.MarshalRead([&host, uuid]() -> Json
                                                           {
                Ref<Scene> scene = host.Context().GetActiveScene ? host.Context().GetActiveScene() : nullptr;
                if (!scene)
                    return { { "ok", false }, { "error", "No active scene" } };
                Entity entity = scene->GetEntityByUUID(UUID(uuid));
                if (!entity || !entity.HasComponent<FoliageComponent>() || !entity.HasComponent<TerrainComponent>())
                    return { { "ok", false }, { "error", "Entity needs terrain and foliage components" } };
                auto& foliage = entity.GetComponent<FoliageComponent>();
                if (!foliage.m_Enabled || foliage.m_Layers.IsEmpty())
                    return { { "ok", false }, { "error", "Foliage is disabled or has no layers" } };
                foliage.m_NeedsRebuild = true;
                return { { "ok", true } }; }));
        };
        registry.Register(std::move(rebuild));

        ToolDef settings;
        settings.Name = "olo_representation_streaming_settings_set";
        settings.Toolset = "render";
        settings.Title = "Set groom and vegetation detail budgets";
        settings.Description = "Change the active scene's optional detail GPU, upload/frame and shared CPU staging budgets in MiB. Zero is unlimited; all three zeroes restore eager loading. The complete drawable fallback is a separately reported resident floor. Does not save the scene.";
        settings.Annotations = MutatingAnnotations(/*idempotent*/ true);
        settings.ProjectWrite = true;
        settings.InputSchema = Schema::Object()
                                   .Prop("residentMegabytes", Schema::Number().Min(0).Max(kMaxStreamingBudgetMegabytes))
                                   .Prop("uploadMegabytesPerFrame", Schema::Number().Min(0).Max(kMaxStreamingBudgetMegabytes))
                                   .Prop("stagingMegabytes", Schema::Number().Min(0).Max(kMaxStreamingBudgetMegabytes))
                                   .NoAdditional();
        settings.OutputSchema = Schema::Object().Prop("ok", Schema::Bool()).Required({ "ok" });
        settings.MainMarshaled = true;
        settings.Handler = [](IAutomationHost& host, const Json& arguments) -> ToolResult
        {
            return ToolResult::Structured(host.MarshalRead([&host, arguments]() -> Json
                                                           {
                Ref<Scene> scene = host.Context().GetActiveScene ? host.Context().GetActiveScene() : nullptr;
                if (!scene)
                    return { { "ok", false }, { "error", "No active scene" } };
                auto& s = scene->GetStreamingSettings();
                if (arguments.contains("residentMegabytes"))
                    s.RepresentationResidentMegabytes = SanitizeStreamingBudgetMegabytes(arguments.at("residentMegabytes").get<f32>());
                if (arguments.contains("uploadMegabytesPerFrame"))
                    s.RepresentationUploadMegabytesPerFrame = SanitizeStreamingBudgetMegabytes(arguments.at("uploadMegabytesPerFrame").get<f32>());
                if (arguments.contains("stagingMegabytes"))
                    s.RepresentationStagingMegabytes = SanitizeStreamingBudgetMegabytes(arguments.at("stagingMegabytes").get<f32>());
                return { { "ok", true } }; }));
        };
        registry.Register(std::move(settings));
    }
} // namespace OloEngine::MCP
