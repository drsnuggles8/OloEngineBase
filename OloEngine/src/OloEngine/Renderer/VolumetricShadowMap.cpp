#include "OloEnginePCH.h"
#include "OloEngine/Renderer/VolumetricShadowMap.h"

#include "OloEngine/Renderer/Commands/CommandDispatch.h"
#include "OloEngine/Renderer/ComputeShader.h"
#include "OloEngine/Renderer/HeapBindingSeam.h"
#include "OloEngine/Renderer/MemoryBarrierFlags.h"
#include "OloEngine/Renderer/PostProcessSettings.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Renderer/Texture3D.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>

namespace OloEngine
{
    VolumetricShadowMap::VolumetricShadowMapData VolumetricShadowMap::s_Data;

    // Two mirrors of the same number: this system sizes the volume with
    // kCascadeCount, the shared UBO sizes its transform arrays with
    // kVsmCascades, and the GLSL sizes them with VSM_CASCADE_COUNT. The first
    // two are pinned here; the third is pinned at runtime by the upload, which
    // writes VsmVolume.y from kCascadeCount and is read back by the generator's
    // own bounds check.
    static_assert(VolumetricShadowMap::kCascadeCount == UBOStructures::AtmosphereShadingUBO::kVsmCascades,
                  "The volumetric shadow volume's cascade count and the atmosphere UBO's transform-array "
                  "size must agree, or a cascade's transform is read from outside the array.");

    namespace VolumetricShadowMath
    {
        LightFrame BuildLightFrame(const glm::vec3& towardLight)
        {
            glm::vec3 toward = towardLight;
            if (const f32 lengthSq = glm::dot(toward, toward);
                !std::isfinite(lengthSq) || lengthSq < 1.0e-12f)
            {
                toward = glm::vec3(0.0f, 1.0f, 0.0f); // degenerate input: straight-down sun
            }
            else
            {
                toward /= std::sqrt(lengthSq);
            }

            LightFrame frame;
            frame.AxisZ = -toward; // the direction the light TRAVELS
            // Pick the reference up vector away from AxisZ so the cross product
            // never degenerates. The switch is discontinuous, which is harmless
            // here precisely BECAUSE the map carries no temporal history: a
            // basis flip re-fits the whole cascade in one frame rather than
            // blending two incompatible frames.
            const glm::vec3 up = (std::abs(frame.AxisZ.y) > 0.99f) ? glm::vec3(0.0f, 0.0f, 1.0f)
                                                                   : glm::vec3(0.0f, 1.0f, 0.0f);
            frame.AxisX = glm::normalize(glm::cross(up, frame.AxisZ));
            frame.AxisY = glm::cross(frame.AxisZ, frame.AxisX);
            return frame;
        }

        CascadeFit FitCascade(const Bounds& domain, const LightFrame& frame, u32 texelsXY, u32 slicesZ)
        {
            CascadeFit fit;
            fit.AxisX = frame.AxisX;
            fit.AxisY = frame.AxisY;
            fit.AxisZ = frame.AxisZ;
            if (!domain.IsValid())
            {
                return fit; // sizes stay zero -> IsValid() false
            }

            constexpr f32 kInf = std::numeric_limits<f32>::max();
            glm::vec3 lo(kInf, kInf, kInf);
            glm::vec3 hi(-kInf, -kInf, -kInf);

            for (u32 corner = 0; corner < 8u; ++corner)
            {
                const glm::vec3 p((corner & 1u) ? domain.Max.x : domain.Min.x,
                                  (corner & 2u) ? domain.Max.y : domain.Min.y,
                                  (corner & 4u) ? domain.Max.z : domain.Min.z);
                const glm::vec3 projected(glm::dot(p, frame.AxisX), glm::dot(p, frame.AxisY),
                                          glm::dot(p, frame.AxisZ));
                lo = glm::min(lo, projected);
                hi = glm::max(hi, projected);
            }

            // Texel-snap the light-space origin on every axis. The cell size is
            // derived from (cells - 1) rather than cells so that after snapping
            // the origin DOWN by up to one cell the box still covers the far
            // edge — a snap that shrinks coverage is worse than no snap,
            // because the domain's far corner would fall outside the footprint
            // and read as unshadowed.
            struct SnappedAxis
            {
                f32 Origin = 0.0f;
                f32 Size = 0.0f;
            };
            const auto snapAxis = [](f32 minValue, f32 maxValue, u32 cells) -> SnappedAxis
            {
                const f32 raw = std::max(maxValue - minValue, 1.0e-3f);
                if (cells < 2u)
                {
                    return { minValue, raw };
                }
                const f32 cell = raw / static_cast<f32>(cells - 1u);
                return { std::floor(minValue / cell) * cell, cell * static_cast<f32>(cells) };
            };

            const SnappedAxis axisX = snapAxis(lo.x, hi.x, texelsXY);
            const SnappedAxis axisY = snapAxis(lo.y, hi.y, texelsXY);
            const SnappedAxis axisZ = snapAxis(lo.z, hi.z, slicesZ);
            fit.SizeX = axisX.Size;
            fit.SizeY = axisY.Size;
            fit.SizeZ = axisZ.Size;
            fit.OriginAbs = axisX.Origin * frame.AxisX + axisY.Origin * frame.AxisY + axisZ.Origin * frame.AxisZ;
            return fit;
        }

