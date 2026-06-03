#include "OloEnginePCH.h"
#include "CinematicTimelinePanel.h"

#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Asset/AssetManager/EditorAssetManager.h"
#include "OloEngine/Cinematic/CinematicEdit.h"
#include "OloEngine/Cinematic/CinematicSequence.h"
#include "OloEngine/Cinematic/CinematicSequenceSerializer.h"
#include "OloEngine/Cinematic/CinematicSystem.h"
#include "OloEngine/Cinematic/CinematicTrack.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Scene.h"

#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/trigonometric.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <optional>
#include <vector>

namespace OloEngine
{
    namespace
    {
        constexpr ImU32 kColInterp[3] = {
            IM_COL32(150, 150, 160, 255), // Constant
            IM_COL32(90, 170, 250, 255),  // Linear
            IM_COL32(250, 180, 70, 255),  // EaseInOut
        };

        ImU32 KeyColor(CinematicInterp interp, bool selected)
        {
            const ImU32 base = kColInterp[std::min<sizet>(static_cast<sizet>(interp), 2)];
            return selected ? IM_COL32(255, 255, 255, 255) : base;
        }

        // Channel colors for the curve preview (x/y/z).
        constexpr ImU32 kAxisColor[3] = {
            IM_COL32(230, 90, 90, 255),
            IM_COL32(110, 210, 110, 255),
            IM_COL32(110, 150, 240, 255),
        };

        const char* LaneKindLabel(int kind)
        {
            switch (kind)
            {
                case 0:
                    return "Translation";
                case 1:
                    return "Rotation";
                case 2:
                    return "Scale";
                case 3:
                    return "Position";
                case 4:
                    return "Rotation";
                case 5:
                    return "FOV";
                case 6:
                    return "Visibility";
                case 7:
                    return "Events";
                default:
                    return "?";
            }
        }
    } // namespace

    // ============================ open / save ================================

    void CinematicTimelinePanel::OpenSequence(AssetHandle handle)
    {
        if (handle == 0)
        {
            return;
        }
        if (AssetManager::GetAssetType(handle) != AssetType::CinematicSequence)
        {
            OLO_WARN("CinematicTimelinePanel: asset {} is not a CinematicSequence", static_cast<u64>(handle));
            return;
        }

        // Edit the cached asset in place so any CinematicComponent referencing it
        // (and the inspector's scrub slider) sees edits live.
        Ref<CinematicSequence> seq = AssetManager::GetAsset<CinematicSequence>(handle);
        if (!seq)
        {
            OLO_WARN("CinematicTimelinePanel: failed to load CinematicSequence {}", static_cast<u64>(handle));
            return;
        }

        m_Sequence = seq;
        m_Handle = handle;
        m_Selection = {};
        m_Playhead = 0.0f;
        m_Playing = false;
        m_Dirty = false;
        m_Open = true;
        m_FocusRequested = true; // bring the tab to front when (re)opened

        m_FilePath.clear();
        if (auto editorAssets = Project::GetAssetManager().As<EditorAssetManager>())
        {
            m_FilePath = editorAssets->GetFileSystemPath(handle);
        }
    }

    void CinematicTimelinePanel::OpenSequence(const std::filesystem::path& path)
    {
        auto editorAssets = Project::GetAssetManager().As<EditorAssetManager>();
        if (!editorAssets)
        {
            return;
        }
        const AssetHandle handle = editorAssets->ImportAsset(path);
        OpenSequence(handle);
    }

    void CinematicTimelinePanel::Reset()
    {
        m_Sequence = nullptr;
        m_Handle = 0;
        m_FilePath.clear();
        m_Selection = {};
        m_Playhead = 0.0f;
        m_Playing = false;
        m_Dirty = false;
    }

    bool CinematicTimelinePanel::Save()
    {
        if (!m_Sequence || m_FilePath.empty())
        {
            OLO_WARN("CinematicTimelinePanel: cannot save — no sequence / unknown path");
            return false;
        }
        if (CinematicSequenceSerializer::Serialize(m_Sequence, m_FilePath.string()))
        {
            m_Dirty = false;
            OLO_INFO("CinematicTimelinePanel: saved '{}'", m_FilePath.string());
            return true;
        }
        OLO_ERROR("CinematicTimelinePanel: failed to save '{}'", m_FilePath.string());
        return false;
    }

    void CinematicTimelinePanel::MarkDirty()
    {
        m_Dirty = true;
    }

    // ============================ helpers ====================================

    f32 CinematicTimelinePanel::TimeToX(f32 time, f32 contentLeft) const
    {
        return contentLeft + (time - m_ScrollSeconds) * m_PixelsPerSecond;
    }

    f32 CinematicTimelinePanel::XToTime(f32 x, f32 contentLeft) const
    {
        return m_ScrollSeconds + (x - contentLeft) / m_PixelsPerSecond;
    }

    f32 CinematicTimelinePanel::SnapTime(f32 time) const
    {
        if (!m_SnapEnabled || m_SnapSeconds <= 0.0f)
        {
            return std::max(0.0f, time);
        }
        const f32 snapped = std::round(time / m_SnapSeconds) * m_SnapSeconds;
        return std::max(0.0f, snapped);
    }

    f32 CinematicTimelinePanel::SequenceDuration() const
    {
        if (!m_Sequence)
        {
            return 1.0f;
        }
        const f32 d = m_Sequence->GetEffectiveDuration();
        return d > 0.0f ? d : 1.0f;
    }

    void CinematicTimelinePanel::ApplyPlayhead()
    {
        if (m_Context && m_Sequence && !m_Context->IsRunning())
        {
            CinematicSystem::ApplyAtTime(*m_Context, *m_Sequence, m_Playhead);
        }
    }

    void CinematicTimelinePanel::UpdatePreview(f32 dt)
    {
        const f32 dur = SequenceDuration();
        m_Playhead += dt;
        if (m_Playhead >= dur)
        {
            if (m_PreviewLoop)
            {
                // Wrap, keeping the overshoot so playback rate stays steady.
                m_Playhead = (dur > 0.0f) ? std::fmod(m_Playhead, dur) : 0.0f;
            }
            else
            {
                m_Playhead = dur;
                m_Playing = false;
            }
        }
        ApplyPlayhead();
    }

    // ============================ insert / delete ============================

