#include "OloEnginePCH.h"
#include "MCP/McpInputInject.h"
#include "MCP/McpToolsCommon.h"

#include <string>
#include <utility>

// Synthetic input injection MCP tool (issue #607): olo_input_inject.
//
// The gap it closes: an agent could confirm the editor builds, renders, and reports
// its scene — but not that a CLICK HANDLER fires. Verifying interactive UI wiring
// (e.g. #593's Animation-panel bone-click -> hierarchy selection) needed a human at
// the keyboard. This tool drives the editor's own input stream so an agent can click,
// drag, and type, then observe the resulting state change.
//
// It is deliberately NOT an OS-level injector (SendInput / SetCursorPos): the editor
// is usually not the foreground window when an agent drives it (a background process
// cannot steal foreground on Windows), so OS injection would land in whatever window
// IS focused — useless, and dangerous, since it would type into the user's real
// foreground app. The events are fed into the editor's OWN GLFW/ImGui input stream
// instead (see EditorLayer::ApplyMcpInputEvent).
//
// The pure half — schema, parsing, key-name mapping, coordinate-space math, and the
// frame-quantized plan (the load-bearing timing) — lives in MCP/McpInputInject.h and
// is unit-tested without a window. This TU is the marshaling shell: resolve, enqueue,
// wait for the frames to be rendered, read back what changed.

namespace OloEngine::MCP
{
    namespace
    {
        namespace Inject = InputInject;

        ToolResult Handle_InputInject(McpServer& server, const Json& args)
        {
            const EditorMcpContext& context = server.Context();
            if (!context.InjectInput || !context.GetInputViewportInfo || !context.GetInputState)
                return ToolResult::Error("Input injection is not available in this build (no editor window).");

            Inject::Request request;
            if (const auto error = Inject::ParseRequest(args, request))
                return ToolResult::Error(*error);

            // Step 1 (main thread): read the live viewport/window geometry, resolve the
            // coordinates against it, build the plan, and enqueue it. Done as ONE job so
            // the geometry the plan was resolved against cannot change under it (a panel
            // relayout between a separate read and a separate enqueue would silently aim
            // the click at the wrong pixel).
            const Json accepted = server.MarshalRead(
                [&context, &request]() -> Json
                {
                    const McpInputViewportInfo info = context.GetInputViewportInfo();
                    if (!info.Available)
                        return Json{ { "__error", "The editor viewport is not ready yet." } };

                    McpInputPlan plan;
                    Inject::ResolvedPoint start;
                    Inject::ResolvedPoint end;
                    if (const auto error = Inject::BuildPlan(request, info, plan, start, end))
                        return Json{ { "__error", *error } };

                    const McpInputInjectResult result = context.InjectInput(plan);
                    if (!result.Available)
                        return Json{ { "__error", "Input injection is not available in this build (no editor window)." } };
                    if (!result.Ok)
                        return Json{ { "__error", result.Message.empty()
                                                      ? std::string("The editor refused the injected input.")
                                                      : result.Message } };

                    return Json{ { "baseFrame", result.BaseFrame },
                                 { "frameCount", result.FrameCount },
                                 { "message", result.Message },
                                 { "panelX", info.PanelX },
                                 { "panelY", info.PanelY },
                                 { "windowX", info.WindowX },
                                 { "windowY", info.WindowY },
                                 { "logicalWidth", info.LogicalWidth },
                                 { "logicalHeight", info.LogicalHeight },
                                 { "dpiScale", info.DpiScale },
                                 { "windowWidth", info.WindowWidth },
                                 { "windowHeight", info.WindowHeight },
                                 { "startWindowX", start.WindowX },
                                 { "startWindowY", start.WindowY },
                                 { "startPixelX", start.ViewportPixelX },
                                 { "startPixelY", start.ViewportPixelY },
                                 { "startInside", start.InsideViewport },
                                 { "endWindowX", end.WindowX },
                                 { "endWindowY", end.WindowY },
                                 { "endPixelX", end.ViewportPixelX },
                                 { "endPixelY", end.ViewportPixelY },
                                 { "endInside", end.InsideViewport } };
                });

            if (accepted.is_object() && accepted.contains("__error"))
                return ToolResult::Error(accepted["__error"].get<std::string>());

            // Step 2: block until the plan has actually been drained AND rendered.
            // Synchronous by default is the whole point — an agent must be able to follow
            // this call immediately with olo_screenshot / olo_scene_summary and see the
            // consequence, not a frame from before the click.
            const auto baseFrame = accepted.value("baseFrame", static_cast<u64>(0));
            const auto frameCount = accepted.value("frameCount", static_cast<u32>(0));
            const bool timedOut = !AwaitRenderedFrames(server, baseFrame, static_cast<int>(frameCount) + 1);

            // Step 3 (main thread): read back what the injection changed.
            const Json stateJson = server.MarshalRead(
                [&context]() -> Json
                {
                    const McpInputStateSnapshot state = context.GetInputState();
                    return Json{ { "available", state.Available },
                                 { "pending", state.Pending },
                                 { "selectedId", state.SelectedEntityId },
                                 { "selectedName", state.SelectedEntityName },
                                 { "hoveredId", state.HoveredEntityId },
                                 { "hoveredName", state.HoveredEntityName },
                                 { "viewportHovered", state.ViewportHovered },
                                 { "mouseX", state.MouseX },
                                 { "mouseY", state.MouseY } };
                });

            McpInputInjectResult result;
            result.Available = true;
            result.Ok = true;
            result.BaseFrame = baseFrame;
            result.FrameCount = frameCount;
            result.Message = accepted.value("message", std::string{});

            McpInputStateSnapshot state;
            state.Available = stateJson.value("available", false);
            state.Pending = stateJson.value("pending", false);
            state.SelectedEntityId = stateJson.value("selectedId", static_cast<u64>(0));
            state.SelectedEntityName = stateJson.value("selectedName", std::string{});
            state.HoveredEntityId = stateJson.value("hoveredId", static_cast<u64>(0));
            state.HoveredEntityName = stateJson.value("hoveredName", std::string{});
            state.ViewportHovered = stateJson.value("viewportHovered", false);
            state.MouseX = stateJson.value("mouseX", 0.0f);
            state.MouseY = stateJson.value("mouseY", 0.0f);

            McpInputViewportInfo info;
            info.Available = true;
            info.PanelX = accepted.value("panelX", 0.0f);
            info.PanelY = accepted.value("panelY", 0.0f);
            info.WindowX = accepted.value("windowX", 0.0f);
            info.WindowY = accepted.value("windowY", 0.0f);
            info.LogicalWidth = accepted.value("logicalWidth", 0.0f);
            info.LogicalHeight = accepted.value("logicalHeight", 0.0f);
            info.DpiScale = accepted.value("dpiScale", 1.0f);
            info.WindowWidth = accepted.value("windowWidth", static_cast<u32>(0));
            info.WindowHeight = accepted.value("windowHeight", static_cast<u32>(0));

            Inject::ResolvedPoint start;
            start.WindowX = accepted.value("startWindowX", 0.0f);
            start.WindowY = accepted.value("startWindowY", 0.0f);
            start.ViewportPixelX = accepted.value("startPixelX", 0.0f);
            start.ViewportPixelY = accepted.value("startPixelY", 0.0f);
            start.InsideViewport = accepted.value("startInside", false);

            Inject::ResolvedPoint end;
            end.WindowX = accepted.value("endWindowX", 0.0f);
            end.WindowY = accepted.value("endWindowY", 0.0f);
            end.ViewportPixelX = accepted.value("endPixelX", 0.0f);
            end.ViewportPixelY = accepted.value("endPixelY", 0.0f);
            end.InsideViewport = accepted.value("endInside", false);

            return ToolResult::Text(Inject::ToJson(result, state, info, request, start, end, timedOut).dump(2));
        }
    } // namespace

