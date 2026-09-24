# A scene with a stochastic groom requests its own temporal resolve (#1429)

Read before touching `Scene::CountGroomsNeedingTemporalResolve`, `Renderer3D::RequestSceneTemporalResolve`,
`TemporalUpscalePolicy::WantsEngineTAA` or any pipeline site that decides whether engine TAA runs.

## The rules

1. **A scene holding a groom on `StochasticAlpha` asks for a temporal resolve every frame, and
   engine TAA runs for it with `TAAEnabled` unticked.** The mode is refused without a resolve
   ([groom-strand-visibility.md](groom-strand-visibility.md) rule 6), and the opaque tier it falls
   to draws no strand narrower than a pixel — which at any real framing is every strand. So a
   refused coat is not noisy, it is gone: `GroomAnimals.olo` opened on bald horses and a hairless
   head until someone ticked TAA by hand. Measured headless with the clips frozen: the short coat
   and the human cover **0 px** refused against 1.8 % and 7.2 % of the frame honoured.

2. **Request it, do not persist it.** The scene publishes the count before every `BeginScene`,
   which consumes it. A TAA key in the scene file would outlive the groom that needed it and would
   never reach a scene built in code. Nothing is written to disk, so the request cannot go stale.

3. **Every pipeline site reads `data.EngineTAAWanted`, never `PostProcess.TAAEnabled`.** It is
   latched once in `PrepareFrame`, before the jitter, so the jitter, the resolved velocity (MSAA
   deferred), GTAO's noise phase, the TAA pass and the groom's composition decision agree. FSR2
   still subsumes it through `ShouldRunEngineTAA`. A request is granted only while the TAA pass is
   ready: granting one to a pass still compiling jitters the frame with nothing to average it.

4. **Scene-wide, not per view.** A request driven by frustum or LOD culling would switch TAA on and
   off as the camera turns, and each switch resets the jitter sequence and the temporal histories.

5. **The refused tier stays reachable, and is loud when reached.**
   `RendererSettings::HonourSceneTemporalResolveRequests` (diagnostic, not persisted; MCP
   `olo_renderer_settings_set scenetemporalresolve ignore`) refuses the request. The pass then logs
   a WARNING naming the bald count and the editor viewport shows a red banner. A benchmark arm that
   pins `TAAEnabled` pins this switch with it, so "TAA off" measures no TAA.

## Not done, and why

A fallback tier that draws sub-pixel hair without any resolve (#1429 option 2) was not built. It
is a new composition mode and needs its own measured comparison in `GroomCoveragePropertyTests`.

## Evidence

- CPU: `OloEngine/tests/Groom/GroomSceneTemporalResolveTest.cpp` (policy, producer, loader, the
  shipped scene), `McpRendererSettingsApply.SceneTemporalResolveReachesTheRefusedTierAndBack`.
- Pixels: `GroomAnimalsAcceptanceEvidenceTest.AFreshSceneWithTAAOffStillCoatsItsSubjects` writes
  `GroomAnimalsSceneResolve{,Off}_GL_<Path>_<Subject>.png`. **Freeze the clips before an A/B in that
  fixture**: the edit-mode preview advances any playing clip, so without it the coat-on minus
  coat-off difference counts the legs moving as coat (the refused arm first read 85–125 % of the
  honoured one while its PNG showed a bald horse).
