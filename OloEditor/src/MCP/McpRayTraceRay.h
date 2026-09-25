#pragma once

// Pure request parsing and result shaping for `olo_rt_trace_ray` (issue #607).
//
// WHAT THE TOOL IS FOR. `olo_rt_scene_stats` reports the ray-tracing scene's
// COUNTERS: how many BLASes, how many TLAS instances, what the builder refused.
// Those answer "was the TLAS built". They cannot answer "is the TLAS CORRECT" —
// an instance transform that transposed, a geometry that went in at the wrong
// slot and a stale BLAS all produce perfectly healthy counters. Tracing a ray
// you know the answer to is the only question that separates them.
//
// The shader and its device tenant already existed (#978:
// assets/shaders/compute/RayTracingProbe.comp, pinned by
// RayTracingDevice.DeterministicRaysReportMissClosestHitTransformAndAlpha).
// What this adds is the editor-side half.
//
// THE MISS CASE IS WHY IT IS TRUSTWORTHY, so it is reported as a first-class
// answer with the ray echoed beside it, never as an absent entry. `status`
// keeps "nothing came back yet" (pending), "this device cannot answer"
// (unavailable, with the reason) and "the trace ran" (answered) apart — an
// all-miss answer and an empty one are otherwise the same bytes.
//
// This header deliberately knows nothing about Renderer3D or the GPU probe, so
// every externally visible shape can be pinned in headless unit tests. The
// structs below MIRROR RayTracing::RayTracingProbe's; the handler converts.
//
// CAMERA RAYS, AND WHY THEY ARE KEPT APART FROM WORLD RAYS. A request names
// exactly ONE ray source: 'rays' (a deterministic world-space batch),
// 'viewportPixel' or 'viewportNormalized' (one ray through the editor camera,
// the same shapes and arithmetic olo_terrain_pick takes — McpViewportRay.h).
// A camera ray is NOT deterministic: it moves with the camera pose, so a hit
// that changes between two calls may be the pose, not the renderer. Mixing the
// two forms in one batch would hide which rays those were. So every reply
// names its `raySource`, and a camera reply also carries `replay` — the
// resolved world ray as a ready-to-send 'rays' argument — so a camera trace
// becomes a deterministic worldRay A/B by pasting it back.

#include "MCP/McpSchemaBuilder.h"
#include "MCP/McpViewportRay.h"

#include "OloEngine/Core/Base.h"

