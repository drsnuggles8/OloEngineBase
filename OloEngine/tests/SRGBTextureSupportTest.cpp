#include "OloEnginePCH.h"
#include <gtest/gtest.h>

// AssetSerializer.h forward-declares Scene; SceneSerializer (declared in the
// same header) uses Ref<Scene> implicitly via virtual destructors which forces
// the test TU to see the complete Scene type before instantiation. Including
// Scene.h here resolves that without pulling more headers into the test API.
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Asset/AssetSerializer.h"
#include "OloEngine/Renderer/GPUResourceQueue.h"
#include "OloEngine/Renderer/ComputeShader.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Task/Scheduler.h"
#include "OloEngine/Task/NamedThreads.h"

#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>
#include <thread>

using namespace OloEngine;

// Pins the sRGB plumbing landed for the OpenGL backend so a future refactor
// can't silently drop colour-space conversion on albedo textures (which is
// the bug the PR was raised to fix: PBR shaders assume the GPU performs
// sRGB->linear on sample, but the backend was emitting GL_RGBA8).

TEST(SRGBTextureSupport, TextureSpecificationDefaultsToLinear)
{
    TextureSpecification spec;
    EXPECT_FALSE(spec.SRGB)
        << "Default-constructed TextureSpecification must be linear so existing "
           "data textures (normal / metallic-roughness / AO) keep their current "
           "behaviour. Only colour textures (albedo / emissive) opt in.";
}

TEST(SRGBTextureSupport, TextureSpecificationCarriesSrgbFlag)
{
    TextureSpecification spec;
    spec.SRGB = true;
    EXPECT_TRUE(spec.SRGB);
}

TEST(SRGBTextureSupport, RawTextureDataKeepsLinearDefault)
{
    RawTextureData raw;
    EXPECT_FALSE(raw.SRGB)
        << "RawTextureData must default to linear — TextureSerializer flips it "
           "to sRGB by filename heuristic, but unrelated callers (e.g. tests, "
           "procedural textures) should keep the safe linear default.";
}

// Filename heuristic for the asset-pipeline path-load route. Model.cpp passes
// sRGB explicitly per aiTextureType; this heuristic only fires for standalone
// .png drag-drops that hit TextureSerializer::TryLoadData / TryLoadRawData.

TEST(SRGBTextureSupport, FilenameHeuristic_ClassifiesAlbedoAsSrgb)
{
    EXPECT_TRUE(TextureSerializer::IsLikelyColorTextureByName("character_albedo.png"));
    EXPECT_TRUE(TextureSerializer::IsLikelyColorTextureByName("Brick_BaseColor.jpg"));
    EXPECT_TRUE(TextureSerializer::IsLikelyColorTextureByName("wood_diffuse.tga"));
    EXPECT_TRUE(TextureSerializer::IsLikelyColorTextureByName("Cerberus_A.png"));
    EXPECT_TRUE(TextureSerializer::IsLikelyColorTextureByName("door_color.png"));
    EXPECT_TRUE(TextureSerializer::IsLikelyColorTextureByName("torch_emissive.png"));
    EXPECT_TRUE(TextureSerializer::IsLikelyColorTextureByName("lantern_emission.png"));
}

TEST(SRGBTextureSupport, FilenameHeuristic_ClassifiesDataTexturesAsLinear)
{
    EXPECT_FALSE(TextureSerializer::IsLikelyColorTextureByName("character_normal.png"));
    EXPECT_FALSE(TextureSerializer::IsLikelyColorTextureByName("Brick_Roughness.jpg"));
    EXPECT_FALSE(TextureSerializer::IsLikelyColorTextureByName("Metal_Metallic.png"));
    EXPECT_FALSE(TextureSerializer::IsLikelyColorTextureByName("wood_AO.png"));
    EXPECT_FALSE(TextureSerializer::IsLikelyColorTextureByName("terrain_height.png"));
    EXPECT_FALSE(TextureSerializer::IsLikelyColorTextureByName("Cerberus_N.png"));
    EXPECT_FALSE(TextureSerializer::IsLikelyColorTextureByName("Cerberus_M.png"));
    EXPECT_FALSE(TextureSerializer::IsLikelyColorTextureByName("Cerberus_R.png"));
    EXPECT_FALSE(TextureSerializer::IsLikelyColorTextureByName("road_bump.png"));
    EXPECT_FALSE(TextureSerializer::IsLikelyColorTextureByName("rock_displacement.png"));
    EXPECT_FALSE(TextureSerializer::IsLikelyColorTextureByName("packed_orm.png"));
}

