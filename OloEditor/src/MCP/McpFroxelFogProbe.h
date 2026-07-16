#pragma once

// Pure, engine-light froxel<->world mapping + JSON shaping for the
// olo_froxel_fog_probe MCP tool (issue #607; relates to #435).
//
// The motivating failure: every volumetric-fog contract we have compares FINAL
// FRAME pixels. When the fog looks wrong, that tells you nothing about WHERE it
// went wrong — the scatter compute could be injecting the wrong density, the
// integration could be accumulating it wrongly, or the composite tap in
// PostProcess_Fog.glsl could be sampling the right volume at the wrong froxel.
// Sampling the volume itself at a known world position separates those three
// without an intermediate-buffer PNG round trip: if the raw scatter at the
// froxel under the camera ray is zero, the scatter pass is wrong; if it is
// right but the frame is not, the composite tap is wrong.
//
// EVERYTHING here mirrors the froxel mapping the shaders use. Get that wrong
// and the tool is a confident liar — it would report a neighbouring cell's
// values as if they were the sampled point's. The two shader sources this
// transcribes:
//
//   * assets/shaders/compute/FroxelFogScatter.comp, main():
//       screenUV  = (gid.xy + 0.5) / dims.xy
//       ndc       = screenUV * 2 - 1
//       viewDepth = near * exp2(log2(far/near) * (gid.z + jitter) / dims.z)
//       nearView  = u_FroxelInverseProjection * vec4(ndc, -1, 1)
//       viewRay   = nearView.xyz / nearView.w
//       viewPos   = viewRay * (viewDepth / max(-viewRay.z, 1e-4))
//       relPos    = (u_FroxelInverseView * vec4(viewPos, 1)).xyz
//       absPos    = relPos + u_FroxelRenderOrigin.xyz
//     (the temporal z-jitter is a per-frame dither WITHIN the slice; the probe
//     uses the slice CENTRE, 0.5, which is the jitter's expectation.)
//
//   * assets/shaders/compute/FroxelFogIntegrate.comp, main():
//       sliceFarDepth = near * exp2(log2(far/near) * (z + 1) / dims.z)
//     — i.e. slice z spans view depths [D(z/dimZ), D((z+1)/dimZ)], which is what
//     the reported cell bounds use.
//
// The depth distribution is EXPONENTIAL (not linear), so a linear inverse would
// be silently wrong everywhere except the two end slices — that is exactly the
// class of bug the round-trip unit test in McpFroxelFogProbeTest.cpp pins.
//
// The handler in McpToolsRender.cpp does the GL-bound work on the main thread
// (glGetTextureSubImage of a 1x1x1 region out of the scatter / integrated 3D
// volumes — never the whole volume) and hands the values here. Only
// Core/Base.h, glm and nlohmann::json are pulled in, so this compiles and
// unit-tests headlessly, the same split McpRenderProbePixel.h uses.

#include "OloEngine/Core/Base.h"

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <string>

namespace OloEngine::MCP::FroxelFog
{
    using Json = nlohmann::json;

    // The froxel volume's frame state — the CPU mirror of the FroxelFogData UBO
    // (UBOStructures::FroxelFogUBO) the two compute shaders read. Filled from
    // VolumetricFogPass::GetFroxelVolumeState() on the main thread; every field
    // is the value the shader saw on the frame that last ran the chain.
    struct Volume
    {
        i32 DimX = 0;
        i32 DimY = 0;
        i32 DimZ = 0;
        f32 Near = 0.0f;           // fogNear   (UBO DepthParams.x)
        f32 Far = 0.0f;            // fogFar    (UBO DepthParams.y)
        f32 LogFarOverNear = 0.0f; // log2(far/near) (UBO DepthParams.z)

        // view = render-RELATIVE world -> view (the pass builds it with
        // MakeViewRelative); InverseView is the UBO's u_FroxelInverseView.
        glm::mat4 View{ 1.0f };
        glm::mat4 InverseView{ 1.0f };
        glm::mat4 Projection{ 1.0f };
        glm::mat4 InverseProjection{ 1.0f };
        glm::vec3 RenderOrigin{ 0.0f };
    };

