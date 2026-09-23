// =============================================================================
// RenderCommandHandleInitTest.cpp
//
// Building a render command must not draw a UUID (issue #1420).
//
// `AssetHandle` is `UUID`, and a bare `AssetHandle x;` member default-constructs
// one, which draws a random ID. `CommandAllocator::AllocatePacketWithCommand<T>`
// value-initialises T, and `Renderer3D::DrawMeshParallel` /
// `DrawAnimatedMeshParallel` call it on the `SubmitMeshesParallel` workers. So
// every parallel-submitted mesh used to draw two IDs from the shared generator on
// a worker thread, for values the submitter overwrites on the next line. Every
// AssetHandle member in RenderCommand.h now has `= 0`.
//
// The draw count is per thread (`UUID::GetDrawCountOnThisThread`), so the
// assertions measure only what the building thread did.
//
// `WorkersBuildMeshCommandsWithoutDrawingIds` is the TSan job's cover for
// `SubmitMeshesParallel`: the real function needs an initialised Renderer3D
// (GL meshes, shaders, frame data), and the Linux sanitizer jobs have no GL
// context. It builds the same command, through the same allocator call, on
// several threads at once, which is the part of the worker path that touched
// the generator.
// =============================================================================

// OLO_TEST_LAYER: plumbing

#include "OloEnginePCH.h"

#include "OloEngine/Core/UUID.h"
#include "OloEngine/Renderer/Commands/CommandAllocator.h"
#include "OloEngine/Renderer/Commands/CommandPacket.h"
#include "OloEngine/Renderer/Commands/RenderCommand.h"

#include <gtest/gtest.h>

#include <array>
#include <latch>
#include <thread>
#include <vector>

using namespace OloEngine; // NOLINT(google-build-using-namespace) — test file, brevity preferred

namespace
{
    // Builds T both ways a command is built — a value-initialised local, and the
    // allocator's placement-new — and reports any UUID draw and any handle that
    // is not the 0 "no handle" value. `handlesOf` lists T's AssetHandle members.
    template<typename T, typename HandlesOf>
    ::testing::AssertionResult BuildsWithoutDrawingAnId(const char* typeName, HandlesOf handlesOf)
    {
        CommandAllocator allocator;

        const u64 before = UUID::GetDrawCountOnThisThread();
        const T local{};
        CommandPacket* packet = allocator.AllocatePacketWithCommand<T>();
        const u64 draws = UUID::GetDrawCountOnThisThread() - before;

        if (!packet)
        {
            return ::testing::AssertionFailure() << typeName << ": AllocatePacketWithCommand returned null";
        }
        if (draws != 0)
        {
            return ::testing::AssertionFailure() << typeName << ": building two commands drew " << draws
                                                 << " UUID(s); an AssetHandle member is missing its `= 0`";
        }
        for (const T* cmd : { &local, static_cast<const T*>(packet->GetCommandData<T>()) })
        {
            for (const u64 handle : handlesOf(*cmd))
            {
                if (handle != 0)
                {
                    return ::testing::AssertionFailure() << typeName << ": a fresh command carries handle " << handle
                                                         << ", expected the 0 'no handle' value";
                }
            }
        }
        return ::testing::AssertionSuccess();
    }
} // namespace

TEST(RenderCommandHandleInit, CounterDetectsAnImplicitAssetHandleConstruction)
{
    struct CommandWithBareHandle
    {
        AssetHandle handle;
    };

    const u64 before = UUID::GetDrawCountOnThisThread();
    const CommandWithBareHandle command{};
    EXPECT_GE(UUID::GetDrawCountOnThisThread() - before, 1u);
    EXPECT_NE(static_cast<u64>(command.handle), 0u);
}

TEST(RenderCommandHandleInit, EveryCommandWithAnAssetHandleBuildsWithoutDrawingAnId)
{
    EXPECT_TRUE(BuildsWithoutDrawingAnId<DrawMeshCommand>(
        "DrawMeshCommand", [](const DrawMeshCommand& c)
        { return std::array<u64, 2>{ c.meshHandle, c.shaderHandle }; }));
    EXPECT_TRUE(BuildsWithoutDrawingAnId<DrawMeshInstancedCommand>(
        "DrawMeshInstancedCommand", [](const DrawMeshInstancedCommand& c)
        { return std::array<u64, 2>{ c.meshHandle, c.shaderHandle }; }));
    EXPECT_TRUE(BuildsWithoutDrawingAnId<DrawSkyboxCommand>(
        "DrawSkyboxCommand", [](const DrawSkyboxCommand& c)
        { return std::array<u64, 2>{ c.meshHandle, c.shaderHandle }; }));
    EXPECT_TRUE(BuildsWithoutDrawingAnId<DrawInfiniteGridCommand>(
        "DrawInfiniteGridCommand", [](const DrawInfiniteGridCommand& c)
        { return std::array<u64, 1>{ c.shaderHandle }; }));
    EXPECT_TRUE(BuildsWithoutDrawingAnId<DrawQuadCommand>(
        "DrawQuadCommand", [](const DrawQuadCommand& c)
        { return std::array<u64, 1>{ c.shaderHandle }; }));
}

TEST(RenderCommandHandleInit, WorkersBuildMeshCommandsWithoutDrawingIds)
{
    constexpr u32 workerCount = 8;
    constexpr u32 commandsPerWorker = 5'000;

    std::vector<u64> drawsPerWorker(workerCount, ~u64{ 0 });
    std::vector<u32> builtPerWorker(workerCount, 0);
    std::latch start(workerCount);

    std::vector<std::thread> workers;
    workers.reserve(workerCount);
    for (u32 w = 0; w < workerCount; ++w)
    {
        workers.emplace_back(
            [&, w]
            {
                // One allocator per worker, as WorkerSubmitContext gives each
                // SubmitMeshesParallel worker its own.
                CommandAllocator allocator;
                start.arrive_and_wait();
                const u64 before = UUID::GetDrawCountOnThisThread();
                for (u32 i = 0; i < commandsPerWorker; ++i)
                {
                    // The shape of DrawMeshParallel (and of DrawAnimatedMeshParallel,
                    // which builds the same command): allocate, then overwrite the
                    // handles with the asset's.
                    CommandPacket* packet = allocator.AllocatePacketWithCommand<DrawMeshCommand>();
                    if (!packet)
                    {
                        continue;
                    }
                    auto* cmd = packet->GetCommandData<DrawMeshCommand>();
                    cmd->header.type = CommandType::DrawMesh;
                    cmd->meshHandle = UUID(1000u + i);
                    cmd->shaderHandle = UUID(2000u + w);
                    ++builtPerWorker[w];
                }
                drawsPerWorker[w] = UUID::GetDrawCountOnThisThread() - before;
            });
    }
    for (std::thread& worker : workers)
    {
        worker.join();
    }

    for (u32 w = 0; w < workerCount; ++w)
    {
        EXPECT_EQ(builtPerWorker[w], commandsPerWorker) << "worker " << w;
        EXPECT_EQ(drawsPerWorker[w], 0u) << "worker " << w << " drew UUIDs while building DrawMeshCommands";
    }
}
