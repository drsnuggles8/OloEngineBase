#include "OloEnginePCH.h"
#include "Platform/OpenGL/OpenGLTemporalUpscaler.h"

#if OLO_WITH_FSR2

#include "OloEngine/Renderer/RHI/RHIResourceRegistry.h"

#include <GLFW/glfw3.h>
#include <glad/gl.h>

// The FSR2 headers pull in their OWN glad copy for the PFNGL* typedefs. Both are
// glad 2.x generated from the same registry, so the include guards make the
// second one a no-op — but only if ours is first, which the include above
// guarantees. See cmake/fsr2.cmake for why we do not try to unify them.
#include <ffx_fsr2.h>
#include <ffx_fsr2_gl.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <vector>

namespace OloEngine
{
    namespace
    {
        // FSR2's GL backend binds its own UBOs, textures, samplers and images to
        // FIXED slot indices taken straight from the DirectX register numbers in
        // the shader permutations, and it never puts them back. Those indices are
        // in the same namespace as the engine's: the constant buffer of the
        // compute-luminance-pyramid pass lands on binding 5, which is
        // `MultiLightBuffer` in PBR_MultiLight.glsl, and other passes hit 3, 4,
        // 11, 12, 14 and 18 — `ShadowData` sits at 6 and the whole range is live.
        //
        // The engine binds those UBOs once rather than per frame, so a single
        // dispatch silently unlights EVERY LATER FRAME: the forward shader reads
        // FSR2's constants as its light array and every lit surface collapses to
        // ambient. Measured on the #684 evidence scene, the corruption follows
        // the DISPATCH, not the technique — running the spatial upscaler after
        // FSR2 had run once made the spatial frame equally dark (SceneColor mean
        // 65.5 -> 32.3), which is what separates this from an FSR2 bug.
        //
        // GLStateGuard is deliberately no help here: it documents that per-slot
        // texture and UBO bindings are NOT restored, because for a render pass
        // that is ~185 GL calls of pure overhead. This pass is different — it is
        // a third-party dispatch that treats the binding namespace as its own,
        // and it runs once a frame.
        //
        // The ranges are the maxima the shader permutations actually declare
        // (FSR2_BIND_{SRV,UAV,CB}_*) rather than a guess, and are then clamped to
        // what the driver reports — a slot past the limit is not "nothing bound",
        // it is GL_INVALID_VALUE on both the query and the bind.
        class FSR2BindingScope
        {
          public:
            FSR2BindingScope()
            {
                m_UboSlots = ClampToDriverLimit(GL_MAX_UNIFORM_BUFFER_BINDINGS, kUboSlots);
                m_TextureSlots = ClampToDriverLimit(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, kTextureSlots);
                m_ImageSlots = ClampToDriverLimit(GL_MAX_IMAGE_UNITS, kImageSlots);

                for (u32 i = 0; i < m_UboSlots; ++i)
                {
                    GLint buffer = 0;
                    GLint64 start = 0;
                    GLint64 size = 0;
                    glGetIntegeri_v(GL_UNIFORM_BUFFER_BINDING, i, &buffer);
                    glGetInteger64i_v(GL_UNIFORM_BUFFER_START, i, &start);
                    glGetInteger64i_v(GL_UNIFORM_BUFFER_SIZE, i, &size);
                    m_Ubo[i] = { static_cast<GLuint>(buffer), start, size };
                }

                for (u32 i = 0; i < m_TextureSlots; ++i)
                {
                    GLint sampler = 0;
                    glGetIntegeri_v(GL_SAMPLER_BINDING, i, &sampler);
                    m_Sampler[i] = static_cast<GLuint>(sampler);

                    // There is no core indexed query for a texture binding, so
                    // walk the units. The active unit is restored below.
                    glActiveTexture(GL_TEXTURE0 + i);
                    GLint texture = 0;
                    glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
                    m_Texture2D[i] = static_cast<GLuint>(texture);
                }

                for (u32 i = 0; i < m_ImageSlots; ++i)
                {
                    GLint name = 0;
                    GLint level = 0;
                    GLint layered = 0;
                    GLint layer = 0;
                    GLint access = GL_READ_WRITE;
                    GLint format = GL_RGBA8;
                    glGetIntegeri_v(GL_IMAGE_BINDING_NAME, i, &name);
                    glGetIntegeri_v(GL_IMAGE_BINDING_LEVEL, i, &level);
                    glGetIntegeri_v(GL_IMAGE_BINDING_LAYERED, i, &layered);
                    glGetIntegeri_v(GL_IMAGE_BINDING_LAYER, i, &layer);
                    glGetIntegeri_v(GL_IMAGE_BINDING_ACCESS, i, &access);
                    glGetIntegeri_v(GL_IMAGE_BINDING_FORMAT, i, &format);
                    // An EMPTY image slot reports format 0, and glBindImageTexture
                    // rejects 0 as a format even when the texture is 0. Any sized
                    // format is accepted for an unbind, so normalise it rather than
                    // skipping the slot — skipping would leave FSR2's binding in
                    // place, which is the whole bug.
                    if (format == 0)
                        format = GL_RGBA8;
                    m_Image[i] = { static_cast<GLuint>(name), level,
                                   layered != GL_FALSE, layer,
                                   static_cast<GLenum>(access), static_cast<GLenum>(format) };
                }

                GLint activeUnit = GL_TEXTURE0;
                glGetIntegerv(GL_ACTIVE_TEXTURE, &activeUnit);
                m_ActiveTexture = static_cast<GLenum>(activeUnit);
            }

