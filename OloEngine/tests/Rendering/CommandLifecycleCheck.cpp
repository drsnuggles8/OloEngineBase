#include "OloEnginePCH.h"
#include "CommandLifecycleCheck.h"

#include "OloEngine/Renderer/Commands/CommandLifecycle.h"

#include <gtest/gtest.h>

#include <string>

namespace OloEngine::Tests::CommandLifecycleCheck
{
    namespace
    {
        class CommandLifecycleListener : public ::testing::EmptyTestEventListener
        {
          public:
            void OnTestStart(const ::testing::TestInfo& /*info*/) override
            {
                m_CountAtStart = CommandLifecycle::GetUnexpectedViolationCount();
            }

            void OnTestEnd(const ::testing::TestInfo& info) override
            {
                const u64 raised = CommandLifecycle::GetUnexpectedViolationCount() - m_CountAtStart;
                if (raised == 0)
                    return;

                std::string message = "Command lifecycle: " + std::to_string(raised) +
                                      " violation(s) raised during this test outside any "
                                      "CommandLifecycle::ScopedExpectedViolations. A packet, a command bucket or a "
                                      "FrameDataBuffer range was written after it was frozen for replay; "
                                      "OloEngine-Tests.log names the operation (search for 'Command lifecycle "
                                      "violation'). See Renderer/Commands/CommandLifecycle.h.";
                const char* const file = info.file() != nullptr ? info.file() : __FILE__;
                const int line = info.file() != nullptr ? info.line() : __LINE__;
                ADD_FAILURE_AT(file, line) << message;
            }

          private:
            u64 m_CountAtStart = 0;
        };

        bool s_Registered = false;
    } // namespace

    void RegisterListener()
    {
        if (s_Registered)
            return;
        s_Registered = true;
        CommandLifecycle::SetValidationEnabled(true);
        ::testing::UnitTest::GetInstance()->listeners().Append(new CommandLifecycleListener());
    }
} // namespace OloEngine::Tests::CommandLifecycleCheck
