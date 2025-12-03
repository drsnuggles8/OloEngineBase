# OloEngine AI Agent Guide

## 1. Big Picture
C++ based engine using modern OpenGL (4.6 with DSA). Core library in `OloEngine/` (Rendering, Scene/ECS, Physics 2D/3D, Asset, Scripting). Tooling/editor in `OloEditor/` (ImGui panels, content browser, gizmos). Example applications: `Sandbox3D/`, `Sandbox2D/`. Dual scripting runtimes: `OloEngine-ScriptCore/` (C# via Mono) + `OloEngine-LuaScriptCore/` (Lua via Sol2). Run-time asset + mono dependencies expect working directory = `OloEditor/`.

## 2. Build & Run Workflow
Generate VS solutions with `scripts/Win-GenerateProject*.bat`. Clean by deleting `build/` or using `scripts/Win-DeleteStuff.bat`.
Use VS Code tasks (e.g. `build-oloeditor-debug`, `run-oloeditor-debug`, `run-tests-debug`).
Always run samples/editor from `OloEditor/` cwd or assets won't resolve. Dependencies fetched automatically (FetchContent + CPM) into `OloEngine/vendor/`; never edit vendor code.

## 3. Core Patterns
ECS: EnTT + `Entity` wrapper (UUID). Add components via `entity.AddComponent<TransformComponent>()`. Keep component headers minimal; serialization hooks live in YAML converters (see `Core/YAMLConverters.h`). Assets: `AssetManager::LoadAssetFromFile()` returns handle, retrieve typed asset with `GetAsset<T>()`; hot-reload triggers `AssetReloadedEvent`. Rendering: stateless layered command queue; queue population separated from execution (inspired by Molecular Matters). Physics: Jolt (3D) + Box2D (2D) with custom collision layers; access 3D via `scene->GetPhysicsScene()`. Profiling: Wrap functions/blocks with `OLO_PROFILE_FUNCTION()` / `OLO_PROFILE_SCOPE(name)`; increment metrics via `RendererProfiler` & track memory with `RendererMemoryTracker`.

## 4. Scripting Integration
C#: Classes inherit `Entity` base; override `OnUpdate(float dt)`, access components through helpers. Lua: Functions operate on an entity binding (`entity:GetComponent("TransformComponent")`). Keep API surface mirrored where possible. When adding new components, ensure binding layer updates both C# (ScriptCore) and Lua (LuaScriptCore) plus serialization.

## 5. Conventions & Style
C++20 baseline (aim C++23 or even C++26). 4-space indent. Classes PascalCase; members `m_PascalCase`; statics `s_PascalCase`. Primitive typedefs in `Core/Base.h` (`u32`, `f32`, etc.). Use `Ref<T>` smart pointer (`Core/Ref.h`). Headers use `#pragma once`. Prefer RAII for GL resources. Include what you use (project headers in quotes, third-party/system in angle brackets). Braces on new lines except trivial cases.

## 6. Adding/Modifying Systems
Place engine code under `OloEngine/src/<Subsystem>/`. Add public API to clearly named headers; update editor integration (panel or gizmo) in `OloEditor/src/` if user-facing. For new asset types: implement loader, register with `AssetManager`, extend YAML serialization & hot-reload handling.

## 7. Testing & Debugging
Tests live in `OloEngine/tests/` (GoogleTest). Use `run-tests-debug` task after changes. Use asset hot-reload for iteration—modify files under `OloEditor/assets/` and rely on filewatch events.

## 8. Common Pitfalls
Wrong working dir → missing shaders/mono assemblies.
Editing vendor code → lost on next configure.
Forgetting serialization/component binding leads to scenes not persisting or scripts failing.

## 9. Update Policy
Keep this file synced with major architecture shifts (render backend changes, scripting API additions, asset pipeline adjustments). Remove stale sections; avoid aspirational features.

> If any subsystem description is unclear or missing (audio, animation, command queue nuances) ask for expansion.