            ~FSR2BindingScope()
            {
                for (u32 i = 0; i < m_UboSlots; ++i)
                {
                    const Buffer& b = m_Ubo[i];
                    // A slot bound with glBindBufferBase reports start = size = 0;
                    // replaying that through glBindBufferRange would bind an empty
                    // range instead of the whole buffer, so the two cases differ.
                    if (b.Size > 0)
                        glBindBufferRange(GL_UNIFORM_BUFFER, i, b.Name, static_cast<GLintptr>(b.Start),
                                          static_cast<GLsizeiptr>(b.Size));
                    else
                        glBindBufferBase(GL_UNIFORM_BUFFER, i, b.Name);
                }

                for (u32 i = 0; i < m_TextureSlots; ++i)
                {
                    glBindSampler(i, m_Sampler[i]);
                    glBindTextureUnit(i, m_Texture2D[i]);
                }

                for (u32 i = 0; i < m_ImageSlots; ++i)
                {
                    const Image& img = m_Image[i];
                    glBindImageTexture(i, img.Name, img.Level, img.Layered ? GL_TRUE : GL_FALSE, img.Layer,
                                       img.Access, img.Format);
                }

                glActiveTexture(m_ActiveTexture);
            }

            FSR2BindingScope(const FSR2BindingScope&) = delete;
            FSR2BindingScope(FSR2BindingScope&&) = delete;
            FSR2BindingScope& operator=(const FSR2BindingScope&) = delete;
            FSR2BindingScope& operator=(FSR2BindingScope&&) = delete;

          private:
            // Slots past the driver's limit are not "nothing bound", they are an
            // error on both the query and the bind, so clamp instead of assuming.
            [[nodiscard]] static u32 ClampToDriverLimit(GLenum pname, u32 wanted)
            {
                GLint limit = 0;
                glGetIntegerv(pname, &limit);
                if (limit <= 0)
                    return 0u;
                return std::min(wanted, static_cast<u32>(limit));
            }

            // Highest slot each FSR2 permutation family declares, plus one.
            static constexpr u32 kUboSlots = 19u;     // FSR2_BIND_CB_*  peaks at 18
            static constexpr u32 kTextureSlots = 13u; // FSR2_BIND_SRV_* peaks at 12
            static constexpr u32 kImageSlots = 18u;   // FSR2_BIND_UAV_* peaks at 17

            struct Buffer
            {
                GLuint Name = 0u;
                GLint64 Start = 0;
                GLint64 Size = 0;
            };

            struct Image
            {
                GLuint Name = 0u;
                GLint Level = 0;
                bool Layered = false;
                GLint Layer = 0;
                GLenum Access = GL_READ_WRITE;
                GLenum Format = GL_RGBA8;
            };

            std::array<Buffer, kUboSlots> m_Ubo{};
            std::array<GLuint, kTextureSlots> m_Sampler{};
            std::array<GLuint, kTextureSlots> m_Texture2D{};
            std::array<Image, kImageSlots> m_Image{};
            GLenum m_ActiveTexture = GL_TEXTURE0;

            u32 m_UboSlots = 0u;
            u32 m_TextureSlots = 0u;
            u32 m_ImageSlots = 0u;
        };