    void CinematicTimelinePanel::InsertKeyAtPlayhead(LaneKind kind, sizet trackIndex)
    {
        if (!m_Sequence)
        {
            return;
        }
        const f32 t = SnapTime(m_Playhead);

        // Resolve the target entity (if any) so the inserted key snapshots the
        // entity's current value — authoring "set a keyframe here" matches what
        // you see in the viewport.
        std::optional<Entity> target;
        UUID targetUuid = 0;
        switch (kind)
        {
            case LaneKind::TransformTranslation:
            case LaneKind::TransformRotation:
            case LaneKind::TransformScale:
                if (trackIndex < m_Sequence->TransformTracks.size())
                    targetUuid = m_Sequence->TransformTracks[trackIndex].Target;
                break;
            case LaneKind::CameraPosition:
            case LaneKind::CameraRotation:
            case LaneKind::CameraFov:
                if (trackIndex < m_Sequence->CameraTracks.size())
                    targetUuid = m_Sequence->CameraTracks[trackIndex].Target;
                break;
            case LaneKind::Visibility:
                if (trackIndex < m_Sequence->VisibilityTracks.size())
                    targetUuid = m_Sequence->VisibilityTracks[trackIndex].Target;
                break;
            case LaneKind::Event:
                break;
        }
        if (m_Context && targetUuid != 0)
        {
            target = m_Context->TryGetEntityWithUUID(targetUuid);
        }

        sizet newIndex = 0;
        switch (kind)
        {
            case LaneKind::TransformTranslation:
            {
                auto& tr = m_Sequence->TransformTracks[trackIndex];
                glm::vec3 v = tr.Translation.Evaluate(t, glm::vec3(0.0f));
                if (target && target->HasComponent<TransformComponent>())
                    v = target->GetComponent<TransformComponent>().Translation;
                newIndex = CinematicEdit::InsertKeySorted(tr.Translation.Keys, { t, v, CinematicInterp::Linear });
                break;
            }
            case LaneKind::TransformRotation:
            {
                auto& tr = m_Sequence->TransformTracks[trackIndex];
                glm::quat q = tr.Rotation.Evaluate(t, glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
                if (target && target->HasComponent<TransformComponent>())
                    q = target->GetComponent<TransformComponent>().GetRotation();
                newIndex = CinematicEdit::InsertKeySorted(tr.Rotation.Keys, { t, q, CinematicInterp::Linear });
                break;
            }
            case LaneKind::TransformScale:
            {
                auto& tr = m_Sequence->TransformTracks[trackIndex];
                glm::vec3 v = tr.Scale.Evaluate(t, glm::vec3(1.0f));
                if (target && target->HasComponent<TransformComponent>())
                    v = target->GetComponent<TransformComponent>().Scale;
                newIndex = CinematicEdit::InsertKeySorted(tr.Scale.Keys, { t, v, CinematicInterp::Linear });
                break;
            }
            case LaneKind::CameraPosition:
            {
                auto& tr = m_Sequence->CameraTracks[trackIndex];
                glm::vec3 v = tr.Position.Evaluate(t, glm::vec3(0.0f));
                if (target && target->HasComponent<TransformComponent>())
                    v = target->GetComponent<TransformComponent>().Translation;
                newIndex = CinematicEdit::InsertKeySorted(tr.Position.Keys, { t, v, CinematicInterp::Linear });
                break;
            }
            case LaneKind::CameraRotation:
            {
                auto& tr = m_Sequence->CameraTracks[trackIndex];
                glm::quat q = tr.Rotation.Evaluate(t, glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
                if (target && target->HasComponent<TransformComponent>())
                    q = target->GetComponent<TransformComponent>().GetRotation();
                newIndex = CinematicEdit::InsertKeySorted(tr.Rotation.Keys, { t, q, CinematicInterp::Linear });
                break;
            }
            case LaneKind::CameraFov:
            {
                auto& tr = m_Sequence->CameraTracks[trackIndex];
                f32 fov = tr.VerticalFovRadians.Evaluate(t, glm::radians(45.0f));
                if (target && target->HasComponent<CameraComponent>())
                    fov = target->GetComponent<CameraComponent>().Camera.GetPerspectiveVerticalFOV();
                newIndex = CinematicEdit::InsertKeySorted(tr.VerticalFovRadians.Keys, { t, fov, CinematicInterp::Linear });
                break;
            }
            case LaneKind::Visibility:
            {
                auto& tr = m_Sequence->VisibilityTracks[trackIndex];
                // Insert the opposite of the current value so the new key visibly
                // does something (a duplicate-value key would be a no-op).
                const bool current = tr.EvaluateAt(t, true);
                newIndex = CinematicEdit::InsertKeySorted(tr.Keys, { t, !current });
                break;
            }
            case LaneKind::Event:
            {
                auto& tr = m_Sequence->EventTracks[trackIndex];
                newIndex = CinematicEdit::InsertKeySorted(tr.Keys, { t, "event" });
                break;
            }
        }

        m_Selection = { true, kind, trackIndex, newIndex };
        MarkDirty();
    }

    void CinematicTimelinePanel::DeleteSelected()
    {
        if (!m_Selection.Valid || !m_Sequence)
        {
            return;
        }
        const sizet ti = m_Selection.TrackIndex;
        const sizet ki = m_Selection.KeyIndex;
        bool removed = false;
        switch (m_Selection.Kind)
        {
            case LaneKind::TransformTranslation:
                if (ti < m_Sequence->TransformTracks.size())
                    removed = CinematicEdit::RemoveKeyAt(m_Sequence->TransformTracks[ti].Translation.Keys, ki);
                break;
            case LaneKind::TransformRotation:
                if (ti < m_Sequence->TransformTracks.size())
                    removed = CinematicEdit::RemoveKeyAt(m_Sequence->TransformTracks[ti].Rotation.Keys, ki);
                break;
            case LaneKind::TransformScale:
                if (ti < m_Sequence->TransformTracks.size())
                    removed = CinematicEdit::RemoveKeyAt(m_Sequence->TransformTracks[ti].Scale.Keys, ki);
                break;
            case LaneKind::CameraPosition:
                if (ti < m_Sequence->CameraTracks.size())
                    removed = CinematicEdit::RemoveKeyAt(m_Sequence->CameraTracks[ti].Position.Keys, ki);
                break;
            case LaneKind::CameraRotation:
                if (ti < m_Sequence->CameraTracks.size())
                    removed = CinematicEdit::RemoveKeyAt(m_Sequence->CameraTracks[ti].Rotation.Keys, ki);
                break;
            case LaneKind::CameraFov:
                if (ti < m_Sequence->CameraTracks.size())
                    removed = CinematicEdit::RemoveKeyAt(m_Sequence->CameraTracks[ti].VerticalFovRadians.Keys, ki);
                break;
            case LaneKind::Visibility:
                if (ti < m_Sequence->VisibilityTracks.size())
                    removed = CinematicEdit::RemoveKeyAt(m_Sequence->VisibilityTracks[ti].Keys, ki);
                break;
            case LaneKind::Event:
                if (ti < m_Sequence->EventTracks.size())
                    removed = CinematicEdit::RemoveKeyAt(m_Sequence->EventTracks[ti].Keys, ki);
                break;
        }
        if (removed)
        {
            m_Selection = {};
            MarkDirty();
        }
    }

    // ============================ key lane drawing ===========================

    template<typename KeyVec>
    bool CinematicTimelinePanel::DrawKeyLane(KeyVec& keys, LaneKind kind, sizet trackIndex,
                                             const ImVec2& laneMin, const ImVec2& laneMax, f32 trackHeaderWidth)
    {
        bool mutated = false;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const f32 contentLeft = laneMin.x + trackHeaderWidth;
        const f32 cy = (laneMin.y + laneMax.y) * 0.5f;

        dl->PushClipRect(ImVec2(contentLeft, laneMin.y), ImVec2(laneMax.x, laneMax.y), true);

        for (sizet i = 0; i < keys.size(); ++i)
        {
            const f32 x = TimeToX(keys[i].Time, contentLeft);
            if (x < contentLeft - s_KeyHalfSize || x > laneMax.x + s_KeyHalfSize)
            {
                continue; // offscreen
            }

            const bool selected = m_Selection.Valid && m_Selection.Kind == kind &&
                                  m_Selection.TrackIndex == trackIndex && m_Selection.KeyIndex == i;

            // Interp mode only exists on the keyed (curve) channels; visibility /
            // event keys have no interp, draw them in a neutral colour.
            CinematicInterp interp = CinematicInterp::Linear;
            if constexpr (requires { keys[i].Interp; })
            {
                interp = keys[i].Interp;
            }
            const ImU32 col = (kind == LaneKind::Event || kind == LaneKind::Visibility)
                                  ? (selected ? IM_COL32(255, 255, 255, 255) : IM_COL32(200, 160, 220, 255))
                                  : KeyColor(interp, selected);

            // Diamond marker.
            const ImVec2 c(x, cy);
            const f32 h = s_KeyHalfSize;
            dl->AddQuadFilled(ImVec2(c.x, c.y - h), ImVec2(c.x + h, c.y), ImVec2(c.x, c.y + h), ImVec2(c.x - h, c.y), col);
            if (selected)
            {
                dl->AddQuad(ImVec2(c.x, c.y - h - 1), ImVec2(c.x + h + 1, c.y), ImVec2(c.x, c.y + h + 1), ImVec2(c.x - h - 1, c.y), IM_COL32(20, 20, 20, 255), 1.5f);
            }

            // Event keys: draw the name next to the diamond.
            if constexpr (requires { keys[i].Name; })
            {
                dl->AddText(ImVec2(x + h + 2.0f, cy - ImGui::GetFontSize() * 0.5f), IM_COL32(220, 220, 220, 255), keys[i].Name.c_str());
            }

            // Interaction button over the diamond (submitted on top of the lane
            // background, which set SetNextItemAllowOverlap).
            char id[64];
            std::snprintf(id, sizeof(id), "##key_%d_%zu_%zu", static_cast<int>(kind), trackIndex, i);
            ImGui::SetCursorScreenPos(ImVec2(x - h, cy - h));
            ImGui::InvisibleButton(id, ImVec2(h * 2.0f, h * 2.0f));

            if (ImGui::IsItemActivated())
            {
                m_Selection = { true, kind, trackIndex, i };
            }
            if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
            {
                const f32 newTime = SnapTime(XToTime(ImGui::GetIO().MousePos.x, contentLeft));
                const sizet ni = CinematicEdit::MoveKeyTime(keys, i, newTime);
                m_Selection = { true, kind, trackIndex, ni };
                mutated = true;
                break; // vector reordered — stop iterating this frame
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("t = %.3fs", static_cast<double>(keys[i].Time));
            }
        }

        dl->PopClipRect();
        return mutated;
    }

    // ============================ timeline ===================================

    void CinematicTimelinePanel::DrawRuler(ImDrawList* drawList, const ImVec2& origin, f32 laneWidth, f32 height)
    {
        const f32 contentLeft = origin.x + s_TrackHeaderWidth;
        const f32 right = origin.x + laneWidth;
        const ImU32 bg = IM_COL32(40, 40, 46, 255);
        drawList->AddRectFilled(origin, ImVec2(right, origin.y + height), bg);

        // Choose a "nice" tick spacing so labels don't overlap (aim ~80px apart).
        const f32 targetPx = 80.0f;
        f32 step = targetPx / m_PixelsPerSecond;
        const std::array<f32, 9> nice = { 0.05f, 0.1f, 0.25f, 0.5f, 1.0f, 2.0f, 5.0f, 10.0f, 30.0f };
        for (f32 n : nice)
        {
            if (n >= step)
            {
                step = n;
                break;
            }
        }
        if (step <= 0.0f)
        {
            step = 1.0f;
        }

        const f32 firstTime = std::max(0.0f, m_ScrollSeconds);
        const f32 startTick = std::floor(firstTime / step) * step;
        for (f32 t = startTick;; t += step)
        {
            const f32 x = TimeToX(t, contentLeft);
            if (x > right)
            {
                break;
            }
            if (x >= contentLeft)
            {
                drawList->AddLine(ImVec2(x, origin.y + height * 0.5f), ImVec2(x, origin.y + height), IM_COL32(120, 120, 130, 255));
                char buf[32];
                std::snprintf(buf, sizeof(buf), "%.2gs", static_cast<double>(t));
                drawList->AddText(ImVec2(x + 2.0f, origin.y + 2.0f), IM_COL32(180, 180, 190, 255), buf);
            }
            if (t > m_ScrollSeconds + (laneWidth / m_PixelsPerSecond) + step)
            {
                break;
            }
        }

        // Duration end marker.
        const f32 durX = TimeToX(SequenceDuration(), contentLeft);
        if (durX >= contentLeft && durX <= right)
        {
            drawList->AddLine(ImVec2(durX, origin.y), ImVec2(durX, origin.y + height), IM_COL32(220, 80, 80, 200), 1.0f);
        }
    }

    void CinematicTimelinePanel::DrawPlayhead(ImDrawList* drawList, const ImVec2& origin, f32 laneWidth, f32 timelineHeight)
    {
        const f32 contentLeft = origin.x + s_TrackHeaderWidth;
        const f32 x = TimeToX(m_Playhead, contentLeft);
        if (x < contentLeft || x > origin.x + laneWidth)
        {
            return;
        }
        drawList->AddLine(ImVec2(x, origin.y), ImVec2(x, origin.y + timelineHeight), IM_COL32(255, 220, 60, 230), 1.5f);
        drawList->AddTriangleFilled(ImVec2(x - 5.0f, origin.y), ImVec2(x + 5.0f, origin.y), ImVec2(x, origin.y + 7.0f), IM_COL32(255, 220, 60, 255));
    }

    void CinematicTimelinePanel::DrawTracksAndTimeline()
    {
        // ---- ruler strip (fixed, drives scrubbing) ----
        const ImVec2 rulerOrigin = ImGui::GetCursorScreenPos();
        const f32 viewWidth = ImGui::GetContentRegionAvail().x;
        const f32 contentLeft = rulerOrigin.x + s_TrackHeaderWidth;
        ImDrawList* dl = ImGui::GetWindowDrawList();

        ImGui::InvisibleButton("##ruler", ImVec2(viewWidth, s_RulerHeight));
        const bool rulerActive = ImGui::IsItemActive();
        if (rulerActive && ImGui::GetIO().MousePos.x >= contentLeft)
        {
            m_Playhead = SnapTime(XToTime(ImGui::GetIO().MousePos.x, contentLeft));
            m_Playing = false;
            ApplyPlayhead();
        }
        DrawRuler(dl, rulerOrigin, viewWidth, s_RulerHeight);
        DrawPlayhead(dl, rulerOrigin, viewWidth, s_RulerHeight);

        // Wheel over the ruler: zoom (ctrl) or pan.
        if (ImGui::IsItemHovered())
        {
            const f32 wheel = ImGui::GetIO().MouseWheel;
            if (wheel != 0.0f)
            {
                if (ImGui::GetIO().KeyCtrl)
                {
                    const f32 mouseTime = XToTime(ImGui::GetIO().MousePos.x, contentLeft);
                    m_PixelsPerSecond = std::clamp(m_PixelsPerSecond * (wheel > 0.0f ? 1.15f : 0.87f), s_MinPixelsPerSecond, s_MaxPixelsPerSecond);
                    // Keep the time under the cursor stationary.
                    m_ScrollSeconds = std::max(0.0f, mouseTime - (ImGui::GetIO().MousePos.x - contentLeft) / m_PixelsPerSecond);
                }
                else
                {
                    m_ScrollSeconds = std::max(0.0f, m_ScrollSeconds - wheel * (40.0f / m_PixelsPerSecond));
                }
            }
        }

        // ---- lanes (vertical scroll) ----
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::BeginChild("##lanes", ImVec2(0, 0), false);

        const ImVec2 canvasOrigin = ImGui::GetCursorScreenPos();
        const f32 laneWidth = ImGui::GetContentRegionAvail().x;
        ImDrawList* ldl = ImGui::GetWindowDrawList();

        // Build the lane list from the sequence (fixed order: transform, camera,
        // visibility, event tracks; continuous tracks expand to sub-lanes).
        struct Lane
        {
            LaneKind Kind;
            sizet TrackIndex;
            std::string Header;
        };
        std::vector<Lane> lanes;
        const auto entityName = [&](UUID uuid) -> std::string
        {
            if (m_Context && uuid != 0)
            {
                if (auto e = m_Context->TryGetEntityWithUUID(uuid))
                {
                    return e->GetName();
                }
                return "<missing>";
            }
            return "<none>";
        };

        for (sizet i = 0; i < m_Sequence->TransformTracks.size(); ++i)
        {
            const std::string n = entityName(m_Sequence->TransformTracks[i].Target);
            lanes.push_back({ LaneKind::TransformTranslation, i, n + " \xE2\x96\xB8 Translation" });
            lanes.push_back({ LaneKind::TransformRotation, i, n + " \xE2\x96\xB8 Rotation" });
            lanes.push_back({ LaneKind::TransformScale, i, n + " \xE2\x96\xB8 Scale" });
        }
        for (sizet i = 0; i < m_Sequence->CameraTracks.size(); ++i)
        {
            const std::string n = entityName(m_Sequence->CameraTracks[i].Target);
            lanes.push_back({ LaneKind::CameraPosition, i, n + " \xE2\x96\xB8 Cam Pos" });
            lanes.push_back({ LaneKind::CameraRotation, i, n + " \xE2\x96\xB8 Cam Rot" });
            lanes.push_back({ LaneKind::CameraFov, i, n + " \xE2\x96\xB8 Cam FOV" });
        }
        for (sizet i = 0; i < m_Sequence->VisibilityTracks.size(); ++i)
        {
            lanes.push_back({ LaneKind::Visibility, i, entityName(m_Sequence->VisibilityTracks[i].Target) + " \xE2\x96\xB8 Visible" });
        }
        for (sizet i = 0; i < m_Sequence->EventTracks.size(); ++i)
        {
            const std::string& tn = m_Sequence->EventTracks[i].Name;
            lanes.push_back({ LaneKind::Event, i, (tn.empty() ? std::string("Events") : tn) });
        }

        const f32 totalH = std::max(1.0f, static_cast<f32>(lanes.size()) * s_LaneHeight);
        ImGui::Dummy(ImVec2(laneWidth, totalH)); // reserve scroll height

        bool mutated = false;
        for (sizet li = 0; li < lanes.size(); ++li)
        {
            const Lane& lane = lanes[li];
            const ImVec2 laneMin(canvasOrigin.x, canvasOrigin.y + static_cast<f32>(li) * s_LaneHeight);
            const ImVec2 laneMax(canvasOrigin.x + laneWidth, laneMin.y + s_LaneHeight);

            // Row background (alternating) + header.
            const ImU32 rowBg = (li & 1u) ? IM_COL32(32, 32, 38, 255) : IM_COL32(28, 28, 33, 255);
            ldl->AddRectFilled(ImVec2(laneMin.x + s_TrackHeaderWidth, laneMin.y), laneMax, rowBg);
            ldl->AddRectFilled(laneMin, ImVec2(laneMin.x + s_TrackHeaderWidth, laneMax.y), IM_COL32(45, 45, 52, 255));
            ldl->AddText(ImVec2(laneMin.x + 24.0f, laneMin.y + 3.0f), IM_COL32(210, 210, 215, 255), lane.Header.c_str());
            ldl->AddLine(ImVec2(laneMin.x, laneMax.y), ImVec2(laneMax.x, laneMax.y), IM_COL32(20, 20, 24, 255));

            // Header "+key" button (insert at playhead).
            char addId[48];
            std::snprintf(addId, sizeof(addId), "+##add_%zu", li);
            ImGui::SetCursorScreenPos(ImVec2(laneMin.x + 2.0f, laneMin.y + 2.0f));
            if (ImGui::SmallButton(addId))
            {
                InsertKeyAtPlayhead(lane.Kind, lane.TrackIndex);
                mutated = true;
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("Add %s key at playhead", LaneKindLabel(static_cast<int>(lane.Kind)));
            }

            // Lane background interaction: double-click in the key area inserts a
            // key at that time; single click on empty space deselects.
            char bgId[48];
            std::snprintf(bgId, sizeof(bgId), "##lanebg_%zu", li);
            ImGui::SetCursorScreenPos(ImVec2(laneMin.x + s_TrackHeaderWidth, laneMin.y));
            ImGui::SetNextItemAllowOverlap();
            ImGui::InvisibleButton(bgId, ImVec2(std::max(1.0f, laneWidth - s_TrackHeaderWidth), s_LaneHeight));
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            {
                m_Playhead = SnapTime(XToTime(ImGui::GetIO().MousePos.x, laneMin.x + s_TrackHeaderWidth));
                InsertKeyAtPlayhead(lane.Kind, lane.TrackIndex);
                mutated = true;
            }
            else if (ImGui::IsItemActivated())
            {
                m_Selection = {}; // click empty space deselects
            }

            // Keys (drawn + interactive on top of the background button).
            switch (lane.Kind)
            {
                case LaneKind::TransformTranslation:
                    mutated |= DrawKeyLane(m_Sequence->TransformTracks[lane.TrackIndex].Translation.Keys, lane.Kind, lane.TrackIndex, laneMin, laneMax, s_TrackHeaderWidth);
                    break;
                case LaneKind::TransformRotation:
                    mutated |= DrawKeyLane(m_Sequence->TransformTracks[lane.TrackIndex].Rotation.Keys, lane.Kind, lane.TrackIndex, laneMin, laneMax, s_TrackHeaderWidth);
                    break;
                case LaneKind::TransformScale:
                    mutated |= DrawKeyLane(m_Sequence->TransformTracks[lane.TrackIndex].Scale.Keys, lane.Kind, lane.TrackIndex, laneMin, laneMax, s_TrackHeaderWidth);
                    break;
                case LaneKind::CameraPosition:
                    mutated |= DrawKeyLane(m_Sequence->CameraTracks[lane.TrackIndex].Position.Keys, lane.Kind, lane.TrackIndex, laneMin, laneMax, s_TrackHeaderWidth);
                    break;
                case LaneKind::CameraRotation:
                    mutated |= DrawKeyLane(m_Sequence->CameraTracks[lane.TrackIndex].Rotation.Keys, lane.Kind, lane.TrackIndex, laneMin, laneMax, s_TrackHeaderWidth);
                    break;
                case LaneKind::CameraFov:
                    mutated |= DrawKeyLane(m_Sequence->CameraTracks[lane.TrackIndex].VerticalFovRadians.Keys, lane.Kind, lane.TrackIndex, laneMin, laneMax, s_TrackHeaderWidth);
                    break;
                case LaneKind::Visibility:
                    mutated |= DrawKeyLane(m_Sequence->VisibilityTracks[lane.TrackIndex].Keys, lane.Kind, lane.TrackIndex, laneMin, laneMax, s_TrackHeaderWidth);
                    break;
                case LaneKind::Event:
                    mutated |= DrawKeyLane(m_Sequence->EventTracks[lane.TrackIndex].Keys, lane.Kind, lane.TrackIndex, laneMin, laneMax, s_TrackHeaderWidth);
                    break;
            }
        }

        // Playhead over the lanes.
        DrawPlayhead(ldl, ImVec2(canvasOrigin.x, canvasOrigin.y), laneWidth, totalH);

        if (lanes.empty())
        {
            ldl->AddText(ImVec2(canvasOrigin.x + 12.0f, canvasOrigin.y + 8.0f), IM_COL32(150, 150, 150, 255),
                         "No tracks. Use 'Add Track' in the toolbar.");
        }

        ImGui::EndChild();
        ImGui::PopStyleVar();

        if (mutated)
        {
            MarkDirty();
            ApplyPlayhead();
        }
    }

    // ============================ inspector ==================================

    void CinematicTimelinePanel::DrawInspector()
    {
        ImGui::BeginChild("##inspector", ImVec2(0, 0), true);

        if (!m_Selection.Valid)
        {
            ImGui::TextDisabled("Select a keyframe to edit it. Double-click a lane to add one; drag to move; right side shows the curve.");
            ImGui::EndChild();
            return;
        }

        // Validate selection still in range (tracks/keys can change).
        auto clampedKey = [&](sizet count) -> bool
        {
            if (m_Selection.KeyIndex >= count)
            {
                m_Selection = {};
                return false;
            }
            return true;
        };

        const sizet ti = m_Selection.TrackIndex;
        bool changed = false;

        ImGui::Text("Selected key");
        ImGui::SameLine();
        if (ImGui::SmallButton("Delete Key"))
        {
            DeleteSelected();
            ImGui::EndChild();
            return;
        }
        ImGui::Separator();

        // Common: time editor (re-sorts via MoveKeyTime). Implemented per-kind so
        // we can re-fetch the right vector.
        const auto editInterp = [&](CinematicInterp& interp)
        {
            int cur = static_cast<int>(interp);
            const char* items[] = { "Constant", "Linear", "EaseInOut" };
            if (ImGui::Combo("Interpolation", &cur, items, 3))
            {
                interp = static_cast<CinematicInterp>(std::clamp(cur, 0, 2));
                changed = true;
            }
        };

        switch (m_Selection.Kind)
        {
            case LaneKind::TransformTranslation:
            case LaneKind::TransformScale:
            {
                auto& tr = m_Sequence->TransformTracks[ti];
                auto& channel = (m_Selection.Kind == LaneKind::TransformTranslation) ? tr.Translation : tr.Scale;
                if (!clampedKey(channel.Keys.size()))
                    break;
                auto& key = channel.Keys[m_Selection.KeyIndex];
                f32 t = key.Time;
                if (ImGui::DragFloat("Time", &t, 0.01f, 0.0f, 1e6f, "%.3fs"))
                {
                    m_Selection.KeyIndex = CinematicEdit::MoveKeyTime(channel.Keys, m_Selection.KeyIndex, t);
                    changed = true;
                    break;
                }
                if (ImGui::DragFloat3("Value", &key.Value.x, 0.02f))
                    changed = true;
                editInterp(key.Interp);
                break;
            }
            case LaneKind::CameraPosition:
            {
                auto& channel = m_Sequence->CameraTracks[ti].Position;
                if (!clampedKey(channel.Keys.size()))
                    break;
                auto& key = channel.Keys[m_Selection.KeyIndex];
                f32 t = key.Time;
                if (ImGui::DragFloat("Time", &t, 0.01f, 0.0f, 1e6f, "%.3fs"))
                {
                    m_Selection.KeyIndex = CinematicEdit::MoveKeyTime(channel.Keys, m_Selection.KeyIndex, t);
                    changed = true;
                    break;
                }
                if (ImGui::DragFloat3("Value", &key.Value.x, 0.02f))
                    changed = true;
                editInterp(key.Interp);
                break;
            }
            case LaneKind::TransformRotation:
            case LaneKind::CameraRotation:
            {
                CinematicQuatChannel* channel = (m_Selection.Kind == LaneKind::TransformRotation)
                                                    ? &m_Sequence->TransformTracks[ti].Rotation
                                                    : &m_Sequence->CameraTracks[ti].Rotation;
                if (!clampedKey(channel->Keys.size()))
                    break;
                auto& key = channel->Keys[m_Selection.KeyIndex];
                f32 t = key.Time;
                if (ImGui::DragFloat("Time", &t, 0.01f, 0.0f, 1e6f, "%.3fs"))
                {
                    m_Selection.KeyIndex = CinematicEdit::MoveKeyTime(channel->Keys, m_Selection.KeyIndex, t);
                    changed = true;
                    break;
                }
                glm::vec3 euler = glm::degrees(glm::eulerAngles(key.Value));
                if (ImGui::DragFloat3("Euler (deg)", &euler.x, 0.5f))
                {
                    key.Value = glm::quat(glm::radians(euler));
                    changed = true;
                }
                editInterp(key.Interp);
                break;
            }
            case LaneKind::CameraFov:
            {
                auto& channel = m_Sequence->CameraTracks[ti].VerticalFovRadians;
                if (!clampedKey(channel.Keys.size()))
                    break;
                auto& key = channel.Keys[m_Selection.KeyIndex];
                f32 t = key.Time;
                if (ImGui::DragFloat("Time", &t, 0.01f, 0.0f, 1e6f, "%.3fs"))
                {
                    m_Selection.KeyIndex = CinematicEdit::MoveKeyTime(channel.Keys, m_Selection.KeyIndex, t);
                    changed = true;
                    break;
                }
                f32 deg = glm::degrees(key.Value);
                if (ImGui::DragFloat("FOV (deg)", &deg, 0.25f, 1.0f, 179.0f))
                {
                    key.Value = glm::radians(deg);
                    changed = true;
                }
                editInterp(key.Interp);
                break;
            }
            case LaneKind::Visibility:
            {
                auto& keys = m_Sequence->VisibilityTracks[ti].Keys;
                if (!clampedKey(keys.size()))
                    break;
                auto& key = keys[m_Selection.KeyIndex];
                f32 t = key.Time;
                if (ImGui::DragFloat("Time", &t, 0.01f, 0.0f, 1e6f, "%.3fs"))
                {
                    m_Selection.KeyIndex = CinematicEdit::MoveKeyTime(keys, m_Selection.KeyIndex, t);
                    changed = true;
                    break;
                }
                if (ImGui::Checkbox("Visible", &key.Visible))
                    changed = true;
                break;
            }
            case LaneKind::Event:
            {
                auto& keys = m_Sequence->EventTracks[ti].Keys;
                if (!clampedKey(keys.size()))
                    break;
                auto& key = keys[m_Selection.KeyIndex];
                f32 t = key.Time;
                if (ImGui::DragFloat("Time", &t, 0.01f, 0.0f, 1e6f, "%.3fs"))
                {
                    m_Selection.KeyIndex = CinematicEdit::MoveKeyTime(keys, m_Selection.KeyIndex, t);
                    changed = true;
                    break;
                }
                std::snprintf(m_EventNameBuffer, sizeof(m_EventNameBuffer), "%s", key.Name.c_str());
                if (ImGui::InputText("Event name", m_EventNameBuffer, sizeof(m_EventNameBuffer)))
                {
                    key.Name = m_EventNameBuffer;
                    changed = true;
                }
                break;
            }
        }

        if (changed)
        {
            MarkDirty();
            ApplyPlayhead();
        }

        DrawCurvePreview();

        ImGui::EndChild();
    }

    // ============================ curve preview ==============================

    void CinematicTimelinePanel::DrawCurvePreview()
    {
        if (!m_Selection.Valid || !m_Sequence)
        {
            return;
        }
        const sizet ti = m_Selection.TrackIndex;

        // Build up to 3 sampled component series + the key times. We sample the
        // channel's Evaluate() so the plotted shape reflects the per-key interp
        // modes (Constant = steps, Linear = straight, EaseInOut = smooth) — this
        // is the "see the curve you authored" feedback.
        const f32 dur = SequenceDuration();
        constexpr int kN = 160;
        int compCount = 0;
        std::array<std::vector<f32>, 3> series;
        std::vector<f32> keyTimes;
        bool isStep = false;

        const auto sampleVec3 = [&](const CinematicVec3Channel& ch)
        {
            compCount = 3;
            for (int s = 0; s < kN; ++s)
            {
                const f32 t = dur * static_cast<f32>(s) / static_cast<f32>(kN - 1);
                const glm::vec3 v = ch.Evaluate(t, glm::vec3(0.0f));
                series[0].push_back(v.x);
                series[1].push_back(v.y);
                series[2].push_back(v.z);
            }
            for (const auto& k : ch.Keys)
                keyTimes.push_back(k.Time);
        };
        const auto sampleQuat = [&](const CinematicQuatChannel& ch)
        {
            compCount = 3;
            for (int s = 0; s < kN; ++s)
            {
                const f32 t = dur * static_cast<f32>(s) / static_cast<f32>(kN - 1);
                const glm::vec3 e = glm::degrees(glm::eulerAngles(ch.Evaluate(t, glm::quat(1.0f, 0.0f, 0.0f, 0.0f))));
                series[0].push_back(e.x);
                series[1].push_back(e.y);
                series[2].push_back(e.z);
            }
            for (const auto& k : ch.Keys)
                keyTimes.push_back(k.Time);
        };
        const auto sampleFloat = [&](const CinematicFloatChannel& ch, bool degrees)
        {
            compCount = 1;
            for (int s = 0; s < kN; ++s)
            {
                const f32 t = dur * static_cast<f32>(s) / static_cast<f32>(kN - 1);
                f32 v = ch.Evaluate(t, 0.0f);
                series[0].push_back(degrees ? glm::degrees(v) : v);
            }
            for (const auto& k : ch.Keys)
                keyTimes.push_back(k.Time);
        };

        switch (m_Selection.Kind)
        {
            case LaneKind::TransformTranslation:
                sampleVec3(m_Sequence->TransformTracks[ti].Translation);
                break;
            case LaneKind::TransformScale:
                sampleVec3(m_Sequence->TransformTracks[ti].Scale);
                break;
            case LaneKind::CameraPosition:
                sampleVec3(m_Sequence->CameraTracks[ti].Position);
                break;
            case LaneKind::TransformRotation:
                sampleQuat(m_Sequence->TransformTracks[ti].Rotation);
                break;
            case LaneKind::CameraRotation:
                sampleQuat(m_Sequence->CameraTracks[ti].Rotation);
                break;
            case LaneKind::CameraFov:
                sampleFloat(m_Sequence->CameraTracks[ti].VerticalFovRadians, /*degrees*/ true);
                break;
            case LaneKind::Visibility:
            {
                isStep = true;
                compCount = 1;
                const auto& tr = m_Sequence->VisibilityTracks[ti];
                for (int s = 0; s < kN; ++s)
                {
                    const f32 t = dur * static_cast<f32>(s) / static_cast<f32>(kN - 1);
                    series[0].push_back(tr.EvaluateAt(t, true) ? 1.0f : 0.0f);
                }
                for (const auto& k : tr.Keys)
                    keyTimes.push_back(k.Time);
                break;
            }
            case LaneKind::Event:
                ImGui::Separator();
                ImGui::TextDisabled("Event track — %zu event(s)", m_Sequence->EventTracks[ti].Keys.size());
                return;
        }

        if (compCount == 0 || series[0].empty())
        {
            return;
        }

        // Y-range across all active components.
        f32 vmin = series[0][0];
        f32 vmax = series[0][0];
        for (int c = 0; c < compCount; ++c)
        {
            for (f32 v : series[c])
            {
                vmin = std::min(vmin, v);
                vmax = std::max(vmax, v);
            }
        }
        if (vmax - vmin < 1e-4f)
        {
            vmin -= 1.0f;
            vmax += 1.0f;
        }
        const f32 pad = (vmax - vmin) * 0.1f;
        vmin -= pad;
        vmax += pad;

        ImGui::Separator();
        ImGui::TextDisabled("Curve preview (samples the interpolation across the sequence)");

        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const f32 w = ImGui::GetContentRegionAvail().x;
        const f32 h = std::max(60.0f, ImGui::GetContentRegionAvail().y - 4.0f);
        ImGui::Dummy(ImVec2(w, h));
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(origin, ImVec2(origin.x + w, origin.y + h), IM_COL32(24, 24, 28, 255));
        dl->PushClipRect(origin, ImVec2(origin.x + w, origin.y + h), true);

        const auto valueToY = [&](f32 v) -> f32
        {
            const f32 n = (v - vmin) / (vmax - vmin);
            return origin.y + h - n * h;
        };
        const auto timeToX = [&](f32 t) -> f32
        {
            const f32 n = (dur > 0.0f) ? (t / dur) : 0.0f;
            return origin.x + n * w;
        };

        // Playhead marker on the preview.
        const f32 phx = timeToX(std::min(m_Playhead, dur));
        dl->AddLine(ImVec2(phx, origin.y), ImVec2(phx, origin.y + h), IM_COL32(255, 220, 60, 120), 1.0f);

        // Component polylines.
        for (int c = 0; c < compCount; ++c)
        {
            const ImU32 col = (compCount == 1) ? IM_COL32(230, 200, 90, 255) : kAxisColor[c];
            std::vector<ImVec2> pts;
            pts.reserve(series[c].size());
            for (sizet s = 0; s < series[c].size(); ++s)
            {
                const f32 t = dur * static_cast<f32>(s) / static_cast<f32>(kN - 1);
                pts.push_back(ImVec2(timeToX(t), valueToY(series[c][s])));
            }
            dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), col, ImDrawFlags_None, isStep ? 2.0f : 1.5f);
        }

        // Key time ticks along the bottom.
        for (f32 kt : keyTimes)
        {
            const f32 x = timeToX(std::min(kt, dur));
            dl->AddTriangleFilled(ImVec2(x - 3.0f, origin.y + h), ImVec2(x + 3.0f, origin.y + h), ImVec2(x, origin.y + h - 6.0f), IM_COL32(200, 200, 210, 220));
        }

        dl->PopClipRect();
    }