        glm::mat4 MakeRelWorldToTex(const CascadeFit& fit, const glm::vec3& renderOrigin)
        {
            if (!fit.IsValid())
            {
                return glm::mat4(1.0f);
            }

            const glm::vec3 originRel = fit.OriginAbs - renderOrigin;
            const glm::vec3 rowX = fit.AxisX / fit.SizeX;
            const glm::vec3 rowY = fit.AxisY / fit.SizeY;
            const glm::vec3 rowZ = fit.AxisZ / fit.SizeZ;

            // Rows of the linear part, so `m * vec4(p, 1)` yields
            // dot(p - originRel, axis) / size per component.
            glm::mat4 m(1.0f);
            m[0] = glm::vec4(rowX.x, rowY.x, rowZ.x, 0.0f);
            m[1] = glm::vec4(rowX.y, rowY.y, rowZ.y, 0.0f);
            m[2] = glm::vec4(rowX.z, rowY.z, rowZ.z, 0.0f);
            m[3] = glm::vec4(-glm::dot(originRel, rowX), -glm::dot(originRel, rowY), -glm::dot(originRel, rowZ), 1.0f);
            return m;
        }

        glm::mat4 MakeTexToAbsWorld(const CascadeFit& fit)
        {
            if (!fit.IsValid())
            {
                return glm::mat4(1.0f);
            }

            glm::mat4 m(1.0f);
            m[0] = glm::vec4(fit.AxisX * fit.SizeX, 0.0f);
            m[1] = glm::vec4(fit.AxisY * fit.SizeY, 0.0f);
            m[2] = glm::vec4(fit.AxisZ * fit.SizeZ, 0.0f);
            m[3] = glm::vec4(fit.OriginAbs, 1.0f);
            return m;
        }

