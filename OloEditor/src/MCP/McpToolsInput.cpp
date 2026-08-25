#include "OloEnginePCH.h"
#include "MCP/McpEditorLiveness.h"
#include "MCP/McpInputInject.h"
#include "MCP/McpSchemaBuilder.h"
#include "MCP/McpToolsCommon.h"

#include <atomic>
#include <memory>
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

            // Cancellation token shared with the marshaled job below. MarshalRead can
            // give up on a timeout while the job it enqueued is still queued, and that
            // job would then inject a plan AFTER this call has already returned an
            // error — input the caller never learned about, landing in whatever the
            // editor is doing by then. The job re-checks this flag immediately before
            // injecting; the timeout path raises it. Same shape as the screenshot
            // tool's PoseGuardState, and shared_ptr for the same reason: a copy
            // carried into the job keeps the flag alive even if this frame unwinds.
            const auto abandoned = std::make_shared<std::atomic<bool>>(false);

            // Step 1 (main thread): read the live viewport/window geometry, resolve the
            // coordinates against it, build the plan, and enqueue it. Done as ONE job so
            // the geometry the plan was resolved against cannot change under it (a panel
            // relayout between a separate read and a separate enqueue would silently aim
            // the click at the wrong pixel).
            //
            // `request` is captured BY VALUE: on the same timeout path, a reference to
            // this frame's local would already be dangling by the time the job ran.
            Json accepted;
            try
            {
                accepted = server.MarshalRead(
                    [&context, request, abandoned]() -> Json
                    {
                        // Refuse UP FRONT when the editor is not running frames (issue
                        // #607). The queue drains one frame per EditorLayer::OnUpdate, and
                        // an iconified editor never reaches OnUpdate at all — so accepting
                        // here would queue a plan that can never be applied, block the
                        // caller for the full settle timeout, and then leave the queue
                        // occupied so every LATER call reports "an injected input sequence
                        // is still in flight" with no hint of the real cause. That is the
                        // exact loop this refusal breaks; note it is deliberately a
                        // pre-check, not a post-hoc timeout message, because the damage is
                        // the stuck queue rather than the wasted wait.
                        if (context.GetEditorLiveness)
                        {
                            if (const McpEditorLiveness liveness = context.GetEditorLiveness();
                                EditorLiveness::IsStale(liveness))
                            {
                                return Json{ { "__error", "Cannot inject input: " + EditorLiveness::StallReason(liveness) } };
                            }
                        }

                        const McpInputViewportInfo info = context.GetInputViewportInfo();
                        // A pure `resetOffset` carries no coordinates, so it needs no
                        // viewport geometry — and it is the one call that must keep
                        // working when the Viewport panel is closed, since that is when
                        // a session most needs to hand the cursor back.
                        if (!info.Available && !request.ResetOnly)
                            return Json{ { "__error", "The editor viewport is not ready yet." } };

                        McpInputPlan plan;
                        Inject::ResolvedPoint start;
                        Inject::ResolvedPoint end;
                        Inject::ResolvedDelta delta;
                        if (const auto error = Inject::BuildPlan(request, info, plan, start, end, &delta))
                            return Json{ { "__error", *error } };

                        // Last check before the side effect: if this call already timed
                        // out and returned, injecting now would be input from nowhere.
                        if (abandoned->load(std::memory_order_acquire))
                            return Json{ { "__error", "Injection abandoned (the call timed out before the editor ran it)." } };

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
                                     { "endInside", end.InsideViewport },
                                     { "deltaWindowDx", delta.WindowDx },
                                     { "deltaWindowDy", delta.WindowDy },
                                     { "deltaFrames", delta.Frames },
                                     { "deltaReset", delta.Reset } };
                    });
            }
            catch (...)
            {
                // MarshalRead gave up (timeout, or the server is stopping) but its job
                // may still be queued. Disarm it before propagating, so it cannot
                // inject after this call has reported failure.
                abandoned->store(true, std::memory_order_release);
                throw;
            }

            if (accepted.is_object() && accepted.contains("__error"))
                return ToolResult::Error(accepted["__error"].get<std::string>());

            // Step 2: block until the plan has actually been drained AND rendered.
            // Synchronous by default is the whole point — an agent must be able to follow
            // this call immediately with olo_screenshot / olo_scene_summary and see the
            // consequence, not a frame from before the click.
            const auto baseFrame = accepted.value("baseFrame", static_cast<u64>(0));
            const auto frameCount = accepted.value("frameCount", static_cast<u32>(0));
            const bool timedOut = !AwaitRenderedFrames(server, baseFrame, static_cast<int>(frameCount) + 1);

            // Step 3 (main thread): read back what the injection changed — plus the
            // liveness, so a timeout can name its cause instead of leaving the caller
            // to guess (the editor could have been minimized DURING the settle wait,
            // which the step-1 pre-check cannot see).
            const Json stateJson = server.MarshalRead(
                [&context]() -> Json
                {
                    const McpInputStateSnapshot state = context.GetInputState();
                    Json j{ { "available", state.Available },
                            { "pending", state.Pending },
                            { "selectedId", state.SelectedEntityId },
                            { "selectedName", state.SelectedEntityName },
                            { "hoveredId", state.HoveredEntityId },
                            { "hoveredName", state.HoveredEntityName },
                            { "viewportHovered", state.ViewportHovered },
                            { "mouseX", state.MouseX },
                            { "mouseY", state.MouseY },
                            { "mouseOffsetX", state.MouseOffsetX },
                            { "mouseOffsetY", state.MouseOffsetY },
                            // The injected-cursor landing measurement (issue #854) —
                            // what makes `ok` a checked claim rather than an assertion.
                            { "cursorLandingValid", state.CursorLandingValid },
                            { "cursorAskedX", state.CursorAskedX },
                            { "cursorAskedY", state.CursorAskedY },
                            { "cursorLandedX", state.CursorLandedX },
                            { "cursorLandedY", state.CursorLandedY },
                            // ImGui's own hit-test resolution (issue #921) — named
                            // "imgui*" to avoid colliding with the entity-hover keys
                            // above, which are a completely different concept.
                            { "imguiHoveredWindow", state.HoveredWindowName },
                            { "imguiHoveredId", state.HoveredId },
                            { "imguiActiveId", state.ActiveId } };
                    if (context.GetEditorLiveness)
                    {
                        const McpEditorLiveness liveness = context.GetEditorLiveness();
                        j["liveness"] = EditorLiveness::ToJson(liveness);
                        j["stallReason"] = EditorLiveness::StallReason(liveness);
                    }
                    return j;
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
            state.MouseOffsetX = stateJson.value("mouseOffsetX", 0.0f);
            state.MouseOffsetY = stateJson.value("mouseOffsetY", 0.0f);
            state.CursorLandingValid = stateJson.value("cursorLandingValid", false);
            state.CursorAskedX = stateJson.value("cursorAskedX", 0.0f);
            state.CursorAskedY = stateJson.value("cursorAskedY", 0.0f);
            state.CursorLandedX = stateJson.value("cursorLandedX", 0.0f);
            state.CursorLandedY = stateJson.value("cursorLandedY", 0.0f);
            state.HoveredWindowName = stateJson.value("imguiHoveredWindow", std::string{});
            state.HoveredId = stateJson.value("imguiHoveredId", static_cast<u32>(0));
            state.ActiveId = stateJson.value("imguiActiveId", static_cast<u32>(0));

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

            Inject::ResolvedDelta delta;
            delta.WindowDx = accepted.value("deltaWindowDx", 0.0f);
            delta.WindowDy = accepted.value("deltaWindowDy", 0.0f);
            delta.Frames = accepted.value("deltaFrames", 1);
            delta.Reset = accepted.value("deltaReset", false);

            Json out = Inject::ToJson(result, state, info, request, start, end, timedOut, &delta,
                                      stateJson.value("stallReason", std::string{}));
            if (const Json liveness = stateJson.value("liveness", Json(nullptr)); !liveness.is_null())
                out["liveness"] = liveness;
            return ToolResult::Structured(out);
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
            "Actions: click / move / drag (mouse, ABSOLUTE), mouseDelta (mouse, RELATIVE), key "
            "(press/release/tap, with modifiers), text (type into the focused widget).\n\n"
            "ABSOLUTE vs RELATIVE. click/move/drag inject a cursor POSITION, held only while the call is in "
            "flight — right for ImGui widgets, menus and gizmo drags, and useless for anything that "
            "integrates 'mouse - lastMousePos' ACROSS frames, such as a mouse-look rig: it sees the position "
            "arrive as a spike and, when the call ends, that spike's mirror, and the two cancel to zero. Use "
            "mouseDelta {dx, dy} for those: it applies a DISPLACEMENT that persists exactly as a real mouse "
            "movement would, so the rotation is never taken back. Spread a large movement over several frames "
            "with 'frames' (a rig may reject an implausible single-frame jump as a discontinuity), and put the "
            "virtual cursor back with resetOffset:true. mouseDelta does NOT move the ImGui cursor — it targets "
            "the engine's poll-based Input:: API — so keep using move/drag for anything ImGui-facing. Every "
            "reply reports the accumulated displacement as after.mouseOffset.\n\n"
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
        tool.OutputSchema = Schema::Object()
                                .Prop("available", Schema::Bool())
                                .Prop("ok", Schema::Bool().Desc("Injection accepted, every planned frame rendered before the settle timeout, AND (for click/move/drag) the injected cursor position verifiably became the editor's cursor. False always means the 'after' state cannot be trusted as the result of what you asked for; 'message' says which of the three failed."))
                                .Prop("framesInjected", Schema::Int().Min(0))
                                .Prop("message", Schema::String())
                                .Prop("resolved", Schema::Object().Desc("Mouse actions (click/move/drag) only; omitted for key/text. A resolved point {windowX, windowY, viewportPixelX, viewportPixelY, insideViewport} for click/move, or {from, to} of two such points for drag."))
                                .Prop("resolvedDelta", Schema::Object()
                                                           .Prop("windowDx", Schema::Number())
                                                           .Prop("windowDy", Schema::Number())
                                                           .Prop("frames", Schema::Int().Min(1))
                                                           .Prop("reset", Schema::Bool())
                                                           .Desc("mouseDelta only: the requested displacement converted to window-client logical pixels (the units Input::GetMousePosition reports), the number of frames it was spread over, and whether the accumulated offset was zeroed first."))
                                .Prop("liveness", EditorLiveness::SchemaNode())
                                .Prop("viewport", Schema::Object()
                                                      .Prop("pixelWidth", Schema::Number())
                                                      .Prop("pixelHeight", Schema::Number())
                                                      .Prop("dpiScale", Schema::Number())
                                                      .Desc("Viewport geometry the coordinates were resolved against; mouse actions only."))
                                .Prop("after", Schema::Object()
                                                   .Prop("pending", Schema::Bool().Desc("True when injected events are still queued (only after a settle timeout)."))
                                                   .Prop("viewportHovered", Schema::Bool())
                                                   .Prop("mouseX", Schema::Number().Desc("The editor cursor's CURRENT position, in window-client logical pixels. This is a live read taken after the plan finished, so it is not the place to check whether an injection landed — use 'cursorLanding' for that."))
                                                   .Prop("mouseY", Schema::Number())
                                                   .Prop("cursorLanding", Schema::Object()
                                                                              .Prop("askedX", Schema::Number())
                                                                              .Prop("askedY", Schema::Number())
                                                                              .Prop("landedX", Schema::Number())
                                                                              .Prop("landedY", Schema::Number())
                                                                              .Prop("landed", Schema::Bool())
                                                                              .Desc("click/move/drag only: the last position the injection asked for and where the editor's cursor actually ended up, in window-client logical pixels. An injected position is a request to the ImGui backend, not a guarantee; when these disagree the events were delivered somewhere OTHER than where you aimed, and 'ok' is false."))
                                                   .Prop("mouseOffset", Schema::Object()
                                                                            .Prop("x", Schema::Number())
                                                                            .Prop("y", Schema::Number())
                                                                            .Desc("Accumulated relative displacement from mouseDelta calls, in window-client logical pixels. Unlike an absolute injection this PERSISTS after the call — that is what lets a delta-integrating consumer register the movement. Zero it with mouseDelta { resetOffset: true }."))
                                                   .Prop("selectedEntity", Schema::Raw(Json{ { "type", Json::array({ "object", "null" }) } }).Desc("{id, name} of the selected entity, or null when none."))
                                                   .Prop("hoveredEntity", Schema::Raw(Json{ { "type", Json::array({ "object", "null" }) } }).Desc("{id, name} of the entity under the cursor, or null when none."))
                                                   .Prop("hoveredWindow", Schema::Raw(Json{ { "type", Json::array({ "string", "null" }) } }).Desc("issue #921: the ImGui window name under the cursor on the last frame the injected plan ran (g.HoveredWindow), or null when ImGui resolved no window there at all. Distinguishes 'the click reached a window but missed every widget' (this set, hoveredId 0) from 'the click never reached ImGui' (null)."))
                                                   .Prop("hoveredId", Schema::Int().Desc("issue #921: the ImGui item id under the cursor (g.HoveredId), 0 when no widget was hovered even though hoveredWindow may be set."))
                                                   .Prop("activeId", Schema::Int().Desc("issue #921: the ImGui item id currently holding the mouse (g.ActiveId), 0 when nothing is active — normal for a plain click once ButtonBehavior releases it."))
                                                   .Desc("Post-injection editor state — the state change the injection caused."))
                                .Required({ "available", "ok", "framesInjected", "message", "after" });
        tool.MainMarshaled = true;
        tool.Handler = Handle_InputInject;
        server.RegisterTool(std::move(tool));
    }
} // namespace OloEngine::MCP
