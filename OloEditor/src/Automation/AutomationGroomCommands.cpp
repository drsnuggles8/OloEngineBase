#include "OloEnginePCH.h"
#include "Automation/AutomationGroomCommands.h"

#include "Automation/AutomationRegistry.h"
#include "Groom/GroomBindingAuthoring.h"
#include "MCP/McpSchemaBuilder.h"
#include "MCP/McpServer.h"
#include "OloEngine/Groom/GroomBinding.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Scene.h"

#include <algorithm>
#include <charconv>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace OloEngine::Automation
{
    namespace
    {
        using Json = nlohmann::json;
        namespace Schema = MCP::Schema;

        Json Error(std::string message)
        {
            return Json{ { "__error", std::move(message) } };
        }

        std::optional<UUID> ParseIdentity(const Json& args, std::string_view key)
        {
            const auto it = args.find(std::string(key));
            if (it == args.end() || !it->is_string())
            {
                return std::nullopt;
            }
            const auto& value = it->get_ref<const std::string&>();
            if (value.empty() || !std::ranges::all_of(value, [](char ch)
                                                      { return ch >= '0' && ch <= '9'; }))
            {
                return std::nullopt;
            }
            u64 parsed = 0;
            const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
            if (error != std::errc{} || end != value.data() + value.size() || parsed == 0)
            {
                return std::nullopt;
            }
            return UUID(parsed);
        }
    } // namespace

    void RegisterGroomCommands(AutomationRegistry& registry)
    {
        AutomationCommand command;
        command.Name = "olo_groom_bind";
        command.Title = "Bind a groom to a body";
        command.Description =
            "Bind the groom on `entity` to the animated surface of `target`, cook the .ologroombinding next to the "
            "groom, import it, and point the entity's GroomBindingComponent at it -- the automation surface for the "
            "inspector's Build Binding action (issue #1249).\n\n"
            "Returns the QUALITY of the bind, not just a handle: rootsExact / rootsClamped / rootsDistant and the "
            "maximum rest distance are how a groom bound to the wrong body is a number rather than a look. A large "
            "rootsDistant means the coat does not sit on that surface.\n\n"
            "Edit mode only, and deliberately so: a binding records the body's BIND POSE, and building it from an "
            "animated pose produces one that is correct for that frame and wrong for every other, which nothing "
            "downstream can detect.";
        command.Toolset = "scene";
        command.InputSchema =
            Schema::Object()
                .Prop("entity", Schema::String().Desc("Nonzero decimal UUID of the entity carrying the "
                                                      "GroomComponent. A GroomBindingComponent is added if absent."))
                .Prop("target", Schema::String().Desc("Nonzero decimal UUID of the body to bind to. Omitted binds "
                                                      "to the groom entity's own mesh."))
                .Prop("searchRadius",
                      Schema::Number().Desc("Object-space distance past which a root is counted Distant rather than "
                                            "Exact or Clamped. Default 0.25. Roots past it are still bound -- "
                                            "dropping them would leave a bald patch -- but are reported."))
                .Required({ "entity" })
                .NoAdditional();
        command.OutputSchema =
            Schema::Object()
                .Prop("ok", Schema::Bool())
                .Prop("binding", Schema::String().Desc("Asset handle of the cooked binding, as a decimal string."))
                .Prop("path", Schema::String().Desc("Project-relative path the binding was written to."))
                .Prop("rootsBound", Schema::Int().Min(0))
                .Prop("rootsExact", Schema::Int().Min(0))
                .Prop("rootsClamped", Schema::Int().Min(0))
                .Prop("rootsDistant", Schema::Int().Min(0))
                .Prop("rootsOnDegenerateTriangles", Schema::Int().Min(0))
                .Prop("maxRestDistance", Schema::Number())
                .Prop("meanRestDistance", Schema::Number())
                .Required({ "ok", "binding", "path", "rootsBound" });
        command.Annotations =
            Json{ { "readOnlyHint", false }, { "destructiveHint", true }, { "idempotentHint", false }, { "openWorldHint", false } };
        command.ProjectWrite = true;
        command.MainMarshaled = true;
        // IRREVERSIBLE, not None: None means "does not mutate", and this writes
        // an asset file and imports it. Routing it through the editor undo stack
        // would be worse than not offering undo at all -- a single Ctrl-Z would
        // take back the component assignment while leaving the cooked binding on
        // disk and in the registry, which is a project state this command cannot
        // describe. Rebinding is cheap and deterministic, and that is the honest
        // way back.
        command.Undo = AutomationUndo::Irreversible;
        command.Handler = [](IAutomationHost& host, const Json& args)
        {
            const auto getScene = host.Context().GetActiveScene;
            Json result = host.MarshalRead(
                [args, getScene]() -> Json
                {
                    if (!getScene)
                    {
                        return Error("Groom binding requires an active editor scene.");
                    }
                    Ref<Scene> scene = getScene();
                    if (!scene)
                    {
                        return Error("Groom binding is available only with an active scene.");
                    }
                    if (scene->IsRunning())
                    {
                        return Error("Stop Play mode first: a binding records the body's bind pose, and building it "
                                     "from an animated pose is wrong for every other frame.");
                    }

                    const auto groomId = ParseIdentity(args, "entity");
                    if (!groomId)
                    {
                        return Error("`entity` must be a nonzero decimal UUID string.");
                    }
                    Entity groomEntity = scene->GetEntityByUUID(*groomId);
                    if (!groomEntity)
                    {
                        return Error("No entity with that UUID exists in the active scene.");
                    }

                    Entity targetEntity = groomEntity;
                    if (args.contains("target"))
                    {
                        const auto targetId = ParseIdentity(args, "target");
                        if (!targetId)
                        {
                            return Error("`target` must be a nonzero decimal UUID string.");
                        }
                        targetEntity = scene->GetEntityByUUID(*targetId);
                        if (!targetEntity)
                        {
                            return Error("No target entity with that UUID exists in the active scene.");
                        }
                    }

                    f32 searchRadius = 0.25f;
                    if (const auto it = args.find("searchRadius"); it != args.end() && it->is_number())
                    {
                        searchRadius = it->get<f32>();
                    }
                    if (!std::isfinite(searchRadius) || searchRadius <= 0.0f)
                    {
                        return Error("`searchRadius` must be a positive finite length.");
                    }

                    const auto outcome =
                        GroomAuthoring::BuildAndImportBinding(*scene, groomEntity, targetEntity, searchRadius);
                    if (!outcome.m_Ok)
                    {
                        return Error("Build binding failed: " + outcome.m_Reason);
                    }

                    // The component is assigned HERE rather than inside the
                    // authoring helper, because the inspector's button routes
                    // the same assignment through the undo stack and this
                    // command deliberately does not -- see AutomationUndo::None
                    // above.
                    if (!groomEntity.HasComponent<GroomBindingComponent>())
                    {
                        groomEntity.AddComponent<GroomBindingComponent>();
                    }
                    auto& binding = groomEntity.GetComponent<GroomBindingComponent>();
                    binding.m_Binding = outcome.m_Binding;
                    binding.m_TargetEntity = targetEntity == groomEntity ? UUID(0) : targetEntity.GetUUID();
                    binding.m_Enabled = true;

                    return Json{
                        { "ok", true },
                        { "binding", std::to_string(static_cast<u64>(outcome.m_Binding)) },
                        { "path", outcome.m_RelativePath.generic_string() },
                        { "rootsBound", outcome.m_Stats.RootsBound },
                        { "rootsExact", outcome.m_Stats.RootsExact },
                        { "rootsClamped", outcome.m_Stats.RootsClamped },
                        { "rootsDistant", outcome.m_Stats.RootsDistant },
                        { "rootsOnDegenerateTriangles", outcome.m_Stats.RootsOnDegenerateTriangles },
                        { "maxRestDistance", outcome.m_Stats.MaxRestDistance },
                        { "meanRestDistance", outcome.m_Stats.MeanRestDistance },
                    };
                });
            if (result.contains("__error"))
            {
                return AutomationResult::Error(result.at("__error").get<std::string>());
            }
            return AutomationResult::Structured(result);
        };
        registry.Register(std::move(command));
    }
} // namespace OloEngine::Automation