    [[nodiscard]] inline bool IsUsable(const Volume& v)
    {
        return v.DimX > 0 && v.DimY > 0 && v.DimZ > 0 &&
               std::isfinite(v.Near) && std::isfinite(v.Far) &&
               v.Near > 0.0f && v.Far > v.Near && v.LogFarOverNear > 0.0f;
    }

    // View depth (positive metres in front of the camera) at a normalised slice
    // coordinate t in [0,1]: the shaders' exponential distribution
    // near * exp2(log2(far/near) * t), so t=0 -> near and t=1 -> far.
    [[nodiscard]] inline f32 SliceViewDepth(const Volume& v, f32 t)
    {
        return v.Near * std::exp2(v.LogFarOverNear * t);
    }

    // Inverse of SliceViewDepth: the normalised slice coordinate a view depth
    // falls at. Outside [0,1] when the depth is nearer than the volume's near
    // plane or beyond its far plane (the caller reports that honestly rather
    // than clamping silently).
    [[nodiscard]] inline f32 SliceCoordForViewDepth(const Volume& v, f32 viewDepth)
    {
        if (!(viewDepth > 0.0f) || !(v.LogFarOverNear > 0.0f))
            return 0.0f;
        return std::log2(viewDepth / v.Near) / v.LogFarOverNear;
    }

    // View-space ray through a normalised screen position (uv in [0,1]^2), as
    // the shaders build it: unproject the NDC point on the near plane.
    [[nodiscard]] inline glm::vec3 ViewRayThroughUV(const Volume& v, f32 u, f32 w)
    {
        const glm::vec4 ndc(u * 2.0f - 1.0f, w * 2.0f - 1.0f, -1.0f, 1.0f);
        const glm::vec4 nearView = v.InverseProjection * ndc;
        const f32 wComponent = (std::abs(nearView.w) > 1e-9f) ? nearView.w : 1e-9f;
        return glm::vec3(nearView) / wComponent;
    }

    // View position on that ray at `viewDepth` — the shaders' secant scaling
    // (a froxel is a frustum cell, so a cell's far corner is FURTHER from the
    // camera than its axial depth; scaling the ray, not the axis, is what makes
    // the mapping match).
    [[nodiscard]] inline glm::vec3 ViewPosOnRay(const glm::vec3& viewRay, f32 viewDepth)
    {
        return viewRay * (viewDepth / std::max(-viewRay.z, 1e-4f));
    }

    // Absolute-world position of an arbitrary (possibly fractional) froxel
    // coordinate. (x + 0.5, y + 0.5, z + 0.5) is the cell CENTRE — the same
    // point FroxelFogScatter.comp shades (modulo its intra-slice temporal
    // jitter, whose expectation is the centre).
    [[nodiscard]] inline glm::vec3 FroxelToWorld(const Volume& v, f32 fx, f32 fy, f32 fz)
    {
        const f32 u = fx / static_cast<f32>(v.DimX);
        const f32 w = fy / static_cast<f32>(v.DimY);
        const glm::vec3 viewRay = ViewRayThroughUV(v, u, w);
        const f32 viewDepth = SliceViewDepth(v, fz / static_cast<f32>(v.DimZ));
        const glm::vec3 viewPos = ViewPosOnRay(viewRay, viewDepth);
        const glm::vec3 relPos = glm::vec3(v.InverseView * glm::vec4(viewPos, 1.0f));
        return relPos + v.RenderOrigin;
    }

    [[nodiscard]] inline glm::vec3 FroxelCenterWorld(const Volume& v, i32 x, i32 y, i32 z)
    {
        return FroxelToWorld(v, static_cast<f32>(x) + 0.5f, static_cast<f32>(y) + 0.5f,
                             static_cast<f32>(z) + 0.5f);
    }

    // Where an absolute-world position lands in the froxel volume. The exact
    // inverse of FroxelToWorld: back through the render origin, the relative
    // view matrix and the projection, then the exponential slice distribution.
    struct FroxelCoord
    {
        f32 X = 0.0f; // continuous froxel coords (cell centres sit at +0.5)
        f32 Y = 0.0f;
        f32 Z = 0.0f;
        i32 IX = 0; // the containing cell (floored, then clamped into the volume)
        i32 IY = 0;
        i32 IZ = 0;
        f32 ViewDepth = 0.0f; // positive metres in front of the camera
        bool InFrustum = false;
        bool InDepthRange = false;
        bool Clamped = false; // the returned cell was clamped into the volume
    };

