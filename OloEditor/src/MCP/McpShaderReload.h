#pragma once

// Pure, engine-light result shaping for the olo_shader_reload MCP tool (issue
// #316 — the rendering inner-loop harness). The tool reloads/recompiles a
// single shader from disk by name so an agent can run the tight shader loop —
// edit .glsl -> reload -> read the compile/link log -> screenshot — without
// restarting the editor.
//
// The handler in McpTools.cpp does the editor/GL-bound work on the main thread
// (look the shader up in the Renderer3D / Renderer2D ShaderLibrary, call
// Shader::Reload(), read back the post-reload status + compile log from the
// ShaderDebugger), then hands the gathered facts here to be turned into the tool
// JSON. Keeping the status-token mapping and the JSON schema in a free function
// with NO renderer / editor / GPU dependencies means this contract is unit-tested
// directly (the MCP test binary compiles the dispatch core but deliberately NOT
// McpTools.cpp), the same split McpRenderExplain.h / McpPhysicsExplain.h use.
//
// Only the shader compilation-status enum (Renderer/Shader.h) and nlohmann::json
// are pulled in; everything else is the standard library.

#include "OloEngine/Renderer/Shader.h" // ShaderCompilationStatus

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace OloEngine::MCP::ShaderReload
{
    using Json = nlohmann::json;

    // Lowercase status token reported by the tool. The authoritative status comes
    // from Shader::GetCompilationStatus() (build-independent — it does not depend
    // on the debug-only ShaderDebugger), so this mapping is the tool's status
    // contract. After a synchronous Reload() the status is normally "ready" or
    // "failed"; "compiling"/"pending" only appear if an async link has not been
    // force-completed.
    [[nodiscard]] inline const char* StatusToken(ShaderCompilationStatus status) noexcept
    {
        switch (status)
        {
            case ShaderCompilationStatus::Pending:
                return "pending";
            case ShaderCompilationStatus::Compiling:
                return "compiling";
            case ShaderCompilationStatus::Ready:
                return "ready";
            case ShaderCompilationStatus::Failed:
                return "failed";
        }
        return "unknown";
    }

    // Owner label reported under "libraries" for a shader that lives in no
    // ShaderLibrary at all — a render-pass member resolved through the engine's
    // process-wide ShaderRegistry (issue #607). Kept as a named constant so the
    // handler and the tests cannot drift.
    inline constexpr const char* kPassOwnedLabel = "PassOwned";

    // What kind of GL program was reloaded. Compute shaders have no
    // ShaderCompilationStatus of their own (ComputeShader exposes IsValid()), so
    // the handler maps valid -> Ready / invalid -> Failed; "kind" tells the agent
    // which contract it is reading.
    enum class ShaderKind : u8
    {
        Graphics,
        Compute
    };

    [[nodiscard]] inline const char* KindToken(ShaderKind kind) noexcept
    {
        return kind == ShaderKind::Compute ? "compute" : "graphics";
    }

    // Pipeline objects built from the reloaded copies, summed (issue #607). On a
    // backend with a PSO cache (Vulkan) a module rebuild is not yet a visible
    // change: every pipeline built from the old modules is invalidated and
    // rebuilt lazily by the next draw that uses the shader. Before this the
    // tool answered `ready` at the module rebuild, which said nothing about
    // whether any pipeline had picked the change up.
    struct Pipelines
    {
        bool Tracked = false; // a backend PSO cache answered (false on OpenGL)
        u32 Invalidated = 0;  // pipelines the reload invalidated
        u32 Live = 0;         // pipelines built since, from the new modules
        bool CreationFailed = false;
        std::string CreationFailure;
        u32 SettleFrames = 0;      // frames waited for a draw to rebuild them
        bool FrameRendered = true; // false when no frame rendered in that wait
    };

    // The status contract on a PSO backend, layered over the module status:
    //   * a failed module rebuild, or a failed pipeline creation, is `failed`;
    //   * pipelines rebuilt since the reload is `ready`;
    //   * nothing to rebuild (no pipeline existed yet) is `ready`: the first
    //     draw builds one from the new modules, so nothing stale can be used;
    //   * invalidated but not rebuilt is `pending`, with the reason. That is a
    //     shader no draw used in the settle window, typically one that belongs
    //     to the other render path.
    struct Resolution
    {
        ShaderCompilationStatus Status = ShaderCompilationStatus::Ready;
        std::string Note;
    };

    [[nodiscard]] inline Resolution ResolveStatus(ShaderCompilationStatus moduleStatus, const Pipelines& pipelines)
    {
        Resolution r;
        r.Status = moduleStatus;
        if (moduleStatus != ShaderCompilationStatus::Ready || !pipelines.Tracked)
            return r;
        if (pipelines.CreationFailed)
        {
            r.Status = ShaderCompilationStatus::Failed;
            r.Note = "The shader modules rebuilt, but creating a pipeline from them failed: " + pipelines.CreationFailure +
                     ". Draws using this shader are skipped until it is fixed.";
            return r;
        }
        if (pipelines.Live > 0)
        {
            r.Note = std::to_string(pipelines.Live) + " pipeline(s) rebuilt from the new modules (" +
                     std::to_string(pipelines.Invalidated) + " invalidated).";
            return r;
        }
        if (pipelines.Invalidated == 0)
        {
            r.Note = "No pipeline had been built from this shader yet, so none was stale; the first draw that uses it "
                     "builds one from the new modules.";
            return r;
        }
        r.Status = ShaderCompilationStatus::Pending;
        if (pipelines.SettleFrames == 0)
        {
            r.Note = std::to_string(pipelines.Invalidated) +
                     " pipeline(s) invalidated, but this host cannot render frames on request, so whether a draw "
                     "rebuilt them was not observed.";
        }
        else if (!pipelines.FrameRendered)
        {
            r.Note = std::to_string(pipelines.Invalidated) +
                     " pipeline(s) invalidated, but no frame rendered while waiting, so none has been rebuilt. The "
                     "editor may be minimised or stalled; render a frame and reload again to confirm.";
        }
        else
        {
            r.Note = std::to_string(pipelines.Invalidated) + " pipeline(s) invalidated and none rebuilt within " +
                     std::to_string(pipelines.SettleFrames) +
                     " frame(s): no draw used this shader. It may belong to another render path; switch to it and "
                     "reload again.";
        }
        return r;
    }

    // Facts gathered by the handler on the main thread after the reload.
    struct Result
    {
        std::string Name;                                                // the requested shader name
        bool Found = false;                                              // the name resolved in a library or the ShaderRegistry
        std::vector<std::string> Libraries;                              // owners that held it: "Renderer3D" / "Renderer2D" / "PassOwned"
        ShaderKind Kind = ShaderKind::Graphics;                          // graphics vs compute program
        ShaderCompilationStatus Status = ShaderCompilationStatus::Ready; // post-reload status of the primary copy
        bool Ok = false;                                                 // every reloaded copy is Ready
        u32 RendererId = 0;                                              // current GL program id of the primary copy (0 if a link failed)
        std::string Log;                                                 // compile/link error log (empty on a clean reload; best-effort, debug builds)
        Pipelines PipelineState;                                         // PSO-backend pipeline facts; Tracked = false on OpenGL
        std::string Note;                                                // why the status is what it is, when the pipelines decided it
    };

    // Shape the tool's JSON response. Mirrors the { name, status, log } contract
    // from the issue, with the extra found/libraries/kind/ok/rendererId fields so
    // the agent can tell "not found" from "found but failed" and knows who owned
    // the shader.
    [[nodiscard]] inline Json ToJson(const Result& r)
    {
        Json j;
        j["name"] = r.Name;
        j["found"] = r.Found;
        j["libraries"] = r.Libraries;
        j["kind"] = KindToken(r.Kind);
        j["status"] = StatusToken(r.Status);
        j["ok"] = r.Ok;
        j["rendererId"] = r.RendererId;
        j["log"] = r.Log;
        if (r.PipelineState.Tracked)
        {
            j["pipelines"] = Json{ { "invalidated", r.PipelineState.Invalidated },
                                   { "rebuilt", r.PipelineState.Live },
                                   { "creationFailed", r.PipelineState.CreationFailed },
                                   { "settleFrames", r.PipelineState.SettleFrames },
                                   { "frameRendered", r.PipelineState.FrameRendered } };
        }
        if (!r.Note.empty())
            j["note"] = r.Note;
        return j;
    }
} // namespace OloEngine::MCP::ShaderReload
