---
paths:
  - "vcpkg.json"
  - "cmake/**"
  - "CMakePresets.json"
---

# Dependencies and build configuration

- Dependencies come from `vcpkg.json`; `VCPKG_ROOT` must be set (issue #773). One-time setup and the
  traps: [vcpkg-dependency-management.md](../../docs/agent-rules/vcpkg-dependency-management.md).
- Check the registry version before moving a dependency: a port at a different version than the pin
  is a common failure.
- `OloEngine/vendor/` (FetchContent) and `<buildDir>/vcpkg_installed/` are not yours: never edit
  them; patch a port via `cmake/overlay-ports/`.
- `STEAMWORKS_SDK_ROOT` is optional and exactly one TU may include a Valve header
  ([steamworks-platform-integration.md](../../docs/agent-rules/steamworks-platform-integration.md)).
- `cmake -P` scripts need a policy pin: CI's CMake 3.31 runs them with OLD policies.
- Build trees, caches and memory: [build-trees-and-windows-asan.md](../../docs/agent-rules/build-trees-and-windows-asan.md).
