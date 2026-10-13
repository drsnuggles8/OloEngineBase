# Scene render settings

**A setting the renderer draws with but the scene owns goes in
`SceneTransition::SceneRenderSettings`, and in the three places that struct does not reach.**
Post-processing, fog, wind, snow, precipitation and the rendering path are scene render settings:
the scene stores them, and `SceneTransition::ApplySceneRenderSettings` publishes them to Renderer3D
whenever a host makes a scene the one it draws. The hosts are the editor's scene open, the editor's
Play-mode scene switch, and OloRuntime's start, reload and script-driven switch.

## Adding one

1. A field in `SceneRenderSettings`, moved by all four members: `Capture` (scene to struct),
   `CaptureRenderer` (renderer to struct, which is what an editor save writes), `Apply` (struct to
   scene) and `PublishToRenderer` (struct to renderer).
2. Both emitters in `SceneSerializer.cpp`: `Serialize` (the `.olo` file) and `SerializeToYAML` (the
   asset-pack form). Writing only one makes the setting vanish from cooked packs.
3. `SceneSerializer::ApplySceneSettings`, the one reader every load path shares. Reset the field to
   its default before reading, because the binary sidecar and `DeserializeFromYAML` can apply onto a
   scene that already has a value.
4. `Scene::Copy`, or Play mode starts on the default
   ([scene-copy-must-carry-scene-level-settings.md](scene-copy-must-carry-scene-level-settings.md)).

The binary sidecar (`.scenebin`) copies every top-level settings key, so it needs nothing.

A value that can be wrong (an enum spelled by name) fails the load with the reason. Falling back to
the default is the bug this guide exists for: the game boots, draws, and silently lacks what the
author picked.

## Editor behaviour

The editor's live renderer settings are what a save writes, so the scene saved carries what the user
was looking at. Opening a scene therefore replaces the editor's live values with the scene's,
rendering path included. An MCP write after the open wins until the next open; scripts open first and
set the path afterwards. An MCP write does not dirty the scene: making it dirty would make the next
`olo_scene_open` refuse under the dirty-scene policy, in every live script that switches path and
then opens the next scene.

## Why

Until #1601 the rendering path was not in the set. A scene could author SSR and SSGI, which run only
on Deferred, and a game shipped through Build Game ran Forward regardless. Nothing failed: the
runtime loaded, drew a correct-looking Forward frame, and the only sign was a missing reflection.
Scenes without the key load as Forward, so every scene saved before the key existed renders as it did.
