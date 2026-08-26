#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Water/WaterDisturbanceSystem.h"

#include "OloEngine/Renderer/ComputeShader.h"
#include "OloEngine/Renderer/HeapBindingSeam.h"
#include "OloEngine/Renderer/MemoryBarrierFlags.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Renderer/UniformBuffer.h"

#include <algorithm>
#include <cmath>

namespace OloEngine
{
    namespace
    {
        [[nodiscard("the finiteness result must be used")]] bool IsFinite2(const glm::vec2& v) noexcept
        {
            return std::isfinite(v.x) && std::isfinite(v.y);
        }
    } // namespace

    WaterDisturbanceSystem::WaterDisturbanceData WaterDisturbanceSystem::s_Data;

    void WaterDisturbanceSystem::Init()
    {
        OLO_PROFILE_FUNCTION();

        if (s_Data.m_Initialized)
        {
            OLO_CORE_WARN("WaterDisturbanceSystem::Init called when already initialized");
            return;
        }

        // RG16F, not R8 and not a single-channel 16-bit float. The precision
        // argument (an R8 field's multiplicative decay rounds back to where it
        // started and never fades) and the reason the format is two-channel
        // (the engine's ImageFormat has no R16F, and adding one is a
        // cross-backend change with a silent-null failure mode) are both
        // written up in WaterDisturbanceField.h §4.
        TextureSpecification spec;
        spec.Width = static_cast<u32>(WaterDisturbance::kResolution);
        spec.Height = static_cast<u32>(WaterDisturbance::kResolution);
        spec.Format = ImageFormat::RG16F;
        // No mips: the field is sampled at LOD 0 by a shader that already fades
        // it with distance, and a mip chain on a texture written by compute
        // every frame would need a regenerate the sampling never pays for.
        spec.GenerateMips = false;

        s_Data.m_FieldTexture = Texture2D::Create(spec);

        s_Data.m_UpdateShader =
            ComputeShader::Create("assets/shaders/compute/WaterDisturbance_Update.comp");

        s_Data.m_ParamsUBO = UniformBuffer::Create(UBOStructures::WaterDisturbanceUBO::GetSize(),
                                                   ShaderBindingLayout::UBO_WATER_DISTURBANCE);

        if (!s_Data.m_FieldTexture || !s_Data.m_UpdateShader || !s_Data.m_ParamsUBO)
        {
            OLO_CORE_ERROR("WaterDisturbanceSystem::Init — failed to create GPU resources; "
                           "boat wake foam is disabled for this session");
            s_Data.m_FieldTexture.Reset();
            s_Data.m_UpdateShader.Reset();
            s_Data.m_ParamsUBO.Reset();
            return;
        }

        s_Data.m_NeedsClear = true;
        s_Data.m_HasValidWindow = false;
        s_Data.m_Initialized = true;
        OLO_CORE_INFO("WaterDisturbanceSystem initialized ({}x{} RG16F, {:.1f} m field)",
                      WaterDisturbance::kResolution, WaterDisturbance::kResolution,
                      WaterDisturbance::kFieldExtentMetres);
    }

    void WaterDisturbanceSystem::Shutdown()
    {
        OLO_PROFILE_FUNCTION();

        s_Data.m_FieldTexture.Reset();
        s_Data.m_UpdateShader.Reset();
        s_Data.m_ParamsUBO.Reset();
        s_Data.m_SplatCount = 0;
        s_Data.m_DroppedSplats = 0;
        s_Data.m_HasValidWindow = false;
        s_Data.m_NeedsClear = true;
        s_Data.m_Initialized = false;
    }

    bool WaterDisturbanceSystem::IsInitialized()
    {
        return s_Data.m_Initialized;
    }

