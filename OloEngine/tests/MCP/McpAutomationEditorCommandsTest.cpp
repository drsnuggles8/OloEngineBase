// OLO_TEST_LAYER: unit
//
// The editor command registry (issue #1131, slice 1).
//
// Pins three things:
//   1. The DECLARED action table (MCP::EditorActions::kEditorActions) names only
//      commands the real surface registers, so a renamed command fails here
//      rather than leaving a stale row.
//   2. olo_editor_actions reports every declared row, joined with the registry
//      it runs in -- including a row whose command THIS registry does not hold,
//      which is marked rather than dropped.
//   3. The four actions that gained commands (pause, step, gizmo, shader pack)
//      are unavailable on a host with no editor hooks, forward their arguments
//      to the hooks when present, refuse bad arguments before touching a hook,
//      and carry the authority class they declare (only the shader pack is a
//      consented write).
//
// No editor, no window, no server: the host is a fake whose MarshalRead runs
// the job inline and whose EditorMcpContext hooks record what they were given.

#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "Automation/AutomationCommand.h"
#include "Automation/AutomationEditorCommands.h"
#include "Automation/AutomationHost.h"
#include "Automation/AutomationRegistry.h"
#include "Automation/AutomationResult.h"
#include "Automation/AutomationSchemaValidation.h"
#include "MCP/McpEditorActions.h"
#include "MCP/McpServer.h"
#include "MCP/McpTools.h"