        // Resolve an engine handle to the GL name FSR2 wants. Returns 0 on any
        // failure, which is also GL's "no object" — legitimate here because
        // FSR2 is handed textures the engine just rendered into, so a 0 is
        // always a bug on our side and never a valid input.
        [[nodiscard]] GLuint ResolveGLTexture(RHI::ResourceHandle handle)
        {
            if (!handle.IsValid())
                return 0u;

            const RHI::NativeHandle native = RHI::ResourceRegistry::Get().ResolveTaggedForBackend(handle);
            if (native.Owner != RHI::Backend::OpenGL)
                return 0u;

            return static_cast<GLuint>(native.Value);
        }

        // FSR2 needs each resource's sized internal format. Asking GL beats
        // threading formats down from the render pass: the pass would have to
        // restate what the framebuffer already knows, and the two would drift the
        // first time an attachment format changed.
        [[nodiscard]] GLenum QueryInternalFormat(GLuint texture)
        {
            if (texture == 0u)
                return 0u;

            GLint format = 0;
            glGetTextureLevelParameteriv(texture, 0, GL_TEXTURE_INTERNAL_FORMAT, &format);
            return static_cast<GLenum>(format);
        }

        void FsrMessageCallback(FfxFsr2MsgType type, const wchar_t* message)
        {
            // The runtime hands us wide strings; narrow them the dumb way rather
            // than dragging in a locale conversion for a log line.
            std::string narrow;
            narrow.reserve(256);
            for (const wchar_t* c = message; c != nullptr && *c != L'\0'; ++c)
                narrow.push_back(static_cast<char>(*c < 128 ? *c : '?'));

            if (type == FFX_FSR2_MESSAGE_TYPE_ERROR)
                OLO_CORE_ERROR("FSR2: {}", narrow);
            else
                OLO_CORE_WARN("FSR2: {}", narrow);
        }

        class OpenGLTemporalUpscaler final : public TemporalUpscaler
        {
          public:
            // Probes the DEVICE eagerly, in the constructor, and this is
            // load-bearing rather than an optimisation.
            //
            // The obvious design — "status becomes Available once Configure
            // succeeds" — deadlocks: the pipeline only enables the pass when the
            // upscaler reports itself available, Configure only runs inside the
            // enabled pass's Execute, so the status can never leave NotConfigured
            // and FSR2 never runs. That is not hypothetical; it is what this class
            // did on its first run, and because the visual tests SKIP on an
            // unavailable upscaler it reported itself as a clean skip rather than
            // a failure.
            //
            // So "can this device run FSR2" is answered up front and separately
            // from "is a context built". The probe is cheap — it loads the GL
            // function table and queries extensions; it allocates none of the
            // history/lock/luminance targets that Configure does, so nothing is
            // paid by a session that never selects the temporal technique.
            OpenGLTemporalUpscaler()
            {
                ProbeDevice();
            }

            ~OpenGLTemporalUpscaler() override
            {
                DestroyContext();
                DestroyNeutralExposureTexture();
            }

            [[nodiscard]] TemporalUpscalerStatus GetStatus() const noexcept override
            {
                return m_Status;
            }