    bool WaterDisturbanceSystem::SubmitSplat(const WaterDisturbanceSplat& splat)
    {
        OLO_PROFILE_FUNCTION();

        // THE validation boundary. Everything downstream — including
        // WaterDisturbance::SplatWeight and its GLSL twin — assumes finite
        // inputs, which is what lets those two stay expression-for-expression
        // identical instead of each inventing its own fallback.
        if (!IsFinite2(splat.m_From) || !IsFinite2(splat.m_To) ||
            !std::isfinite(splat.m_Radius) || !std::isfinite(splat.m_Strength) ||
            !std::isfinite(splat.m_Softness))
        {
            return false;
        }

        // A zero-radius or zero-strength splat would occupy a queue slot and
        // contribute nothing; reject it here rather than let it crowd out a
        // real one when the queue is under pressure.
        if (!(splat.m_Radius > 0.0f) || !(splat.m_Strength > 0.0f))
        {
            return false;
        }

        // Deliberately NOT gated on m_Initialized: the queue is plain CPU
        // state, so a headless scene tick records wake with no renderer at all
        // and the next Update simply finds a full queue or none. Gating here
        // would make the gameplay side behave differently depending on whether
        // a GPU happened to be present.
        if (s_Data.m_SplatCount >= WaterDisturbance::kMaxSplatsPerFrame)
        {
            ++s_Data.m_DroppedSplats;
            // Throttled: a scene that overruns does so every frame, and an
            // unthrottled warning would bury the log it is meant to be visible
            // in. The counter is the real signal; this is the pointer to it.
            if (s_Data.m_DroppedSplats == 1u || (s_Data.m_DroppedSplats % 600u) == 0u)
            {
                OLO_CORE_WARN("WaterDisturbanceSystem — splat queue full ({} slots); {} dropped so far. "
                              "The visible wake is incomplete.",
                              WaterDisturbance::kMaxSplatsPerFrame, s_Data.m_DroppedSplats);
            }
            return false;
        }

        s_Data.m_Splats[s_Data.m_SplatCount] = splat;
        ++s_Data.m_SplatCount;
        return true;
    }

    void WaterDisturbanceSystem::Update(const WaterDisturbanceSettings& settings, glm::vec2 followXZ,
                                        Timestep dt)
    {
        OLO_PROFILE_FUNCTION();

        s_Data.m_Settings = settings;

        if (!s_Data.m_Initialized)
        {
            s_Data.m_SplatCount = 0;
            return;
        }

        if (!settings.m_Enabled)
        {
            // Drop the queue AND mark the field for a clear, so re-enabling
            // does not resurrect a wake that has been logically absent for
            // minutes. Without this the field would be frozen rather than
            // decayed while disabled — the same cross-frame-history defect
            // docs/agent-rules/runtime-scene-switching.md is about.
            s_Data.m_SplatCount = 0;
            s_Data.m_NeedsClear = true;
            s_Data.m_HasValidWindow = false;
            return;
        }

        if (!IsFinite2(followXZ))
        {
            // A non-finite camera would snap the window to a garbage lattice
            // and the field would address nowhere. Hold the previous window.
            followXZ = s_Data.m_HasValidWindow
                           ? WaterDisturbance::WindowCentreWorld(s_Data.m_LatticeMin)
                           : glm::vec2(0.0f);
        }

        const glm::ivec2 newLatticeMin = WaterDisturbance::LatticeMinForCentre(followXZ);
        // On the first frame after Init/Reset there is no previous window. Point
        // it at the new one so nothing is treated as "carried over" — the
        // ResetAll flag is what actually clears the texture that frame.
        s_Data.m_PrevLatticeMin = s_Data.m_HasValidWindow ? s_Data.m_LatticeMin : newLatticeMin;
        s_Data.m_LatticeMin = newLatticeMin;

        const f32 deltaSeconds = std::clamp(static_cast<f32>(dt), 0.0f, 0.25f);
        const f32 halfLife = (std::isfinite(settings.m_HalfLifeSeconds) && settings.m_HalfLifeSeconds > 0.0f)
                                 ? settings.m_HalfLifeSeconds
                                 : 6.0f;
        s_Data.m_DecayFactor = WaterDisturbance::DecayFactor(halfLife, deltaSeconds);

        UploadComputeParams();

        s_Data.m_UpdateShader->Bind();

        // Persistent lifetime: the field is system-owned and lives across
        // frames, so its descriptor is memoised rather than ring-allocated —
        // same reasoning as the snow-depth clipmap.
        HeapBinding::BindImageOrOffset(0, s_Data.m_FieldTexture->GetRHIHandle(), 0, false, 0,
                                       RHI::Access::StorageReadWrite, RHI::Format::RG16Float,
                                       RHI::HeapSlotLifetime::Persistent);
        HeapBinding::FlushOffsets();

        const u32 groups =
            (static_cast<u32>(WaterDisturbance::kResolution) + WaterDisturbance::kWorkGroupSize - 1u) /
            WaterDisturbance::kWorkGroupSize;
        RenderCommand::DispatchCompute(groups, groups, 1);
        RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderImageAccess |
                                     MemoryBarrierFlags::TextureFetch);

