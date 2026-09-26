# Editor input coordinates and ImGui viewports

Rules for code that places or measures the editor's cursor: `olo_input_inject`, anything reading
`EditorLayer` window geometry, and any probe run from outside the editor process. Each rule
cost a session a wrong number during #607.

## 1. Window-client coordinates are physical pixels on Windows

GLFW makes the editor per-monitor DPI aware (`GetWindowDpiAwarenessContext` = 2). GLFW window
coordinates, the cursor-position callback, `io.DisplaySize` and every ImGui panel rect are
therefore physical pixels, and `io.DisplayFramebufferScale` is 1.0. On a 150% display a
1280x720 window request becomes a 1920x1080 client (`GLFW_SCALE_TO_MONITOR`), and 3840x2054
when maximised on a 4K monitor.

Take the extent from the ImGui main viewport or `glfwGetWindowSize`, not from a cached size.
`WindowsWindow::m_Data` kept the 1280x720 request until the first real resize, because the
size callback is installed after `glfwCreateWindow` has already rescaled the window. That
stale value was `olo_input_inject`'s window-space bound, so the right-hand dock and the Content
Browser could not be reached. Both platform windows now read the size back after creation.

`Window::s_HighDPIScaleFactor` is the monitor content scale, not "ImGui units to framebuffer
pixels". The factor for that is `io.DisplayFramebufferScale`. See the issue filed from #607
about the viewport render size, which multiplies by the former.

## 2. Tell ImGui which viewport the synthetic cursor is over

With `ImGuiConfigFlags_ViewportsEnable`, `ImGui_ImplGlfw_UpdateMouseData` reports the viewport
the PHYSICAL mouse hovers, through `io.AddMouseViewportEvent`, every frame. While an agent drives
the editor that is none. ImGui then hit-tests `g.MouseLastHoveredViewport`. Two consequences:

- A panel floating in its own OS window (every graph editor on first open: `imgui.ini` puts
  them at desktop (60, 60), outside a restored editor) is never hovered by a synthetic cursor.
- After a plan that targeted such a window, main-window points keep hit-testing THAT viewport:
  `hoveredWindow` came back null over the right-hand dock.

`ImGuiLayer::SetMouseViewportOverride(id)` queues the event after `ImGui_ImplGlfw_NewFrame`, so
it is the last word. `EditorLayer` sets it on every injected position, to the panel's viewport
or to the main viewport, and clears it when the plan drains. The position itself stays relative
to the main window's client origin; the backend adds the main window's desktop position back.

Do not try to pick "the OS window on top" for a window-space point. Measured: ImGui's
`LastFocusedStampCount` fallback chose the MCP Diagnostics Server window, which Windows had
BELOW the editor, and the real top window at that point was a Windows Security dialog from
another process. Window space means the main dockspace; floating panels use `space:"panel"`.

## 3. Docked windows are "child windows"

`ImGui::BeginDocked` ORs `ImGuiWindowFlags_ChildWindow` into a docked window. A filter that
drops child windows drops every docked panel. Keep a window when it is not a child OR
`DockIsActive`, and skip `ImGuiWindowFlags_DockNodeHost`. For a docked window,
`DockTabIsVisible` means "this is the selected tab"; an unselected tab is still submitted
every frame (`WasActive` is true), so test that flag, not `WasActive`.

## 4. Measuring the editor from another process

- `GetClientRect` answers in the CALLER's DPI context. From an unaware PowerShell it returns
  1280x720 for the 1920x1080 editor. Call `SetThreadDpiAwarenessContext(-4)` first.
- `Process.MainWindowHandle` can be a floating ImGui viewport's OS window rather than the
  editor. Find the window whose title ends in ` - OloEditor`.
- `driver.ps1 -Action shot` restores and foregrounds the window before capturing, so it
  un-maximises the editor. Use a plain `PrintWindow` for a maximised capture.

## 5. Per-pass frame capture is OpenGL-only

`RenderGraphFrameCapture`'s blit, backbuffer read-source selection and alpha swizzle are
defined only in `Platform/OpenGL` (`FrameCaptureBackend.h`). The Render Graph Debugger
auto-captures by default, so opening it on Vulkan installed the hook and the first captured
pass executed a null GL pointer (`EXCEPTION_ACCESS_VIOLATION` at 0x0 in
`Detail::SetTextureAlphaSwizzleOne`). `RenderGraphFrameCapture::IsSupported()` now refuses the
hook off OpenGL and the panel says so. A Vulkan capture path would need its own backend.
