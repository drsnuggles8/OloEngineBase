# Plan: Enable POD Command Sorting for 3D Rendering

Convert render commands to pure POD structs using asset handles and renderer IDs, implement 64-bit radix sort, and enable command sorting. Single bucket approach (no separate opaque/transparent). Multi-threading (TLS + per-thread buckets) deferred to future branch.

## Steps

### 1. Convert commands to POD in RenderCommand.h
**File:** `OloEngine/src/OloEngine/Renderer/Commands/RenderCommand.h`

- Replace `Ref<Mesh>`, `Ref<Shader>`, `Ref<Texture2D>` → `AssetHandle` (u64)
- Replace `Ref<VertexArray>` → `RendererID` (u32) using existing `GetRendererID()`
- Inline `RenderState` values as POD flags (depth write bool, blend mode enum)
- Replace `std::span<glm::mat4> boneMatrices` → `u32 boneBufferOffset` + `u32 boneCount`
- Replace `std::vector<glm::mat4> transforms` in instanced command → offset+count into frame buffer

### 2. Add frame-local staging buffer for dynamic data
**Location:** `OloEngine/src/OloEngine/Renderer/Commands/`

- Create `FrameDataBuffer` class for bone matrices, per-instance transforms
- Allocate from `CommandAllocator` during `BeginScene()`, reset each frame
- Provide `AllocateBoneMatrices(count)` / `AllocateTransforms(count)` → returns offset; caller writes data

### 3. Update command submission in Renderer3D.cpp
**File:** `OloEngine/src/OloEngine/Renderer/Renderer3D.cpp`

- In `DrawMesh()`, `DrawAnimatedMesh()`, `DrawQuad()`, etc.: use `mesh->GetHandle()`, `shader->GetHandle()`, `texture->GetHandle()`
- Use `vertexArray->GetRendererID()` for VAO ID
- Store bone matrices / instance transforms in `FrameDataBuffer`, store offset in command

### 4. Add handle resolution in CommandDispatch.cpp
**File:** `OloEngine/src/OloEngine/Renderer/Commands/CommandDispatch.cpp`

- At dispatch time: resolve `AssetHandle` → `Ref<T>` via `AssetManager::GetAsset<T>(handle)`
- Cache resolved assets per-frame in small lookup table to avoid repeated hash lookups
- Bind VAO directly with `glBindVertexArray(cmd->vaoID)`

### 5. Implement 64-bit radix sort in CommandBucket.cpp
**File:** `OloEngine/src/OloEngine/Renderer/Commands/CommandBucket.cpp`

- Add `RadixSort64()` function using LSB radix sort with 8-bit digits (8 passes for 64-bit keys)
- Replace current `std::stable_sort` in `SortCommands()` with radix sort
- Sort on `DrawKey` which already has proper bit packing (viewport, layer, shader, material, depth)

### 6. Enable sorting in SceneRenderPass.cpp
**File:** `OloEngine/src/OloEngine/Renderer/RenderGraph/SceneRenderPass.cpp`

- Uncomment `m_CommandBucket.SortCommands()` before `ExecuteCommands()`
- Verify `DrawKey` is populated correctly during command creation in `Renderer3D`

## Reference Material

- **Molecular Matters Blog:** Stateless, Layered, Multi-threaded Rendering (Parts 1-4):
https://blog.molecular-matters.com/2014/11/06/stateless-layered-multi-threaded-rendering-part-1/
https://blog.molecular-matters.com/2014/11/13/stateless-layered-multi-threaded-rendering-part-2-stateless-api-design/
https://blog.molecular-matters.com/2014/12/16/stateless-layered-multi-threaded-rendering-part-3-api-design-details/
https://blog.molecular-matters.com/2015/02/13/stateless-layered-multi-threaded-rendering-part-4-memory-management-synchronization/
- **tuxalin/CommandBuffer:** GitHub reference implementation with radix sort and POD commands (github.com/tuxalin/CommandBuffer)
