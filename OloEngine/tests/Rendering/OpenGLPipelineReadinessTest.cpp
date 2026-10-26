// OLO_TEST_LAYER: shaderpipe
//
// #1345 — what "ready" means on OpenGL, measured on a real GL 4.6 context.
//
// GL has no pipeline object: a program is ready when its link has completed.
// These pin that the counters OpenGL reports through RendererAPI's readiness
// snapshot are the ones that exist there, and that each step is counted apart:
// the source compiles, the link, a bind that had to force a link that had not
// finished (the GL form of a request that came too late), and, for a second
// identical program, either a program-binary restore or a fresh link — never
// neither. The driver's own recompile on a program's first draw has no GL
// signal and is not asserted on (the tool reports it as unmeasurable; see
// McpPipelineReadinessTest).

#include "OloEnginePCH.h"

#include <gtest/gtest.h>

#include "../TestTempDir.h"
#include "OloEngine/Renderer/PipelineReadiness.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/RendererAPI.h"
#include "OloEngine/Renderer/Shader.h"
#include "Platform/OpenGL/OpenGLContext.h"
#include "PropertyTests/RenderPropertyTest.h"

#include <chrono>
#include <format>
#include <fstream>

using namespace OloEngine; // NOLINT(google-build-using-namespace) — test file, brevity preferred

namespace
{
    // A program whose source no cache has seen: the token makes its content
    // hash (and therefore every SPIR-V / program-binary cache key) new.
    std::filesystem::path WriteUniqueShader()
    {
        const auto token = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto path = OloEngine::Tests::TempFile("ReadinessProbe.glsl");
        std::ofstream source(path, std::ios::trunc);
        // Both stages carry the token, so neither is a cache hit from an earlier run.
        source << "#type vertex\n#version 460 core\n"
               << std::format("const float kUniqueV = float({}u % 991u) / 991.0;\n", token % 1000000u)
               << "void main() { gl_Position = vec4(0.0, 0.0, kUniqueV * 0.0, 1.0); }\n"
                  "#type fragment\n#version 460 core\n"
                  "layout(location = 0) out vec4 o_Color;\n"
               << std::format("const float kUnique = float({}u % 997u) / 997.0;\n", token % 1000000u)
               << "void main() { o_Color = vec4(kUnique, 0.0, 0.0, 1.0); }\n";
        return path;
    }
} // namespace

TEST(OpenGLPipelineReadiness, CompileLinkAndForcedLinkAreCountedApart)
{
    OLO_ENSURE_GPU_OR_SKIP();
    ASSERT_EQ(RendererAPI::GetAPI(), RendererAPI::API::OpenGL);
    const auto path = WriteUniqueShader();

    const ShaderPreparationStats before = ShaderPreparationCounters::Get().Snapshot();
    Ref<Shader> shader = Shader::Create(path.generic_string());
    ASSERT_TRUE(shader);
    const ShaderPreparationStats created = ShaderPreparationCounters::Get().Snapshot();
    EXPECT_GE(created.SourceCompiles - before.SourceCompiles, 2u) << "Both stages of new source are compiled";
    EXPECT_EQ(created.ProgramLinks - before.ProgramLinks, 1u) << "New source cannot come from a program binary";
    EXPECT_EQ(created.ProgramBinaryHits, before.ProgramBinaryHits);

    // The first bind finishes a link the driver is still running in the
    // background; without parallel shader compile the link was synchronous
    // and nothing is left to force.
    shader->Bind();
    const ShaderPreparationStats bound = ShaderPreparationCounters::Get().Snapshot();
    EXPECT_EQ(bound.LinksForcedAtUse - created.LinksForcedAtUse, OpenGLContext::HasParallelShaderCompile() ? 1u : 0u);
    EXPECT_TRUE(shader->IsReady());
    shader->Unbind();
    shader.Reset();

    // The same source again: every stage is either compiled or served from the
    // SPIR-V cache, and the program is either restored from its binary or
    // linked — the readiness accounting never loses a step.
    Ref<Shader> again = Shader::Create(path.generic_string());
    ASSERT_TRUE(again);
    const ShaderPreparationStats second = ShaderPreparationCounters::Get().Snapshot();
    EXPECT_GE((second.SourceCompiles - bound.SourceCompiles) + (second.SpirvCacheHits - bound.SpirvCacheHits), 2u);
    EXPECT_EQ((second.ProgramLinks - bound.ProgramLinks) + (second.ProgramBinaryHits - bound.ProgramBinaryHits), 1u);
    again->Bind();
    again->Unbind();
    again.Reset();
}

TEST(OpenGLPipelineReadiness, TheSnapshotReportsTheProgramLinkModelWithNoPipelineCounters)
{
    OLO_ENSURE_GPU_OR_SKIP();
    ASSERT_EQ(RendererAPI::GetAPI(), RendererAPI::API::OpenGL);
    const PipelineReadinessSnapshot snapshot = RenderCommand::GetPipelineReadiness();
    EXPECT_EQ(snapshot.Model, PipelineReadinessModel::OpenGLProgramLink);
    EXPECT_FALSE(snapshot.PrecacheEnabled) << "Nothing is precached on GL; there is no pipeline object to prepare";
    EXPECT_EQ(snapshot.Pipelines.Misses(), 0u);
    EXPECT_EQ(snapshot.Pipelines.PreparedFirstUses, 0u);
    EXPECT_GT(snapshot.Shaders.ProgramLinks + snapshot.Shaders.ProgramBinaryHits, 0u)
        << "The GL counters themselves are live in this process";
}