            bool Configure(const TemporalUpscalerConfig& config) override
            {
                if (m_Status != TemporalUpscalerStatus::Available)
                    return false;

                if (config.DisplayWidth == 0u || config.DisplayHeight == 0u ||
                    config.MaxRenderWidth == 0u || config.MaxRenderHeight == 0u)
                {
                    OLO_CORE_WARN("FSR2: refusing to configure with a zero dimension ({}x{} render, {}x{} display)",
                                  config.MaxRenderWidth, config.MaxRenderHeight, config.DisplayWidth, config.DisplayHeight);
                    return false;
                }

                // Idempotent by contract — the render pass calls this every frame.
                if (m_ContextCreated && config == m_Config)
                    return true;

                DestroyContext();

                m_ScratchBuffer.assign(ffxFsr2GetScratchMemorySizeGL(), std::byte{});

                FfxFsr2Interface backendInterface{};
                if (const FfxErrorCode err = ffxFsr2GetInterfaceGL(&backendInterface,
                                                                   m_ScratchBuffer.data(),
                                                                   m_ScratchBuffer.size(),
                                                                   reinterpret_cast<ffx_glGetProcAddress>(GLFWAPI::glfwGetProcAddress));
                    err != FFX_OK)
                {
                    OLO_CORE_ERROR("FSR2: ffxFsr2GetInterfaceGL failed ({})", static_cast<i32>(err));
                    m_Status = TemporalUpscalerStatus::DeviceUnsupported;
                    return false;
                }

                FfxFsr2ContextDescription desc{};
                desc.flags = 0u;
                if (config.HighDynamicRange)
                    desc.flags |= FFX_FSR2_ENABLE_HIGH_DYNAMIC_RANGE;
                if (config.InvertedDepth)
                    desc.flags |= FFX_FSR2_ENABLE_DEPTH_INVERTED;
                if (config.InfiniteDepth)
                    desc.flags |= FFX_FSR2_ENABLE_DEPTH_INFINITE;
                if (config.AutoExposure)
                    desc.flags |= FFX_FSR2_ENABLE_AUTO_EXPOSURE;
                if (config.MotionVectorsIncludeJitter)
                    desc.flags |= FFX_FSR2_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION;
#ifdef OLO_DEBUG
                desc.flags |= FFX_FSR2_ENABLE_DEBUG_CHECKING;
#endif
                // See the commandList note in Dispatch: OpenGL has neither a
                // device object nor a command list to hand FSR2, so its null
                // checks on both have to be waived or every dispatch fails.
                desc.flags |= FFX_FSR2_ALLOW_NULL_DEVICE_AND_COMMAND_LIST;
                // Motion vectors are at RENDER resolution, so
                // DISPLAY_RESOLUTION_MOTION_VECTORS does not apply. They DO carry
                // the jitter, which is what the cancellation flag above is for —
                // FSR2 derives the per-frame cancellation itself from the
                // jitterOffset we hand it each dispatch, so there is nothing more
                // to pass.
                desc.maxRenderSize = { config.MaxRenderWidth, config.MaxRenderHeight };
                desc.displaySize = { config.DisplayWidth, config.DisplayHeight };
                desc.callbacks = backendInterface;
                desc.device = nullptr; // the GL backend takes its device from the current context
                desc.fpMessage = &FsrMessageCallback;

                if (const FfxErrorCode err = ffxFsr2ContextCreate(&m_Context, &desc); err != FFX_OK)
                {
                    OLO_CORE_ERROR("FSR2: ffxFsr2ContextCreate failed ({}) for {}x{} -> {}x{}",
                                   static_cast<i32>(err), config.MaxRenderWidth, config.MaxRenderHeight,
                                   config.DisplayWidth, config.DisplayHeight);
                    m_Status = TemporalUpscalerStatus::DeviceUnsupported;
                    m_ScratchBuffer.clear();
                    return false;
                }

                m_ContextCreated = true;
                m_Config = config;
                // A brand new context has no history, so the first frame through it
                // must not reproject. The caller may also ask for a reset; this is
                // the reset it cannot know about.
                m_ForceResetNextDispatch = true;

                OLO_CORE_INFO("FSR2: context created — {}x{} max render -> {}x{} display (HDR {}, inverted depth {}, auto exposure {})",
                              config.MaxRenderWidth, config.MaxRenderHeight, config.DisplayWidth, config.DisplayHeight,
                              config.HighDynamicRange, config.InvertedDepth, config.AutoExposure);
                return true;
            }