        Bounds FogVolumeWorldBounds(const glm::mat4& worldToLocal, i32 shape, const glm::vec3& extents)
        {
            Bounds invalid;
            // Shape constants mirror FogVolumeShape / FogVolumeCommon.glsl:
            // 0 = box, 1 = sphere (radius = extents.x), 2 = cylinder
            // (radius = extents.x, half-height = extents.y, Y-aligned).
            glm::vec3 localHalf = glm::abs(extents);
            if (shape == 1)
            {
                localHalf = glm::vec3(std::abs(extents.x));
            }
            else if (shape == 2)
            {
                localHalf = glm::vec3(std::abs(extents.x), std::abs(extents.y), std::abs(extents.x));
            }
            else
            {
                // BOX, and every unknown shape id with it. Matching
                // FogVolumeCommon.glsl's evaluateVolumeSDF, whose own chain ends
                // `else // BOX (default)` — a shape the shader treats as a box
                // must be bounded as a box here or the cascade is fitted to
                // something the medium is not.
                localHalf = glm::abs(extents);
            }
            if (localHalf.x <= 0.0f || localHalf.y <= 0.0f || localHalf.z <= 0.0f)
            {
                return invalid;
            }

            const glm::mat4 localToWorld = glm::inverse(worldToLocal);
            constexpr f32 kInf = std::numeric_limits<f32>::max();
            glm::vec3 lo(kInf, kInf, kInf);
            glm::vec3 hi(-kInf, -kInf, -kInf);
            for (u32 corner = 0; corner < 8u; ++corner)
            {
                const glm::vec3 local((corner & 1u) ? localHalf.x : -localHalf.x,
                                      (corner & 2u) ? localHalf.y : -localHalf.y,
                                      (corner & 4u) ? localHalf.z : -localHalf.z);
                const glm::vec4 world = localToWorld * glm::vec4(local, 1.0f);
                if (!std::isfinite(world.x) || !std::isfinite(world.y) || !std::isfinite(world.z))
                {
                    return invalid; // a singular WorldToLocal cannot be bounded
                }
                lo = glm::min(lo, glm::vec3(world));
                hi = glm::max(hi, glm::vec3(world));
            }

            Bounds bounds;
            bounds.Min = lo;
            bounds.Max = hi;
            return bounds.IsValid() ? bounds : invalid;
        }

        Bounds UnionBounds(const Bounds& a, const Bounds& b)
        {
            if (!a.IsValid())
            {
                return b;
            }
            if (!b.IsValid())
            {
                return a;
            }
            Bounds merged;
            merged.Min = glm::min(a.Min, b.Min);
            merged.Max = glm::max(a.Max, b.Max);
            return merged;
        }

        Bounds ClampBoundsToWindow(const Bounds& bounds, const glm::vec3& center, f32 halfExtent)
        {
            if (!bounds.IsValid() || halfExtent <= 0.0f)
            {
                return Bounds{};
            }
            const glm::vec3 windowMin = center - glm::vec3(halfExtent);
            const glm::vec3 windowMax = center + glm::vec3(halfExtent);
            Bounds clipped;
            clipped.Min = glm::max(bounds.Min, windowMin);
            clipped.Max = glm::min(bounds.Max, windowMax);
            return clipped.IsValid() ? clipped : Bounds{};
        }
    } // namespace VolumetricShadowMath

    namespace
    {
        using VolumetricShadowMath::Bounds;
        using VolumetricShadowMath::BuildLightFrame;
        using VolumetricShadowMath::CascadeFit;
        using VolumetricShadowMath::ClampBoundsToWindow;
        using VolumetricShadowMath::FitCascade;
        using VolumetricShadowMath::FogVolumeWorldBounds;
        using VolumetricShadowMath::LightFrame;
        using VolumetricShadowMath::MakeRelWorldToTex;
        using VolumetricShadowMath::MakeTexToAbsWorld;
        using VolumetricShadowMath::UnionBounds;

        // Must match local_size_x/y in VolumetricShadow_Generate.comp
        constexpr u32 kLocalSize = 8;

        // The cloud cascade's domain: the layer slab over a camera-centred
        // window. The window is what bounds the march at a low sun — see
        // FitCascade's comment.
        [[nodiscard]] Bounds BuildCloudDomain(const CloudscapeRenderState& clouds, const glm::vec3& cameraPosAbsolute)
        {
            const f32 bottom = clouds.LayerBottom;
            const f32 top = std::max(clouds.LayerTop, bottom + 1.0f);
            const f32 halfExtent = std::max(clouds.VolumetricShadowExtent, 100.0f) * 0.5f;

            Bounds domain;
            domain.Min = glm::vec3(cameraPosAbsolute.x - halfExtent, bottom, cameraPosAbsolute.z - halfExtent);
            domain.Max = glm::vec3(cameraPosAbsolute.x + halfExtent, top, cameraPosAbsolute.z + halfExtent);
            return domain;
        }

