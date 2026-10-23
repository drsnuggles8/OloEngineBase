---
paths:
  - "OloEngine/src/**/*Component.h"
  - "OloEngine/src/**/*Components.h"
  - "OloEngine/src/**/*Component.cpp"
  - "OloEngine/src/OloEngine/Scene/Scene.{h,cpp}"
  - "OloEngine/src/OloEngine/Scene/SceneSerializer.cpp"
  - "OloEngine/src/OloEngine/SaveGame/**"
  - "OloEditor/src/Panels/SceneHierarchyPanel.cpp"
  - "tools/OloHeaderTool/**"
---

# ECS components

- **Adding or changing a component has touch-points no hook catches**: save-game `Serialize` +
  `RegisterAll`, Lua registration, the editor inspector and Add Component menu, and every field in a
  hand-written copy ctor / `operator==`. Before the first edit read
  [component-serializer-codegen.md](../../docs/agent-rules/component-serializer-codegen.md)
  (`olo-read-gates` enforces it) and follow its
  [§0 touch-point table](../../docs/agent-rules/component-serializer-codegen-details.md#0-adding-or-changing-a-component-every-touch-point).
- **A helper struct named `*Component`** is swept into every generated list by the header-tool scan
  and breaks the build in unrelated TUs. Use another suffix.
- **Owning groups**: hot loops use EnTT owning groups and a component may be owned by one group only.
  Read the ownership map at the top of `Scene/Scene.cpp` before adding one.
- **Editor undo**: `DrawComponent<T>` records undo only for a trivially copyable or
  equality-comparable component. Opt a non-trivial one in with
  `auto operator==(const T&) const -> bool = default;`
  ([cpp-coding-quality.md §7](../../docs/agent-rules/cpp-coding-quality.md)).
- **OloHeaderTool** generates the scripting glue, component lists, scene serializer blocks and MCP
  field registry from `OloEngine/src/` (target `GenerateBindings`). Generated files are tracked: if
  they look stale, build `GenerateBindings` and re-stage them. Never hand-edit them.
- Component `std::string` fields stay `std::` (binding surface), as do maps.