            bool Dispatch(const TemporalUpscalerDispatch& dispatch) override
            {
                if (!m_ContextCreated || m_Status != TemporalUpscalerStatus::Available)
                    return false;

                const GLuint color = ResolveGLTexture(dispatch.Color);
                const GLuint depth = ResolveGLTexture(dispatch.Depth);
                const GLuint velocity = ResolveGLTexture(dispatch.Velocity);
                const GLuint output = ResolveGLTexture(dispatch.Output);
                if (color == 0u || depth == 0u || velocity == 0u || output == 0u)
                {
                    OLO_CORE_WARN("FSR2: skipping dispatch — unresolved input (color {}, depth {}, velocity {}, output {})",
                                  color, depth, velocity, output);
                    return false;
                }

                if (dispatch.RenderWidth == 0u || dispatch.RenderHeight == 0u)
                    return false;

                FfxFsr2DispatchDescription desc{};
                // There is no GL command list — the backend records straight into
                // the current context and ignores this field entirely. FSR2's own
                // validation rejects a null one unless we opted out at create
                // time, which is what ALLOW_NULL_DEVICE_AND_COMMAND_LIST is for.
                desc.commandList = nullptr;
                desc.color = ffxGetTextureResourceGL(color, dispatch.RenderWidth, dispatch.RenderHeight,
                                                     QueryInternalFormat(color), L"FSR2_InputColor");
                desc.depth = ffxGetTextureResourceGL(depth, dispatch.RenderWidth, dispatch.RenderHeight,
                                                     QueryInternalFormat(depth), L"FSR2_InputDepth");
                desc.motionVectors = ffxGetTextureResourceGL(velocity, dispatch.RenderWidth, dispatch.RenderHeight,
                                                             QueryInternalFormat(velocity), L"FSR2_InputVelocity");
                desc.output = ffxGetTextureResourceGL(output, m_Config.DisplayWidth, m_Config.DisplayHeight,
                                                      QueryInternalFormat(output), L"FSR2_Output");
                // See EnsureNeutralExposureTexture: a neutral 1.0 keeps FSR2 from
                // baking its metered exposure into the output that our tone
                // mapper is about to expose for a second time.
                desc.exposure = EnsureNeutralExposureTexture()
                                    ? ffxGetTextureResourceGL(m_NeutralExposure, 1u, 1u, GL_R32F, L"FSR2_Exposure")
                                    : ffxGetTextureResourceGL(0u, 1u, 1u, 0u, L"FSR2_Exposure");
                desc.reactive = ffxGetTextureResourceGL(0u, 1u, 1u, 0u, L"FSR2_Reactive");
                desc.transparencyAndComposition = ffxGetTextureResourceGL(0u, 1u, 1u, 0u, L"FSR2_TransparencyAndComposition");

                desc.jitterOffset = { dispatch.JitterPixels.x, dispatch.JitterPixels.y };
                desc.motionVectorScale = { dispatch.MotionVectorScale.x, dispatch.MotionVectorScale.y };
                desc.renderSize = { dispatch.RenderWidth, dispatch.RenderHeight };
                desc.enableSharpening = dispatch.EnableSharpening;
                desc.sharpness = dispatch.Sharpness;
                // FSR2 wants MILLISECONDS here, and reads it as a wall-clock hint
                // for its lock lifetime — feeding it seconds makes every lock
                // expire ~1000x too slowly, which looks like heavy ghosting.
                desc.frameTimeDelta = dispatch.DeltaTimeSeconds * 1000.0f;
                desc.preExposure = 1.0f;
                desc.reset = dispatch.ResetHistory || m_ForceResetNextDispatch;
                desc.cameraNear = dispatch.NearPlane;
                desc.cameraFar = dispatch.FarPlane;
                desc.cameraFovAngleVertical = dispatch.VerticalFovRadians;
                desc.viewSpaceToMetersFactor = 1.0f;
                // FALSE, despite the engine's projections being OpenGL's
                // [-1, 1] NDC ones. This looks wrong and is not:
                //
                //   * FSR2 samples the depth TEXTURE, and a GL depth attachment
                //     stores WINDOW depth, which glDepthRange's default maps as
                //     z_window = 0.5 * z_ndc + 0.5.
                //   * That affine remap is exactly the difference between the GL
                //     and D3D projection matrices in z. So the value in the
                //     texture already IS the [0, 1] device depth FSR2 assumes,
                //     and its near/far -> view-space transform is correct as-is.
                //
                // The flag would describe a pipeline whose depth BUFFER holds
                // [-1, 1] (a glDepthRange(-1, 1)), which this engine never sets —
                // and the GL backend refuses it anyway: ffx_fsr2.cpp asserts
                // `deviceDepthNegativeOneToOne == false` with "OpenGL depth
                // convention not yet supported", so passing true would trip a
                // debug assert and, in release, silently pick a transform for a
                // depth encoding we are not sending.
                desc.deviceDepthNegativeOneToOne = false;

                // Scoped so the bindings are back before anything else runs —
                // see FSR2BindingScope for what the runtime clobbers and why.
                FfxErrorCode err = FFX_OK;
                {
                    const FSR2BindingScope bindingScope;
                    err = ffxFsr2ContextDispatch(&m_Context, &desc);
                }
                m_ForceResetNextDispatch = false;

                if (err != FFX_OK)
                {
                    OLO_CORE_ERROR("FSR2: ffxFsr2ContextDispatch failed ({})", static_cast<i32>(err));
                    return false;
                }

                // FSR2 barriers BETWEEN its own passes, but the last write to the
                // output image is followed by nothing — and the very next thing
                // the engine does is SAMPLE that image as a texture in Bloom. An
                // image-store-to-texture-fetch hazard is not something GL will
                // report; it shows as an intermittently stale or torn upscale,
                // most visibly on the first frames after a resize.
                glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);

