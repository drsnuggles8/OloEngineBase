# UI System Guide

OloEngine includes an ECS-based UI system for building screen-space user interfaces. UI elements are standard entities with specialized components, rendered as an overlay on top of both 2D and 3D scenes.

## Quick Start (Editor)

1. **Right-click** in the Scene Hierarchy panel → **Create UI** → pick a widget (e.g. "Button")
2. A **UI Canvas** is automatically created if one doesn't exist, and the widget is parented under it
3. The widget appears immediately in the editor viewport — no need to enter Play mode
4. Select the widget in the hierarchy to edit its properties (colors, text, size, anchors, etc.) in the **Properties** panel

## Architecture Overview

```
UICanvasComponent          ← Root: defines render mode, sort order, reference resolution
  └─ Entity + UIRectTransformComponent + [Widget Component]
       └─ Nested children (also with UIRectTransformComponent)
```

Every frame the system runs three passes:
1. **Layout** (`UILayoutSystem::ResolveLayout`) — walks the canvas hierarchy and resolves anchor-based `UIRectTransformComponent` values into pixel-space `UIResolvedRectComponent`
2. **Rendering** (`UIRenderer`) — draws each resolved widget as a screen-space overlay using `Renderer2D`
3. **Input** (`UIInputSystem::ProcessInput`) — hit-tests mouse position against resolved rects and updates interactive widget state (buttons, sliders, checkboxes, etc.)

## Entity Structure

A minimal visible UI element requires **three things**:

| Component | Purpose |
|---|---|
| `UICanvasComponent` | Marks the root of a UI hierarchy (one per canvas) |
| `UIRectTransformComponent` | Anchor-based positioning and sizing (on every UI entity including the canvas) |
| A widget component (e.g. `UIPanelComponent`) | Determines what is drawn |

The canvas entity must be the **parent** of widget entities via `RelationshipComponent`. The "Create UI" menu in the editor handles this automatically.

## Components Reference

### Layout & Structure

**UICanvasComponent** — Root container for UI elements.
- `m_RenderMode` — `ScreenSpaceOverlay` (default) or `WorldSpace`
- `m_ScaleMode` — `ConstantPixelSize` or `ScaleWithScreenSize`
- `m_SortOrder` — Drawing order between canvases (higher = on top)
- `m_ReferenceResolution` — Design resolution for scale mode (default: 1920×1080)