        // The fog cascade's domain: the height-fog slab over a camera-centred
        // window, widened by every enabled fog volume that reaches into a
        // larger window around the camera. Without the volume union, a fog
        // volume placed off to one side would sit outside the footprint and be
        // reported unshadowed — which is exactly the acceptance criterion this
        // cascade exists for.
        [[nodiscard]] Bounds BuildFogDomain(const FogSettings& fog, const FogVolumesUBOData& fogVolumes,
                                            const glm::vec3& cameraPosAbsolute)
        {
            // The setting is a FLOOR, not the final answer: any froxel outside
            // this window reads back "unshadowed", so a window that does not
            // reach the froxel volume produces a visible boundary partway into
            // the fog — a lighting seam, not a missing setting.
            //
            // And the froxel volume is a FRUSTUM, not a sphere of radius
            // `fogFar`. Its far-plane CORNERS sit further from the camera than
            // its far distance does: at the editor's 60-degree vertical FOV and
            // 4:3, a corner is at fogFar * sqrt(1 + tan^2(hfov/2) +
            // tan^2(vfov/2)) ~= 1.33 * fogFar, and at a 16:9 90-degree game FOV
            // ~= 1.55. kFrustumCornerReach covers both with margin; using
            // fogFar alone left the far screen corners of a default-fog frame
            // (End 300) ~46 m outside the window, unshadowed after a ~24 m fade.
            //
            // fogFar mirrors VolumetricFogPass::Execute's derivation, minus its
            // `min(..., cameraFar)`: this runs before the camera's clip planes
            // are extracted, and erring LARGE is the safe direction here (a
            // window wider than the froxels costs resolution, a narrower one
            // costs correctness).
            constexpr f32 kFrustumCornerReach = 1.6f;
            const f32 froxelFar = std::clamp(fog.End, 20.0f, 500.0f) * kFrustumCornerReach;
            const f32 halfExtent = std::max(std::max(fog.VolumetricSelfShadowExtent, 4.0f) * 0.5f, froxelFar);

            // QUANTIZE THE CAMERA before deriving anything from it, or the
            // domain's EXTENT becomes a continuous function of camera position
            // and defeats FitCascade's snap entirely.
            //
            // The snap keeps the light-space ORIGIN on a grid whose cell size is
            // `raw / (cells - 1)` — derived from the extent. That is stable only
            // while the extent is. The XZ extent is fixed (+/- halfExtent) and a
            // box's projected width does not depend on where the box is, so XZ
            // was always fine. The Y extent is NOT: `bottom` follows
            // min(cameraY, HeightOffset), so once the camera drops below
            // HeightOffset every frame gets a slightly different cell size, the
            // floor() lands on a different grid, and samples slide sub-texel —
            // the exact shimmer the z-snap exists to prevent, fed straight into
            // the froxel scatter's 0.9-weight history. Clipping a partially
            // overlapping fog volume against a continuously moving window does
            // the same thing.
            //
            // One texel of the cascade's own XZ footprint is the natural
            // quantum: the window then moves in whole texels, exactly as
            // CloudShadowMap snaps its centre.
            const f32 quantum = std::max(2.0f * halfExtent / static_cast<f32>(VolumetricShadowMap::kResolution),
                                         1.0e-3f);
            const auto quantize = [quantum](f32 v)
            { return std::floor(v / quantum) * quantum; };
            const glm::vec3 cameraSnapped(quantize(cameraPosAbsolute.x), quantize(cameraPosAbsolute.y),
                                          quantize(cameraPosAbsolute.z));

            Bounds domain;
            if (fog.Density > 0.0f)
            {
                // Height fog decays upward as exp(-HeightFalloff * max(h, 0))
                // and is FLAT below HeightOffset, so there is no downward decay
                // to fit — the window bounds that side.
                const f32 scaleHeights = (fog.HeightFalloff > 1.0e-4f) ? (5.0f / fog.HeightFalloff)
                                                                       : (halfExtent * 2.0f);
                const f32 top = quantize(fog.HeightOffset + std::clamp(scaleHeights, 5.0f, halfExtent * 2.0f));
                // A QUARTER of the window below, not a whole one. Fog under the
                // camera has to be in the box — the light path to a low froxel
                // passes through it — but the box's extent along the light is
                // divided by kSlicesPerCascade, so every metre of empty depth
                // below the ground is paid for in march resolution everywhere
                // else. A quarter keeps the slices near the medium.
                const f32 bottom = quantize(std::min(cameraSnapped.y, fog.HeightOffset) - halfExtent * 0.25f);

                domain.Min = glm::vec3(cameraSnapped.x - halfExtent, bottom, cameraSnapped.z - halfExtent);
                domain.Max = glm::vec3(cameraSnapped.x + halfExtent, std::max(top, bottom + quantum),
                                       cameraSnapped.z + halfExtent);
            }

            // A distant volume may widen the cascade, but only so far: past
            // this window it would cost every near-camera texel its resolution.
            const f32 volumeWindowHalf = halfExtent * 4.0f;
            const i32 count = std::min(fogVolumes.VolumeCount.x, static_cast<i32>(FogVolumesUBOData::MAX_FOG_VOLUMES));
            for (i32 i = 0; i < count; ++i)
            {
                const FogVolumeData& volume = fogVolumes.Volumes[static_cast<u32>(i)];
                if (const f32 density = volume.ColorAndDensity.a * volume.ShapeAndFalloff.z; !(density > 0.0f))
                {
                    continue;
                }
                const Bounds volumeBounds =
                    FogVolumeWorldBounds(volume.WorldToLocal, static_cast<i32>(volume.ShapeAndFalloff.x + 0.5f),
                                         glm::vec3(volume.Extents));
                const Bounds clipped = ClampBoundsToWindow(volumeBounds, cameraSnapped, volumeWindowHalf);
                domain = UnionBounds(domain, clipped);
            }

            return domain;
        }