                // FSR2 leaves its last compute program bound. On NVIDIA the
                // driver re-validates the BOUND program at glClear, so handing
                // the next pass's clear an FSR2 compute program is the exact
                // shape gl-clear-program-revalidation.md is about. Unbinding is
                // cheaper than auditing every downstream clear site.
                glUseProgram(0);

                return true;
            }

            [[nodiscard]] i32 GetJitterPhaseCount(u32 renderWidth, u32 displayWidth) const override
            {
                if (renderWidth == 0u || displayWidth == 0u)
                    return 1;
                return std::max(1, ffxFsr2GetJitterPhaseCount(static_cast<i32>(renderWidth), static_cast<i32>(displayWidth)));
            }

            [[nodiscard]] glm::vec2 GetJitterOffset(i32 phaseIndex, i32 phaseCount) const override
            {
                if (phaseCount <= 0)
                    return glm::vec2(0.0f);

                f32 x = 0.0f;
                f32 y = 0.0f;
                // ffxFsr2GetJitterOffset takes a 0-based index and does its own
                // modulo, but it returns FFX_ERROR_INVALID_ARGUMENT for a
                // negative one, so clamp rather than trusting the caller.
                (void)ffxFsr2GetJitterOffset(&x, &y, std::max(0, phaseIndex), phaseCount);
                return glm::vec2(x, y);
            }

          private:
            // Cheap, allocation-free "does this GPU support FSR2 at all". The
            // GL backend REQUIRES GL_KHR_shader_subgroup and answers
            // FFX_ERROR_BACKEND_API_ERROR without it, so this is a real question
            // with a real negative answer, not a formality.
            void ProbeDevice()
            {
                std::vector<std::byte> probeScratch(ffxFsr2GetScratchMemorySizeGL(), std::byte{});

                FfxFsr2Interface backendInterface{};
                if (const FfxErrorCode err = ffxFsr2GetInterfaceGL(&backendInterface,
                                                                   probeScratch.data(),
                                                                   probeScratch.size(),
                                                                   reinterpret_cast<ffx_glGetProcAddress>(GLFWAPI::glfwGetProcAddress));
                    err != FFX_OK)
                {
                    OLO_CORE_WARN("FSR2: ffxFsr2GetInterfaceGL failed during the device probe ({})", static_cast<i32>(err));
                    m_Status = TemporalUpscalerStatus::DeviceUnsupported;
                    return;
                }

                // CreateBackendContext loads the function table the capability
                // query then reads through, so the two must be called as a pair.
                if (const FfxErrorCode err = backendInterface.fpCreateBackendContext(&backendInterface, nullptr); err != FFX_OK)
                {
                    OLO_CORE_WARN("FSR2: backend context probe failed ({}) — temporal upscaling unavailable", static_cast<i32>(err));
                    m_Status = TemporalUpscalerStatus::DeviceUnsupported;
                    return;
                }

                FfxDeviceCapabilities caps{};
                const FfxErrorCode capsErr = backendInterface.fpGetDeviceCapabilities(&backendInterface, &caps, nullptr);
                (void)backendInterface.fpDestroyBackendContext(&backendInterface);

                if (capsErr != FFX_OK)
                {
                    OLO_CORE_WARN("FSR2: this GPU/driver does not meet the OpenGL backend's requirements ({}) — "
                                  "GL_KHR_shader_subgroup with compute support is mandatory",
                                  static_cast<i32>(capsErr));
                    m_Status = TemporalUpscalerStatus::DeviceUnsupported;
                    return;
                }

                m_Status = TemporalUpscalerStatus::Available;
                OLO_CORE_INFO("FSR2: device supports temporal upscaling (wave size {}-{}, fp16 {})",
                              caps.waveLaneCountMin, caps.waveLaneCountMax, caps.fp16Supported);
            }

