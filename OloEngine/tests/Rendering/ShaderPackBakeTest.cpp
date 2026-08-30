// =============================================================================
// ShaderPackBakeTest.cpp
//
// The headless CI producer for issue #908: baking a `ShaderPack.osp` from
// this repo's shaders. Deliberately gated behind `--olo-bake-shader-pack=<path>`
// (see TestOptions) rather than running every suite invocation — a bake is a
// build STEP, not a correctness check on its own (that's
// ShaderPackContentHashTest.cpp and ShaderPackTest.cpp), and it writes a real
// multi-megabyte file as a side effect.
//
// No GL context anywhere in this path (verified per the issue's invalidation-
// contract spike): Renderer2D::GetShaderFilepaths() / Renderer3D::GetShaderFilepaths()
// return plain string lists with no engine init, and ShaderPack::CreateFromFilepaths
// uses only the CPU-side Shader::PrepareBatch (read, preprocess, shaderc,
// SPIRV-Cross). This is exactly what makes it runnable on an ordinary CI
// runner instead of needing the self-hosted GPU box.
//
// OLO_TEST_LAYER: shaderpipe
// =============================================================================

#include "OloEnginePCH.h"

#include <gtest/gtest.h>
#include "TestOptions.h"

#include "OloEngine/Renderer/Renderer2D.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/ShaderPack.h"

#include <filesystem>
#include <vector>

using namespace OloEngine;

namespace
{
    // RAII rather than a plain restore-after-the-fact: a filesystem/shaderc
    // exception (or an ASSERT_* early-return, which is just a `return`, not a
    // throw) between the cwd switch and the manual restore would otherwise
    // leave the whole gtest PROCESS running from OloEditor/ for every test
    // after this one in the same binary invocation — exactly the class of
    // "wrong working directory" cascade CLAUDE.md's "Working directory
    // matters" pitfall warns about, just self-inflicted instead of a launch
    // mistake.
    class ScopedCurrentPath
    {
      public:
        explicit ScopedCurrentPath(const std::filesystem::path& newPath)
            : m_Previous(std::filesystem::current_path())
        {
            std::filesystem::current_path(newPath);
        }
        ~ScopedCurrentPath()
        {
            std::error_code ec;
            std::filesystem::current_path(m_Previous, ec);
        }
        ScopedCurrentPath(const ScopedCurrentPath&) = delete;
        auto operator=(const ScopedCurrentPath&) -> ScopedCurrentPath& = delete;

      private:
        std::filesystem::path m_Previous;
    };
} // namespace

TEST(ShaderPackBakeTest, BakeWhenRequested)
{
    const auto& opts = OloEngine::Tests::Options();
    if (opts.BakeShaderPackPath.empty())
    {
        GTEST_SKIP() << "pass --olo-bake-shader-pack=<path> to run this "
                        "(the headless CI producer for issue #908)";
    }

    const std::filesystem::path outputPath = std::filesystem::absolute(opts.BakeShaderPackPath);
    const auto originalCwd = std::filesystem::current_path();

    // The shader filepath lists ("assets/shaders/...") are resolved relative
    // to OloEditor/ — the same working directory Renderer2D::Init() /
    // Renderer3D::Init() run from in every other context (CLAUDE.md "Working
    // directory matters"). ctest runs this binary from the repo root, so
    // step into OloEditor/ for the bake and step back out unconditionally
    // afterward, regardless of how it goes (see ScopedCurrentPath above), so
    // a failure here doesn't leave every OTHER test in this process
    // resolving paths from the wrong place.
    ASSERT_TRUE(std::filesystem::exists(originalCwd / "OloEditor" / "assets" / "shaders"))
        << "expected to run from the repo root (the ctest / CI convention), found: " << originalCwd.string();

    bool baked = false;
    std::vector<std::string> filepaths;
    {
        ScopedCurrentPath cwdGuard(originalCwd / "OloEditor");

        filepaths = Renderer2D::GetShaderFilepaths();
        for (auto& path : Renderer3D::GetShaderFilepaths())
        {
            filepaths.push_back(std::move(path));
        }

        baked = ShaderPack::CreateFromFilepaths(filepaths, outputPath);
    }

    ASSERT_TRUE(baked) << "shader pack bake failed — see the [ShaderPack] log output above";
    ASSERT_TRUE(std::filesystem::exists(outputPath));

    // Read it back through the real loader as a sanity check on what was
    // just written, the same way a packaged runtime will load it.
    const ShaderPack pack(outputPath);
    EXPECT_TRUE(pack.IsLoaded());
    EXPECT_GT(pack.GetShaderCount(), 0u);
    EXPECT_EQ(pack.GetShaderCount(), static_cast<u32>(filepaths.size()))
        << "every enumerated shader should have compiled and packed cleanly; "
           "check the log above for any '[ShaderPack] Skipping' warnings";

    OLO_CORE_INFO("[ShaderPackBake] Baked {} shaders to '{}'", pack.GetShaderCount(), outputPath.string());
}
