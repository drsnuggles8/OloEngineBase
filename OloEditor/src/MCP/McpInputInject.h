#pragma once

// Pure, engine-light core of the olo_input_inject MCP tool (issue #607): the
// inputSchema, argument parsing, key-name -> GLFW keycode mapping, coordinate-space
// math, the frame-quantized injection PLAN builder, and the result -> JSON shaping.
//
// Why a plan instead of "just send the event": a click is not an instant. ImGui only
// registers one if the button is held DOWN across at least one full frame (a same-frame
// press+release never fires IsItemClicked), and the editor's viewport entity picking is
// TWO frames behind the cursor (it issues an async PBO readback at the ImGui mouse
// position one frame and reads it back the next). So a "click at (x, y)" that actually
// selects the entity under (x, y) is: move -> settle a few frames -> press -> hold ->
// release. Getting that wrong makes the tool silently do nothing, which is worse than
// not having it. BuildPlan owns that timing and is unit-tested here rather than being
// re-derived by eye in EditorLayer.
//
// ABSOLUTE vs RELATIVE (the "mouseDelta" action, issue #607). Every action above
// injects an ABSOLUTE cursor position, held only while the plan is in flight and
// dropped when it drains. That is exactly right for ImGui widgets, menus and gizmo
// drags, which read a position — and provably useless for a consumer that integrates
// `Input::GetMousePosition() - lastPos` ACROSS frames, such as a mouse-look rig. Such
// a consumer sees the override arrive as a one-frame spike and, the frame the plan
// drains, that spike's exact mirror; the two cancel to zero net rotation. Measured on
// the #645 player rig (PR #748): four equal horizontal moves each produced
// dYaw = 0.000, with pitch pinned at exactly MaxPitchDeg.
//
// Holding the override for LONGER cannot fix that. The mirror is produced by
// REVERTING the override, not by its brevity, so the integral over any plan is still
// exactly zero however many frames it is latched for — which is why this shipped as a
// relative action rather than the `holdFrames` parameter the gap report also floated.
// A displacement is modelled the way a real mouse implements one: SyntheticInput
// accumulates a persistent cursor OFFSET that deliberately outlives the plan, so the
// consumer registers the movement once and never sees it taken back. Absolute
// injection is untouched (the override still wins outright while set), and hardware
// motion still passes through — the reported cursor is simply shifted, exactly as a
// real movement would have left it. `resetOffset` is how you put it back.
//
// The ACTIONS cannot live in this header — feeding ImGuiIO / the GLFW callbacks and
// reading the ECS selection are editor + window bound, and a unit test must not need a
// window. So they are routed through the EditorMcpContext hooks (GetInputViewportInfo /
// InjectInput / GetInputState): the editor wires them to the ImGui GLFW backend, a
// headless host leaves them null ("not available"), and a test injects a fake. Same
// split as McpSceneControl.h / McpReloadScript.h — so this pulls no extra editor TU
// into the MCP test binary.

