# Restore main-view AO bindings after reflection replay

`PlanarReflectionRenderPass` suspends forward screen-space AO while replaying
geometry from the mirror camera. `BindSceneResources` then publishes white
fallbacks for both AO and depth. Clearing the suspension flag and restoring the
camera UBO do not restore those resources.

Call `CommandDispatch::BindForwardScreenSpaceAO()` after clearing the flag. This
republishes the main-view textures on OpenGL and the descriptor-heap offsets on
Vulkan through the existing backend-aware binding helpers.

Do not rely on `GLStateGuard` for this cleanup: its core-state restoration does
not restore texture bindings, even though its diagnostic says it is restoring
escaped state. During #1488 verification, the live Forward/Forward+ scene logged
repeated mutations of texture slots 19 and 20 after reflection replay.

`PlanarReflectionVisualEvidenceTest.MirrorReplayRestoresMainViewAoBindings`
renders a real forward frame with GTAO, requires real non-white depth/AO inputs,
then executes the reflection pass and checks that both bindings survive. The
test failed before the fix and passed afterwards. An empty opaque bucket
isolates the pass's resource publication; it does not establish reflected image
quality or Vulkan execution. Keep the existing reflection on/off image test and
the live OpenGL/Vulkan path checks for those claims.