        [[nodiscard]] VolumetricShadowMap::CascadeState MakeCascadeState(const Bounds& domain, const glm::vec3& towardLight,
                                                                         const glm::vec3& renderOrigin, f32 strength)
        {
            VolumetricShadowMap::CascadeState state;
            if (!domain.IsValid() || !(strength > 0.0f))
            {
                return state;
            }

            const LightFrame frame = BuildLightFrame(towardLight);
            const CascadeFit fit = FitCascade(domain, frame, VolumetricShadowMap::kResolution,
                                              VolumetricShadowMap::kSlicesPerCascade);
            if (!fit.IsValid())
            {
                return state;
            }

            state.Enabled = true;
            state.RelWorldToTex = MakeRelWorldToTex(fit, renderOrigin);
            state.TexToAbsWorld = MakeTexToAbsWorld(fit);
            state.StepLength = fit.SizeZ / static_cast<f32>(VolumetricShadowMap::kSlicesPerCascade);
            state.Strength = std::clamp(strength, 0.0f, 1.0f);
            return state;
        }
    } // namespace

    void VolumetricShadowMap::PrepareFrame(const CloudscapeRenderState& clouds, const FogSettings& fog,
                                           const FogVolumesUBOData& fogVolumes,
                                           const glm::vec3& cameraPosAbsolute, const glm::vec3& renderOrigin,
                                           const glm::vec3& fogTowardLight, bool cloudFieldReady)
    {
        OLO_PROFILE_FUNCTION();

        s_Data.m_Cascades = {};

        // A latched creation failure disables both cascades AT THE SOURCE.
        // Without this the enable lane the caller uploads says "available"
        // while Dispatch() returns before it can ever publish the sampler, so
        // both consumers pass their `u_VsmParams[c].x < 0.5` gate and sample an
        // unbound texture unit — which reads (0,0,0,1) and therefore *happens*
        // to mean "no shadowing", but only by accident of what an incomplete GL
        // texture returns. The failure is sticky, so one frame can still slip
        // through (the one during which creation failed); everything after it
        // is honestly off. CloudShadowMap's `IsReady()` gate three lines below
        // the upload is the same idea.
        if (s_Data.m_CreationFailed)
        {
            return;
        }

        if (clouds.Enabled && clouds.VolumetricSelfShadow && cloudFieldReady)
        {
            s_Data.m_Cascades[std::to_underlying(Cascade::Cloud)] =
                MakeCascadeState(BuildCloudDomain(clouds, cameraPosAbsolute), clouds.SunDirection, renderOrigin,
                                 clouds.VolumetricShadowStrength);
        }

        // The fog cascade rides the froxel path: the analytic fog fallback has
        // no per-sample march to attach a transmittance to.
        if (fog.Enabled && fog.EnableVolumetric && fog.EnableVolumetricSelfShadow)
        {
            s_Data.m_Cascades[std::to_underlying(Cascade::Fog)] =
                MakeCascadeState(BuildFogDomain(fog, fogVolumes, cameraPosAbsolute), fogTowardLight, renderOrigin,
                                 fog.VolumetricSelfShadowStrength);
        }
    }