#include "MCP/McpSchemaBuilder.h"
#include "MCP/McpServer.h"
#include "OloEngine/Core/KeyCodes.h"
#include "OloEngine/Core/MouseCodes.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace OloEngine::MCP::InputInject
{
    using Json = nlohmann::json;

    // ---- timing constants (the load-bearing part) ------------------------------

    // Frames to let the editor absorb a cursor move before a button press lands on
    // it. One for the ImGui event queue to be applied at NewFrame, one for the
    // picking pass to ISSUE its async readback at the new pixel, one for it to READ
    // that readback back into m_HoveredEntity — plus one spare so a single
    // frame-budget-throttled frame (which skips picking entirely) doesn't shift the
    // whole plan under us.
    inline constexpr int s_MoveSettleFrames = 4;
    // Frames a button stays down between press and release. Must be >= 1 so ImGui
    // sees a full frame of MouseDown; a same-frame press+release is invisible to it.
    inline constexpr int s_ButtonHoldFrames = 2;
    // Idle frames between the two clicks of a double-click.
    inline constexpr int s_DoubleClickGapFrames = 1;
    // Frames appended after the last event so the editor has processed and RENDERED
    // the consequence before the tool reads back the resulting state / the agent
    // takes a screenshot.
    inline constexpr int s_TrailingSettleFrames = 3;

    // ---- request -----------------------------------------------------------------

    enum class Action
    {
        Click,
        Move,
        Drag,
        MouseDelta,
        Key,
        Text
    };

    enum class Space
    {
        // Pixels of the viewport framebuffer at its NATIVE resolution — the exact
        // pixel grid olo_screenshot reads back BEFORE its maxWidth downscale. Origin
        // top-left, +Y down.
        Viewport,
        // Fractional [0, 1] across the viewport, origin top-left. Downscale-proof:
        // the right space to use after a downscaled olo_screenshot.
        ViewportNormalized,
        // OS window client-area pixels (logical), origin top-left. The space the ImGui
        // panels live in — use it to click a menu / button / hierarchy row.
        Window
    };

    enum class KeyAction
    {
        Press, // down, and LEAVE it held (pair with a later "release")
        Release,
        Tap // down, hold a frame, up
    };

    struct Modifiers
    {
        bool Ctrl = false;
        bool Shift = false;
        bool Alt = false;

        [[nodiscard]] bool Any() const noexcept
        {
            return Ctrl || Shift || Alt;
        }
    };

    struct Request
    {
        Action Act = Action::Click;
        Space CoordSpace = Space::Viewport;
        Modifiers Mods;

        f32 X = 0.0f; // click / move / drag-from
        f32 Y = 0.0f;
        f32 ToX = 0.0f; // drag-to
        f32 ToY = 0.0f;

        i32 Button = Mouse::ButtonLeft;
        bool DoubleClick = false;
        int Steps = 8; // drag interpolation steps

        // mouseDelta: a RELATIVE displacement, in `CoordSpace`'s units, and how many
        // consecutive frames to spread it over. ResetOffset zeroes the accumulated
        // displacement first (the "put the virtual cursor back on the hardware one"
        // operation).
        f32 Dx = 0.0f;
        f32 Dy = 0.0f;
        int DeltaFrames = 1;
        bool ResetOffset = false;

        i32 KeyCode = 0; // GLFW key code (== OloEngine KeyCode)
        std::string KeyName;
        KeyAction KeyAct = KeyAction::Tap;

        std::string Text;
    };

    // ---- key names ---------------------------------------------------------------

    struct KeyNameEntry
    {
        std::string_view Name;
        i32 Code;
    };

    // The named (non-printable) keys. Printable single characters ("a", "5", "-") are
    // handled by the ASCII fold below, so this table only carries what a character
    // cannot express. Names are lowercase; lookup folds the caller's input.
    [[nodiscard]] inline const std::vector<KeyNameEntry>& NamedKeys()
    {
        static const std::vector<KeyNameEntry> table = {
            { "escape", Key::Escape },
            { "esc", Key::Escape },
            { "enter", Key::Enter },
            { "return", Key::Enter },
            { "tab", Key::Tab },
            { "backspace", Key::Backspace },
            { "insert", Key::Insert },
            { "delete", Key::Delete },
            { "del", Key::Delete },
            { "right", Key::Right },
            { "left", Key::Left },
            { "down", Key::Down },
            { "up", Key::Up },
            { "pageup", Key::PageUp },
            { "pagedown", Key::PageDown },
            { "home", Key::Home },
            { "end", Key::End },
            { "space", Key::Space },
            { "capslock", Key::CapsLock },
            { "f1", Key::F1 },
            { "f2", Key::F2 },
            { "f3", Key::F3 },
            { "f4", Key::F4 },
            { "f5", Key::F5 },
            { "f6", Key::F6 },
            { "f7", Key::F7 },
            { "f8", Key::F8 },
            { "f9", Key::F9 },
            { "f10", Key::F10 },
            { "f11", Key::F11 },
            { "f12", Key::F12 },
            { "ctrl", Key::LeftControl },
            { "control", Key::LeftControl },
            { "leftcontrol", Key::LeftControl },
            { "rightcontrol", Key::RightControl },
            { "shift", Key::LeftShift },
            { "leftshift", Key::LeftShift },
            { "rightshift", Key::RightShift },
            { "alt", Key::LeftAlt },
            { "leftalt", Key::LeftAlt },
            { "rightalt", Key::RightAlt },
        };
        return table;
    }

    // Lowercase an ASCII string (locale-independent — key names are ASCII).
    [[nodiscard]] inline std::string AsciiLower(std::string_view text)
    {
        std::string out(text);
        for (char& c : out)
        {
            if (c >= 'A' && c <= 'Z')
                c = static_cast<char>(c - 'A' + 'a');
        }
        return out;
    }

    // Map a key NAME to a GLFW key code. Accepts a single printable ASCII character
    // ("s", "S", "5", "-") or one of the named keys above (case-insensitive).
    // Returns nullopt when the name is not recognised.
    [[nodiscard]] inline std::optional<i32> MapKeyName(std::string_view name)
    {
        if (name.empty())
            return std::nullopt;

        const std::string folded = AsciiLower(name);

        if (folded.size() == 1)
        {
            const char c = folded[0];
            // OloEngine KeyCodes are GLFW codes, and GLFW's printable-key codes are the
            // UPPERCASE ASCII values (GLFW_KEY_A == 'A' == 65, GLFW_KEY_0 == '0').
            if (c >= 'a' && c <= 'z')
                return static_cast<i32>(c - 'a' + 'A');
            if (c >= '0' && c <= '9')
                return static_cast<i32>(c);
            switch (c)
            {
                case ' ':
                    return Key::Space;
                case '\'':
                    return Key::Apostrophe;
                case ',':
                    return Key::Comma;
                case '-':
                    return Key::Minus;
                case '.':
                    return Key::Period;
                case '/':
                    return Key::Slash;
                case ';':
                    return Key::Semicolon;
                case '=':
                    return Key::Equal;
                case '[':
                    return Key::LeftBracket;
                case '\\':
                    return Key::Backslash;
                case ']':
                    return Key::RightBracket;
                case '`':
                    return Key::GraveAccent;
                default:
                    return std::nullopt;
            }
        }

        for (const KeyNameEntry& entry : NamedKeys())
        {
            if (entry.Name == folded)
                return entry.Code;
        }
        return std::nullopt;
    }

    [[nodiscard]] inline std::optional<i32> MapButtonName(std::string_view name)
    {
        const std::string folded = AsciiLower(name);
        if (folded == "left")
            return Mouse::ButtonLeft;
        if (folded == "right")
            return Mouse::ButtonRight;
        if (folded == "middle")
            return Mouse::ButtonMiddle;
        return std::nullopt;
    }

    [[nodiscard]] inline std::optional<Space> MapSpaceName(std::string_view name)
    {
        const std::string folded = AsciiLower(name);
        if (folded == "viewport")
            return Space::Viewport;
        if (folded == "viewportnorm")
            return Space::ViewportNormalized;
        if (folded == "window")
            return Space::Window;
        return std::nullopt;
    }

    // ---- argument parsing ---------------------------------------------------------

    // Parse the tool arguments into a Request. The server has already enforced the
    // inputSchema (types, enums, required `action`), so this only enforces the
    // per-action, cross-field constraints JSON-Schema cannot express: which coordinate
    // / key / text fields each action actually requires, and that every number is
    // finite. Returns a human-readable error, or nullopt on success.
    [[nodiscard]] inline std::optional<std::string> ParseRequest(const Json& args, Request& out)
    {
        const std::string action = AsciiLower(args.value("action", std::string{}));
        if (action == "click")
            out.Act = Action::Click;
        else if (action == "move")
            out.Act = Action::Move;
        else if (action == "drag")
            out.Act = Action::Drag;
        else if (action == "mousedelta")
            out.Act = Action::MouseDelta;
        else if (action == "key")
            out.Act = Action::Key;
        else if (action == "text")
            out.Act = Action::Text;
        else
            return "Invalid 'action': expected one of click, move, drag, key, text.";

        // Coordinate space (mouse actions only; harmless otherwise).
        if (args.contains("space"))
        {
            const auto space = MapSpaceName(args["space"].get<std::string>());
            if (!space)
                return "Invalid 'space': expected one of viewport, viewportNorm, window.";
            out.CoordSpace = *space;
        }

        // Modifiers.
        if (args.contains("modifiers"))
        {
            if (!args["modifiers"].is_array())
                return "Invalid 'modifiers': expected an array of \"ctrl\" / \"shift\" / \"alt\".";
            for (const Json& entry : args["modifiers"])
            {
                if (!entry.is_string())
                    return "Invalid 'modifiers': expected an array of \"ctrl\" / \"shift\" / \"alt\".";
                const std::string modifier = AsciiLower(entry.get<std::string>());
                if (modifier == "ctrl" || modifier == "control")
                    out.Mods.Ctrl = true;
                else if (modifier == "shift")
                    out.Mods.Shift = true;
                else if (modifier == "alt")
                    out.Mods.Alt = true;
                else
                    return "Invalid 'modifiers' entry '" + modifier + "': expected ctrl, shift, or alt.";
            }
        }

        const auto readFinite = [&args](const char* name, f32& target) -> std::optional<std::string>
        {
            if (!args.contains(name) || !args[name].is_number())
                return std::string("Missing required argument '") + name + "' (a number).";
            const auto value = args[name].get<f32>();
            if (!std::isfinite(value))
                return std::string("Invalid '") + name + "': expected a finite number.";
            target = value;
            return std::nullopt;
        };

        switch (out.Act)
        {
            case Action::Click:
            case Action::Move:
            {
                if (auto error = readFinite("x", out.X))
                    return error;
                if (auto error = readFinite("y", out.Y))
                    return error;
                break;
            }
            case Action::Drag:
            {
                if (auto error = readFinite("fromX", out.X))
                    return error;
                if (auto error = readFinite("fromY", out.Y))
                    return error;
                if (auto error = readFinite("toX", out.ToX))
                    return error;
                if (auto error = readFinite("toY", out.ToY))
                    return error;
                if (args.contains("steps"))
                    out.Steps = static_cast<int>(std::clamp<i64>(args["steps"].get<i64>(), 1, 64));
                break;
            }
            case Action::MouseDelta:
            {
                out.ResetOffset = args.value("resetOffset", false);
                // dx/dy are required UNLESS this is a pure reset — "put the virtual
                // cursor back" is a meaningful call on its own, and demanding
                // dx:0, dy:0 to express it would be noise.
                const bool hasDx = args.contains("dx");
                const bool hasDy = args.contains("dy");
                if (!hasDx && !hasDy && out.ResetOffset)
                    break;
                if (auto error = readFinite("dx", out.Dx))
                    return error;
                if (auto error = readFinite("dy", out.Dy))
                    return error;
                if (args.contains("frames"))
                    out.DeltaFrames = static_cast<int>(std::clamp<i64>(args["frames"].get<i64>(), 1, 64));
                break;
            }
            case Action::Key:
            {
                if (!args.contains("key") || !args["key"].is_string())
                    return "Missing required argument 'key' (e.g. \"s\", \"delete\", \"f5\").";
                out.KeyName = args["key"].get<std::string>();
                const auto code = MapKeyName(out.KeyName);
                if (!code)
                    return "Unknown 'key' \"" + out.KeyName +
                           "\": expected a single printable character (\"s\", \"5\") or a named key "
                           "(escape, enter, tab, backspace, delete, up/down/left/right, home, end, "
                           "pageup, pagedown, space, f1-f12, ctrl, shift, alt).";
                out.KeyCode = *code;

                if (args.contains("keyAction"))
                {
                    const std::string keyAction = AsciiLower(args["keyAction"].get<std::string>());
                    if (keyAction == "press")
                        out.KeyAct = KeyAction::Press;
                    else if (keyAction == "release")
                        out.KeyAct = KeyAction::Release;
                    else if (keyAction == "tap")
                        out.KeyAct = KeyAction::Tap;
                    else
                        return "Invalid 'keyAction': expected press, release, or tap.";
                }
                break;
            }
            case Action::Text:
            {
                if (!args.contains("text") || !args["text"].is_string())
                    return "Missing required argument 'text' (the characters to type).";
                out.Text = args["text"].get<std::string>();
                if (out.Text.empty())
                    return "Invalid 'text': must not be empty.";
                if (out.Text.size() > 256)
                    return "Invalid 'text': at most 256 characters per call.";
                break;
            }
        }

        if (out.Act == Action::Click || out.Act == Action::Drag)
        {
            if (args.contains("button"))
            {
                const auto button = MapButtonName(args["button"].get<std::string>());
                if (!button)
                    return "Invalid 'button': expected left, right, or middle.";
                out.Button = *button;
            }
            out.DoubleClick = args.value("doubleClick", false);
        }
        return std::nullopt;
    }

    // ---- coordinate spaces ---------------------------------------------------------

    // The viewport framebuffer's native pixel size — what olo_screenshot reads back
    // before downscaling, and therefore what Space::Viewport coordinates index.
    [[nodiscard]] inline f32 ViewportPixelWidth(const McpInputViewportInfo& info)
    {
        return info.LogicalWidth * info.DpiScale;
    }
    [[nodiscard]] inline f32 ViewportPixelHeight(const McpInputViewportInfo& info)
    {
        return info.LogicalHeight * info.DpiScale;
    }

    struct ResolvedPoint
    {
        // Window-client logical pixels (what the injected MousePos event carries).
        f32 WindowX = 0.0f;
        f32 WindowY = 0.0f;
        // The same point expressed in native viewport pixels — echoed back so the
        // agent can confirm which viewport pixel the click actually landed on.
        f32 ViewportPixelX = 0.0f;
        f32 ViewportPixelY = 0.0f;
        bool InsideViewport = false;
    };

    // Resolve an (x, y) in `space` to window-client logical pixels.
    //
    // The chain, in logical pixels throughout:
    //   viewport-native-px --(/DpiScale)--> viewport-logical
    //   + panel origin (ImGui screen coords) --> ImGui screen
    //   - window client origin (ImGui screen coords) --> window client
    // The last step is the multi-viewport correction: with ImGuiConfigFlags_ViewportsEnable
    // (which this editor sets) ImGui screen coordinates are DESKTOP coordinates, so the
    // window's own position must come back out. GLFW's cursor-pos callback — where the
    // injected event enters — speaks window-client coordinates and adds the window
    // position back itself, so the two conversions cancel exactly.
    [[nodiscard]] inline std::optional<std::string> ResolvePoint(const McpInputViewportInfo& info, Space space,
                                                                 f32 x, f32 y, ResolvedPoint& out)
    {
        const f32 pixelWidth = ViewportPixelWidth(info);
        const f32 pixelHeight = ViewportPixelHeight(info);

        f32 imguiScreenX = 0.0f;
        f32 imguiScreenY = 0.0f;

        switch (space)
        {
            case Space::Viewport:
            case Space::ViewportNormalized:
            {
                if (pixelWidth < 1.0f || pixelHeight < 1.0f)
                    return "The editor viewport has no size yet (is the Viewport panel open?).";

                const f32 pixelX = (space == Space::Viewport) ? x : x * pixelWidth;
                const f32 pixelY = (space == Space::Viewport) ? y : y * pixelHeight;

                out.ViewportPixelX = pixelX;
                out.ViewportPixelY = pixelY;
                out.InsideViewport = pixelX >= 0.0f && pixelY >= 0.0f && pixelX < pixelWidth && pixelY < pixelHeight;
                if (!out.InsideViewport)
                {
                    return "Point (" + std::to_string(static_cast<int>(pixelX)) + ", " +
                           std::to_string(static_cast<int>(pixelY)) + ") is outside the viewport, which is " +
                           std::to_string(static_cast<int>(pixelWidth)) + "x" +
                           std::to_string(static_cast<int>(pixelHeight)) +
                           " native pixels. Viewport coordinates are pixels of the olo_screenshot image at NATIVE "
                           "resolution — if you read the pixel off a downscaled screenshot, use space:\"viewportNorm\" "
                           "with fractional [0,1] coordinates instead.";
                }

                imguiScreenX = info.PanelX + (pixelX / info.DpiScale);
                imguiScreenY = info.PanelY + (pixelY / info.DpiScale);
                break;
            }
            case Space::Window:
            {
                // Already window-client; lift into ImGui screen space so the shared
                // conversion below (and the viewport-pixel echo) still applies.
                imguiScreenX = info.WindowX + x;
                imguiScreenY = info.WindowY + y;

                const f32 pixelX = (imguiScreenX - info.PanelX) * info.DpiScale;
                const f32 pixelY = (imguiScreenY - info.PanelY) * info.DpiScale;
                out.ViewportPixelX = pixelX;
                out.ViewportPixelY = pixelY;
                out.InsideViewport = pixelWidth >= 1.0f && pixelHeight >= 1.0f && pixelX >= 0.0f && pixelY >= 0.0f &&
                                     pixelX < pixelWidth && pixelY < pixelHeight;
                break;
            }
        }

        out.WindowX = imguiScreenX - info.WindowX;
        out.WindowY = imguiScreenY - info.WindowY;

        if (info.WindowWidth > 0 && info.WindowHeight > 0)
        {
            if (out.WindowX < 0.0f || out.WindowY < 0.0f ||
                out.WindowX >= static_cast<f32>(info.WindowWidth) ||
                out.WindowY >= static_cast<f32>(info.WindowHeight))
            {
                return "Resolved window-client point (" + std::to_string(static_cast<int>(out.WindowX)) + ", " +
                       std::to_string(static_cast<int>(out.WindowY)) + ") is outside the editor window (" +
                       std::to_string(info.WindowWidth) + "x" + std::to_string(info.WindowHeight) + ").";
            }
        }
        return std::nullopt;
    }

    // Convert a RELATIVE displacement in `space` into the window-client logical
    // pixels the poll-based Input:: API reports — the units a delta-integrating
    // consumer's sensitivity is expressed in.
    //
    // No bounds check, unlike ResolvePoint: a displacement names no position, so
    // "outside the viewport" is not a thing it can be. The scale factors mirror
    // ResolvePoint exactly so the two spaces mean the same thing for both actions:
    //   viewport      native viewport pixels -> logical, i.e. / DpiScale
    //   viewportNorm  fraction of the viewport -> logical, i.e. * logical extent
    //   window        already logical window pixels
    [[nodiscard]] inline std::optional<std::string> ResolveDelta(const McpInputViewportInfo& info, Space space,
                                                                 f32 dx, f32 dy, f32& outDx, f32& outDy)
    {
        switch (space)
        {
            case Space::Viewport:
            {
                if (!(info.DpiScale > 0.0f))
                    return "The editor viewport has no DPI scale yet (is the Viewport panel open?).";
                outDx = dx / info.DpiScale;
                outDy = dy / info.DpiScale;
                break;
            }
            case Space::ViewportNormalized:
            {
                if (info.LogicalWidth < 1.0f || info.LogicalHeight < 1.0f)
                    return "The editor viewport has no size yet (is the Viewport panel open?).";
                outDx = dx * info.LogicalWidth;
                outDy = dy * info.LogicalHeight;
                break;
            }
            case Space::Window:
            {
                outDx = dx;
                outDy = dy;
                break;
            }
        }
        return std::nullopt;
    }

    // ---- plan building --------------------------------------------------------------

    namespace Detail
    {
        inline McpInputEvent MousePos(f32 x, f32 y)
        {
            McpInputEvent e;
            e.Type = McpInputEvent::Kind::MousePos;
            e.X = x;
            e.Y = y;
            return e;
        }
        inline McpInputEvent MouseDeltaEvent(f32 dx, f32 dy)
        {
            McpInputEvent e;
            e.Type = McpInputEvent::Kind::MouseDelta;
            e.X = dx;
            e.Y = dy;
            return e;
        }
        inline McpInputEvent MouseOffsetResetEvent()
        {
            McpInputEvent e;
            e.Type = McpInputEvent::Kind::MouseOffsetReset;
            return e;
        }
        inline McpInputEvent MouseButton(i32 button, bool down)
        {
            McpInputEvent e;
            e.Type = McpInputEvent::Kind::MouseButton;
            e.Code = button;
            e.Down = down;
            return e;
        }
        inline McpInputEvent KeyEvent(i32 key, bool down)
        {
            McpInputEvent e;
            e.Type = McpInputEvent::Kind::Key;
            e.Code = key;
            e.Down = down;
            return e;
        }
        inline McpInputEvent CharEvent(u32 codepoint)
        {
            McpInputEvent e;
            e.Type = McpInputEvent::Kind::Char;
            e.Code = static_cast<i32>(codepoint);
            return e;
        }

        inline void AppendModifiers(std::vector<McpInputEvent>& frame, const Modifiers& mods, bool down)
        {
            if (mods.Ctrl)
                frame.push_back(KeyEvent(Key::LeftControl, down));
            if (mods.Shift)
                frame.push_back(KeyEvent(Key::LeftShift, down));
            if (mods.Alt)
                frame.push_back(KeyEvent(Key::LeftAlt, down));
        }

        // Append `count` frames with no events (settle time).
        inline void AppendIdleFrames(McpInputPlan& plan, int count)
        {
            for (int i = 0; i < count; ++i)
                plan.Frames.emplace_back();
        }
    } // namespace Detail

    // The resolved form of a "mouseDelta" request: the total displacement in
    // window-client logical pixels, and how it was quantized into frames.
    struct ResolvedDelta
    {
        f32 WindowDx = 0.0f;
        f32 WindowDy = 0.0f;
        int Frames = 1;
        bool Reset = false;
    };

    // Turn a parsed Request into the frame-by-frame plan the editor drains.
    // `info` is only consulted for the mouse actions (to resolve coordinates); pass a
    // default-constructed one for key/text. `outStart` / `outEnd` receive the resolved
    // points (for the result JSON) when the action is coordinate-bearing; `outDelta`
    // (optional) receives the resolved displacement for Action::MouseDelta.
    [[nodiscard]] inline std::optional<std::string> BuildPlan(const Request& request, const McpInputViewportInfo& info,
                                                              McpInputPlan& plan, ResolvedPoint& outStart,
                                                              ResolvedPoint& outEnd, ResolvedDelta* outDelta = nullptr)
    {
        using namespace Detail;
        plan.Frames.clear();

        switch (request.Act)
        {
            case Action::Move:
            {
                if (auto error = ResolvePoint(info, request.CoordSpace, request.X, request.Y, outStart))
                    return error;
                outEnd = outStart;
                plan.Frames.push_back({ MousePos(outStart.WindowX, outStart.WindowY) });
                AppendIdleFrames(plan, s_MoveSettleFrames);
                break;
            }
            case Action::Click:
            {
                if (auto error = ResolvePoint(info, request.CoordSpace, request.X, request.Y, outStart))
                    return error;
                outEnd = outStart;

                plan.Frames.push_back({ MousePos(outStart.WindowX, outStart.WindowY) });
                // Settle, then arm the modifiers one frame BEFORE the press so both
                // ImGui and the poll-based Input:: overlay already see them held when
                // the button-press event is dispatched.
                AppendIdleFrames(plan, s_MoveSettleFrames - 1);
                {
                    std::vector<McpInputEvent> frame;
                    AppendModifiers(frame, request.Mods, true);
                    plan.Frames.push_back(std::move(frame));
                }

                const int clicks = request.DoubleClick ? 2 : 1;
                for (int i = 0; i < clicks; ++i)
                {
                    if (i > 0)
                        AppendIdleFrames(plan, s_DoubleClickGapFrames);
                    plan.Frames.push_back({ MouseButton(request.Button, true) });
                    AppendIdleFrames(plan, s_ButtonHoldFrames);
                    plan.Frames.push_back({ MouseButton(request.Button, false) });
                }

                if (request.Mods.Any())
                {
                    std::vector<McpInputEvent> frame;
                    AppendModifiers(frame, request.Mods, false);
                    plan.Frames.push_back(std::move(frame));
                }
                AppendIdleFrames(plan, s_TrailingSettleFrames);
                break;
            }
            case Action::Drag:
            {
                if (auto error = ResolvePoint(info, request.CoordSpace, request.X, request.Y, outStart))
                    return error;
                if (auto error = ResolvePoint(info, request.CoordSpace, request.ToX, request.ToY, outEnd))
                    return error;

                plan.Frames.push_back({ MousePos(outStart.WindowX, outStart.WindowY) });
                AppendIdleFrames(plan, s_MoveSettleFrames - 1);
                {
                    std::vector<McpInputEvent> frame;
                    AppendModifiers(frame, request.Mods, true);
                    plan.Frames.push_back(std::move(frame));
                }
                plan.Frames.push_back({ MouseButton(request.Button, true) });
                AppendIdleFrames(plan, s_ButtonHoldFrames);

                // Interpolate one move per frame; the last step lands exactly on the
                // requested end point (no float drift).
                for (int step = 1; step <= request.Steps; ++step)
                {
                    const f32 t = static_cast<f32>(step) / static_cast<f32>(request.Steps);
                    const f32 x = (step == request.Steps) ? outEnd.WindowX
                                                          : outStart.WindowX + (outEnd.WindowX - outStart.WindowX) * t;
                    const f32 y = (step == request.Steps) ? outEnd.WindowY
                                                          : outStart.WindowY + (outEnd.WindowY - outStart.WindowY) * t;
                    plan.Frames.push_back({ MousePos(x, y) });
                }

                AppendIdleFrames(plan, s_ButtonHoldFrames);
                plan.Frames.push_back({ MouseButton(request.Button, false) });
                if (request.Mods.Any())
                {
                    std::vector<McpInputEvent> frame;
                    AppendModifiers(frame, request.Mods, false);
                    plan.Frames.push_back(std::move(frame));
                }
                AppendIdleFrames(plan, s_TrailingSettleFrames);
                break;
            }
            case Action::MouseDelta:
            {
                ResolvedDelta delta;
                delta.Frames = std::max(1, request.DeltaFrames);
                delta.Reset = request.ResetOffset;
                if (auto error = ResolveDelta(info, request.CoordSpace, request.Dx, request.Dy, delta.WindowDx,
                                              delta.WindowDy))
                    return error;

                // The reset gets a frame of its own: it is a genuine discontinuity
                // (the virtual cursor jumps back onto the hardware one) and folding it
                // into the same frame as a following displacement would hand a delta
                // consumer their algebraic sum, hiding both.
                if (delta.Reset)
                    plan.Frames.push_back({ MouseOffsetResetEvent() });

                // Spread the displacement over `Frames` consecutive frames, walking
                // the running TOTAL rather than adding a rounded per-frame step, so
                // the frames sum to exactly the requested displacement with no drift.
                //
                // Why spreading matters: a delta consumer is entitled to reject an
                // implausible single-frame jump as a discontinuity, and several do —
                // PlayerRigSystem::SampleLookDelta drops any sample worth more than
                // kMaxLookDegreesPerSample (180°) of rotation, precisely so a window
                // move / minimize-restore cannot fling the camera. A real mouse
                // delivers a large movement as many small per-frame deltas, and
                // `frames` is how this tool does the same.
                f32 appliedX = 0.0f;
                f32 appliedY = 0.0f;
                for (int frame = 1; frame <= delta.Frames; ++frame)
                {
                    const f32 t = static_cast<f32>(frame) / static_cast<f32>(delta.Frames);
                    const f32 targetX = (frame == delta.Frames) ? delta.WindowDx : delta.WindowDx * t;
                    const f32 targetY = (frame == delta.Frames) ? delta.WindowDy : delta.WindowDy * t;
                    plan.Frames.push_back({ MouseDeltaEvent(targetX - appliedX, targetY - appliedY) });
                    appliedX = targetX;
                    appliedY = targetY;
                }

                AppendIdleFrames(plan, s_TrailingSettleFrames);
                if (outDelta)
                    *outDelta = delta;
                break;
            }
            case Action::Key:
            {
                if (request.KeyAct != KeyAction::Release)
                {
                    std::vector<McpInputEvent> frame;
                    AppendModifiers(frame, request.Mods, true);
                    if (!frame.empty())
                        plan.Frames.push_back(std::move(frame));
                }

                switch (request.KeyAct)
                {
                    case KeyAction::Press:
                        plan.Frames.push_back({ KeyEvent(request.KeyCode, true) });
                        break;
                    case KeyAction::Release:
                        plan.Frames.push_back({ KeyEvent(request.KeyCode, false) });
                        break;
                    case KeyAction::Tap:
                        plan.Frames.push_back({ KeyEvent(request.KeyCode, true) });
                        AppendIdleFrames(plan, s_ButtonHoldFrames);
                        plan.Frames.push_back({ KeyEvent(request.KeyCode, false) });
                        break;
                }

                // A "press" deliberately LEAVES the modifiers held too — the caller is
                // building a chord by hand and will send the matching "release".
                if (request.Mods.Any() && request.KeyAct != KeyAction::Press)
                {
                    std::vector<McpInputEvent> frame;
                    AppendModifiers(frame, request.Mods, false);
                    plan.Frames.push_back(std::move(frame));
                }
                AppendIdleFrames(plan, s_TrailingSettleFrames);
                break;
            }
            case Action::Text:
            {
                std::vector<McpInputEvent> frame;
                // ASCII only: ImGui's AddInputCharacter takes a codepoint, and the
                // schema caps `text` at 256 characters, so a byte-per-codepoint pass is
                // exact for the ASCII the editor's text fields accept. A non-ASCII byte
                // is rejected rather than silently mangled into a garbage codepoint.
                for (const char c : request.Text)
                {
                    const auto byte = static_cast<unsigned char>(c);
                    if (byte < 0x20 || byte > 0x7E)
                        return "Invalid 'text': only printable ASCII characters are supported.";
                    frame.push_back(CharEvent(static_cast<u32>(byte)));
                }
                plan.Frames.push_back(std::move(frame));
                AppendIdleFrames(plan, s_TrailingSettleFrames);
                break;
            }
        }
        return std::nullopt;
    }

    // ---- schema ---------------------------------------------------------------------

    [[nodiscard]] inline Json InputSchema()
    {
        return Schema::Object()
            .Prop("action", Schema::String()
                                .Enum({ "click", "move", "drag", "mouseDelta", "key", "text" })
                                .Desc("click = press+release at (x, y). move = just move the cursor. "
                                      "drag = press at (fromX, fromY), move in steps, release at (toX, toY). "
                                      "mouseDelta = displace the cursor by (dx, dy) RELATIVELY, for mouse-look rigs "
                                      "and other consumers that integrate movement across frames. "
                                      "key = press/release/tap a key. text = type characters into the focused widget."))
            .Prop("x", Schema::Number().Desc("click/move: horizontal coordinate in 'space'."))
            .Prop("y", Schema::Number().Desc("click/move: vertical coordinate in 'space' (origin top-left, +Y down)."))
            .Prop("fromX", Schema::Number().Desc("drag: start horizontal coordinate in 'space'."))
            .Prop("fromY", Schema::Number().Desc("drag: start vertical coordinate in 'space'."))
            .Prop("toX", Schema::Number().Desc("drag: end horizontal coordinate in 'space'."))
            .Prop("toY", Schema::Number().Desc("drag: end vertical coordinate in 'space'."))
            .Prop("space", Schema::String()
                               .Enum({ "viewport", "viewportNorm", "window" })
                               .Desc("Coordinate space (default \"viewport\"). "
                                     "\"viewport\" = pixels of the olo_screenshot image at NATIVE resolution "
                                     "(origin top-left); if your screenshot was downscaled by maxWidth, these are NOT "
                                     "its pixels. \"viewportNorm\" = fractional [0,1] across the viewport — "
                                     "downscale-proof, and the safest choice after a screenshot. \"window\" = OS window "
                                     "client pixels (origin top-left) — use this to click ImGui panels, menus, and "
                                     "buttons OUTSIDE the 3D viewport. For mouseDelta the same three spaces scale the "
                                     "DISPLACEMENT: \"window\" is 1:1 with the units Input::GetMousePosition reports, "
                                     "which is what a rig's look sensitivity is calibrated in."))
            .Prop("button", Schema::String().Enum({ "left", "right", "middle" }).Desc("click/drag: mouse button (default left)."))
            .Prop("doubleClick", Schema::Bool().Desc("click: send two clicks in quick succession (default false)."))
            .Prop("steps", Schema::Int().Min(1).Max(64).Desc("drag: interpolation steps between the endpoints, one per frame (default 8)."))
            .Prop("dx", Schema::Number().Desc("mouseDelta: horizontal displacement in 'space' units (+ = right)."))
            .Prop("dy", Schema::Number().Desc("mouseDelta: vertical displacement in 'space' units (+ = down)."))
            .Prop("frames", Schema::Int().Min(1).Max(64).Desc("mouseDelta: spread the displacement over this many consecutive frames "
                                                              "(default 1). A consumer may reject an implausibly large single-frame jump as "
                                                              "a discontinuity — PlayerRigSystem drops any look sample worth over 180 deg — "
                                                              "so deliver a big movement the way a real mouse does, as several small deltas."))
            .Prop("resetOffset", Schema::Bool()
                                     .Desc("mouseDelta: zero the accumulated relative displacement first, putting the "
                                           "virtual cursor back onto the hardware one (default false). Valid on its "
                                           "own, without dx/dy. NOTE this is itself a jump, and a delta consumer sees "
                                           "it as one."))
            .Prop("modifiers", Schema::Array(Schema::String().Enum({ "ctrl", "shift", "alt" }))
                                   .Desc("Modifier keys held for the duration of the action (e.g. [\"ctrl\"] for "
                                         "ctrl-click multi-select)."))
            .Prop("key", Schema::String().Desc("key: a single printable character (\"s\", \"5\") or a named key: escape, "
                                               "enter, tab, backspace, delete, insert, up/down/left/right, home, end, "
                                               "pageup, pagedown, space, f1-f12, ctrl, shift, alt."))
            .Prop("keyAction", Schema::String()
                                   .Enum({ "press", "release", "tap" })
                                   .Desc("key: \"tap\" (default) presses and releases. \"press\" leaves the key HELD "
                                         "until a matching \"release\" call."))
            .Prop("text", Schema::String().Desc("text: printable ASCII characters to type into the focused ImGui widget "
                                                "(max 256)."))
            .Required({ "action" })
            .NoAdditional();
    }

    // ---- result shaping ---------------------------------------------------------------

    [[nodiscard]] inline Json ToJson(const McpInputInjectResult& result, const McpInputStateSnapshot& state,
                                     const McpInputViewportInfo& info, const Request& request,
                                     const ResolvedPoint& start, const ResolvedPoint& end, bool timedOut,
                                     const ResolvedDelta* delta = nullptr,
                                     const std::string& stallReason = {})
    {
        Json j;
        j["available"] = result.Available;
        j["ok"] = result.Ok && !timedOut;
        j["framesInjected"] = result.FrameCount;
        j["message"] = result.Message;
        if (timedOut)
        {
            // Name the CAUSE when the editor could tell us one. A bare "timed out" is
            // what made the iconified-window failure take an out-of-band IsIconic()
            // P/Invoke to diagnose (issue #607): the tool knew the loop was parked and
            // said nothing.
            j["message"] = stallReason.empty()
                               ? std::string("Injected, but timed out waiting for the editor to finish processing "
                                             "the input (is it responsive?). The state below may be stale.")
                               : "Injected, but the editor never processed it. " + stallReason +
                                     " The state below is from before the injection.";
        }

        if (request.Act == Action::MouseDelta && delta != nullptr)
        {
            j["resolvedDelta"] = Json{ { "windowDx", delta->WindowDx },
                                       { "windowDy", delta->WindowDy },
                                       { "frames", delta->Frames },
                                       { "reset", delta->Reset } };
        }

        if (request.Act == Action::Click || request.Act == Action::Move || request.Act == Action::Drag)
        {
            const auto pointJson = [](const ResolvedPoint& point) -> Json
            {
                return Json{ { "windowX", point.WindowX },
                             { "windowY", point.WindowY },
                             { "viewportPixelX", point.ViewportPixelX },
                             { "viewportPixelY", point.ViewportPixelY },
                             { "insideViewport", point.InsideViewport } };
            };
            j["resolved"] = (request.Act == Action::Drag)
                                ? Json{ { "from", pointJson(start) }, { "to", pointJson(end) } }
                                : pointJson(start);
            j["viewport"] = Json{ { "pixelWidth", ViewportPixelWidth(info) },
                                  { "pixelHeight", ViewportPixelHeight(info) },
                                  { "dpiScale", info.DpiScale } };
        }

        // The state change the injection caused — the whole reason the tool exists.
        Json after;
        after["pending"] = state.Pending;
        after["viewportHovered"] = state.ViewportHovered;
        after["mouseX"] = state.MouseX;
        after["mouseY"] = state.MouseY;
        // Always reported, not only for mouseDelta: the accumulated offset is the one
        // piece of injection state that OUTLIVES a call, so a session that has left it
        // non-zero should be able to see that from any injection reply.
        after["mouseOffset"] = Json{ { "x", state.MouseOffsetX }, { "y", state.MouseOffsetY } };
        after["selectedEntity"] = state.SelectedEntityId == 0
                                      ? Json(nullptr)
                                      : Json{ { "id", std::to_string(state.SelectedEntityId) },
                                              { "name", state.SelectedEntityName } };
        after["hoveredEntity"] = state.HoveredEntityId == 0
                                     ? Json(nullptr)
                                     : Json{ { "id", std::to_string(state.HoveredEntityId) },
                                             { "name", state.HoveredEntityName } };
        j["after"] = std::move(after);
        return j;
    }
} // namespace OloEngine::MCP::InputInject