        s_Data.m_SplatCount = 0;
        s_Data.m_NeedsClear = false;
        s_Data.m_HasValidWindow = true;
    }

    void WaterDisturbanceSystem::UploadComputeParams()
    {
        UBOStructures::WaterDisturbanceUBO params{};
        params.LatticeMin = s_Data.m_LatticeMin;
        params.PrevLatticeMin = s_Data.m_PrevLatticeMin;
        params.TexelSize = WaterDisturbance::kTexelSizeMetres;
        params.DecayFactor = s_Data.m_DecayFactor;
        params.Resolution = WaterDisturbance::kResolution;
        params.SplatCount = static_cast<i32>(s_Data.m_SplatCount);
        params.ResetAll = s_Data.m_NeedsClear ? 1 : 0;

        for (u32 i = 0; i < s_Data.m_SplatCount; ++i)
        {
            const WaterDisturbanceSplat& src = s_Data.m_Splats[i];
            params.Splats[i].P0Radius = glm::vec4(src.m_From.x, src.m_From.y, src.m_Radius,
                                                  std::clamp(src.m_Strength, 0.0f, 1.0f));
            params.Splats[i].P1Shape = glm::vec4(src.m_To.x, src.m_To.y, src.m_Softness, 0.0f);
        }

        // The Bind() must follow the SetData: on the Vulkan route every SetData
        // mints a fresh arena address (ADR 0011 §4), so binding first would
        // point the dispatch at the previous frame's block.
        s_Data.m_ParamsUBO->SetData(&params, UBOStructures::WaterDisturbanceUBO::GetSize());
        s_Data.m_ParamsUBO->Bind();
    }

    void WaterDisturbanceSystem::BindFieldTexture()
    {
        OLO_PROFILE_FUNCTION();

        if (!s_Data.m_Initialized || !s_Data.m_FieldTexture)
            return;

        // PUBLISH, NOT BIND — Water.glsl exists in both a slot-based and a
        // bindless variant, and which one is in flight is not knowable here
        // (the water shaders bind themselves inside CommandBucket::Execute).
        // The seam does both: stages the heap offset for a bindless consumer
        // and issues the slot bind for a slot-based one. Binding only would
        // leave the bindless variant indexing the previous frame's offset,
        // which shows as a wake in the wrong place rather than as an error.
        HeapBinding::PublishTextureOffsetAndBind(ShaderBindingLayout::TEX_WATER_DISTURBANCE,
                                                 s_Data.m_FieldTexture->GetRHIHandle(),
                                                 RHI::HeapSlotLifetime::Persistent);
    }

    RHI::ResourceHandle WaterDisturbanceSystem::GetFieldTextureHandle()
    {
        if (s_Data.m_Initialized && s_Data.m_FieldTexture)
            return s_Data.m_FieldTexture->GetRHIHandle();
        return RHI::NullResource;
    }

    glm::vec4 WaterDisturbanceSystem::GetShaderParams()
    {
        // w <= 0 is the disabled state. Reported for every reason the field
        // could be unusable — not initialized, disabled, or never yet written —
        // so a caller that packs this unconditionally into the water UBO cannot
        // put a stale or uninitialised field on screen.
        if (!s_Data.m_Initialized || !s_Data.m_Settings.m_Enabled || !s_Data.m_HasValidWindow)
            return glm::vec4(0.0f);

        const glm::vec2 centre = WaterDisturbance::WindowCentreWorld(s_Data.m_LatticeMin);
        const f32 intensity = std::isfinite(s_Data.m_Settings.m_Intensity)
                                  ? std::clamp(s_Data.m_Settings.m_Intensity, 0.0f, 4.0f)
                                  : 1.0f;
        return { centre.x, centre.y, WaterDisturbance::kInvFieldExtentMetres, intensity };
    }

    glm::vec4 WaterDisturbanceSystem::GetShaderParams2()
    {
        f32 fadeStart = std::isfinite(s_Data.m_Settings.m_FadeStartMetres)
                            ? std::clamp(s_Data.m_Settings.m_FadeStartMetres, 0.0f, 2000.0f)
                            : 60.0f;
        f32 fadeEnd = std::isfinite(s_Data.m_Settings.m_FadeEndMetres)
                          ? std::clamp(s_Data.m_Settings.m_FadeEndMetres, 0.0f, 4000.0f)
                          : 220.0f;
        // smoothstep(edge0, edge1, x) is undefined for edge0 >= edge1; a scene
        // that authored them inverted would get an implementation-defined wake
        // rather than a clamped one.
        fadeEnd = std::max(fadeEnd, fadeStart + 1.0f);
        return { fadeStart, fadeEnd, WaterDisturbance::kEdgeFadeStart, 0.0f };
    }

    void WaterDisturbanceSystem::Reset()
    {
        OLO_PROFILE_FUNCTION();

        s_Data.m_NeedsClear = true;
        s_Data.m_HasValidWindow = false;
        s_Data.m_SplatCount = 0;
        s_Data.m_DroppedSplats = 0;
    }

    u32 WaterDisturbanceSystem::GetQueuedSplatCount()
    {
        return s_Data.m_SplatCount;
    }

    u32 WaterDisturbanceSystem::GetDroppedSplatCount()
    {
        return s_Data.m_DroppedSplats;
    }
} // namespace OloEngine
