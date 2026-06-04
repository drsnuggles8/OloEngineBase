# Video Playback

Implements GitHub issue [#176](https://github.com/drsnuggles8/OloEngineBase/issues/176): decode and
render video to a texture for cutscenes, studio logos, splash screens, credits, and in-world
surfaces (TV screens, billboards, security-camera feeds).

## Decode backends — pl_mpeg (default) + FFmpeg (optional)

`VideoDecoder` dispatches by file extension to one of two pluggable backends behind its pImpl
([IVideoDecoderBackend](../OloEngine/src/OloEngine/Video/VideoDecoderBackend.h)):

- **pl_mpeg** ([PlMpegBackend.cpp](../OloEngine/src/OloEngine/Video/PlMpegBackend.cpp)) — MPEG-1
  video + MP2 audio for `.mpg` / `.mpeg` / `.m1v`. A single-file, public-domain decoder
  ([pl_mpeg](https://github.com/phoboslab/pl_mpeg)) vendored via `FetchContent` with *zero*
  external dependencies. Always available; the implementation is compiled once in
  [PlMpeg.cpp](../OloEngine/src/OloEngine/Video/PlMpeg.cpp).
- **FFmpeg/libav** ([FFmpegBackend.cpp](../OloEngine/src/OloEngine/Video/FFmpegBackend.cpp)) —
  everything else (H.264/HEVC/VP9/MPEG-4 in MP4/MOV/MKV/AVI, AAC/MP3/AC3 audio). Built **from
  source** (like every other dependency) by [OloEngine/vendor/ffmpeg.cmake](../OloEngine/vendor/ffmpeg.cmake),
  which runs [scripts/build-ffmpeg.sh](../scripts/build-ffmpeg.sh) under an `ExternalProject`.
  **Opt-in** via `-DOLO_VIDEO_FFMPEG=ON` (off by default — building FFmpeg from source needs a
  MSYS/MINGW bash + nasm + gmake + Visual Studio). When on, `FFmpegBackend.cpp` compiles
  (guarded by `OLO_VIDEO_FFMPEG`) and the runtime DLLs are copied next to the app exes.

The build is decode-only (no encoders/muxers/programs/network) and converts to RGBA8
(swscale) and interleaved-stereo float (swresample) — the formats the rest of the pipeline
expects. [scripts/build-ffmpeg.sh](../scripts/build-ffmpeg.sh) is **cross-platform**:

- **Windows** — builds with `--toolchain=msvc` so the import libs link natively into the
  MSVC engine. Locates Visual Studio via `vswhere`, imports the MSVC env into the MINGW
  shell, and neutralises FFmpeg's `-showIncludes` dependency step (which native-Windows
  `gmake` mangles). Needs Git-bash + `nasm` + `gmake` (Strawberry Perl ships both).
- **Linux** — native gcc/clang build (`./configure && make`) producing `lib<name>.so`. Needs
  `bash` + `nasm` + `make`.

Both produce the same selective codec set; `cmake/ffmpeg.cmake` resolves the per-OS library
names (`.lib`/`.dll` vs `.so`) and copies the runtime libs next to the exe (with an `$ORIGIN`
rpath on Linux).

### CI

The default Windows/Linux CI leaves `OLO_VIDEO_FFMPEG` **off**, so it is unaffected. A
separate opt-in workflow — [.github/workflows/video-ffmpeg.yml](../.github/workflows/video-ffmpeg.yml),
manual-dispatch + weekly — builds `-DOLO_VIDEO_FFMPEG=ON` on **both** `windows-latest` and
`ubuntu-24.04`, installs `nasm` (choco/apt), **caches** `vendor/ffmpeg-install` (so only the
first/post-bump run pays the ~10-15 min build), and runs the decode test against a fetched
sample MP4. Both from-source builds are verified: the Windows path end-to-end (build → backend
compile → link → decode a 1080p H.264 MP4); the Linux path producing the FFmpeg `.so`s.

To convert media to MPEG-1 for the dependency-free path: `ffmpeg -i input.mp4 -c:v mpeg1video
-q:v 4 -c:a mp2 output.mpg`. With `OLO_VIDEO_FFMPEG=ON`, `.mp4`/`.mov`/`.mkv` play directly.

## Architecture

`OloEngine/src/OloEngine/Video/`

| File | Role |
|---|---|
| `VideoDecoder.{h,cpp}` | Opens a file; decodes RGBA frames + (optional) audio; seek; metadata. pImpl over pl_mpeg. Single-thread-confined. |
| `VideoTexture.{h,cpp}` | Streams decoded RGBA8 frames to a `Texture2D` (DSA `SetData`). No-ops until initialized, so the player runs headlessly in tests. |
| `VideoPlayer.{h,cpp}` | Transport state machine + **background decode thread** filling a bounded frame ring buffer; a main-thread presentation clock selects the frame in `Update(dt)`; `UpdateTexture()` does the GPU upload. `RefCounted`. |
| `VideoSystem.{h,cpp}` | Per-frame driver. Ticks the global fullscreen player + every `VideoOverlay`/`VideoSurface` component (lazy player creation, decode, upload, material-albedo binding). |
| `PlMpeg.cpp` | The single pl_mpeg implementation TU. |

### Threading & A/V sync

- Decode happens on a dedicated thread per `VideoPlayer`, producing into a bounded
  `std::deque` ring buffer (capacity 8). The main thread never decodes on the hot path — it
  only advances the clock, picks the newest ready frame, and uploads it. This satisfies the
  "no main-thread decode stalls" acceptance criterion.
- **Audio + A/V sync:** when the file has an audio track, the decode thread also feeds a
  [VideoAudioStream](../OloEngine/src/OloEngine/Video/VideoAudioStream.cpp) — a custom
  `ma_data_source` over a lock-free `ma_pcm_rb`, attached to the engine's shared `ma_engine`.
  The audio playback position (frames the audio thread has actually consumed) becomes the
  **master clock**, and video frames are presented against it so picture locks to sound. With
  no audio track the clock falls back to wall-time accumulation. Seeks flush the ring and
  rebase the clock; underruns fill silence without advancing the clock (video waits for audio).
- **GPU upload:** `VideoTexture` creates its texture with `TextureSpecification::Streaming`, so
  the GL backend uploads each frame through a **double-buffered PBO ring** (orphan +
  `UNSYNCHRONIZED` map) — the CPU copy and GPU DMA overlap instead of stalling the render thread.
- The `VideoDecoder` (pl_mpeg `plm_t`) is **only** touched by the decode thread while running.
  Seeks are requested via an atomic and serviced on the decode thread, so there is no decoder
  data race. `Unload()`/destructor signal-and-join before closing the decoder.

### ECS components (all six engine touch-points wired)

- `VideoOverlayComponent` — fullscreen overlay (cutscenes, splash). Fields: `VideoPath`,
  `PlayOnStart`, `SkipOnInput`, `Looping`, `Volume`. Esc/Space/Enter skips when `SkipOnInput`.
- `VideoSurfaceComponent` — world-space video. Fields: `VideoPath`, `AutoPlay`, `Looping`,
  `Volume`. Each frame the decoded texture is bound to the entity's `MaterialComponent` albedo.

Both are referenced by **asset-relative path** (not `AssetHandle`) for now; the runtime
`Ref<VideoPlayer>` is transient (not serialized, reset to null on copy). They round-trip
through `SceneSerializer` and `SaveGameComponentSerializer`, are registered for Lua, expose
`OLO_PROPERTY` reflection for C#, and appear under **Add Component → Video Overlay / Video Surface**.

### Scripting

- **Lua**: a `Video` global (`Video.PlayFullscreen(path[, loop][, onFinished])`, `Video.Stop()`,
  `Video.Skip()`, `Video.IsPlaying()`, `Video.SetSkippable(b)`), plus usertypes for both
  components (`Play`/`Pause`/`Stop`, `videoPath`, `looping`, `volume`, …).
- **C#**: a `Video` static class (`PlayFullscreen`, `Stop`, `Skip`, `IsPlaying`) over Mono
  internal calls in `ScriptGlue.cpp`.

## Tested

`OloEngine/tests/VideoPlaybackTest.cpp` (`unit`): decoder failure paths, the player transport
state machine + input clamping, and the texture no-op-until-initialized contract — all GL-free
and CI-runnable. A real-decode test (`VideoDecoderFixture`) runs only when an MPEG-1 fixture is
supplied via `OLO_TEST_VIDEO=<path.mpg>` and otherwise `GTEST_SKIP`s cleanly.

## Rendering

- **Fullscreen overlay:** `Scene::RenderUIOverlay` (run during the `UICompositePass`, on top of
  the scene + UI) draws the active fullscreen player's texture as a letterboxed quad with an
  opaque black backdrop, via the already-verified `UIRenderer::DrawRect` screen-space path. The
  composite output is what the runtime and editor viewport show. Verified by a pixel-readback
  evidence test ([VideoOverlayVisualEvidenceTest.cpp](../OloEngine/tests/Rendering/PropertyTests/VideoOverlayVisualEvidenceTest.cpp),
  PNG → `OloEditor/assets/tests/visual/VideoOverlay_VisualEvidence.png`): centre = frame,
  letterbox bars = black.
- **World-space:** `VideoSystem` binds the decoded texture to the entity material's albedo each
  frame (reuses the existing mesh/material pipeline — no new pass).
- **Static images:** `VideoSystem::ShowFullscreenImage` / `VideoPlayer::PresentImage` display a
  single RGBA image through the same overlay path (splash screens / studio logos / a poster
  frame) — no decoder or thread.

## Implemented (full issue #176 acceptance-criteria coverage)

decoder (pl_mpeg + FFmpeg), RGBA texture streaming via **PBO double-buffering**, full player
transport (play/pause/stop/seek/loop/speed), background-thread decode with frame queue, **audio
track + A/V sync** (audio-master clock through miniaudio), both ECS components, YAML + save-game
serialization, Add-Component menu + inspector, content-browser **video thumbnails** (first-frame
preview), Lua + C# bindings, the **fullscreen-overlay compositing path** and the **world-space**
render path, and unit + visual-evidence tests.

### Remaining nice-to-haves (not blocking)

- FFmpeg is **opt-in** (`OLO_VIDEO_FFMPEG=ON`); without it only MPEG-1 (`.mpg`) plays.
- Playback-speed control applies only in the video-only (no-audio) path; with audio the master
  clock keeps playback at 1×.
- Subtitle tracks and hardware-accelerated decode (DXVA/NVDEC) are not wired.
