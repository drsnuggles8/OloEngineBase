// OLO_TEST_LAYER: shaderpipe
#include "OloEnginePCH.h"

#include "RenderPropertyTest.h"
#include "Rendering/ShaderHarness.h"

#include "OloEngine/Core/DebugLevers.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/ShaderSourceScan.h"
#include "Platform/OpenGL/OpenGLShader.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

// =============================================================================
// OpenGLShaderRouteTest -- the route an OpenGL program takes to the driver
// (#1533, docs/agent-rules/gl-shader-route.md).
//
// A shader that names OLO_GL_GLSL_ROUTE outside comments reaches GL as
// SPIRV-Cross GLSL text through glShaderSource; every other one as
// glShaderBinary SPIR-V. These pin:
//   * every production shader that opts in links on the text route -- the
//     tessellated terrain among them, whose third and fourth stage ids used to
//     land past the route's two-slot array on the stack;
//   * a shader that does not opt in stays on SPIR-V;
//   * OLO_GL_SHADERS_FROM_GLSL forces the route both ways, which is the A/B.
// =============================================================================

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;

        // Restores the lever whatever the test does to it.
        struct LeverBack
        {
            ~LeverBack()
            {
                Levers::SetGLShadersFromGlsl(Levers::Tristate::Unset);
            }
        };

        // The production shaders that name the token outside comments, by the
        // path Shader::Create takes. The include closure counts, as it does for
        // the engine, which scans the include-resolved stages: since #1569 the
        // terrain depth program names the token in
        // include/TerrainDepthVertexStage.glsl, not in its own file.
        [[nodiscard]] std::vector<std::string> OptedInShaders()
        {
            std::vector<std::string> paths;
            const fs::path root = fs::path{ OLO_TEST_EDITOR_ROOT } / "assets" / "shaders";
            for (const fs::directory_entry& entry : fs::directory_iterator(root))
            {
                if (!entry.is_regular_file() || entry.path().extension() != ".glsl")
                {
                    continue;
                }
                std::vector<fs::path> files = ShaderHarness::IncludeClosure(root, entry.path());
                files.push_back(entry.path());
                if (std::ranges::any_of(files,
                                        [](const fs::path& file)
                                        {
                                            return ShaderSourceScan::MentionsOutsideComments(
                                                ShaderHarness::ReadWholeFile(file), "OLO_GL_GLSL_ROUTE");
                                        }))
                {
                    paths.push_back("assets/shaders/" + entry.path().filename().string());
                }
            }
            std::ranges::sort(paths);
            return paths;
        }

        [[nodiscard]] const OpenGLShader* AsGL(const Ref<Shader>& shader)
        {
            return shader ? dynamic_cast<const OpenGLShader*>(shader.Raw()) : nullptr;
        }
    } // namespace

    TEST(OpenGLShaderRoute, EveryShaderThatOptsInLinksOnTheGlslTextRoute)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        LeverBack back;
        Levers::SetGLShadersFromGlsl(Levers::Tristate::Unset);

        const std::vector<std::string> opted = OptedInShaders();
        // The families the route was measured on, so a lost token is caught.
        for (const char* expected : { "assets/shaders/Foliage_Instance.glsl", "assets/shaders/Foliage_Impostor.glsl",
                                      "assets/shaders/Terrain_PBR.glsl", "assets/shaders/Terrain_Depth.glsl" })
        {
            EXPECT_TRUE(std::ranges::find(opted, std::string(expected)) != opted.end())
                << expected << " no longer names OLO_GL_GLSL_ROUTE";
        }
        // A colour program and the prepass it depth-tests against take ONE
        // route (rule 5): `invariant gl_Position` holds only within one
        // compiler, and with the foliage twins on different routes the forward
        // colour pass lost leaf fragments to the prepass's depth.
        const std::array<std::pair<const char*, const char*>, 3> twins{ {
            { "assets/shaders/Foliage_Instance.glsl", "assets/shaders/Foliage_Instance_DepthNormal.glsl" },
            { "assets/shaders/Foliage_Impostor.glsl", "assets/shaders/Foliage_Impostor_DepthNormal.glsl" },
            { "assets/shaders/Terrain_PBR.glsl", "assets/shaders/Terrain_Depth.glsl" },
        } };
        for (const auto& [colour, prepass] : twins)
        {
            const bool colourOpted = std::ranges::find(opted, std::string(colour)) != opted.end();
            const bool prepassOpted = std::ranges::find(opted, std::string(prepass)) != opted.end();
            EXPECT_EQ(colourOpted, prepassOpted)
                << colour << " and its prepass twin " << prepass << " take different GL routes";
        }
        for (const std::string& path : opted)
        {
            SCOPED_TRACE(path);
            const Ref<Shader> shader = Shader::Create(path);
            const OpenGLShader* gl = AsGL(shader);
            ASSERT_NE(gl, nullptr) << "not an OpenGL shader";
            EXPECT_TRUE(shader->IsReady()) << "did not link on the text route -- see OloEngine.log";
            EXPECT_TRUE(gl->IsGlslTextRoute()) << "named OLO_GL_GLSL_ROUTE and took the SPIR-V route";
        }
    }

    TEST(OpenGLShaderRoute, AShaderThatDoesNotOptInStaysOnSpirv)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        LeverBack back;
        Levers::SetGLShadersFromGlsl(Levers::Tristate::Unset);

        const Ref<Shader> shader = Shader::Create("assets/shaders/PBR_MultiLight.glsl");
        const OpenGLShader* gl = AsGL(shader);
        ASSERT_NE(gl, nullptr);
        ASSERT_TRUE(shader->IsReady());
        EXPECT_FALSE(gl->IsGlslTextRoute()) << "a shader that names no OLO_GL_GLSL_ROUTE took the text route";
    }

    TEST(OpenGLShaderRoute, TheLeverForcesTheRouteBothWays)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        LeverBack back;

        Levers::SetGLShadersFromGlsl(Levers::Tristate::Off);
        const Ref<Shader> spirv = Shader::Create("assets/shaders/Foliage_Instance.glsl");
        ASSERT_NE(AsGL(spirv), nullptr);
        ASSERT_TRUE(spirv->IsReady());
        EXPECT_FALSE(AsGL(spirv)->IsGlslTextRoute()) << "OLO_GL_SHADERS_FROM_GLSL=0 left an opted-in shader on text";

        Levers::SetGLShadersFromGlsl(Levers::Tristate::On);
        const Ref<Shader> text = Shader::Create("assets/shaders/PBR_MultiLight.glsl");
        ASSERT_NE(AsGL(text), nullptr);
        ASSERT_TRUE(text->IsReady());
        EXPECT_TRUE(AsGL(text)->IsGlslTextRoute()) << "OLO_GL_SHADERS_FROM_GLSL=1 left a shader on SPIR-V";
    }
} // namespace OloEngine::Tests
