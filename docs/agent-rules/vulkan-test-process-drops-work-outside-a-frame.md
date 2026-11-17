# In the Vulkan test process, GPU work outside a frame is dropped

Applies to every `RendererAttachedTest` suite that runs in `OloEngine-Tests --olo-rhi=vulkan`
(`RunsInVulkanProcess()` returns true), and to anything that bakes or draws from `BuildScene`.

## Rules

1. **Issue every draw, clear, dispatch, blit and bake inside `RunInFrame`,** in `BuildScene` as
   much as in the test body. That includes an environment made with
   `EnvironmentMap::CreateFromCubemap`, a sky from `ProceduralSky::Generate`, an impostor atlas and a
   probe bake. OpenGL runs such a call at once; the Vulkan backend has no frame to record it into
   and drops it.
2. **A dropped call leaves its target unwritten, and unwritten is not black.** The texture keeps
   what its memory held: zero while the allocation is fresh, another test's texture once the
   allocator returns a freed block. A test that reads it passes alone and fails after a suite that
   freed a texture of the same size.
3. **The fixture fails the test.** `RendererAttachedTest::TearDown` compares
   `TestRenderBackend::CallsDroppedOutsideAFrame()` with its value at `SetUp` and fails on any
   growth, with the count. Do not work round it: move the work into a frame.
4. **A readback is not such a call.** `ReadTextureImage`, `Texture2D::GetData` and
   `TextureCubemap::GetFaceData` submit by themselves outside a frame and are fine there.

## What happened (#1352)

`ProceduralSkyBakeTest` and `ImpostorBakeEvidenceTest` were ported to the Vulkan process, and the
scene filter went red in a suite neither touches:
`GroomSceneShadowVisualEvidenceTest.BothDirectionsAreVisibleInTheSameFrameOnEveryRenderingPath`.
Its coat read a luma of 175 where it reads 51 alone, and almost none of it was in shadow.

The predecessor was the sky bake test, but not the state it sets: with its state changes switched
off the groom test still failed, and with any ONE of its three readbacks switched off it passed.
What mattered was the pattern of textures created and freed before the groom test.

The groom fixture gives its scene a black sky, so that a coat pixel in full shadow is black. It
built that environment in `BuildScene`, outside a frame. On Vulkan the convolutions that fill the
irradiance and prefiltered cubes were dropped, and the two cubes kept their memory's contents:
zero on a fresh allocation, and after the sky test a freed Preetham sky of the same size. The
"black" sky lit the coat.

The backend had counted the drops all along (`StubKind::OutsideRecording`, 111 calls for that
fixture) and warned once per process. The fixture checked the counter only inside a frame. It now
checks it from `SetUp` to `TearDown`.
