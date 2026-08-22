#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/RHI/RHITypes.h"

#include <glm/glm.hpp>

#include <string_view>

namespace OloEngine
{
    // @brief Backend-neutral seam for a temporal upscaler (#684).
    //
    // WHY THIS INTERFACE EXISTS AT ALL. A temporal upscaler is handed raw driver
    // objects — FSR2's GL backend wants GLuint texture names, not our handles —
    // and `RHIBoundaryRatchetTest` baselines native-handle lookups outside
    // `Renderer/Debug/` and `Platform/` at ZERO. So the render pass must not see
    // a GLuint, and this is the line it stops at: the pass speaks
    // RHI::ResourceHandle, and `Platform/<Backend>/` resolves those with
    // `RHIResourceRegistry::ResolveNativeForBackend`. Adding a second
    // implementation (a Vulkan FSR2, DLSS, XeSS) means adding a Create() branch,
    // not touching the pass.
    //
    // LIFETIME. `Configure` owns a large pool of internal history/lock/luminance
    // targets sized to the display resolution, so it is expensive and must be
    // called only when a size or option genuinely changed — the implementations
    // no-op a Configure that would reproduce the current state. `Dispatch` is the
    // per-frame call. Destroying the object destroys the history, which is
    // exactly what a reset needs.

    // What the upscaler cannot do anything about, reported so callers can say WHY
    // rather than silently falling back. Ordered from "will never work here" to
    // "would work if you changed a setting".
    // NOTE what Available does and does not mean. It is a statement about the
    // DEVICE — "this build, this backend and this GPU can run a temporal upscale"
    // — NOT "a context is currently built". It must be answerable BEFORE
    // Configure has ever run, because the pipeline uses it to decide whether to
    // enable the pass at all, and the pass is what calls Configure. Tying it to a
    // successful Configure is a deadlock the pass can never leave, and one that
    // presents as a clean "unsupported" skip rather than as a failure.
    enum class TemporalUpscalerStatus : u8
    {
        Available = 0,
        NotCompiledIn,      // OLO_WITH_FSR2=0 for this build (see cmake/fsr2.cmake)
        BackendUnsupported, // Running on an RHI backend this implementation has no path for
        DeviceUnsupported,  // The GPU/driver cannot run it (a missing GL feature, or a rejected create)
        NotConfigured,      // The device probe has not run yet — a transient startup state only
    };

    [[nodiscard]] constexpr std::string_view ToString(TemporalUpscalerStatus status) noexcept
    {
        switch (status)
        {
            case TemporalUpscalerStatus::Available:
                return "available";
            case TemporalUpscalerStatus::NotCompiledIn:
                return "not compiled into this build";
            case TemporalUpscalerStatus::BackendUnsupported:
                return "unsupported on the active RHI backend";
            case TemporalUpscalerStatus::DeviceUnsupported:
                return "unsupported by this GPU or driver";
            case TemporalUpscalerStatus::NotConfigured:
                return "not configured";
        }
        return "unknown";
    }

    // Everything that is fixed for a given output size + depth convention.
    struct TemporalUpscalerConfig
    {
        u32 DisplayWidth = 0;
        u32 DisplayHeight = 0;
        // The largest render resolution that will ever be fed to Dispatch. FSR2
        // sizes its internal targets from this, so a render scale ABOVE it is a
        // reconfigure, not a cheap change.
        u32 MaxRenderWidth = 0;
        u32 MaxRenderHeight = 0;
        // Colour is HDR (linear, unbounded, pre-tone-map). True for this engine:
        // the upscaler runs before ToneMapRenderPass.
        bool HighDynamicRange = true;
        // Depth is reversed-Z (1 at the near plane). Must match what the depth
        // buffer actually holds — get it wrong and FSR2's disocclusion test
        // inverts, which reads as trailing edges on moving geometry rather than
        // as an error.
        bool InvertedDepth = false;
        // Depth uses an infinite far plane.
        bool InfiniteDepth = false;
        // Let the upscaler meter exposure itself and BAKE IT INTO THE OUTPUT.
        //
        // False for this engine, which is the opposite of the intuitive answer.
        // Tone mapping (and auto-exposure) run AFTER the upscaler, so an output
        // that already carries an exposure gets exposed a second time and the
        // frame comes out systematically darker — measured at 36% on the FSR2
        // path. Implementations that set this false must supply a NEUTRAL
        // exposure rather than none: with no exposure resource bound, FSR2 falls
        // back to its internal metering and the flag has no effect at all.
        bool AutoExposure = false;