**UIRectTransformComponent** — Anchor-based layout (similar to Unity's RectTransform).
- `m_AnchorMin` / `m_AnchorMax` — Normalized anchor points (0–1 range relative to parent)
- `m_AnchoredPosition` — Offset from the anchor center point
- `m_SizeDelta` — Width/height when anchors are collapsed (same point), or inset when stretched
- `m_Pivot` — Pivot point for rotation/scaling (0.5, 0.5 = center)
- `m_Rotation` — Rotation in radians
- `m_Scale` — Local scale multiplier

**UIGridLayoutComponent** — Auto-arranges children in a grid.
- `m_CellSize` — Size of each cell
- `m_Spacing` — Gap between cells
- `m_Padding` — Insets from container edges (left, right, top, bottom)
- `m_StartCorner` — `UpperLeft`, `UpperRight`, `LowerLeft`, `LowerRight`
- `m_StartAxis` — `Horizontal` (fill rows first) or `Vertical` (fill columns first)
- `m_ConstraintCount` — Fixed column/row count (0 = flexible based on container width)

### Visual Widgets

**UIPanelComponent** — Solid or textured background rectangle.
- `m_BackgroundColor`, `m_BackgroundTexture`

**UIImageComponent** — Displays a texture with optional 9-slice borders.
- `m_Texture`, `m_Color` (tint), `m_BorderInsets` (left, right, top, bottom for 9-slice)

**UITextComponent** — Renders text with font, color, and alignment.
- `m_Text`, `m_FontAsset`, `m_FontSize`, `m_Color`
- `m_Alignment` — `TopLeft`, `TopCenter`, ..., `BottomRight` (9 options)
- `m_Kerning`, `m_LineSpacing`

**UIProgressBarComponent** — Non-interactive fill bar.
- `m_Value`, `m_MinValue`, `m_MaxValue`
- `m_FillMethod` — `Horizontal` or `Vertical`
- `m_BackgroundColor`, `m_FillColor`

### Interactive Widgets

All interactive widgets have an `m_Interactable` flag. Set to `false` to disable input.

**UIButtonComponent** — Clickable button with state-based coloring.
- `m_NormalColor`, `m_HoveredColor`, `m_PressedColor`, `m_DisabledColor`
- `m_State` — Runtime read-only: `Normal`, `Hovered`, `Pressed`, `Disabled`

**UISliderComponent** — Draggable value slider.
- `m_Value`, `m_MinValue`, `m_MaxValue`
- `m_Direction` — `LeftToRight`, `RightToLeft`, `TopToBottom`, `BottomToTop`
- `m_BackgroundColor`, `m_FillColor`, `m_HandleColor`

**UICheckboxComponent** — Toggle checkbox.
- `m_IsChecked`
- `m_UncheckedColor`, `m_CheckedColor`, `m_CheckmarkColor`

**UIToggleComponent** — On/off toggle switch.
- `m_IsOn`
- `m_OffColor`, `m_OnColor`, `m_KnobColor`

**UIInputFieldComponent** — Editable text input.
- `m_Text`, `m_Placeholder`, `m_FontAsset`, `m_FontSize`
- `m_TextColor`, `m_PlaceholderColor`, `m_BackgroundColor`
- `m_CharacterLimit` — 0 = no limit

**UIDropdownComponent** — Selection from a list of options.
- `m_Options` — Vector of `UIDropdownOption` (each has `m_Label`)
- `m_SelectedIndex`
- `m_BackgroundColor`, `m_HighlightColor`, `m_TextColor`
- `m_FontAsset`, `m_FontSize`, `m_ItemHeight`

**UIScrollViewComponent** — Scrollable content container.
- `m_ScrollPosition`, `m_ContentSize`
- `m_ScrollDirection` — `Vertical`, `Horizontal`, `Both`
- `m_ScrollSpeed`
- `m_ShowHorizontalScrollbar`, `m_ShowVerticalScrollbar`
- `m_ScrollbarColor`, `m_ScrollbarTrackColor`

## Scripting API

### C# (Mono)

All UI components are accessible from C# scripts. Each component class extends `Component` and provides property accessors:

```csharp
// Get a reference to a UI slider on this entity
var slider = GetComponent<UISliderComponent>();

// Read and write the value
float current = slider.Value;
slider.Value = 0.75f;
slider.MinValue = 0.0f;
slider.MaxValue = 1.0f;

// Check a toggle
var toggle = GetComponent<UIToggleComponent>();
if (toggle.IsOn) { /* ... */ }

// Change button colors
var button = GetComponent<UIButtonComponent>();
button.NormalColor = new Vector4(0.2f, 0.5f, 1.0f, 1.0f);

// Read button state (0=Normal, 1=Hovered, 2=Pressed, 3=Disabled)
int state = button.State;

// Update UI text
var text = GetComponent<UITextComponent>();
text.Text = $"Score: {score}";
text.FontSize = 32.0f;

// Move a widget
var rect = GetComponent<UIRectTransformComponent>();
rect.AnchoredPosition = new Vector2(100.0f, 50.0f);
rect.SizeDelta = new Vector2(200.0f, 40.0f);
```

Available C# component classes: `UICanvasComponent`, `UIRectTransformComponent`, `UIImageComponent`, `UIPanelComponent`, `UITextComponent`, `UIButtonComponent`, `UISliderComponent`, `UICheckboxComponent`, `UIProgressBarComponent`, `UIInputFieldComponent`, `UIScrollViewComponent`, `UIDropdownComponent`, `UIGridLayoutComponent`, `UIToggleComponent`.

### Lua (Sol2)

UI component types are registered as Sol2 usertypes with direct member access:

```lua
-- Access component members directly
local slider = entity:GetComponent("UISliderComponent")
slider.value = 0.5
slider.minValue = 0.0
slider.maxValue = 1.0

local toggle = entity:GetComponent("UIToggleComponent")
toggle.isOn = true

local text = entity:GetComponent("UITextComponent")
text.text = "Hello World"
text.fontSize = 24.0
text.color = vec4.new(1.0, 0.8, 0.0, 1.0)
```

Lua property names use camelCase (matching the C++ member names without the `m_` prefix).

## Manual Setup (Without the Create UI Menu)

If you prefer to assemble UI entities manually:

1. Create an entity, add `UICanvasComponent` and `UIRectTransformComponent`
2. Create a child entity, add `UIRectTransformComponent`
3. Set the child's parent to the canvas entity (via `Entity::SetParent()` in code, or drag in the hierarchy)
4. Add a widget component to the child (e.g. `UIButtonComponent`)
5. Configure the `UIRectTransformComponent` anchors and size to position the widget

## Serialization

All UI components are fully serialized to YAML scene files. Textures and fonts are saved by file path and reloaded on scene load. Runtime-only state (button state, slider dragging, dropdown open state, etc.) is not serialized.