TEST(SRGBTextureSupport, FilenameHeuristic_DataKeywordsOverrideColorKeywords)
{
    // Data keywords scan first, so a filename that mentions both — e.g.
    // "color" + "roughness" — is correctly classified as the data half. The
    // heuristic requires a separator on shorter ambiguous suffixes (like AO)
    // to avoid false positives on words that happen to contain the letters.
    EXPECT_FALSE(TextureSerializer::IsLikelyColorTextureByName("wood_colorRoughness.png"));
    EXPECT_FALSE(TextureSerializer::IsLikelyColorTextureByName("Diffuse_Metallic.png"));
    EXPECT_FALSE(TextureSerializer::IsLikelyColorTextureByName("base_color_normal.png"));
}

TEST(SRGBTextureSupport, FilenameHeuristic_AmbiguousFilesDefaultLinear)
{
    EXPECT_FALSE(TextureSerializer::IsLikelyColorTextureByName("Texture001.png"));
    EXPECT_FALSE(TextureSerializer::IsLikelyColorTextureByName("untitled.png"));
    EXPECT_FALSE(TextureSerializer::IsLikelyColorTextureByName(""));
}

TEST(SRGBTextureSupport, FilenameHeuristic_IsCaseInsensitive)
{
    EXPECT_TRUE(TextureSerializer::IsLikelyColorTextureByName("ALBEDO.PNG"));
    EXPECT_TRUE(TextureSerializer::IsLikelyColorTextureByName("Diffuse.TGA"));
    EXPECT_FALSE(TextureSerializer::IsLikelyColorTextureByName("Normal.PNG"));
}

// ===========================================================================
// GPUResourceQueue — async compute-shader creation
//
// Pins the producer side of the async compute-shader path that backs
// GPUResourceQueue::CreateComputeShaderCommand:
//   * the CPU-only file load + #include preprocessing
//     (ComputeShader::LoadSourceFromFile), the off-main-thread half of
//     CreateFromFileAsync();
//   * the command's failure plumbing (empty source -> callback(nullptr), no GL);
//     and
//   * the producer hand-off: a loaded source enqueues a CreateComputeShaderCommand
//     that a later GPUResourceQueue::ProcessAll() will turn into a GPU program.
//
// These run headless in CI with no GL context, so they stop at the enqueue. The
// final GL compile leg (ProcessAll -> ComputeShader::CreateFromSource, the path
// every GPU subsystem already uses) needs a live context and is not re-exercised
// here; the worker-thread dispatch itself IS exercised (see the *Async* test).
// ===========================================================================

namespace
{
    // Locate a real compute shader under the editor asset tree, probing the
    // working directories the test binary may run from (OloEditor/ under ctest,
    // or the repo root when the exe is launched directly).
    std::string ResolveComputeShaderPath(std::string_view filename)
    {
        namespace fs = std::filesystem;
        const fs::path candidates[] = {
            fs::path("assets/shaders/compute") / filename,           // cwd = OloEditor (ctest)
            fs::path("OloEditor/assets/shaders/compute") / filename, // cwd = repo root
            fs::current_path() / "OloEditor" / "assets" / "shaders" / "compute" / filename,
        };
        for (const auto& c : candidates)
        {
            std::error_code ec;
            if (fs::exists(c, ec))
                return c.string();
        }
        return {};
    }
} // namespace

TEST(GPUResourceQueueComputeShader, LoadSourceFromFileReadsAndNamesAComputeShader)
{
    const std::string path = ResolveComputeShaderPath("Snow_Clear.comp");
    ASSERT_FALSE(path.empty())
        << "Could not locate Snow_Clear.comp from CWD " << std::filesystem::current_path().string();

    const auto loaded = ComputeShader::LoadSourceFromFile(path);
    EXPECT_TRUE(loaded.IsValid());
    EXPECT_EQ(loaded.Name, "Snow_Clear");
    EXPECT_NE(loaded.Source.find("#version"), std::string::npos);
    EXPECT_NE(loaded.Source.find("local_size"), std::string::npos);
}

TEST(GPUResourceQueueComputeShader, LoadSourceFromFileResolvesIncludes)
{
    // Particle_Simulate.comp #includes ../include/WindSampling.glsl — after
    // preprocessing the directive must be consumed (spliced in or replaced by a
    // failure comment), so no literal #include should survive in the output.
    const std::string path = ResolveComputeShaderPath("Particle_Simulate.comp");
    ASSERT_FALSE(path.empty());

    const auto loaded = ComputeShader::LoadSourceFromFile(path);
    ASSERT_TRUE(loaded.IsValid());
    EXPECT_EQ(loaded.Source.find("#include"), std::string::npos)
        << "ProcessIncludes left an unresolved #include in the preprocessed source";
}