    void RegisterInputTools(McpServer& server)
    {
        ToolDef tool;
        tool.Name = "olo_input_inject";
        tool.Toolset = "input";
        tool.Title = "Inject synthetic input";
        // A synthetic click can drag a gizmo and a synthetic key can delete an entity,
        // so this mutates the PROJECT, not just inspection state — it goes through the
        // same consent gate as the other write tools.
        tool.ProjectWrite = true;
        tool.Annotations = MutatingAnnotations(/*idempotent*/ false);
        tool.Description =
            "Inject synthetic mouse/keyboard input into the running editor so you can verify INTERACTIVE UI "
            "wiring — that a click handler actually fires and produces the right selection/state change — "
            "instead of only confirming the editor renders. Drives the editor's own GLFW/ImGui input stream "
            "(never the OS cursor, so it cannot type into whatever window you have focused). "
            "This is a WRITE tool: refused unless 'Allow writes' is enabled in the editor's MCP panel (or "
            "OLO_MCP_ALLOW_WRITES=1), because a click can move a gizmo and a key can delete an entity.\n\n"
            "Actions: click / move / drag (mouse), key (press/release/tap, with modifiers), text (type into "
            "the focused widget).\n\n"
            "COORDINATES. 'space' picks the frame of reference:\n"
            "  viewport      (default) pixels of the olo_screenshot image at NATIVE resolution, origin "
            "top-left. Note olo_screenshot downscales to maxWidth — if it did, its pixels are NOT these.\n"
            "  viewportNorm  fractional [0,1] across the viewport. Downscale-proof; prefer it when you read "
            "a coordinate off a screenshot (x = px/imageWidth, y = py/imageHeight).\n"
            "  window        OS window client pixels — the space the ImGui panels/menus/buttons live in. "
            "Use this to click UI OUTSIDE the 3D viewport.\n\n"
            "Calls are SYNCHRONOUS: the injected events are applied one frame at a time (a click is press, "
            "hold, release across frames — a same-frame press+release is invisible to ImGui) and the call "
            "returns only once those frames have been rendered, so you can immediately follow with "
            "olo_screenshot / olo_scene_summary and see the result. The response's 'after' block already "
            "reports the resulting selected/hovered entity and cursor position.";
        tool.InputSchema = Inject::InputSchema();
        tool.MainMarshaled = true;
        tool.Handler = Handle_InputInject;
        server.RegisterTool(std::move(tool));
    }
} // namespace OloEngine::MCP