    void VolumetricShadowMap::Dispatch()
    {
        OLO_PROFILE_FUNCTION();

        if (s_Data.m_CreationFailed)
        {
            return; // already logged
        }

        const bool anyCascadeEnabled = AnyCascadeEnabled();
        if (!anyCascadeEnabled && !s_Data.m_Volume)
        {
            // Never run and nothing to run: no allocation, and nothing to keep
            // bound either. Both consumers gate on VsmParams[c].x, which the
            // atmosphere upload just zeroed, so the sampler they declare at
            // TEX_VOLUMETRIC_SHADOW is never read.
            return;
        }

        // The frame that disables the LAST cascade still dispatches, once: the
        // kernel's disabled-cascade path zeroes the slices, and without that the
        // persistent volume keeps the optical depth of whatever frame last
        // marched it. Nothing renders wrong — the consumers gate on
        // VsmParams[c].x — but two things quietly rot. The
        // `VolumetricShadowVolume` graph import, whose whole purpose is to
        // answer "does the volume hold a gradient or is it flat?", would answer
        // from a frame that is no longer on screen; and the kernel's clear is
        // the only guard left for a future consumer that forgets the gate.
        // After the clear there is nothing stale to clear again, so an
        // all-disabled frame costs one dispatch and then nothing.
        const bool needsClearingDispatch = !anyCascadeEnabled && s_Data.m_HoldsStaleData;
        const bool dispatching = anyCascadeEnabled || needsClearingDispatch;

        if (dispatching && (!s_Data.m_Volume || !s_Data.m_GenerateShader))
        {
            if (!s_Data.m_Volume)
            {
                Texture3DSpecification spec;
                spec.Width = kResolution;
                spec.Height = kResolution;
                spec.Depth = kVolumeDepth;
                spec.Format = Texture3DFormat::R32F;
                spec.Repeat = false; // clamp-to-edge: a tap past the last slice reads the column total
                s_Data.m_Volume = Texture3D::Create(spec);
            }
            if (!s_Data.m_GenerateShader)
            {
                s_Data.m_GenerateShader = ComputeShader::Create("assets/shaders/compute/VolumetricShadow_Generate.comp");
            }

            const bool volumeValid = s_Data.m_Volume && s_Data.m_Volume->GetRHIHandle().IsValid();
            const bool shaderValid = s_Data.m_GenerateShader && s_Data.m_GenerateShader->IsValid();
            if (!volumeValid || !shaderValid)
            {
                OLO_CORE_ERROR("VolumetricShadowMap::Dispatch failed — {}",
                               !shaderValid ? "VolumetricShadow_Generate.comp could not be loaded/compiled"
                                            : "R32F shadow volume could not be created");
                s_Data.m_Volume = nullptr;
                s_Data.m_GenerateShader = nullptr;
                s_Data.m_CreationFailed = true;
                return;
            }
        }

        if (dispatching)
        {
            // NOTE: the compute reads the cascade transforms out of the
            // atmosphere UBO (54), the cloud field out of UBO 53 + samplers
            // 59/60/61, and the fog fields out of UBOs 17/20 — the caller
            // uploaded and bound all of them BEFORE this call (see the class
            // comment in VolumetricShadowMap.h).
            s_Data.m_GenerateShader->Bind();

            // Persistent: the volume is owned by this system for the process's
            // life, not by the frame graph, so its descriptor is memoised once
            // rather than re-minted from the transient ring every frame.
            HeapBinding::BindImageOrOffset(0, s_Data.m_Volume->GetRHIHandle(), 0, true, 0,
                                           RHI::Access::StorageWrite, RHI::Format::R32Float,
                                           RHI::HeapSlotLifetime::Persistent);
            HeapBinding::FlushOffsets();

            constexpr u32 kGroups = (kResolution + kLocalSize - 1) / kLocalSize;
            RenderCommand::DispatchCompute(kGroups, kGroups, kCascadeCount);

            // Both consumers sample the volume as a texture; the image stores
            // must land first.
            RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderImageAccess | MemoryBarrierFlags::TextureFetch);
            s_Data.m_GenerateShader->Unbind();

            s_Data.m_Ready = true;
            s_Data.m_HoldsStaleData = anyCascadeEnabled;
        }

