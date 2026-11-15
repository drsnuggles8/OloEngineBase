// OLO_TEST_LAYER: shaderpipe
// =============================================================================
// ShaderPackLiveLibraryTest — the editor's "Build Shader Pack" on a live
// renderer, on either backend (#1693).
//
// ShaderPack::CreateFromLibraries is what the menu item and the
// olo_editor_build_shader_pack MCP tool call, with the renderer's own shader
// libraries. It used to static_cast every library shader to OpenGLShader; in
// an editor started with --rhi vulkan those are VulkanShaders, so it read a
// different object's memory as SPIR-V maps. It now packs an OpenGLShader from
// memory and bakes any other shader from its file on the CPU path.
//
// Runs in both test processes: the OpenGL one packs in-memory SPIR-V, the
// --olo-rhi=vulkan one re-bakes the VulkanShaders. Both must write a pack
// that holds every Renderer2D shader with both SPIR-V blobs for every stage.
// Renderer2D's library is the small one (five shaders), so the Vulkan
// process's CPU bake stays cheap; Renderer3D goes in as an empty library.
//
// ShaderPackServedOnOpenGL pins the other end: a shader served from a pack is
// registered under its file stem, the name the renderers look it up by. A pack
// entry is keyed by its file path, and that path used to become the shader's
// name, so Renderer3D's Get("PBR_MultiLight") missed, inserted a null Ref and
// the OpenGL editor crashed on its first frame whenever a pack was present.
// Packs are served only on OpenGL, so that fixture runs in the OpenGL process
// alone and stays out of vulkan_scene_filter.txt.
// =============================================================================

#include "OloEnginePCH.h"

#include "RendererAttachedTest.h"
#include "TestRenderBackend.h"
#include "TestTempDir.h"

#include "OloEngine/Renderer/Renderer2D.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/ShaderLibrary.h"
#include "OloEngine/Renderer/ShaderPack.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

using namespace OloEngine;        // NOLINT(google-build-using-namespace) — test file, brevity preferred
using namespace OloEngine::Tests; // NOLINT(google-build-using-namespace)

namespace
{
    class ShaderPackLiveLibrary : public RendererAttachedTest
    {
      protected:
        [[nodiscard]] bool RunsInVulkanProcess() const override
        {
            return true;
        }

        // The pack is built from the renderer's shader libraries; no scene.
        void BuildScene() override {}
    };

    // OpenGL process only (the RendererAttachedTest default): a pack entry
    // becomes a GL program, and packs are not served on any other backend.
    class ShaderPackServedOnOpenGL : public RendererAttachedTest
    {
      protected:
        void BuildScene() override {}
    };

    constexpr const char* kNameProbeSource = R"glsl(
#type vertex
#version 450 core
layout(location = 0) in vec3 a_Position;
void main()
{
    gl_Position = vec4(a_Position, 1.0);
}

#type fragment
#version 450 core
layout(location = 0) out vec4 o_Color;
void main()
{
    o_Color = vec4(0.0, 1.0, 0.0, 1.0);
}
)glsl";

    // Bakes a pack holding one shader file and returns that file's path.
    std::string BakeNameProbePack(const std::filesystem::path& packPath)
    {
        const auto shaderPath = TempFile("PackNameProbe.glsl");
        std::ofstream(shaderPath, std::ios::binary | std::ios::trunc) << kNameProbeSource;
        const std::string filepath = shaderPath.generic_string();
        EXPECT_TRUE(ShaderPack::CreateFromFilepaths({ filepath }, packPath));
        return filepath;
    }
} // namespace

TEST_F(ShaderPackLiveLibrary, BuildShaderPackHoldsBothBlobsForEveryRenderer2DShader)
{
    ShaderLibrary& lib2D = Renderer2D::GetShaderLibrary();
    const std::vector<std::string> names = lib2D.GetAllShaderNames();
    ASSERT_FALSE(names.empty()) << "Renderer2D's library is empty: the renderer did not come up";

    ShaderLibrary noShaders;
    const auto packPath = TempFile("Live.osp");
    ASSERT_TRUE(ShaderPack::CreateFromLibraries(lib2D, noShaders, packPath));

    const ShaderPack pack(packPath);
    ASSERT_TRUE(pack.IsLoaded());
    for (const std::string& name : names)
    {
        const Ref<Shader> shader = lib2D.Get(name);
        ASSERT_TRUE(shader && shader->IsReady()) << name;
        const std::string& filepath = shader->GetFilePath();
        ASSERT_TRUE(pack.Contains(filepath)) << filepath << " is missing from a pack built on "
                                             << TestRenderBackend::EvidenceBackendPrefix();

        const auto entry = pack.LoadEntry(filepath);
        ASSERT_NE(entry, nullptr) << filepath;
        ASSERT_FALSE(entry->Stages.IsEmpty()) << filepath;
        for (const ShaderPackStageData& stage : entry->Stages)
        {
            EXPECT_FALSE(stage.VulkanSPIRV.IsEmpty()) << filepath << " stage " << static_cast<int>(stage.Stage);
            EXPECT_FALSE(stage.OpenGLSPIRV.IsEmpty()) << filepath << " stage " << static_cast<int>(stage.Stage);
        }
    }
}

TEST_F(ShaderPackServedOnOpenGL, APackServedShaderIsRegisteredUnderItsFileStem)
{
    const auto packPath = TempFile("NameProbe.osp");
    const std::string filepath = BakeNameProbePack(packPath);

    // The parallel path: what Renderer3D::Init loads its library through.
    ShaderLibrary parallel;
    parallel.LoadShaderPack(packPath);
    ASSERT_TRUE(parallel.HasShaderPack());
    auto batch = parallel.PrepareParallel({ filepath });
    ASSERT_EQ(batch.m_IsPackLoaded.Num(), 1);
    ASSERT_TRUE(batch.m_IsPackLoaded[0]) << "the probe must be served from the pack, or this pins nothing";
    (void)parallel.FinalizeParallel(std::move(batch));
    EXPECT_TRUE(parallel.Exists("PackNameProbe")) << "a pack-served shader must be found by its file stem";
    EXPECT_FALSE(parallel.Exists(filepath)) << "a pack-served shader was registered under its file path";

    // The one-shader path: ShaderLibrary::Load(filepath).
    ShaderLibrary single;
    single.LoadShaderPack(packPath);
    const Ref<Shader> shader = single.Load(filepath);
    ASSERT_TRUE(shader);
    EXPECT_EQ(shader->GetName(), "PackNameProbe");
    EXPECT_TRUE(single.Exists("PackNameProbe"));
}
