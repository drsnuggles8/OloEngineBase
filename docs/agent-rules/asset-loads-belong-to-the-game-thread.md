# An asset load from a worker runs on the game thread, and so does the release

**Rule.** Loading an asset creates its GPU resources (texture pixels, mesh buffers), and on Vulkan
those uploads are `VulkanOneShot` submits, which are render-thread only by contract. A worker that
loads assets hands each load to the game thread with `Tasks::RunOnGameThreadAndWait`
(`OloEngine/Task/GameThreadCall.h`) and waits. Whatever owns the loaded assets is released there too,
with `Tasks::PostToGameThread`, by moving the last reference into the posted work.

Both helpers run inline when the caller is the game thread or no game thread is attached (the test
binary, a command-line cook). The game thread drains its queue at the top of every frame, outside any
recording.

**A caller that joins the worker from the game thread must cancel first.** A worker blocked on a
queued load would otherwise wait for the very thread that is waiting on it. A cancelled call that is
still queued is abandoned and skipped; one already running is always waited out. Every current joiner
(`BuildGamePanel`, `AssetPackBuilderPanel`, the MCP pack service, `EditorLayer`) cancels before it
joins. A test that plays the editor while a build runs pumps
`GetQueue(GameThread).ProcessAll(true)` between polls, as `McpAssetPackBuildTest` does.

## The failure (Build Game on the Vulkan editor, three crashes deep)

1. **Load phase.** The asset-pack build loads every registry asset on its build thread. The first
   Sponza texture's upload submitted to the frame's `VkQueue` from that thread: device lost two
   seconds in (`VK_ERROR_DEVICE_LOST`, a fault writing to address 0 in a frame batch).
2. **Serialize phase.** With the loads moved, the build crashed again. The serializers read their
   assets through the editor's global `AssetManager`, which loaded anything not resident, on the
   worker. Every pack asset is now made resident on the game thread before serializing.
3. **Release.** The build finished, then the editor crashed in `VulkanDeferredReclaim`. The build's
   temporary asset manager released its assets on the worker, so their GPU resources queued reclaims
   while the frame walked that queue.

GL hid all three: its uploads from a context-less thread fail or no-op instead of losing the device.
The minidump (`lldb -c <dmp>`) named the reclaim queue in one step; the editor log alone did not.
