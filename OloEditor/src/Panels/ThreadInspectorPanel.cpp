#include "OloEnginePCH.h"
#include "ThreadInspectorPanel.h"

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/PlatformTLS.h"
#include "OloEngine/Debug/Profiler.h"
#include "OloEngine/HAL/PlatformProcess.h"
#include "OloEngine/HAL/RunnableThread.h"
#include "OloEngine/HAL/ThreadManager.h"
#include "OloEngine/Task/NamedThreads.h"
#include "OloEngine/Task/Scheduler.h"

#include <imgui.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace OloEngine
{
    namespace
    {
        // Both OloEngine::EThreadPriority and OloEngine::LowLevelTasks::EThreadPriority
        // declare the same enumerators in the same order, so a single mapper keyed on
        // the underlying value serves both.
        const char* PriorityName(i32 value)
        {
            switch (value)
            {
                case 0:
                    return "Normal";
                case 1:
                    return "Above Normal";
                case 2:
                    return "Below Normal";
                case 3:
                    return "Highest";
                case 4:
                    return "Lowest";
                case 5:
                    return "Slightly Below";
                case 6:
                    return "Time Critical";
                default:
                    return "?";
            }
        }

        const char* ThreadTypeName(FRunnableThread::ThreadType type)
        {
            switch (type)
            {
                case FRunnableThread::ThreadType::Real:
                    return "Real";
                case FRunnableThread::ThreadType::Fake:
                    return "Fake";
                case FRunnableThread::ThreadType::Forkable:
                    return "Forkable";
                default:
                    return "?";
            }
        }

        // Classify a thread into a human-friendly role from its registered name. The
        // names are stable: the scheduler names its workers "Foreground Worker #N" /
        // "Background Worker #N", the dedicated threads are "OloEngine::AudioThread"
        // and "OloEngine::NetworkThread", Async() uses "TAsync N", and editor build
        // threads carry "Build"/"AssetPack" in their name.
        const char* ClassifyKind(std::string_view name)
        {
            if (name.starts_with("Foreground Worker"))
            {
                return "Worker (FG)";
            }
            if (name.starts_with("Background Worker"))
            {
                return "Worker (BG)";
            }
            if (name.find("AudioThread") != std::string_view::npos)
            {
                return "Audio";
            }
            if (name.find("NetworkThread") != std::string_view::npos)
            {
                return "Network";
            }
            if (name.starts_with("TAsync"))
            {
                return "Async";
            }
            if (name.find("Build") != std::string_view::npos || name.find("AssetPack") != std::string_view::npos)
            {
                return "Build";
            }
            return "Other";
        }

        struct ThreadRow
        {
            std::string Name;
            const char* Kind = "Other";
            u32 ThreadId = 0;
            i32 Priority = -1; // -1 => unknown (synthetic game-thread row)
            FRunnableThread::ThreadType Type = FRunnableThread::ThreadType::Real;
            bool IsRunning = true;
            bool IsGameThread = false;
        };
    } // namespace

    void ThreadInspectorPanel::OnImGuiRender(bool* p_open)
    {
        OLO_PROFILE_FUNCTION();

        ImGui::SetNextWindowSize(ImVec2(540.0f, 640.0f), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Thread Inspector", p_open))
        {
            ImGui::End();
            return;
        }

        // --- Snapshot the live thread registry under its lock ----------------
        std::vector<ThreadRow> rows;

        // The game/main thread is not created via FRunnableThread, so it never
        // registers. This panel renders on the game thread, so the current thread
        // id IS the game thread id — synthesise the row directly.
        {
            ThreadRow game;
            game.Name = "GameThread (Main)";
            game.Kind = "Game";
            game.ThreadId = FPlatformTLS::GetCurrentThreadId();
            game.Priority = -1;
            game.IsRunning = true;
            game.IsGameThread = true;
            rows.push_back(std::move(game));
        }

        // Cross-reference registry thread ids against the named-thread manager so named
        // threads (audio/network/…) are labelled by role precisely instead of by name
        // string. Both systems key by FPlatformTLS::GetCurrentThreadId().
        struct NamedRole
        {
            const char* Label;
            Tasks::ENamedThread Thread;
            u32 Id;
        };
        NamedRole namedRoles[] = {
            { "Game", Tasks::ENamedThread::GameThread, 0 },
            { "Render", Tasks::ENamedThread::RenderThread, 0 },
            { "Audio", Tasks::ENamedThread::AudioThread, 0 },
            { "Network", Tasks::ENamedThread::NetworkThread, 0 },
        };
        {
            auto& manager = Tasks::FNamedThreadManager::Get();
            for (NamedRole& role : namedRoles)
            {
                role.Id = manager.GetThreadId(role.Thread);
            }
        }
        const auto roleForId = [&namedRoles](u32 id) -> const char*
        {
            if (id != 0)
            {
                for (const NamedRole& role : namedRoles)
                {
                    if (role.Id == id)
                    {
                        return role.Label;
                    }
                }
            }
            return nullptr;
        };

        FThreadManager::Get().ForEachThread(
            [&rows, &roleForId](u32 threadId, FRunnableThread* thread)
            {
                if (thread == nullptr)
                {
                    return;
                }
                ThreadRow row;
                row.Name = thread->GetThreadName();
                // Prefer the precise named-thread role; fall back to name-based heuristics
                // for workers / async / build threads that aren't named threads.
                const char* role = roleForId(threadId);
                row.Kind = role ? role : ClassifyKind(row.Name);
                row.ThreadId = threadId;
                row.Priority = static_cast<i32>(thread->GetThreadPriority());
                row.Type = thread->GetThreadType();
                row.IsRunning = thread->IsRunning();
                rows.push_back(std::move(row));
            });

        // Game thread first, then grouped by kind, then by name — stable display.
        std::stable_sort(rows.begin(), rows.end(),
                         [](const ThreadRow& a, const ThreadRow& b)
                         {
                             if (a.IsGameThread != b.IsGameThread)
                             {
                                 return a.IsGameThread;
                             }
                             if (const int kindCmp = std::strcmp(a.Kind, b.Kind); kindCmp != 0)
                             {
                                 return kindCmp < 0;
                             }
                             return a.Name < b.Name;
                         });

        const sizet runningCount = static_cast<sizet>(
            std::count_if(rows.begin(), rows.end(), [](const ThreadRow& r)
                          { return r.IsRunning; }));

        // --- Summary ---------------------------------------------------------
        auto& scheduler = LowLevelTasks::FScheduler::Get();
        ImGui::Text("Live threads: %zu  (%zu running)", rows.size(), runningCount);
        ImGui::Text("Hardware concurrency: %u logical cores", std::thread::hardware_concurrency());
        ImGui::Text("Task scheduler: %u / %u workers active  (FG: %s, BG: %s)",
                    scheduler.GetNumWorkers(), scheduler.GetMaxNumWorkers(),
                    PriorityName(static_cast<i32>(scheduler.GetWorkerPriority())),
                    PriorityName(static_cast<i32>(scheduler.GetBackgroundPriority())));

        ImGui::Separator();
        ImGui::Checkbox("Only running", &m_OnlyRunning);

        // --- Live threads table ----------------------------------------------
        constexpr ImGuiTableFlags tableFlags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                               ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY;
        // Bound the table height so it scrolls past ~12 rows instead of growing the
        // window unbounded — that keeps the named-thread section below reachable and
        // stops the window from overflowing (and popping out as its own OS viewport).
        const float tableHeight = ImGui::GetTextLineHeightWithSpacing() * 12.0f;
        if (ImGui::BeginTable("ThreadsTable", 6, tableFlags, ImVec2(0.0f, tableHeight)))
        {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Thread ID", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("Priority", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableHeadersRow();

            for (const ThreadRow& row : rows)
            {
                if (m_OnlyRunning && !row.IsRunning)
                {
                    continue;
                }

                ImGui::TableNextRow();

                ImGui::TableNextColumn();
                ImGui::TextUnformatted(row.Name.c_str());

                ImGui::TableNextColumn();
                ImGui::Text("%u", row.ThreadId);

                ImGui::TableNextColumn();
                ImGui::TextUnformatted(row.Kind);

                ImGui::TableNextColumn();
                ImGui::TextUnformatted(row.IsGameThread ? "Real" : ThreadTypeName(row.Type));

                ImGui::TableNextColumn();
                if (row.Priority < 0)
                {
                    ImGui::TextDisabled("--");
                }
                else
                {
                    ImGui::TextUnformatted(PriorityName(row.Priority));
                }

                ImGui::TableNextColumn();
                if (row.IsRunning)
                {
                    ImGui::TextColored(ImVec4(0.4f, 0.85f, 0.4f, 1.0f), "Running");
                }
                else
                {
                    ImGui::TextColored(ImVec4(0.85f, 0.5f, 0.3f, 1.0f), "Stopped");
                }
            }

            ImGui::EndTable();
        }

        // --- Named-thread queue backlog --------------------------------------
        if (ImGui::CollapsingHeader("Named Thread Queues"))
        {
            ImGui::TextDisabled("Logical threads with task queues; Thread ID is the live attached OS thread.");

            if (ImGui::BeginTable("NamedThreadQueues", 3,
                                  ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
            {
                ImGui::TableSetupColumn("Named Thread");
                ImGui::TableSetupColumn("Thread ID");
                ImGui::TableSetupColumn("Pending Tasks");
                ImGui::TableHeadersRow();

                auto& manager = Tasks::FNamedThreadManager::Get();
                for (const NamedRole& role : namedRoles)
                {
                    const bool attached = role.Id != 0;

                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(role.Label);

                    ImGui::TableNextColumn();
                    if (attached)
                    {
                        ImGui::Text("%u", role.Id);
                    }
                    else
                    {
                        ImGui::TextDisabled("not attached");
                    }

                    ImGui::TableNextColumn();
                    if (!attached)
                    {
                        ImGui::TextDisabled("--");
                    }
                    else if (manager.GetQueue(role.Thread).HasPendingTasks(true))
                    {
                        ImGui::TextColored(ImVec4(0.9f, 0.8f, 0.3f, 1.0f), "yes");
                    }
                    else
                    {
                        ImGui::TextDisabled("idle");
                    }
                }
                ImGui::EndTable();
            }
        }

        ImGui::End();
    }
} // namespace OloEngine