    [[nodiscard]] inline FroxelCoord WorldToFroxel(const Volume& v, const glm::vec3& absPos)
    {
        FroxelCoord out;

        const glm::vec3 relPos = absPos - v.RenderOrigin;
        const glm::vec4 viewPos = v.View * glm::vec4(relPos, 1.0f);
        out.ViewDepth = -viewPos.z;

        const glm::vec4 clip = v.Projection * viewPos;
        // Behind (or on) the camera plane: the perspective divide is meaningless,
        // so report the depth failure and leave the XY at the volume centre
        // rather than emitting a NaN froxel.
        if (clip.w > 1e-6f)
        {
            const glm::vec3 ndc = glm::vec3(clip) / clip.w;
            out.X = (ndc.x * 0.5f + 0.5f) * static_cast<f32>(v.DimX);
            out.Y = (ndc.y * 0.5f + 0.5f) * static_cast<f32>(v.DimY);
            out.InFrustum = ndc.x >= -1.0f && ndc.x <= 1.0f && ndc.y >= -1.0f && ndc.y <= 1.0f;
        }
        else
        {
            out.X = static_cast<f32>(v.DimX) * 0.5f;
            out.Y = static_cast<f32>(v.DimY) * 0.5f;
        }

        const f32 sliceCoord = SliceCoordForViewDepth(v, out.ViewDepth);
        out.Z = sliceCoord * static_cast<f32>(v.DimZ);
        out.InDepthRange = out.ViewDepth >= v.Near && out.ViewDepth <= v.Far;

        const auto clampIndex = [&out](f32 value, i32 count)
        {
            const auto raw = static_cast<i32>(std::floor(value));
            const i32 clamped = std::clamp(raw, 0, count - 1);
            if (clamped != raw)
                out.Clamped = true;
            return clamped;
        };
        out.IX = clampIndex(out.X, v.DimX);
        out.IY = clampIndex(out.Y, v.DimY);
        out.IZ = clampIndex(out.Z, v.DimZ);
        return out;
    }

    // World-space extent of one froxel cell: the eight corners of the frustum
    // cell (four screen corners x the slice's near/far view depths), reduced to
    // an axis-aligned box. The slice depths match FroxelFogIntegrate.comp's
    // per-slice march bounds exactly.
    struct CellBounds
    {
        glm::vec3 Min{ 0.0f };
        glm::vec3 Max{ 0.0f };
        f32 NearViewDepth = 0.0f;
        f32 FarViewDepth = 0.0f;
    };

    [[nodiscard]] inline CellBounds FroxelCellBounds(const Volume& v, i32 x, i32 y, i32 z)
    {
        CellBounds bounds;
        bounds.NearViewDepth = SliceViewDepth(v, static_cast<f32>(z) / static_cast<f32>(v.DimZ));
        bounds.FarViewDepth = SliceViewDepth(v, static_cast<f32>(z + 1) / static_cast<f32>(v.DimZ));

        bool first = true;
        for (const i32 dx : { 0, 1 })
        {
            for (const i32 dy : { 0, 1 })
            {
                const glm::vec3 viewRay = ViewRayThroughUV(
                    v, static_cast<f32>(x + dx) / static_cast<f32>(v.DimX),
                    static_cast<f32>(y + dy) / static_cast<f32>(v.DimY));
                for (const f32 depth : { bounds.NearViewDepth, bounds.FarViewDepth })
                {
                    const glm::vec3 viewPos = ViewPosOnRay(viewRay, depth);
                    const glm::vec3 world =
                        glm::vec3(v.InverseView * glm::vec4(viewPos, 1.0f)) + v.RenderOrigin;
                    if (first)
                    {
                        bounds.Min = world;
                        bounds.Max = world;
                        first = false;
                    }
                    else
                    {
                        bounds.Min = glm::min(bounds.Min, world);
                        bounds.Max = glm::max(bounds.Max, world);
                    }
                }
            }
        }
        return bounds;
    }

    // ---- Probe result ------------------------------------------------------