#include <cmath>
#include <glm/glm.hpp>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace OloEngine::MCP::RayTraceRay
{
    using Json = nlohmann::json;

    // Mirrors RayTracingProbe::kMaxRays. Duplicated rather than included so this
    // header stays free of the renderer; the handler static_asserts they agree.
    inline constexpr u32 kMaxRays = 64;

    struct Ray
    {
        glm::vec3 Origin{ 0.0f };
        glm::vec3 Direction{ 0.0f, 0.0f, -1.0f };
        f32 TMin = 0.0f;
        f32 TMax = 1000.0f;
    };

    struct Request
    {
        // WorldRay: Rays came from the caller. Otherwise Viewport is the
        // request and Rays holds the ONE ray the editor camera resolved it to
        // (empty until the handler resolves it).
        ViewportRay::RaySource Source = ViewportRay::RaySource::WorldRay;
        ViewportRay::ViewportInput Viewport;
        std::vector<Ray> Rays;
        bool CullBackFaces = false;
        bool TerminateOnFirstHit = false;
        u32 InstanceMask = 0xFFu;
    };

    struct Hit
    {
        bool IsHit = false;
        f32 Distance = 0.0f;
        glm::vec3 Position{ 0.0f };
        glm::vec2 Barycentrics{ 0.0f };
        u32 InstanceSlot = 0;
        u32 PrimitiveIndex = 0;
        u32 MaterialSlot = 0;
        u32 GeometrySlot = 0;
        glm::vec2 UV{ 0.0f };
        glm::vec3 WorldNormal{ 0.0f };
        f32 WindingSign = 1.0f;
    };

    enum class Status : u8
    {
        Unavailable,
        Pending,
        Answered,
    };

    struct Snapshot
    {
        Status State = Status::Unavailable;
        std::string UnavailableReason;
        u32 BatchId = 0;
        Request Input;
        std::vector<Hit> Hits; ///< 1:1 with Input.Rays when State == Answered
        u32 LatencyFrames = 0;
        u32 SlotsInFlight = 0;
    };

    // ---- parsing -----------------------------------------------------------

    using ViewportRay::IsFinite;
    using ViewportRay::IsNumber;
    using ViewportRay::ParseVec3;

    [[nodiscard]] inline std::optional<std::string> ParseFiniteScalar(const Json& value, std::string_view name, f32& out)
    {
        if (!IsNumber(value))
            return "Invalid '" + std::string(name) + "': expected a finite number.";
        const f64 parsed = value.get<f64>();
        if (!std::isfinite(parsed))
            return "Invalid '" + std::string(name) + "': expected a finite number.";
        out = static_cast<f32>(parsed);
        if (!std::isfinite(out))
            return "Invalid '" + std::string(name) + "': value exceeds the supported float range.";
        return std::nullopt;
    }

    [[nodiscard]] inline std::optional<std::string> ParseWorldRays(const Json& rays, Request& parsed)
    {
        if (!rays.is_array() || rays.empty())
            return "Invalid 'rays': expected a non-empty array of {origin, direction} objects.";
        if (rays.size() > static_cast<sizet>(kMaxRays))
        {
            return "Invalid 'rays': " + std::to_string(rays.size()) + " rays exceeds the per-call limit of " +
                   std::to_string(kMaxRays) + ". This is a probe, not a renderer — split the batch.";
        }

        parsed.Rays.reserve(rays.size());
        for (sizet i = 0; i < rays.size(); ++i)
        {
            const std::string prefix = "rays[" + std::to_string(i) + "]";
            const Json& entry = rays[i];
            if (!entry.is_object() || !entry.contains("origin") || !entry.contains("direction"))
                return "Invalid '" + prefix + "': expected an object with 'origin' and 'direction'.";

            Ray ray;
            if (const auto error = ParseVec3(entry["origin"], prefix + ".origin", ray.Origin))
                return error;
            if (const auto error = ParseVec3(entry["direction"], prefix + ".direction", ray.Direction))
                return error;
            const f32 lengthSq = glm::dot(ray.Direction, ray.Direction);
            if (!std::isfinite(lengthSq) || lengthSq < 1e-12f)
                return "Invalid '" + prefix + ".direction': must have finite, non-zero length.";
            // Normalized here so the reported distance is a WORLD distance and
            // the reported position is origin + direction * distance exactly.
            // The reply echoes the normalized direction, not the input, so the
            // arithmetic a caller checks is the arithmetic that ran.
            ray.Direction /= std::sqrt(lengthSq);

            if (entry.contains("tMin") && !entry["tMin"].is_null())
            {
                if (const auto error = ParseFiniteScalar(entry["tMin"], prefix + ".tMin", ray.TMin))
                    return error;
            }
            if (entry.contains("tMax") && !entry["tMax"].is_null())
            {
                if (const auto error = ParseFiniteScalar(entry["tMax"], prefix + ".tMax", ray.TMax))
                    return error;
            }
            // 0 <= tMin <= tMax is a SPEC REQUIREMENT of rayQueryInitializeEXT,
            // not a convention: violating it is undefined behaviour on the
            // device, not a miss. Refused here so the caller gets a reason.
            if (ray.TMin < 0.0f)
                return "Invalid '" + prefix + ".tMin': must be >= 0.";
            if (ray.TMin > ray.TMax)
                return "Invalid '" + prefix + "': requires tMin <= tMax.";
            parsed.Rays.push_back(ray);
        }
        return std::nullopt;
    }

    [[nodiscard]] inline std::optional<std::string> ParseRequest(const Json& args, Request& out)
    {
        // Exactly one source — see the header comment for why a batch never
        // mixes camera rays with world rays.
        if (const auto error = ViewportRay::CheckExactlyOneSource(args, "rays"))
            return *error + " 'rays' is a batch of deterministic world-space rays; the viewport forms trace ONE ray "
                            "through the editor camera.";

        Request parsed;
        if (ViewportRay::Present(args, "rays"))
        {
            parsed.Source = ViewportRay::RaySource::WorldRay;
            if (const auto error = ParseWorldRays(args["rays"], parsed))
                return error;
        }
        else
        {
            if (const auto error = ViewportRay::ParseViewportInput(args, parsed.Viewport))
                return error;
            parsed.Source = parsed.Viewport.Source;
        }

        if (args.contains("cullBackFaces") && !args["cullBackFaces"].is_null())
        {
            if (!args["cullBackFaces"].is_boolean())
                return "Invalid 'cullBackFaces': expected a boolean.";
            parsed.CullBackFaces = args["cullBackFaces"].get<bool>();
        }
        if (args.contains("terminateOnFirstHit") && !args["terminateOnFirstHit"].is_null())
        {
            if (!args["terminateOnFirstHit"].is_boolean())
                return "Invalid 'terminateOnFirstHit': expected a boolean.";
            parsed.TerminateOnFirstHit = args["terminateOnFirstHit"].get<bool>();
        }
        if (args.contains("instanceMask") && !args["instanceMask"].is_null())
        {
            const Json& mask = args["instanceMask"];
            if (!(mask.is_number_integer() || mask.is_number_unsigned()))
                return "Invalid 'instanceMask': expected an integer in [1, 255].";
            const i64 value = mask.get<i64>();
            if (value < 1 || value > 255)
            {
                // Zero is refused rather than honoured: it makes every instance
                // unhittable, so every ray would report a miss for a reason
                // that has nothing to do with the scene — the least debuggable
                // possible answer.
                return "Invalid 'instanceMask': expected an integer in [1, 255] (0 would make every instance "
                       "unhittable, so every ray would miss for a reason unrelated to the scene).";
            }
            parsed.InstanceMask = static_cast<u32>(value);
        }

        out = std::move(parsed);
        return std::nullopt;
    }

    // ---- shaping -----------------------------------------------------------

    using ViewportRay::Vec2Json;
    using ViewportRay::Vec3Json;

    [[nodiscard]] inline Json RayJson(const Ray& ray)
    {
        return Json{
            { "origin", Vec3Json(ray.Origin) },
            { "direction", Vec3Json(ray.Direction) },
            { "tMin", ray.TMin },
            { "tMax", ray.TMax },
        };
    }

    [[nodiscard]] inline Json FlagsJson(const Request& request)
    {
        return Json{
            { "cullBackFaces", request.CullBackFaces },
            { "terminateOnFirstHit", request.TerminateOnFirstHit },
            { "instanceMask", request.InstanceMask },
        };
    }

    [[nodiscard]] inline const char* StatusToken(Status state)
    {
        switch (state)
        {
            case Status::Unavailable:
                return "unavailable";
            case Status::Pending:
                return "pending";
            case Status::Answered:
                return "answered";
        }
        return "unknown";
    }

    // The args that re-trace exactly this batch as deterministic world rays:
    // paste it back and the camera pose no longer takes part.
    [[nodiscard]] inline Json ReplayJson(const Request& request)
    {
        Json rays = Json::array();
        for (const Ray& ray : request.Rays)
            rays.push_back(RayJson(ray));
        Json replay{ { "rays", std::move(rays) } };
        const Json flags = FlagsJson(request);
        for (const auto& [key, value] : flags.items())
            replay[key] = value;
        return replay;
    }

    [[nodiscard]] inline Json BuildResult(const Snapshot& snapshot)
    {
        Json result{
            { "status", StatusToken(snapshot.State) },
            { "raySource", ViewportRay::SourceToken(snapshot.Input.Source) },
            { "batchId", snapshot.BatchId },
            { "rayCount", static_cast<u32>(snapshot.Input.Rays.size()) },
            { "flags", FlagsJson(snapshot.Input) },
            { "slotsInFlight", snapshot.SlotsInFlight },
        };
        // A camera ray is echoed as the viewport input AND as the world ray it
        // resolved to, in every status that has one: an unavailable device still
        // tells the caller which ray it would have traced.
        if (snapshot.Input.Source != ViewportRay::RaySource::WorldRay)
        {
            result["viewport"] = ViewportRay::ViewportInputJson(snapshot.Input.Viewport);
            if (!snapshot.Input.Rays.empty())
                result["replay"] = ReplayJson(snapshot.Input);
        }

        if (snapshot.State == Status::Unavailable)
        {
            result["reason"] = snapshot.UnavailableReason.empty()
                                   ? "Ray tracing is unavailable in this editor session."
                                   : snapshot.UnavailableReason;
            return result;
        }
        if (snapshot.State == Status::Pending)
        {
            result["note"] = "The trace was queued but its answer has not come back yet. The dispatch is "
                             "recorded inside a frame and read back through a fence, so it needs the editor "
                             "to render — check olo_perf_snapshot's liveness block if this persists.";
            return result;
        }

        result["latencyFrames"] = snapshot.LatencyFrames;

        u32 hitCount = 0;
        Json rays = Json::array();
        for (sizet i = 0; i < snapshot.Input.Rays.size(); ++i)
        {
            Json entry{ { "index", static_cast<u32>(i) }, { "ray", RayJson(snapshot.Input.Rays[i]) } };
            // A miss is an ANSWER, with its ray beside it. An entry that was
            // simply absent would be indistinguishable from a ray that was
            // never traced.
            if (i >= snapshot.Hits.size() || !snapshot.Hits[i].IsHit)
            {
                entry["hit"] = false;
                rays.push_back(std::move(entry));
                continue;
            }
            ++hitCount;
            const Hit& hit = snapshot.Hits[i];
            entry["hit"] = true;
            entry["distance"] = hit.Distance;
            entry["position"] = Vec3Json(hit.Position);
            // All THREE barycentrics. The GPU reports (b1, b2) and b0 is
            // 1 - b1 - b2; making the caller do that subtraction is how a
            // vertex order gets misread.
            entry["barycentrics"] =
                Json::array({ 1.0f - hit.Barycentrics.x - hit.Barycentrics.y, hit.Barycentrics.x, hit.Barycentrics.y });
            entry["instanceSlot"] = hit.InstanceSlot;
            entry["primitiveIndex"] = hit.PrimitiveIndex;
            entry["materialSlot"] = hit.MaterialSlot;
            entry["geometrySlot"] = hit.GeometrySlot;
            entry["uv"] = Vec2Json(hit.UV);
            entry["worldNormal"] = Vec3Json(hit.WorldNormal);
            entry["windingSign"] = hit.WindingSign;
            rays.push_back(std::move(entry));
        }
        result["rays"] = std::move(rays);
        result["hitCount"] = hitCount;
        result["missCount"] = static_cast<u32>(snapshot.Input.Rays.size()) - hitCount;
        return result;
    }

    // ---- schemas -----------------------------------------------------------

    [[nodiscard]] inline Json InputSchema()
    {
        return Schema::Object()
            .Prop("viewportPixel", ViewportRay::ViewportPixelSchema().Desc("ONE ray through the editor camera at this viewport pixel (top-left origin). Exclusive with 'rays' and 'viewportNormalized'. Not deterministic — it moves with the camera — so the reply's 'replay' carries the resolved world ray to re-trace it deterministically."))
            .Prop("viewportNormalized", ViewportRay::ViewportNormalizedSchema())
            .Prop("rays", Schema::Array(Schema::Object()
                                            .Prop("origin", Schema::Vec3("World-space ray origin [x,y,z]."))
                                            .Prop("direction", Schema::Vec3("World-space direction [x,y,z]; finite "
                                                                            "and non-zero. Normalized by the tool, "
                                                                            "and the reply echoes the normalized "
                                                                            "vector."))
                                            .Prop("tMin", Schema::Number().Desc("Near bound along the ray (default 0). Must be >= 0 and <= tMax — the spec requires it, and violating it is undefined behaviour on the device rather than a miss. Use a small epsilon (e.g. 0.001) when the origin sits on a surface."))
                                            .Prop("tMax", Schema::Number().Desc("Far bound along the ray (default 1000)."))
                                            .Required({ "origin", "direction" })
                                            .NoAdditional())
                              .MinItems(1)
                              .MaxItems(static_cast<int>(kMaxRays))
                              .Desc("Deterministic world-space rays to trace, 1 to 64 per call. Exclusive with the viewport forms."))
            .Prop("cullBackFaces", Schema::Bool().Desc("Skip back-facing triangles (default false)."))
            .Prop("terminateOnFirstHit", Schema::Bool().Desc("Stop at the first hit instead of the closest one (default false) — a VISIBILITY ray. The reported distance is then any hit along the ray, not the nearest."))
            .Prop("instanceMask", Schema::Int().Min(1).Max(255).Desc("ANDed with each instance's own mask (default 255 = everything). 0 is refused: it would make every instance unhittable, so every ray would miss for a reason unrelated to the scene."))
            .NoAdditional();
    }

    [[nodiscard]] inline Json OutputSchema()
    {
        return Schema::Object()
            .Prop("status", Schema::String().Enum({ "unavailable", "pending", "answered" }).Desc("'unavailable' means this device/backend cannot trace (see 'reason'); 'pending' means the answer has not come back yet; only 'answered' carries hits."))
            .Prop("reason", Schema::String().Desc("Present only when status is 'unavailable' — why."))
            .Prop("note", Schema::String().Desc("Present only when status is 'pending'."))
            .Prop("raySource", Schema::String().Enum({ "worldRay", "viewportPixel", "viewportNormalized" }).Desc("Which source the rays came from. Anything but 'worldRay' is a camera ray: it depends on the camera pose at the time of the call."))
            .Prop("viewport", Schema::Object().Desc("Camera sources only: the viewport input as given, plus the normalized top-left coordinate it resolved to."))
            .Prop("replay", Schema::Object().Desc("Camera sources only: the resolved world ray as a ready-to-send argument object ({rays:[...], flags}). Send it back to re-trace the same ray with the camera out of the loop."))
            .Prop("batchId", Schema::Int().Min(0).Desc("Identifies this batch. An 'answered' reply always carries the batchId of the rays you submitted, never an older batch's."))
            .Prop("rayCount", Schema::Int().Min(0))
            .Prop("hitCount", Schema::Int().Min(0))
            .Prop("missCount", Schema::Int().Min(0))
            .Prop("latencyFrames", Schema::Int().Min(0).Desc("Frames between the dispatch and the readback."))
            .Prop("slotsInFlight", Schema::Int().Min(0).Desc("Readback ring slots still executing. Persistently full means the CPU is far ahead of the GPU."))
            .Prop("flags", Schema::Object()
                               .Prop("cullBackFaces", Schema::Bool())
                               .Prop("terminateOnFirstHit", Schema::Bool())
                               .Prop("instanceMask", Schema::Int().Min(0))
                               .Required({ "cullBackFaces", "terminateOnFirstHit", "instanceMask" }))
            .Prop("rays", Schema::Array(Schema::Object()
                                            .Prop("index", Schema::Int().Min(0))
                                            .Prop("ray", Schema::Object().Desc("The ray as traced: origin, NORMALIZED direction, tMin, tMax."))
                                            .Prop("hit", Schema::Bool().Desc("A miss is reported as an entry with hit:false, never as an absent one."))
                                            .Prop("distance", Schema::Number().Desc("t along the ray."))
                                            .Prop("position", Schema::Array(Schema::Number()).Desc("origin + direction * distance, computed CPU-side from the ray as traced."))
                                            .Prop("barycentrics", Schema::Array(Schema::Number()).Desc("All three: [b0, b1, b2]."))
                                            .Prop("instanceSlot", Schema::Int().Min(0).Desc("The GPU Scene instance slot (== instanceCustomIndex). Cross-check it against olo_rt_scene_stats and the scene."))
                                            .Prop("primitiveIndex", Schema::Int().Min(0))
                                            .Prop("materialSlot", Schema::Int().Min(0))
                                            .Prop("geometrySlot", Schema::Int().Min(0))
                                            .Prop("uv", Schema::Array(Schema::Number()).Desc("Interpolated UV at the hit."))
                                            .Prop("worldNormal", Schema::Array(Schema::Number()).Desc("World-space shading normal through the shared inverse-transpose (#1326)."))
                                            .Prop("windingSign", Schema::Number().Desc("+1 normally, -1 when the instance transform mirrors the basis."))
                                            .Required({ "index", "ray", "hit" })))
            .Required({ "status", "raySource", "batchId", "rayCount", "flags" });
    }
} // namespace OloEngine::MCP::RayTraceRay
