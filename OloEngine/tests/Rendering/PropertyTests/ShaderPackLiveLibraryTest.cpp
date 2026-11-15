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