            // A 1x1 R32F texture holding exactly 1.0.
            //
            // WHY THIS EXISTS AT ALL, because "just let FSR2 meter it" is the
            // obvious and wrong answer. FSR2 divides the input colour by an
            // exposure to reach its working space and multiplies back on the way
            // out — but the value it multiplies back by is the exposure it
            // METERED, so with auto-exposure the output is exposure-APPLIED. This
            // engine tone maps (and auto-exposes) AFTER the upscaler, so that
            // output then gets exposed a second time and the frame is
            // systematically darker.
            //
            // The signature is unmistakable once seen: the first frame after a
            // history reset is pixel-correct (no exposure has been metered yet)
            // and every frame after it sits at a constant fraction of native.
            // Measured here at 116.5 -> 74.1 mean luma, flat from frame 1 on.
            //
            // Supplying a neutral exposure is the sanctioned "my colour is
            // already in the range you should work in" path. Note that CLEARING
            // FFX_FSR2_ENABLE_AUTO_EXPOSURE is not enough on its own: with no
            // exposure resource bound the runtime falls back to its internal one,
            // which is why toggling that flag alone changed nothing.
            [[nodiscard]] bool EnsureNeutralExposureTexture()
            {
                if (m_NeutralExposure != 0u)
                    return true;

                glCreateTextures(GL_TEXTURE_2D, 1, &m_NeutralExposure);
                if (m_NeutralExposure == 0u)
                    return false;

                glTextureStorage2D(m_NeutralExposure, 1, GL_R32F, 1, 1);
                constexpr f32 kNeutral = 1.0f;
                glTextureSubImage2D(m_NeutralExposure, 0, 0, 0, 1, 1, GL_RED, GL_FLOAT, &kNeutral);
                return true;
            }

            void DestroyNeutralExposureTexture()
            {
                if (m_NeutralExposure != 0u)
                {
                    glDeleteTextures(1, &m_NeutralExposure);
                    m_NeutralExposure = 0u;
                }
            }

            void DestroyContext()
            {
                if (m_ContextCreated)
                {
                    (void)ffxFsr2ContextDestroy(&m_Context);
                    m_ContextCreated = false;
                }
                m_ScratchBuffer.clear();
                m_Config = {};
                // Status is NOT downgraded here. It describes what the DEVICE can
                // do, which tearing a context down does not change — and the other
                // half of the deadlock described at the constructor was exactly
                // this: a resize destroys the context, the status fell back to
                // NotConfigured, and the pass switched itself off for good.
            }

            FfxFsr2Context m_Context{};
            std::vector<std::byte> m_ScratchBuffer;
            TemporalUpscalerConfig m_Config{};
            GLuint m_NeutralExposure = 0u;
            bool m_ContextCreated = false;
            bool m_ForceResetNextDispatch = true;
            // Set by ProbeDevice() in the constructor; thereafter only a Configure
            // failure moves it, and only downwards.
            TemporalUpscalerStatus m_Status = TemporalUpscalerStatus::NotConfigured;
        };
    } // namespace

    Ref<TemporalUpscaler> CreateOpenGLTemporalUpscaler()
    {
        return Ref<OpenGLTemporalUpscaler>::Create().As<TemporalUpscaler>();
    }
} // namespace OloEngine

#else // OLO_WITH_FSR2

namespace OloEngine
{
    namespace
    {
        // Kept a real object rather than a null Ref so every caller can ask WHY
        // instead of branching on a pointer and inventing its own reason.
        class UnavailableTemporalUpscaler final : public TemporalUpscaler
        {
          public:
            [[nodiscard]] TemporalUpscalerStatus GetStatus() const noexcept override
            {
                return TemporalUpscalerStatus::NotCompiledIn;
            }

            bool Configure(const TemporalUpscalerConfig&) override
            {
                return false;
            }

            bool Dispatch(const TemporalUpscalerDispatch&) override
            {
                return false;
            }

            [[nodiscard]] i32 GetJitterPhaseCount(u32, u32) const override
            {
                return 1;
            }

            [[nodiscard]] glm::vec2 GetJitterOffset(i32, i32) const override
            {
                return glm::vec2(0.0f);
            }
        };
    } // namespace

    Ref<TemporalUpscaler> CreateOpenGLTemporalUpscaler()
    {
        return Ref<UnavailableTemporalUpscaler>::Create().As<TemporalUpscaler>();
    }
} // namespace OloEngine

#endif // OLO_WITH_FSR2
