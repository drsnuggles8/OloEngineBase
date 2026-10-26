#pragma once

// olo_perf_pipeline_readiness: the JSON shape of a PipelineReadinessSnapshot
// (issue #1345). Pure: no renderer, no server — the handler gathers the
// snapshot on the main thread and this turns it into the tool's output.
//
// The two backends report different objects (PipelineReadiness.h), so the
// output says which model it is and never fills one backend's fields with
// zeros for the other: on OpenGL `pipelines` is null with the reason, and the
// driver's first-draw compile is listed under `unmeasurable`.

#include "OloEngine/Renderer/PipelineReadiness.h"

#include <nlohmann/json.hpp>

namespace OloEngine::MCP::PipelineReadinessReport
{
    [[nodiscard]] inline nlohmann::json ShadersToJson(const ShaderPreparationStats& s)
    {
        return nlohmann::json{ { "sourceCompiles", s.SourceCompiles },
                               { "sourceCompileMs", s.SourceCompileMs },
                               { "spirvCacheHits", s.SpirvCacheHits },
                               { "moduleCreates", s.ModuleCreates },
                               { "moduleCreateMs", s.ModuleCreateMs },
                               { "programLinks", s.ProgramLinks },
                               { "programLinkIssueMs", s.ProgramLinkIssueMs },
                               { "programBinaryHits", s.ProgramBinaryHits },
                               { "programBinaryRejects", s.ProgramBinaryRejects },
                               { "linksForcedAtUse", s.LinksForcedAtUse },
                               { "linksForcedAtUseMs", s.LinksForcedAtUseMs } };
    }

    [[nodiscard]] inline nlohmann::json PipelinesToJson(const PipelineReadinessSnapshot& r)
    {
        const PipelineRequestCounters& c = r.Pipelines;
        return nlohmann::json{
            { "precacheEnabled", r.PrecacheEnabled },
            { "preparedFirstUses", c.PreparedFirstUses },
            { "misses", nlohmann::json{ { "total", c.Misses() },
                                        { "neverRequested", c.NeverRequested },
                                        { "tooLateQueued", c.TooLateQueued },
                                        { "tooLateInFlight", c.TooLateInFlight },
                                        { "retriedAfterFailure", c.RetriedAfterFailure } } },
            { "readyClaimViolations", c.ReadyClaimViolations },
            { "requests", c.Requests },
            { "duplicateRequests", c.DuplicateRequests },
            { "preparedCreations", c.PreparedCreations },
            { "blockingCreations", c.BlockingCreations },
            { "failedCreations", c.FailedCreations },
            { "discardedStale", c.DiscardedStale },
            { "blockingCreateMs", c.BlockingCreateMs },
            { "blockingCreateMaxMs", c.BlockingCreateMaxMs },
            { "preparedCreateMs", c.PreparedCreateMs },
            { "driverCache", nlohmann::json{ { "hits", c.DriverCacheHits },
                                             { "misses", c.DriverCacheMisses },
                                             { "unknown", c.DriverCacheUnknown } } },
            { "queued", r.Queued },
            { "inFlight", r.InFlight },
            { "cachedPipelines", r.CachedPipelines },
            { "manifestEntries", r.ManifestEntries },
            { "jobsPerFrame", r.JobsPerFrame },
        };
    }

    [[nodiscard]] inline nlohmann::json ToJson(const PipelineReadinessSnapshot& r)
    {
        using Json = nlohmann::json;
        Json out{ { "status", "ok" }, { "model", ToString(r.Model) }, { "shaders", ShadersToJson(r.Shaders) } };
        switch (r.Model)
        {
            case PipelineReadinessModel::VulkanPipelineObjects:
            {
                Json pending = Json::array();
                for (const PipelineReadinessPendingShader& shader : r.PendingShaders)
                {
                    pending.push_back(Json{ { "shader", shader.Shader.ToView() },
                                            { "requested", shader.Requested },
                                            { "prepared", shader.Prepared },
                                            { "failed", shader.Failed } });
                }
                out["pipelines"] = PipelinesToJson(r);
                out["readiness"] = Json{ { "configuration", r.ConfigurationTag.ToView() },
                                         { "renderReady", r.RenderReady },
                                         { "trackedShaders", r.TrackedShaders },
                                         { "readyShaders", r.ReadyShaders },
                                         { "unknownShaders", r.UnknownShaders },
                                         { "unsupportedSignatures", r.UnsupportedSignatures },
                                         { "pendingShaders", std::move(pending) } };
                out["unmeasurable"] = Json::array();
                break;
            }
            case PipelineReadinessModel::OpenGLProgramLink:
                out["pipelines"] = nullptr;
                out["note"] = "OpenGL has no pipeline objects: a program is ready when its link completes. "
                              "shaders.programLinks counts links issued, shaders.linksForcedAtUse the uses (a bind, "
                              "a raw program fetch) that had to wait for one (the GL form of a late request). The driver's own recompile on "
                              "a program's first draw has no GL signal and is not measured.";
                out["readiness"] = nullptr;
                out["unmeasurable"] = Json::array({ "firstDrawDriverCompile" });
                break;
            case PipelineReadinessModel::None:
                out["pipelines"] = nullptr;
                out["readiness"] = nullptr;
                out["note"] = "No renderer backend prepares pipelines in this process.";
                out["unmeasurable"] = Json::array();
                break;
        }
        return out;
    }
} // namespace OloEngine::MCP::PipelineReadinessReport