#include <chrono>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace OloEngine::Automation::Tests
{
    namespace
    {
        using Json = nlohmann::json;
        namespace EditorActions = MCP::EditorActions;

        // A host with an editor context and nothing else. Hooks are installed
        // per test; a test that installs none is the headless case.
        class EditorHooksHost final : public IAutomationHost
        {
          public:
            [[nodiscard]] const MCP::EditorMcpContext& Context() const override
            {
                return ContextData;
            }
            [[nodiscard]] bool IsCurrentCallCancelled() const override
            {
                return false;
            }
            [[nodiscard]] bool PublishArtifact(AutomationArtifact) override
            {
                return false;
            }

            MCP::EditorMcpContext ContextData;
            int MarshalCount = 0;

          protected:
            Json MarshalReadOnMainThread(const std::function<Json()>& job, std::chrono::milliseconds) override
            {
                ++MarshalCount;
                return job();
            }
            void EmitProgressUpdate(f64, f64, const std::string&) const override {}
        };

        const std::vector<std::string> kActionCommands{ "olo_editor_pause", "olo_editor_step", "olo_editor_gizmo_set",
                                                        "olo_editor_build_shader_pack" };

        const Json* FindAction(const Json& actions, const std::string& name)
        {
            for (const Json& action : actions)
            {
                if (action.value("name", std::string{}) == name)
                    return &action;
            }
            return nullptr;
        }
    } // namespace

    class McpAutomationEditorCommands : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            RegisterEditorCommands(m_Registry);
        }

        AutomationInvocation Call(const std::string& name, const Json& arguments = Json::object(),
                                  AutomationWriteConsent consent = AutomationWriteConsent::Withheld)
        {
            return m_Registry.Invoke(m_Host, name, arguments, consent);
        }

        Json Success(const std::string& name, const Json& arguments = Json::object(),
                     AutomationWriteConsent consent = AutomationWriteConsent::Withheld)
        {
            const auto result = Call(name, arguments, consent);
            EXPECT_EQ(result.Outcome, AutomationInvocation::Status::Ok) << name << ": " << result.Message;
            EXPECT_FALSE(result.Result.IsError) << name << ": " << result.Result.Content.dump();
            return result.Result.StructuredContent;
        }

        // Fake editor hooks that record their arguments and answer with m_HookOk.
        void InstallHooks()
        {
            m_Host.ContextData.SetScenePauseState = [this](bool paused)
            {
                m_PauseCalls.push_back(paused);
                MCP::McpEditorPauseResult r;
                r.Available = true;
                r.Ok = m_HookOk;
                r.Changed = m_HookOk;
                r.Paused = m_HookOk && paused;
                r.Mode = m_HookOk ? "play" : "edit";
                r.SceneName = "Fake";
                r.Message = m_HookOk ? "Paused the play session." : "Nothing to pause: the editor is in Edit mode.";
                return r;
            };
            m_Host.ContextData.StepScene = [this](int frames)
            {
                m_StepFrames.push_back(frames);
                MCP::McpEditorStepResult r;
                r.Available = true;
                r.Ok = m_HookOk;
                r.Paused = true;
                r.FramesRequested = frames;
                r.Mode = "play";
                r.SceneName = "Fake";
                r.Message = m_HookOk ? "Stepping." : "Not paused.";
                return r;
            };
            m_Host.ContextData.SetGizmoMode = [this](const std::string& mode)
            {
                m_GizmoModes.push_back(mode);
                MCP::McpEditorGizmoResult r;
                r.Available = true;
                r.Ok = true;
                r.Changed = true;
                r.Mode = mode;
                r.Message = "Gizmo mode set to " + mode + ".";
                return r;
            };
            m_Host.ContextData.BuildShaderPack = [this]()
            {
                ++m_ShaderPackCalls;
                MCP::McpEditorShaderPackResult r;
                r.Available = true;
                r.Ok = m_HookOk;
                r.OutputPath = "assets/ShaderPack.osp";
                r.Message = m_HookOk ? "Shader pack written." : "Shader pack build failed.";
                return r;
            };
        }

        EditorHooksHost m_Host;
        AutomationRegistry m_Registry;

        bool m_HookOk = true;
        std::vector<bool> m_PauseCalls;
        std::vector<int> m_StepFrames;
        std::vector<std::string> m_GizmoModes;
        int m_ShaderPackCalls = 0;
    };

    // ---- 1. the declaration against the real surface ------------------------

    TEST_F(McpAutomationEditorCommands, EveryDeclaredCommandIsRegisteredByTheRealSurface)
    {
        AutomationRegistry registry;
        MCP::RegisterBuiltinCommands(registry);
        const AutomationRegistry::CommandSnapshot snapshot = registry.Snapshot();

        for (const EditorActions::EditorActionDescriptor& action : EditorActions::kEditorActions)
        {
            if (action.Command.empty())
                continue;
            EXPECT_NE(AutomationRegistry::Find(*snapshot, std::string(action.Command)), nullptr)
                << action.Name << " declares '" << action.Command << "', which RegisterBuiltinCommands does not register";
        }
        for (const std::string& name : kActionCommands)
        {
            const AutomationCommand* command = AutomationRegistry::Find(*snapshot, name);
            ASSERT_NE(command, nullptr) << name;
            EXPECT_EQ(command->Toolset, "editor") << name;
        }
        const AutomationCommand* actions = AutomationRegistry::Find(*snapshot, "olo_editor_actions");
        ASSERT_NE(actions, nullptr);
        EXPECT_EQ(actions->Toolset, "editor");
    }

    TEST_F(McpAutomationEditorCommands, DeclaredAuthorityMatchesWhatEachActionTouches)
    {
        const AutomationRegistry::CommandSnapshot snapshot = m_Registry.Snapshot();
        ASSERT_EQ(snapshot->size(), 5u);
        for (const AutomationCommand& command : *snapshot)
        {
            const bool isShaderPack = command.Name == "olo_editor_build_shader_pack";
            EXPECT_EQ(command.ProjectWrite, isShaderPack) << command.Name;
            EXPECT_EQ(command.Undo, isShaderPack ? AutomationUndo::Irreversible : AutomationUndo::None) << command.Name;
            EXPECT_EQ(command.MainMarshaled, command.Name != "olo_editor_actions") << command.Name;
            EXPECT_FALSE(command.OutputSchema.empty()) << command.Name;
            EXPECT_EQ(command.Annotations.value("readOnlyHint", false), command.Name == "olo_editor_actions") << command.Name;
            EXPECT_EQ(command.Annotations.value("openWorldHint", true), false) << command.Name;
        }
    }

    // ---- 2. olo_editor_actions ------------------------------------------------

    TEST_F(McpAutomationEditorCommands, ActionsReportsEveryDeclaredRowJoinedWithTheRegistry)
    {
        AutomationRegistry registry;
        MCP::RegisterBuiltinCommands(registry);
        const auto invoked = registry.Invoke(m_Host, "olo_editor_actions", Json::object());
        ASSERT_EQ(invoked.Outcome, AutomationInvocation::Status::Ok) << invoked.Message;
        ASSERT_FALSE(invoked.Result.IsError) << invoked.Result.Content.dump();
        const Json& out = invoked.Result.StructuredContent;
        // Read-only and not main-marshaled: it never touches the game thread.
        EXPECT_EQ(m_Host.MarshalCount, 0);

        EXPECT_FALSE(AutomationSchema::ValidateArguments(EditorActions::ActionsOutputSchema(), out).has_value());

        sizet expectedAutomated = 0;
        for (const auto& action : EditorActions::kEditorActions)
            expectedAutomated += action.Command.empty() ? 0u : 1u;
        EXPECT_EQ(out.at("count").get<sizet>(), EditorActions::kEditorActions.size());
        EXPECT_EQ(out.at("automated").get<sizet>(), expectedAutomated);
        ASSERT_EQ(out.at("actions").size(), EditorActions::kEditorActions.size());

        for (const Json& row : out.at("actions"))
        {
            const auto* declared = EditorActions::Find(row.at("name").get<std::string>());
            ASSERT_NE(declared, nullptr) << row.dump();
            EXPECT_EQ(row.at("menuPath"), std::string(declared->MenuPath));
            EXPECT_EQ(row.at("shortcut"), std::string(declared->Shortcut));
            if (declared->Command.empty())
            {
                EXPECT_FALSE(row.contains("command")) << row.dump();
                EXPECT_FALSE(row.contains("available")) << row.dump();
                EXPECT_FALSE(row.at("note").get<std::string>().empty()) << row.dump();
            }
            else
            {
                EXPECT_EQ(row.at("command"), std::string(declared->Command));
                EXPECT_TRUE(row.contains("available")) << row.dump();
                EXPECT_TRUE(row.contains("projectWrite")) << row.dump();
                EXPECT_TRUE(row.contains("undo")) << row.dump();
            }
        }

        // Spot checks against known commands.
        const Json* sceneNew = FindAction(out.at("actions"), "scene_new");
        ASSERT_NE(sceneNew, nullptr);
        EXPECT_TRUE(sceneNew->at("projectWrite").get<bool>());
        EXPECT_EQ(sceneNew->at("undo"), "editorUndoStack");
        EXPECT_TRUE(sceneNew->at("available").get<bool>());
        const Json* exit = FindAction(out.at("actions"), "exit");
        ASSERT_NE(exit, nullptr);
        EXPECT_FALSE(exit->contains("command"));

        // The pause action is declared automated but its command needs an editor
        // hook: unavailable here, available once the hook exists.
        const Json* pause = FindAction(out.at("actions"), "pause");
        ASSERT_NE(pause, nullptr);
        EXPECT_EQ(pause->at("command"), "olo_editor_pause");
        EXPECT_FALSE(pause->at("available").get<bool>());
        EXPECT_FALSE(pause->at("projectWrite").get<bool>());
        EXPECT_EQ(pause->at("undo"), "none");
        InstallHooks();
        const auto again = registry.Invoke(m_Host, "olo_editor_actions", Json::object());
        ASSERT_EQ(again.Outcome, AutomationInvocation::Status::Ok);
        const Json* pauseNow = FindAction(again.Result.StructuredContent.at("actions"), "pause");
        ASSERT_NE(pauseNow, nullptr);
        EXPECT_TRUE(pauseNow->at("available").get<bool>());
    }

    TEST_F(McpAutomationEditorCommands, ActionsAutomatedOnlyDropsTheUnautomatedRows)
    {
        AutomationRegistry registry;
        MCP::RegisterBuiltinCommands(registry);
        const auto invoked = registry.Invoke(m_Host, "olo_editor_actions", Json{ { "automatedOnly", true } });
        ASSERT_EQ(invoked.Outcome, AutomationInvocation::Status::Ok) << invoked.Message;
        const Json& out = invoked.Result.StructuredContent;
        EXPECT_EQ(out.at("count"), out.at("automated"));
        EXPECT_LT(out.at("count").get<sizet>(), EditorActions::kEditorActions.size());
        for (const Json& row : out.at("actions"))
            EXPECT_TRUE(row.contains("command")) << row.dump();
        EXPECT_EQ(FindAction(out.at("actions"), "exit"), nullptr);

        const auto bad = registry.Invoke(m_Host, "olo_editor_actions", Json{ { "automatedOnly", "yes" } });
        EXPECT_EQ(bad.Outcome, AutomationInvocation::Status::InvalidArguments);
    }

    TEST_F(McpAutomationEditorCommands, ActionsMarksACommandThisRegistryDoesNotHoldInsteadOfDroppingIt)
    {
        // m_Registry holds only the five editor commands, so every scene/entity
        // row names a command that is declared but not registered here.
        const Json out = Success("olo_editor_actions");
        EXPECT_EQ(out.at("count").get<sizet>(), EditorActions::kEditorActions.size());

        const Json* sceneNew = FindAction(out.at("actions"), "scene_new");
        ASSERT_NE(sceneNew, nullptr);
        EXPECT_EQ(sceneNew->at("command"), "olo_scene_new");
        EXPECT_FALSE(sceneNew->at("available").get<bool>());
        EXPECT_FALSE(sceneNew->contains("projectWrite"));
        EXPECT_TRUE(sceneNew->at("note").get<std::string>().ends_with("(not registered on this host)")) << sceneNew->dump();

        // A registered-but-hookless command is a different fact and says so differently.
        const Json* pause = FindAction(out.at("actions"), "pause");
        ASSERT_NE(pause, nullptr);
        EXPECT_FALSE(pause->at("available").get<bool>());
        EXPECT_TRUE(pause->contains("projectWrite"));
        EXPECT_EQ(pause->at("note").get<std::string>().find("not registered"), std::string::npos);
    }

    // ---- 3. the four actions -----------------------------------------------

    TEST_F(McpAutomationEditorCommands, ActionCommandsAreUnavailableWithoutEditorHooks)
    {
        for (const std::string& name : kActionCommands)
        {
            const auto result = Call(name, Json::object(), AutomationWriteConsent::Granted);
            EXPECT_EQ(result.Outcome, AutomationInvocation::Status::Unavailable) << name << ": " << result.Message;
        }
        EXPECT_EQ(m_Host.MarshalCount, 0);
        // The listing itself needs no editor.
        EXPECT_EQ(Call("olo_editor_actions").Outcome, AutomationInvocation::Status::Ok);
    }

    TEST_F(McpAutomationEditorCommands, PauseForwardsTheFlagAndReturnsTheHookResult)
    {
        InstallHooks();
        const Json out = Success("olo_editor_pause", Json{ { "paused", true } });
        EXPECT_EQ(m_PauseCalls, std::vector<bool>{ true });
        EXPECT_EQ(m_Host.MarshalCount, 1);
        EXPECT_TRUE(out.at("ok").get<bool>());
        EXPECT_TRUE(out.at("paused").get<bool>());
        EXPECT_TRUE(out.at("changed").get<bool>());
        EXPECT_EQ(out.at("mode"), "play");
        EXPECT_EQ(out.at("sceneName"), "Fake");
        EXPECT_FALSE(AutomationSchema::ValidateArguments(EditorActions::PauseOutputSchema(), out).has_value());

        Success("olo_editor_pause", Json{ { "paused", false } });
        EXPECT_EQ(m_PauseCalls, (std::vector<bool>{ true, false }));
    }

    TEST_F(McpAutomationEditorCommands, PauseRefusesBadArgumentsBeforeTouchingTheHook)
    {
        InstallHooks();
        EXPECT_EQ(Call("olo_editor_pause", Json::object()).Outcome, AutomationInvocation::Status::InvalidArguments);
        EXPECT_EQ(Call("olo_editor_pause", Json{ { "paused", "yes" } }).Outcome,
                  AutomationInvocation::Status::InvalidArguments);
        EXPECT_EQ(Call("olo_editor_pause", Json{ { "paused", true }, { "extra", 1 } }).Outcome,
                  AutomationInvocation::Status::InvalidArguments);
        EXPECT_TRUE(m_PauseCalls.empty());
        EXPECT_EQ(m_Host.MarshalCount, 0);
    }

    TEST_F(McpAutomationEditorCommands, HookFailureBecomesAnErrorResultCarryingItsMessage)
    {
        InstallHooks();
        m_HookOk = false;
        const auto result = Call("olo_editor_pause", Json{ { "paused", true } });
        ASSERT_EQ(result.Outcome, AutomationInvocation::Status::Ok) << result.Message;
        EXPECT_TRUE(result.Result.IsError);
        EXPECT_NE(result.Result.Content.dump().find("Nothing to pause"), std::string::npos) << result.Result.Content.dump();
        EXPECT_EQ(m_PauseCalls, std::vector<bool>{ true });

        const auto step = Call("olo_editor_step", Json::object());
        ASSERT_EQ(step.Outcome, AutomationInvocation::Status::Ok) << step.Message;
        EXPECT_TRUE(step.Result.IsError);
        EXPECT_NE(step.Result.Content.dump().find("Not paused"), std::string::npos);
    }

    TEST_F(McpAutomationEditorCommands, StepValidatesTheFrameRangeAndForwardsTheCount)
    {
        InstallHooks();
        EXPECT_EQ(Call("olo_editor_step", Json{ { "frames", 0 } }).Outcome, AutomationInvocation::Status::InvalidArguments);
        EXPECT_EQ(Call("olo_editor_step", Json{ { "frames", 61 } }).Outcome, AutomationInvocation::Status::InvalidArguments);
        EXPECT_EQ(Call("olo_editor_step", Json{ { "frames", "3" } }).Outcome, AutomationInvocation::Status::InvalidArguments);
        EXPECT_TRUE(m_StepFrames.empty());

        const Json one = Success("olo_editor_step");
        EXPECT_EQ(one.at("framesRequested"), 1);
        const Json five = Success("olo_editor_step", Json{ { "frames", 5 } });
        EXPECT_EQ(five.at("framesRequested"), 5);
        EXPECT_TRUE(five.at("paused").get<bool>());
        EXPECT_EQ(m_StepFrames, (std::vector<int>{ 1, 5 }));
        EXPECT_FALSE(AutomationSchema::ValidateArguments(EditorActions::StepOutputSchema(), five).has_value());
    }

    TEST_F(McpAutomationEditorCommands, GizmoRejectsUnknownModesAndForwardsKnownOnes)
    {
        InstallHooks();
        EXPECT_EQ(Call("olo_editor_gizmo_set", Json{ { "mode", "twist" } }).Outcome,
                  AutomationInvocation::Status::InvalidArguments);
        EXPECT_EQ(Call("olo_editor_gizmo_set", Json::object()).Outcome, AutomationInvocation::Status::InvalidArguments);
        EXPECT_TRUE(m_GizmoModes.empty());

        for (const std::string_view mode : EditorActions::kGizmoModes)
        {
            const Json out = Success("olo_editor_gizmo_set", Json{ { "mode", std::string(mode) } });
            EXPECT_EQ(out.at("mode"), std::string(mode));
            EXPECT_FALSE(AutomationSchema::ValidateArguments(EditorActions::GizmoOutputSchema(), out).has_value());
        }
        EXPECT_EQ(m_GizmoModes, (std::vector<std::string>{ "none", "translate", "rotate", "scale" }));
        EXPECT_TRUE(EditorActions::IsGizmoMode("rotate"));
        EXPECT_FALSE(EditorActions::IsGizmoMode("Rotate"));
    }

    TEST_F(McpAutomationEditorCommands, ShaderPackNeedsConsentAndForwardsWhenGranted)
    {
        InstallHooks();
        const auto refused = Call("olo_editor_build_shader_pack");
        EXPECT_EQ(refused.Outcome, AutomationInvocation::Status::WriteConsentWithheld) << refused.Message;
        EXPECT_EQ(m_ShaderPackCalls, 0);
        EXPECT_EQ(m_Host.MarshalCount, 0);

        const Json out = Success("olo_editor_build_shader_pack", Json::object(), AutomationWriteConsent::Granted);
        EXPECT_EQ(m_ShaderPackCalls, 1);
        EXPECT_TRUE(out.at("ok").get<bool>());
        EXPECT_EQ(out.at("outputPath"), "assets/ShaderPack.osp");
        EXPECT_FALSE(AutomationSchema::ValidateArguments(EditorActions::ShaderPackOutputSchema(), out).has_value());

        m_HookOk = false;
        const auto failed = Call("olo_editor_build_shader_pack", Json::object(), AutomationWriteConsent::Granted);
        ASSERT_EQ(failed.Outcome, AutomationInvocation::Status::Ok);
        EXPECT_TRUE(failed.Result.IsError);
        EXPECT_EQ(m_ShaderPackCalls, 2);
    }

    TEST_F(McpAutomationEditorCommands, RuntimeControlCommandsRunWithoutWriteConsent)
    {
        InstallHooks();
        // Withheld is the default consent in Call(); each of these must still run.
        EXPECT_EQ(Call("olo_editor_pause", Json{ { "paused", true } }).Outcome, AutomationInvocation::Status::Ok);
        EXPECT_EQ(Call("olo_editor_step", Json{ { "frames", 2 } }).Outcome, AutomationInvocation::Status::Ok);
        EXPECT_EQ(Call("olo_editor_gizmo_set", Json{ { "mode", "scale" } }).Outcome, AutomationInvocation::Status::Ok);
        EXPECT_EQ(Call("olo_editor_actions").Outcome, AutomationInvocation::Status::Ok);
        EXPECT_EQ(m_Host.MarshalCount, 3);
    }
} // namespace OloEngine::Automation::Tests