    // ============================ toolbar ====================================

    void CinematicTimelinePanel::DrawToolbar()
    {
        if (ImGui::Button("Save"))
        {
            Save();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();

        // Playback controls.
        if (m_Playing)
        {
            if (ImGui::Button("Pause"))
                m_Playing = false;
        }
        else
        {
            if (ImGui::Button("Play"))
            {
                if (m_Playhead >= SequenceDuration())
                    m_Playhead = 0.0f;
                m_Playing = true;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Stop"))
        {
            m_Playing = false;
            m_Playhead = 0.0f;
            ApplyPlayhead();
        }
        ImGui::SameLine();
        ImGui::Checkbox("Loop", &m_PreviewLoop);

        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120.0f);
        ImGui::DragFloat("Time", &m_Playhead, 0.01f, 0.0f, 1e6f, "%.3fs");
        if (ImGui::IsItemDeactivatedAfterEdit() || ImGui::IsItemActive())
        {
            m_Playhead = std::max(0.0f, m_Playhead);
            ApplyPlayhead();
        }

        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();
        if (ImGui::Button("Add Track"))
        {
            ImGui::OpenPopup("##addtrack");
        }
        ImGui::SameLine();
        ImGui::Checkbox("Snap", &m_SnapEnabled);
        if (m_SnapEnabled)
        {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(80.0f);
            ImGui::DragFloat("##snap", &m_SnapSeconds, 0.005f, 0.01f, 1.0f, "%.3fs");
        }

        ImGui::SameLine();
        ImGui::SetNextItemWidth(120.0f);
        ImGui::DragFloat("Zoom", &m_PixelsPerSecond, 1.0f, s_MinPixelsPerSecond, s_MaxPixelsPerSecond, "%.0f px/s");

        // Sequence-level info / explicit duration.
        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();
        f32 explicitDur = m_Sequence->Duration;
        ImGui::SetNextItemWidth(130.0f);
        if (ImGui::DragFloat("Duration", &explicitDur, 0.05f, 0.0f, 1e6f, explicitDur > 0.0f ? "%.2fs" : "auto"))
        {
            m_Sequence->Duration = std::max(0.0f, explicitDur);
            MarkDirty();
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Explicit length. 0 = auto (derive from last keyframe = %.2fs).",
                              static_cast<double>(m_Sequence->ComputeDuration()));
        }
        ImGui::SameLine();
        ImGui::Text("(%s, %.2fs)%s", m_FilePath.empty() ? "unsaved" : m_FilePath.filename().string().c_str(),
                    static_cast<double>(SequenceDuration()), m_Dirty ? " *" : "");
    }

    void CinematicTimelinePanel::DrawAddTrackPopup()
    {
        if (!ImGui::BeginPopup("##addtrack"))
        {
            return;
        }

        ImGui::Text("New track");
        ImGui::Separator();
        const char* kinds[] = { "Transform", "Camera", "Visibility", "Event" };
        ImGui::SetNextItemWidth(160.0f);
        ImGui::Combo("Type", &m_AddTrackKind, kinds, 4);

        // Target entity picker (not needed for Event tracks).
        if (m_AddTrackKind != 3 && m_Context)
        {
            std::string current = "<none>";
            if (m_AddTrackTarget != 0)
            {
                if (auto e = m_Context->TryGetEntityWithUUID(m_AddTrackTarget))
                    current = e->GetName();
            }
            ImGui::SetNextItemWidth(160.0f);
            if (ImGui::BeginCombo("Target", current.c_str()))
            {
                for (auto entityHandle : m_Context->GetAllEntitiesWith<IDComponent>())
                {
                    Entity e{ entityHandle, m_Context.get() };
                    const UUID uuid = e.GetUUID();
                    char label[160];
                    std::snprintf(label, sizeof(label), "%s##%llu", e.GetName().c_str(), static_cast<unsigned long long>(static_cast<u64>(uuid)));
                    if (ImGui::Selectable(label, m_AddTrackTarget == uuid))
                        m_AddTrackTarget = uuid;
                }
                ImGui::EndCombo();
            }
        }

        const bool needsTarget = (m_AddTrackKind != 3);
        ImGui::BeginDisabled(needsTarget && m_AddTrackTarget == 0);
        if (ImGui::Button("Create"))
        {
            switch (m_AddTrackKind)
            {
                case 0:
                {
                    CinematicTransformTrack t;
                    t.Target = m_AddTrackTarget;
                    m_Sequence->TransformTracks.push_back(std::move(t));
                    break;
                }
                case 1:
                {
                    CinematicCameraTrack t;
                    t.Target = m_AddTrackTarget;
                    m_Sequence->CameraTracks.push_back(std::move(t));
                    break;
                }
                case 2:
                {
                    CinematicVisibilityTrack t;
                    t.Target = m_AddTrackTarget;
                    m_Sequence->VisibilityTracks.push_back(std::move(t));
                    break;
                }
                case 3:
                {
                    CinematicEventTrack t;
                    t.Name = "Events";
                    m_Sequence->EventTracks.push_back(std::move(t));
                    break;
                }
                default:
                    break;
            }
            MarkDirty();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
        {
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }

    // ============================ entry point ================================

    void CinematicTimelinePanel::OnImGuiRender(bool* open)
    {
        if (!ImGui::Begin("Cinematic Timeline", open))
        {
            ImGui::End();
            return;
        }
        m_Open = open ? *open : true;

        // Bring the tab to front the frame after a sequence is opened (double-click
        // in the Content Browser or "Edit in Timeline" on the component).
        if (m_FocusRequested)
        {
            ImGui::SetWindowFocus();
            m_FocusRequested = false;
        }

        if (!m_Sequence)
        {
            ImGui::TextWrapped("No cinematic sequence open.\n\nDouble-click a .olocine in the Content Browser, or use 'Edit in Timeline' from a Cinematic Sequence component to open one here.");
            ImGui::End();
            return;
        }

        if (m_Playing)
        {
            UpdatePreview(ImGui::GetIO().DeltaTime);
        }

        DrawToolbar();
        DrawAddTrackPopup();
        ImGui::Separator();

        // Split the remaining space: timeline (top) and key/curve inspector (bottom).
        const f32 avail = ImGui::GetContentRegionAvail().y;
        const f32 inspectorHeight = std::clamp(avail * 0.32f, 120.0f, 260.0f);
        const f32 timelineHeight = std::max(120.0f, avail - inspectorHeight - ImGui::GetStyle().ItemSpacing.y);

        ImGui::BeginChild("##timelineregion", ImVec2(0, timelineHeight), false);
        DrawTracksAndTimeline();
        ImGui::EndChild();

        DrawInspector();

        ImGui::End();
    }
} // namespace OloEngine
