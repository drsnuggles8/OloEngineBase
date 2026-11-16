// OLO_TEST_LAYER: meta
//
// Self-test for the cross-test renderer-backend guard (issue #1693).
// RendererBackendCheck's listener fails a test that leaves the API selector or
// the RenderCommand facade off the process's backend. These pin what it
// compares against and what it reports, in whichever process runs them:
// `OloEngine-Tests` and `OloEngine-Tests --olo-rhi=vulkan` both run this file.
#include "OloEnginePCH.h"

#include "RendererBackendCheck.h"
#include "TestRenderBackend.h"
#include "../ScopedRendererApi.h"

#include "OloEngine/Renderer/RenderCommand.h"

#include <gtest/gtest.h>

using namespace OloEngine;        // NOLINT(google-build-using-namespace) — test file, brevity preferred
using namespace OloEngine::Tests; // NOLINT(google-build-using-namespace)

namespace
{
    [[nodiscard]] RendererAPI::API ThisProcessApi()
    {
        return TestRenderBackend::IsVulkanProcess() ? RendererAPI::API::Vulkan : RendererAPI::API::OpenGL;
    }

    [[nodiscard]] RendererAPI::API TheOtherApi()
    {
        return TestRenderBackend::IsVulkanProcess() ? RendererAPI::API::OpenGL : RendererAPI::API::Vulkan;
    }
} // namespace

// The listener records the backend once, from main(). Recorded before
// TestRenderBackend::SelectFromOptions, the Vulkan process would hold OpenGL
// as "its" backend and fail every test that left it correctly on Vulkan.
TEST(RendererBackendCheckTest, RecordsTheBackendThisProcessRuns)
{
    const RendererBackendCheck::Snapshot& process = RendererBackendCheck::ProcessBackend();
    EXPECT_EQ(process.Api, ThisProcessApi());
    ASSERT_NE(process.FacadeType, nullptr);
    ASSERT_NE(RenderCommand::TryGetRendererAPI(), nullptr);
    EXPECT_EQ(*process.FacadeType, typeid(*RenderCommand::TryGetRendererAPI()))
        << "the facade is still the type the process started with";
    // Only the Vulkan process pins the object: in the OpenGL process the
    // device suites recreate the facade and put a fresh one of the same type back.
    if (TestRenderBackend::IsVulkanProcess())
        EXPECT_EQ(process.Facade, RenderCommand::TryGetRendererAPI())
            << "the windowless context's facade is the one the process started with";
}

TEST(RendererBackendCheckTest, AnUnchangedBackendIsNotALeak)
{
    const RendererBackendCheck::Snapshot now = RendererBackendCheck::Capture();
    std::string detail;
    EXPECT_EQ(RendererBackendCheck::Describe(RendererBackendCheck::ProcessBackend(), now,
                                             TestRenderBackend::IsVulkanProcess(), detail),
              0u)
        << detail;
    EXPECT_TRUE(detail.empty());
}

TEST(RendererBackendCheckTest, ASelectorLeftOnTheOtherApiIsReportedByName)
{
    const RendererBackendCheck::Snapshot process = RendererBackendCheck::ProcessBackend();
    RendererBackendCheck::Snapshot leaked = process;
    leaked.Api = TheOtherApi();

    std::string detail;
    EXPECT_EQ(RendererBackendCheck::Describe(process, leaked, false, detail), 1u);
    EXPECT_NE(detail.find("RendererAPI::GetAPI()"), std::string::npos) << detail;
}

// Identity counts only in the Vulkan process, where the windowless context
// records through the facade object. In the OpenGL process a scope may
// recreate the facade as long as the type it leaves is the process's.
TEST(RendererBackendCheckTest, ARecreatedFacadeIsALeakOnlyWhereIdentityCounts)
{
    const RendererBackendCheck::Snapshot process = RendererBackendCheck::ProcessBackend();
    RendererBackendCheck::Snapshot recreated = process;
    const int otherObject = 0;
    // Compared, never dereferenced.
    recreated.Facade = reinterpret_cast<const RendererAPI*>(&otherObject);

    std::string detail;
    EXPECT_EQ(RendererBackendCheck::Describe(process, recreated, false, detail), 0u) << detail;
    EXPECT_EQ(RendererBackendCheck::Describe(process, recreated, true, detail), 1u);
    EXPECT_NE(detail.find("recreated"), std::string::npos) << detail;
}

TEST(RendererBackendCheckTest, AFacadeOfAnotherTypeIsALeakInEitherProcess)
{
    const RendererBackendCheck::Snapshot process = RendererBackendCheck::ProcessBackend();
    RendererBackendCheck::Snapshot swapped = process;
    // Any type other than the facade's stands in for the other backend's.
    swapped.FacadeType = &typeid(int);

    for (const bool compareIdentity : { false, true })
    {
        std::string detail;
        EXPECT_EQ(RendererBackendCheck::Describe(process, swapped, compareIdentity, detail), 1u)
            << "identity compared: " << compareIdentity;
        EXPECT_NE(detail.find("facade is a"), std::string::npos) << detail;
    }
}

// The scope every test uses gives back what it found, which is the process's
// API here, not a constant. Nested, because the device suites nest a factory
// selection inside a facade selection.
TEST(RendererBackendCheckTest, AScopedSelectionGivesBackTheApiItFound)
{
    ASSERT_EQ(RendererAPI::GetAPI(), ThisProcessApi());
    {
        const ScopedRendererApi outer(TheOtherApi());
        EXPECT_EQ(outer.Previous(), ThisProcessApi());
        EXPECT_EQ(RendererAPI::GetAPI(), TheOtherApi());
        {
            const ScopedVulkanApiSelection inner;
            EXPECT_EQ(RendererAPI::GetAPI(), RendererAPI::API::Vulkan);
        }
        EXPECT_EQ(RendererAPI::GetAPI(), TheOtherApi());
    }
    EXPECT_EQ(RendererAPI::GetAPI(), ThisProcessApi());
}