        // The engine's motion vectors already contain the projection JITTER.
        //
        // True here, and it is not a detail. RenderPipeline bakes each frame's
        // jitter into that frame's ProjectionMatrix and keeps the PREVIOUS
        // frame's jitter in the previous ViewProjection, so a G-Buffer velocity
        // of (ndcCurr - ndcPrev) * 0.5 carries the difference between two
        // jitters — it is NOT zero even for a perfectly static camera. Engine TAA
        // relies on exactly that (it needs no unjitter uniform).
        //
        // A temporal upscaler that is not told this reprojects every pixel by up
        // to a pixel of pure jitter, every frame, forever. The frame does not
        // break: the first frame after a history reset is pixel-correct and every
        // frame after it is stably wrong, which is the least debuggable shape a
        // defect can take.
        bool MotionVectorsIncludeJitter = true;

        [[nodiscard]] auto operator==(const TemporalUpscalerConfig&) const -> bool = default;
    };

    // Everything that changes per frame.
    struct TemporalUpscalerDispatch
    {
        // Inputs, all at RENDER resolution. Colour is pre-tone-map HDR; depth is
        // the scene depth the colour was rendered with; velocity is the engine's
        // RG16F motion-vector target.
        RHI::ResourceHandle Color;
        RHI::ResourceHandle Depth;
        RHI::ResourceHandle Velocity;
        // Output, at DISPLAY resolution.
        RHI::ResourceHandle Output;

        u32 RenderWidth = 0;
        u32 RenderHeight = 0;

        // The sub-pixel projection jitter that was baked into THIS frame's
        // projection matrix, in render-resolution pixels. Must be the same value
        // the camera used — the upscaler subtracts it, so a mismatch is a
        // permanent sub-pixel offset that looks like softness, not like a bug.
        glm::vec2 JitterPixels = glm::vec2(0.0f);

        // Multiplies the sampled motion vector to turn it into a render-resolution
        // pixel displacement pointing from THIS frame's pixel to where the same
        // surface was in the PREVIOUS frame. The engine stores
        // (ndcCurr - ndcPrev) * 0.5, i.e. UV-space current-minus-previous, so this
        // carries both the unit conversion and the sign flip.
        glm::vec2 MotionVectorScale = glm::vec2(0.0f);

        f32 DeltaTimeSeconds = 0.0f;
        f32 NearPlane = 0.1f;
        f32 FarPlane = 1000.0f;
        f32 VerticalFovRadians = 1.0f;

        // FSR2's built-in RCAS. Disabled means the upscaler emits an unsharpened
        // image and the caller is responsible for any sharpening it wants.
        bool EnableSharpening = true;
        f32 Sharpness = 0.5f; // 0 = none, 1 = maximum

        // Discard the accumulated history for this frame. Set it on the first
        // frame after a camera cut, a resolution change or a mode toggle —
        // reprojecting across a cut is what produces a smeared first second.
        bool ResetHistory = false;
    };

    class TemporalUpscaler : public RefCounted
    {
      public:
        ~TemporalUpscaler() override = default;

        [[nodiscard]] virtual TemporalUpscalerStatus GetStatus() const noexcept = 0;

        // "Could this run?", not "is it running". Safe to call before Configure —
        // see the note on TemporalUpscalerStatus::Available.
        [[nodiscard]] bool IsAvailable() const noexcept
        {
            return GetStatus() == TemporalUpscalerStatus::Available;
        }

        // Idempotent: a call that reproduces the current configuration is a no-op,
        // so the caller may (and does) call it every frame. Returns false if the
        // upscaler is now unusable; GetStatus() then says why.
        virtual bool Configure(const TemporalUpscalerConfig& config) = 0;

        // Records the upscale into the currently-bound context. Returns false if
        // nothing was recorded — the caller must then leave its output target
        // alone rather than presenting an uninitialised one.
        virtual bool Dispatch(const TemporalUpscalerDispatch& dispatch) = 0;

        // The jitter sequence is the upscaler's, not the engine's: FSR2 derives
        // both the phase count and the offsets from the render/display ratio, and
        // feeding it the engine's Halton-16 TAA sequence instead would under-sample
        // the reconstruction it is built around.
        [[nodiscard]] virtual i32 GetJitterPhaseCount(u32 renderWidth, u32 displayWidth) const = 0;
        [[nodiscard]] virtual glm::vec2 GetJitterOffset(i32 phaseIndex, i32 phaseCount) const = 0;

        // Backend factory. Never returns null — an unsupported build or backend
        // yields an object whose GetStatus() explains itself, so callers branch on
        // IsAvailable() rather than on a null check.
        [[nodiscard]] static Ref<TemporalUpscaler> Create();
    };
} // namespace OloEngine
