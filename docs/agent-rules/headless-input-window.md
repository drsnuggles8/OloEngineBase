# Headless input has no window

Check `Application::IsHeadless()` before obtaining its window in platform input code.
A live `Application` does not guarantee a live `Window`: dedicated servers deliberately
construct the former without the latter. Route headless calls through the existing
cached key state and no-window cursor fallbacks on both Windows and Linux.

While verifying Mono startup policy in #1540, a real server executed C# and Lua
successfully, then crashed during scene shutdown. The dump traced
`Scene::OnRuntimeStop` through `PlayerRigSystem::ReleaseCursorCapture` and
`Input::GetCursorMode` to `TryGetGlfwWindow`. That helper checked only for a null
application before dereferencing its null window. An empty scene reproduced the
same failure, without either scripting language.

`AppLaunchSmoke.OloServerStopsAHeadlessSceneCleanly` launches a real headless host
with an empty scene and requires both the scene-start log and exit code zero.
Requiring the scene-start log matters: a missing or incorrectly quoted scene path
otherwise skips the shutdown path and can make the regression check pass.
