#include "OloEnginePCH.h"
#include "RendererBackendCheck.h"

#include "TestRenderBackend.h"

#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/RHI/RHIDescriptorHeap.h"

#include <GLFW/glfw3.h>
#include <gtest/gtest.h>

#include <cstdio>
#include <map>

namespace OloEngine::Tests::RendererBackendCheck
{
    namespace
    {
        Snapshot s_Process;
        bool s_Registered = false;
        // OpenGL process: the descriptor heap's enabled flag when the running
        // test started. RenderCommand::Init re-derives it from the lever, so a
        // facade rebuilt below puts it back by hand, as
        // ScopedVulkanRenderCommandSelection does.
        bool s_HeapEnabledAtTestStart = false;
        u32 s_LeakCount = 0;

        // Test or suite name -> what it left changed, for the end-of-run list.
        std::map<std::string, std::string, std::less<>>& LeakLedger()
        {
            static std::map<std::string, std::string, std::less<>> ledger;
            return ledger;
        }

        [[nodiscard]] const char* ApiName(const RendererAPI::API api)
        {
            switch (api)
            {
                case RendererAPI::API::None:
                    return "None";
                case RendererAPI::API::OpenGL:
                    return "OpenGL";
                case RendererAPI::API::Vulkan:
                    return "Vulkan";
            }
            return "unknown";
        }

        [[nodiscard]] std::string FacadeTypeName(const std::type_info* type)
        {
            return type != nullptr ? type->name() : "(no facade)";
        }

        void AppendDetail(std::string& out, const std::string& item)
        {
            if (!out.empty())
                out += "; ";
            out += item;
        }

        // Puts back what can be put back. Returns false when the facade the
        // Vulkan process's windowless context records through was replaced:
        // that object is destroyed and nothing here can bring it back.
        bool Restore(const Snapshot& now)
        {
            if (now.Api != s_Process.Api)
                RendererAPI::SetAPI(s_Process.Api);

            if (TestRenderBackend::IsVulkanProcess())
                return now.Facade == s_Process.Facade;

            if (now.FacadeType != nullptr && s_Process.FacadeType != nullptr && *now.FacadeType != *s_Process.FacadeType)
            {
                // ScopedVulkanRenderCommandSelection's teardown, for a test
                // that swapped the facade and never swapped it back.
                RenderCommand::RecreateForSelectedBackend();
                if (glfwGetCurrentContext() != nullptr)
                {
                    RenderCommand::Init();
                    RHI::DescriptorHeap::Get().SetEnabled(s_HeapEnabledAtTestStart);
                }
            }
            return true;
        }

        // Checks, restores and reports. `where` names the test or suite;
        // `file`/`line` is where its failure is attributed.
        void CheckAndRestore(const std::string& where, const char* file, const int line)
        {
            const Snapshot now = Capture();
            std::string detail;
            if (Describe(s_Process, now, TestRenderBackend::IsVulkanProcess(), detail) == 0)
                return;

            const bool restored = Restore(now);
            ++s_LeakCount;
            LeakLedger()[where] = detail;

            ADD_FAILURE_AT(file, line)
                << "Renderer backend not restored: " << detail << ". " << where
                << " changed the backend this process runs, and every later test inherits it. A scope gives back "
                   "the value it found, never a constant (Rendering/ScopedRendererApi.h, issue #1693). "
                << (restored ? "The listener restored it, so later tests are unaffected."
                             : "The windowless VulkanContext's facade was destroyed and cannot be restored: every "
                               "later scene test in this process is invalid.");
        }

        class RendererBackendListener : public ::testing::EmptyTestEventListener
        {
          public:
            void OnTestStart(const ::testing::TestInfo&) override
            {
                if (!TestRenderBackend::IsVulkanProcess())
                    s_HeapEnabledAtTestStart = RHI::DescriptorHeap::Get().IsEnabled();
            }

            // A test that leaves the backend changed fails whether it passed or
            // skipped: a skip that got far enough to switch the selector leaks
            // exactly as a pass does.
            void OnTestEnd(const ::testing::TestInfo& info) override
            {
                const char* const file = info.file() != nullptr ? info.file() : __FILE__;
                const int line = info.file() != nullptr ? info.line() : __LINE__;
                CheckAndRestore(std::string(info.test_suite_name()) + "." + info.name() +
                                    " (or its suite's SetUpTestSuite)",
                                file, line);
            }

            void OnTestSuiteEnd(const ::testing::TestSuite& suite) override
            {
                CheckAndRestore(std::string(suite.name()) + "'s TearDownTestSuite", __FILE__, __LINE__);
            }

            void OnTestProgramEnd(const ::testing::UnitTest&) override
            {
                const auto& ledger = LeakLedger();
                if (ledger.empty())
                    return;
                std::printf("\n[ RENDERER BACKEND ] %zu test(s) or suite(s) left the renderer backend changed "
                            "(each one FAILED):\n",
                            ledger.size());
                for (const auto& [name, detail] : ledger)
                    std::printf("[ RENDERER BACKEND ]   %s: %s\n", name.c_str(), detail.c_str());
                std::fflush(stdout);
            }
        };
    } // namespace

    Snapshot Capture()
    {
        Snapshot snapshot;
        snapshot.Api = RendererAPI::GetAPI();
        snapshot.Facade = RenderCommand::TryGetRendererAPI();
        if (snapshot.Facade != nullptr)
            snapshot.FacadeType = &typeid(*snapshot.Facade);
        return snapshot;
    }

    u32 Describe(const Snapshot& expected, const Snapshot& now, const bool compareFacadeIdentity, std::string& outDetail)
    {
        u32 changed = 0;
        if (now.Api != expected.Api)
        {
            ++changed;
            AppendDetail(outDetail, std::string("RendererAPI::GetAPI() is ") + ApiName(now.Api) +
                                        ", the process runs " + ApiName(expected.Api));
        }
        const bool sameType = (now.FacadeType == nullptr || expected.FacadeType == nullptr)
                                  ? now.FacadeType == expected.FacadeType
                                  : *now.FacadeType == *expected.FacadeType;
        if (!sameType)
        {
            ++changed;
            AppendDetail(outDetail, "the RenderCommand facade is a " + FacadeTypeName(now.FacadeType) +
                                        ", the process started with a " + FacadeTypeName(expected.FacadeType));
        }
        else if (compareFacadeIdentity && now.Facade != expected.Facade)
        {
            ++changed;
            AppendDetail(outDetail, "the RenderCommand facade was recreated; the windowless VulkanContext records "
                                    "through the one the process started with");
        }
        return changed;
    }

    void RegisterListener()
    {
        if (s_Registered)
            return;
        s_Registered = true;
        s_Process = Capture();
        ::testing::UnitTest::GetInstance()->listeners().Append(new RendererBackendListener());
    }

    const Snapshot& ProcessBackend()
    {
        return s_Process;
    }

    u32 LeakCount()
    {
        return s_LeakCount;
    }
} // namespace OloEngine::Tests::RendererBackendCheck