    // One RGBA texel out of a 3D volume. `Available` false means that volume
    // does not exist this frame — reported with a reason, never as a failed call.
    struct VolumeSample
    {
        bool Available = false;
        std::array<f32, 4> Value{ 0.0f, 0.0f, 0.0f, 0.0f };
        std::string Unavailable;
    };

    // What the handler gathered: the volume state, the resolved froxel, and the
    // two volumes' texels. RAW is FroxelFogScatter.comp's output (rgb =
    // per-froxel in-scattered radiance, a = extinction); INTEGRATED is
    // FroxelFogIntegrate.comp's (rgb = in-scatter accumulated from the camera to
    // this slice, a = transmittance at the slice's far edge) — the one the
    // composite taps. Having BOTH is the entire point: raw right + integrated
    // wrong isolates the integration; both right + frame wrong isolates the tap.
    struct ProbeResult
    {
        Volume Vol;
        FroxelCoord Coord;
        bool FromWorldPos = false;
        glm::vec3 RequestedWorldPos{ 0.0f };
        VolumeSample Raw;
        VolumeSample Integrated;
        std::string Note;
    };

    [[nodiscard]] inline Json Vec3Json(const glm::vec3& v)
    {
        return Json::array({ v.x, v.y, v.z });
    }

    [[nodiscard]] inline Json SampleJson(const VolumeSample& s, const char* rgbKey, const char* alphaKey)
    {
        Json j;
        j["available"] = s.Available;
        if (s.Available)
        {
            j[rgbKey] = Json::array({ s.Value[0], s.Value[1], s.Value[2] });
            j[alphaKey] = s.Value[3];
        }
        else if (!s.Unavailable.empty())
        {
            j["reason"] = s.Unavailable;
        }
        return j;
    }

    [[nodiscard]] inline Json ToJson(const ProbeResult& r)
    {
        const CellBounds bounds = FroxelCellBounds(r.Vol, r.Coord.IX, r.Coord.IY, r.Coord.IZ);
        const glm::vec3 center = FroxelCenterWorld(r.Vol, r.Coord.IX, r.Coord.IY, r.Coord.IZ);

        Json froxel;
        froxel["coords"] = Json::array({ r.Coord.IX, r.Coord.IY, r.Coord.IZ });
        froxel["continuous"] = Json::array({ r.Coord.X, r.Coord.Y, r.Coord.Z });
        froxel["centerWorld"] = Vec3Json(center);
        froxel["viewDepth"] = r.Coord.ViewDepth;
        froxel["clamped"] = r.Coord.Clamped;
        froxel["inFrustum"] = r.Coord.InFrustum;
        froxel["inDepthRange"] = r.Coord.InDepthRange;

        Json cell;
        cell["min"] = Vec3Json(bounds.Min);
        cell["max"] = Vec3Json(bounds.Max);
        cell["nearViewDepth"] = bounds.NearViewDepth;
        cell["farViewDepth"] = bounds.FarViewDepth;
        froxel["cellBounds"] = std::move(cell);

        Json volume;
        volume["dims"] = Json::array({ r.Vol.DimX, r.Vol.DimY, r.Vol.DimZ });
        volume["near"] = r.Vol.Near;
        volume["far"] = r.Vol.Far;
        volume["depthDistribution"] = "exponential: viewDepth = near * exp2(log2(far/near) * (z + 0.5) / dimZ)";
        volume["renderOrigin"] = Vec3Json(r.Vol.RenderOrigin);

        Json j;
        j["volume"] = std::move(volume);
        j["froxel"] = std::move(froxel);
        if (r.FromWorldPos)
            j["requestedWorldPos"] = Vec3Json(r.RequestedWorldPos);
        // rgb = in-scattered radiance, a = extinction (per-froxel media property).
        j["scatter"] = SampleJson(r.Raw, "inScatter", "extinction");
        // rgb = accumulated in-scatter to this slice, a = transmittance — what
        // PostProcess_Fog.glsl trilinearly taps for the composite.
        j["integrated"] = SampleJson(r.Integrated, "inScatter", "transmittance");
        if (!r.Note.empty())
            j["note"] = r.Note;
        return j;
    }
} // namespace OloEngine::MCP::FroxelFog
