# GitHub Copilot Instructions

## Architecture Overview

OloEngine is a multi-layered game engine based on Hazel, supporting both 2D/3D rendering, physics simulation, and dual scripting (C# + Lua). Key components:

- **OloEngine/**: Core engine library with modular subsystems (Renderer, Physics3D, Scene, Scripting, Asset)
- **OloEditor/**: Full-featured editor application with ImGui panels and asset management
- **Sandbox3D/Sandbox2D/**: Example applications demonstrating engine capabilities
- **OloEngine-ScriptCore/**: C# scripting runtime via Mono
- **OloEngine-LuaScriptCore/**: Lua scripting integration via Sol2

## Development Workflow

### Building & Running
- Use VS Code tasks exclusively: `run-sandbox3d-debug`, `run-oloeditor-release`, etc.
- Applications must run from `OloEditor/` working directory (assets/mono dependencies)
- CMake generates solutions in `build/`, binaries in `bin/[Config]/[Target]/`
- Use `scripts/Win-GenerateProject.bat` for initial setup, `scripts/Win-DeleteStuff.bat` to clean (or manually delete the `build/` folder)

### Key Patterns

**Entity-Component-System**: Uses EnTT for scene management. Entities are UUID-based handles, components stored in registries.
```cpp
Entity entity = scene->CreateEntity("MyEntity");
entity.AddComponent<TransformComponent>();
entity.GetComponent<RigidBody3DComponent>().Mass = 5.0f;
```

**Asset System**: UUID-based asset handles with async loading and hot-reloading via filewatch.
```cpp
AssetHandle texHandle = AssetManager::LoadAssetFromFile("texture.png");
Ref<Texture2D> texture = AssetManager::GetAsset<Texture2D>(texHandle);
```

**Physics Integration**: Jolt Physics wrapped in `JoltScene` with custom collision layers and filtering.
```cpp
JoltScene* physics = scene->GetPhysicsScene();
physics->AddRigidBody(entity, colliderShape, bodyType);
```

**Scripting Dual-Runtime**: C# entities inherit `Entity` base class, Lua scripts use component binding.

```csharp
// C# Entity example - inherits from Entity base class
public class Player : Entity
{
    private float speed = 5.0f;
    
    protected override void OnUpdate(float deltaTime)
    {
        // Access components through Entity base class
        var transform = GetComponent<TransformComponent>();
        var input = Input.GetAxis("Horizontal");
        transform.Translation.x += input * speed * deltaTime;
    }
}
```

```lua
-- Lua script example - component binding approach
function OnUpdate(entity, deltaTime)
    -- Get component through binding system
    local transform = entity:GetComponent("TransformComponent")
    local input = Input.GetAxis("Horizontal")
    
    -- Modify component properties directly
    transform.Translation.x = transform.Translation.x + input * 5.0 * deltaTime
end
```

**YAML Serialization**: Comprehensive scene/entity serialization with custom converters in `Core/YAMLConverters.h`.
```cpp
SceneSerializer serializer(scene);
std::string yamlData = serializer.SerializeToYAML();
serializer.DeserializeFromYAML(yamlData);
```

**Threading & Async**: Asset loading uses dedicated worker threads with mutex/condition_variable patterns.
```cpp
Thread m_Thread("Asset Thread");
m_Thread.Dispatch([this]() { AssetThreadFunc(); });
```

**Profiling Integration**: Tracy + custom renderer profilers. Use `OLO_PROFILE_FUNCTION()` and `OLO_PROFILE_SCOPE(name)`.
```cpp
OLO_PROFILE_FUNCTION(); // Profiles entire function
OLO_PROFILE_SCOPE("Custom Section"); // Profiles code block
RendererProfiler::GetInstance().IncrementCounter(MetricType::DrawCalls, 1);
```

## Code Style Guidelines

- **C++ Standards:** Target C++23 where supported by current compilers, baseline C++20 across the repo
- **Naming:** PascalCase for classes, `m_PascalCase` for members, `s_PascalCase` for statics
- **Types:** Custom typedefs (`u8`, `i16`, `i32`, `f32`, `sizet`) defined in `Core/Base.h`
- **Headers:** Use `#pragma once`, RAII for OpenGL resources, STL containers preferred
- **Formatting:** Braces on new lines, 4-space indentation, public before private

## Critical Build Dependencies

**CMake Functions**: Use `olo_*` functions from `cmake/CommonProperties.cmake`:
- `olo_set_output_directories()` - Standardized bin paths  
- `olo_enable_lto()` / `olo_enable_pch()` - Performance optimizations
- `olo_set_compiler_options()` - Common flags across targets

**Vendor Management**: Dependencies fetched via CPM, stored in `OloEngine/vendor/`. Never modify vendor code directly.

**Renderer Backend**: Currently OpenGL-based. Uses SPIR-V for shader compilation via Vulkan SDK tools.

**Debug/Profiling System**: Comprehensive performance tracking with Tracy integration and custom renderer profilers.
- `RendererProfiler` - Frame timing, draw calls, GPU metrics
- `RendererMemoryTracker` - GPU/CPU memory allocation tracking
- `OLO_PROFILE_*` macros for Tracy integration
- Custom debug panels in Sandbox3D demonstrate usage patterns

## Common Integration Points

- **Scene System**: `Scene` owns entities, `SceneCamera`/`EditorCamera` for viewports
- **Layer Stack**: Applications inherit `Layer`, pushed to `Application::LayerStack`  
- **Event System**: Custom events inherit `Event`, dispatched through `Application::OnEvent`
- **Asset Hot-Reload**: Filewatch triggers `AssetReloadedEvent` → update references
- **ImGui Integration**: `ImGuiLayer` for editor UI, custom panels inherit base classes

## VS Code Tasks Usage
Always use VS Code tasks for building/running: `build-sandbox3d-debug`, `run-oloeditor-release`, etc. Tasks ensure correct working directories and environment setup.