        // Published even on a frame where NO cascade ran, as long as the volume
        // exists. Both consumers declare a sampler3D at this slot
        // unconditionally, and a slot left holding whatever the last frame put
        // there is the kind of thing that only misbehaves on one driver
        // (VolumetricFogPass binds shadow-map PLACEHOLDERS for the same
        // reason). The values are stale, which is harmless: the consumers gate
        // on VsmParams[c].x and never sample a disabled cascade.
        //
        // PUBLISHED-GLOBAL (docs/agent-rules/render-pass-published-state.md):
        // the consumers are a fragment pass (the cloud raymarch) and a compute
        // pass (the froxel scatter), neither of which is bound at this call
        // site, so the seam's "is the program in flight bindless" fork has no
        // answer here — publish both the offset and the slot bind.
        HeapBinding::PublishTextureOffsetAndBind(ShaderBindingLayout::TEX_VOLUMETRIC_SHADOW,
                                                 s_Data.m_Volume->GetRHIHandle(),
                                                 RHI::HeapSlotLifetime::Persistent);
        HeapBinding::FlushOffsets();
    }

    void VolumetricShadowMap::Shutdown()
    {
        OLO_PROFILE_FUNCTION();

        const bool hadState = s_Data.m_Volume || s_Data.m_GenerateShader || s_Data.m_CreationFailed;

        if (s_Data.m_Volume)
        {
            // The volume is bound through the tracked slot path, so drop any
            // cached "slot already has this texture" entry before the id is
            // released — a future bind with a recycled id must not be skipped
            // against stale tracking (the CloudShadowMap contract).
            CommandDispatch::InvalidateTextureBinding(s_Data.m_Volume->GetRHIHandle());
        }
        s_Data.m_Volume = nullptr;
        s_Data.m_GenerateShader = nullptr;
        s_Data.m_Cascades = {};
        s_Data.m_Ready = false;
        s_Data.m_CreationFailed = false;
        s_Data.m_HoldsStaleData = false;

        if (hadState)
        {
            OLO_CORE_INFO("VolumetricShadowMap shut down");
        }
    }

    bool VolumetricShadowMap::IsReady()
    {
        return s_Data.m_Ready;
    }

    bool VolumetricShadowMap::HasFailed()
    {
        return s_Data.m_CreationFailed;
    }

    RHI::ResourceHandle VolumetricShadowMap::GetTextureHandle()
    {
        return (s_Data.m_Ready && s_Data.m_Volume) ? s_Data.m_Volume->GetRHIHandle() : RHI::NullResource;
    }

    const VolumetricShadowMap::CascadeState& VolumetricShadowMap::GetCascade(Cascade cascade)
    {
        return s_Data.m_Cascades[std::to_underlying(cascade)];
    }

    bool VolumetricShadowMap::AnyCascadeEnabled()
    {
        return std::ranges::any_of(s_Data.m_Cascades, [](const CascadeState& cascade)
                                   { return cascade.Enabled; });
    }
} // namespace OloEngine