TEST(GPUResourceQueueComputeShader, LoadSourceFromFileFailsCleanlyForMissingFile)
{
    const auto loaded = ComputeShader::LoadSourceFromFile("assets/shaders/compute/__does_not_exist__.comp");
    EXPECT_FALSE(loaded.IsValid());
    EXPECT_TRUE(loaded.Source.empty());
}

TEST(GPUResourceQueueComputeShader, CreateComputeShaderCommandWithEmptySourceReportsNull)
{
    // The failure path touches no GL — Execute() early-returns and reports
    // nullptr through the callback, so this is exercisable headless.
    RawShaderData data;
    data.Name = "EmptyComputeShader";
    // ComputeSource intentionally left empty.

    bool callbackFired = false;
    Ref<ComputeShader> received;
    CreateComputeShaderCommand cmd(std::move(data),
                                   [&](Ref<ComputeShader> s)
                                   {
                                       callbackFired = true;
                                       received = s;
                                   });

    cmd.Execute();

    EXPECT_TRUE(callbackFired);
    EXPECT_FALSE(received);
}

// Mirrors exactly what CreateFromFileAsync's worker does — load the source then
// enqueue a CreateComputeShaderCommand — but synchronously, so it is a clean,
// deterministic headless test of the producer hand-off without standing up the
// task scheduler. (The threaded dispatch itself is verified in the editor.)
TEST(GPUResourceQueueComputeShader, LoadedSourceEnqueuesAComputeShaderCreationCommand)
{
    const std::string path = ResolveComputeShaderPath("Snow_Clear.comp");
    ASSERT_FALSE(path.empty());

    GPUResourceQueue::Clear();
    ASSERT_EQ(GPUResourceQueue::GetPendingCount(), 0u);

    const auto loaded = ComputeShader::LoadSourceFromFile(path);
    ASSERT_TRUE(loaded.IsValid());

    RawShaderData data;
    data.Name = loaded.Name;
    data.ComputeSource = loaded.Source;
    GPUResourceQueue::Enqueue<CreateComputeShaderCommand>(std::move(data), [](Ref<ComputeShader>) {});

    EXPECT_TRUE(GPUResourceQueue::HasPending());
    EXPECT_EQ(GPUResourceQueue::GetPendingCount(), 1u);

    // No ProcessAll() — that GL-compiles the queued source and needs a live
    // context this headless test does not have. Drop the queued command (and its
    // captured callback); the enqueue is what proves the producer hand-off.
    GPUResourceQueue::Clear();
}

// Drives the real CreateFromFileAsync end-to-end: the file is read + preprocessed
// on a task-system worker thread, which then enqueues the GPU-creation command.
// Mirrors FunctionalTest's harness for standing the scheduler up in a non-
// Application test binary (workers stay alive for the process; there is no
// matching StopWorkers).
class GPUResourceQueueAsyncComputeShader : public ::testing::Test
{
  protected:
    static void SetUpTestSuite()
    {
        LowLevelTasks::InitGameThreadId();
        Tasks::FNamedThreadManager::Get().AttachToThread(Tasks::ENamedThread::GameThread);
        LowLevelTasks::FScheduler::Get().StartWorkers();
    }
    void SetUp() override
    {
        GPUResourceQueue::Clear();
    }
    void TearDown() override
    {
        GPUResourceQueue::Clear();
    }
};

TEST_F(GPUResourceQueueAsyncComputeShader, CreateFromFileAsyncLoadsOffThreadAndEnqueuesGPUCreation)
{
    const std::string path = ResolveComputeShaderPath("Snow_Clear.comp");
    ASSERT_FALSE(path.empty());

    ASSERT_EQ(GPUResourceQueue::GetPendingCount(), 0u);

    ComputeShader::CreateFromFileAsync(path, [](Ref<ComputeShader>) {});

    // The worker reads + preprocesses the file off the main thread, then enqueues
    // a CreateComputeShaderCommand. Bounded-wait for that enqueue to appear.
    bool enqueued = false;
    for (int i = 0; i < 500 && !enqueued; ++i) // up to ~5s
    {
        if (GPUResourceQueue::HasPending())
            enqueued = true;
        else
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    EXPECT_TRUE(enqueued) << "CreateFromFileAsync did not enqueue a GPU-creation command";
    EXPECT_EQ(GPUResourceQueue::GetPendingCount(), 1u);

    // No ProcessAll() — the GL compile needs a live context this headless test
    // lacks. Drop the queued command (and its captured callback).
    GPUResourceQueue::Clear();
}
