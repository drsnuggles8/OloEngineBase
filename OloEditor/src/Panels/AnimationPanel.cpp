#include "AnimationPanel.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Core/Timestep.h"
#include "OloEngine/Animation/AnimationSystem.h"
#include "OloEngine/Renderer/Renderer3D.h"

#include <imgui.h>
#include <imgui_internal.h>

namespace OloEngine
{
    AnimationPanel::AnimationPanel(const Ref<Scene>& context)
    {
        SetContext(context);
    }

    void AnimationPanel::SetContext(const Ref<Scene>& context)
    {
        m_Context = context;
        m_SelectedEntity = {};
    }

    void AnimationPanel::SetSelectedEntity(Entity entity)
    {
        m_SelectedEntity = entity;
    }

    void AnimationPanel::OnImGuiRender()
    {
        ImGui::Begin("Animation");

        if (!m_Context)
        {
            ImGui::Text("No scene context");
            ImGui::End();
            return;
        }

        if (!m_SelectedEntity)
        {
            ImGui::Text("Select an entity with animation components");
            ImGui::End();
            return;
        }

        // Validate entity belongs to current context (prevent crash on scene switch)
        if (m_SelectedEntity.GetScene() != m_Context.get())
        {
            ImGui::Text("Entity belongs to different scene");
            ImGui::End();
            return;
        }

        // Check if entity has animation components
        bool hasAnimationState = m_SelectedEntity.HasComponent<AnimationStateComponent>();
        bool hasSkeleton = m_SelectedEntity.HasComponent<SkeletonComponent>();

        if (!hasAnimationState && !hasSkeleton)
        {
            ImGui::Text("Selected entity has no animation components");
            ImGui::Text("Add AnimationStateComponent or SkeletonComponent to enable animation");
            ImGui::End();
            return;
        }

        // Entity name header
        if (m_SelectedEntity.HasComponent<TagComponent>())
        {
            auto& tag = m_SelectedEntity.GetComponent<TagComponent>().Tag;
            ImGui::Text("Entity: %s", tag.c_str());
            ImGui::Separator();
        }

        DrawAnimationControls(m_SelectedEntity);

        ImGui::Separator();

        DrawAnimationTimeline(m_SelectedEntity);

        if (hasSkeleton)
        {
            ImGui::Separator();
            DrawSkeletonVisualization(m_SelectedEntity);

            ImGui::Separator();
            DrawBoneHierarchy(m_SelectedEntity);
        }

        ImGui::End();
    }

    void AnimationPanel::DrawAnimationControls(Entity entity)
    {
        if (!entity.HasComponent<AnimationStateComponent>())
        {
            ImGui::Text("No AnimationStateComponent");
            return;
        }

        auto& animState = entity.GetComponent<AnimationStateComponent>();

        ImGui::Text("Animation Playback");

        // Playback controls
        ImGui::BeginGroup();
        {
            // Play/Pause button - sync with component state
            if (animState.m_IsPlaying)
            {
                if (ImGui::Button("Pause##AnimPlayback"))
                {
                    animState.m_IsPlaying = false;
                    m_IsPlaying = false;
                }
            }
            else
            {
                if (ImGui::Button("Play##AnimPlayback"))
                {
                    animState.m_IsPlaying = true;
                    m_IsPlaying = true;
                }
            }

            ImGui::SameLine();

            // Stop button (reset to beginning)
            if (ImGui::Button("Stop##AnimPlayback"))
            {
                animState.m_IsPlaying = false;
                m_IsPlaying = false;
                animState.m_CurrentTime = 0.0f;
            }

            ImGui::SameLine();

            // Step backward
            if (ImGui::Button("<<##AnimPlayback"))
            {
                animState.m_CurrentTime = std::max(0.0f, animState.m_CurrentTime - 0.1f);
            }

            ImGui::SameLine();

            // Step forward
            if (ImGui::Button(">>##AnimPlayback"))
            {
                animState.m_CurrentTime += 0.1f;
            }
        }
        ImGui::EndGroup();

        // Playback speed
        ImGui::DragFloat("Playback Speed##AnimPlayback", &m_PlaybackSpeed, 0.01f, 0.0f, 5.0f);

        // Loop toggle
        ImGui::Checkbox("Loop##AnimPlayback", &m_LoopPlayback);

        ImGui::Separator();

        // Animation clip selector dropdown
        if (!animState.m_AvailableClips.empty())
        {
            int selectedClip = animState.m_CurrentClipIndex;
            std::string previewLabel = "(none)";
            if (selectedClip >= 0 && selectedClip < static_cast<int>(animState.m_AvailableClips.size()))
            {
                const auto& clip = animState.m_AvailableClips[selectedClip];
                if (clip)
                {
                    previewLabel = clip->Name.empty() ? "(unnamed)" : clip->Name;
                }
            }

            if (ImGui::BeginCombo("Animation Clip##AnimControl", previewLabel.c_str()))
            {
                for (int i = 0; i < static_cast<int>(animState.m_AvailableClips.size()); i++)
                {
                    const auto& clip = animState.m_AvailableClips[i];
                    bool isSelected = (i == selectedClip);
                    std::string itemLabel = clip ? (clip->Name.empty() ? "(unnamed)" : clip->Name) : "(null)";
                    itemLabel += "##" + std::to_string(i); // Ensure unique ID

                    if (ImGui::Selectable(itemLabel.c_str(), isSelected))
                    {
                        animState.m_CurrentClipIndex = i;
                        animState.m_CurrentClip = animState.m_AvailableClips[i];
                        animState.m_CurrentTime = 0.0f;
                        OLO_CORE_INFO("Switched to animation [{}]: '{}'", i,
                                      animState.m_CurrentClip ? animState.m_CurrentClip->Name : "(null)");
                    }

                    if (isSelected)
                    {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }
        }
        else if (animState.m_CurrentClip)
        {
            // Legacy case: single clip without available clips list
            ImGui::Text("Single Clip Mode (re-import for full list)");
        }
        else
        {
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), "No animation clips available");
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Use 'Import Animated Model...' to load");
        }

        // Current clip info
        if (animState.m_CurrentClip)
        {
            ImGui::Text("Current Clip: %s",
                        animState.m_CurrentClip->Name.empty() ? "(unnamed)" : animState.m_CurrentClip->Name.c_str());
            ImGui::Text("Duration: %.2f s", animState.m_CurrentClip->Duration);
            ImGui::Text("Current Time: %.3f s", animState.m_CurrentTime);
        }
        else
        {
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), "Current Clip: None");
        }

        // Blending info
        if (animState.m_Blending)
        {
            ImGui::Text("Blending to next clip...");
            ImGui::ProgressBar(animState.m_BlendFactor, ImVec2(-1, 0), "Blend");
            ImGui::DragFloat("Blend Duration##AnimControl", &animState.m_BlendDuration, 0.01f, 0.0f, 5.0f);
        }

        // Update animation if playing (editor preview mode)
        if (animState.m_IsPlaying && animState.m_CurrentClip)
        {
            // Get delta time from ImGui (for editor preview only)
            f32 deltaTime = ImGui::GetIO().DeltaTime * m_PlaybackSpeed;

            // Get clip duration from the actual clip
            f32 clipDuration = animState.m_CurrentClip->Duration;

            // Update skeleton transforms if we have a skeleton
            if (entity.HasComponent<SkeletonComponent>())
            {
                auto& skelComp = entity.GetComponent<SkeletonComponent>();
                if (skelComp.m_Skeleton)
                {
                    // Call AnimationSystem to compute bone transforms
                    Animation::AnimationSystem::Update(animState, *skelComp.m_Skeleton, deltaTime);
                }
            }
            else
            {
                // No skeleton, just advance time manually
                animState.m_CurrentTime += deltaTime;
            }

            // Handle looping
            if (animState.m_CurrentTime > clipDuration)
            {
                if (m_LoopPlayback)
                {
                    animState.m_CurrentTime = std::fmod(animState.m_CurrentTime, clipDuration);
                }
                else
                {
                    animState.m_CurrentTime = clipDuration;
                    m_IsPlaying = false;
                }
            }
        }
    }

    void AnimationPanel::DrawAnimationTimeline(Entity entity)
    {
        if (!entity.HasComponent<AnimationStateComponent>())
            return;

        auto& animState = entity.GetComponent<AnimationStateComponent>();

        ImGui::Text("Timeline");

        // Timeline zoom controls
        ImGui::SetNextItemWidth(100.0f);
        ImGui::DragFloat("Zoom##Timeline", &m_TimelineZoom, 0.1f, 0.1f, 10.0f);
        ImGui::SameLine();
        if (ImGui::Button("Reset Zoom##Timeline"))
        {
            m_TimelineZoom = 1.0f;
            m_TimelineOffset = 0.0f;
        }

        // Get actual clip duration
        f32 clipDuration = animState.m_CurrentClip ? animState.m_CurrentClip->Duration : 2.0f;

        ImGui::SetNextItemWidth(-1);
        if (ImGui::SliderFloat("##TimelineScrubber", &animState.m_CurrentTime, 0.0f, clipDuration, "Time: %.3f s"))
        {
            // User is scrubbing, pause playback
            m_IsPlaying = false;
        }

        // Visual timeline representation
        ImVec2 timelinePos = ImGui::GetCursorScreenPos();
        ImVec2 timelineSize = ImVec2(ImGui::GetContentRegionAvail().x, 40.0f);

        ImDrawList* drawList = ImGui::GetWindowDrawList();

        // Timeline background
        drawList->AddRectFilled(
            timelinePos,
            ImVec2(timelinePos.x + timelineSize.x, timelinePos.y + timelineSize.y),
            IM_COL32(40, 40, 40, 255));

        // Timeline border
        drawList->AddRect(
            timelinePos,
            ImVec2(timelinePos.x + timelineSize.x, timelinePos.y + timelineSize.y),
            IM_COL32(80, 80, 80, 255));

        // Time markers
        f32 visibleDuration = clipDuration / m_TimelineZoom;
        f32 pixelsPerSecond = timelineSize.x / visibleDuration;
        f32 markerInterval = 0.5f; // Marker every 0.5 seconds

        for (f32 t = 0.0f; t <= clipDuration; t += markerInterval)
        {
            f32 x = timelinePos.x + (t - m_TimelineOffset) * pixelsPerSecond;
            if (x >= timelinePos.x && x <= timelinePos.x + timelineSize.x)
            {
                drawList->AddLine(
                    ImVec2(x, timelinePos.y),
                    ImVec2(x, timelinePos.y + 10.0f),
                    IM_COL32(100, 100, 100, 255));
            }
        }

        // Playhead
        f32 playheadX = timelinePos.x + (animState.m_CurrentTime - m_TimelineOffset) * pixelsPerSecond;
        if (playheadX >= timelinePos.x && playheadX <= timelinePos.x + timelineSize.x)
        {
            drawList->AddLine(
                ImVec2(playheadX, timelinePos.y),
                ImVec2(playheadX, timelinePos.y + timelineSize.y),
                IM_COL32(255, 100, 100, 255),
                2.0f);

            // Playhead triangle
            drawList->AddTriangleFilled(
                ImVec2(playheadX - 5.0f, timelinePos.y),
                ImVec2(playheadX + 5.0f, timelinePos.y),
                ImVec2(playheadX, timelinePos.y + 8.0f),
                IM_COL32(255, 100, 100, 255));
        }

        // Make timeline interactive
        ImGui::InvisibleButton("##TimelineInteract", timelineSize);
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
        {
            ImVec2 mousePos = ImGui::GetMousePos();
            f32 normalizedX = (mousePos.x - timelinePos.x) / timelineSize.x;
            animState.m_CurrentTime = std::clamp(normalizedX * visibleDuration + m_TimelineOffset, 0.0f, clipDuration);
            m_IsPlaying = false;
        }

        ImGui::Dummy(ImVec2(0, 10)); // Spacing after timeline
    }

    void AnimationPanel::DrawBoneHierarchy(Entity entity)
    {
        if (!entity.HasComponent<SkeletonComponent>())
            return;

        auto& skelComp = entity.GetComponent<SkeletonComponent>();

        if (ImGui::CollapsingHeader("Bone Hierarchy", ImGuiTreeNodeFlags_DefaultOpen))
        {
            if (!skelComp.m_Skeleton)
            {
                ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), "No skeleton loaded");
                return;
            }

            ImGui::Text("Total Bones: %zu", skelComp.m_Skeleton->m_BoneNames.size());

            // Show bone entity mappings if available
            if (entity.HasComponent<AnimationStateComponent>())
            {
                auto& animState = entity.GetComponent<AnimationStateComponent>();
                ImGui::Text("Mapped Bone Entities: %zu", animState.m_BoneEntityIds.size());
            }

            ImGui::Separator();

            // Bone list with actual names from skeleton
            if (ImGui::BeginChild("BoneList", ImVec2(0, 200), true))
            {
                auto boneCount = skelComp.m_Skeleton->m_BoneNames.size();
                for (size_t i = 0; i < boneCount; ++i)
                {
                    // Use actual bone name from skeleton
                    const std::string& boneName = skelComp.m_Skeleton->m_BoneNames[i];
                    std::string displayName = boneName.empty()
                                                  ? "Bone " + std::to_string(i)
                                                  : boneName;

                    // Show parent info in tooltip
                    if (ImGui::Selectable(displayName.c_str()))
                    {
                        // TODO: Select bone entity in hierarchy when clicked
                    }

                    // Tooltip with bone details
                    if (ImGui::IsItemHovered())
                    {
                        ImGui::BeginTooltip();
                        ImGui::Text("Bone Index: %zu", i);
                        if (i < skelComp.m_Skeleton->m_ParentIndices.size())
                        {
                            i32 parentIdx = skelComp.m_Skeleton->m_ParentIndices[i];
                            if (parentIdx >= 0 && parentIdx < static_cast<i32>(boneCount))
                            {
                                ImGui::Text("Parent: %s", skelComp.m_Skeleton->m_BoneNames[parentIdx].c_str());
                            }
                            else
                            {
                                ImGui::Text("Parent: (root)");
                            }
                        }
                        ImGui::EndTooltip();
                    }
                }
            }
            ImGui::EndChild();

            // Cache management
            if (ImGui::Button("Invalidate Bone Cache"))
            {
                skelComp.InvalidateCache();
            }
            ImGui::SameLine();
            ImGui::TextDisabled("(?)");
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("Invalidates the tag-to-entity cache.\nUse after modifying bone entity structure.");
            }
        }
    }

    void AnimationPanel::DrawSkeletonVisualization(Entity entity)
    {
        if (!entity.HasComponent<SkeletonComponent>())
            return;

        auto& skelComp = entity.GetComponent<SkeletonComponent>();

        if (ImGui::CollapsingHeader("Skeleton Visualization", ImGuiTreeNodeFlags_DefaultOpen))
        {
            if (!skelComp.m_Skeleton)
            {
                ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), "No skeleton to visualize");
                return;
            }

            bool settingsChanged = false;

            // Main toggle
            if (ImGui::Checkbox("Show Skeleton", &m_ShowSkeleton))
            {
                settingsChanged = true;
            }

            if (m_ShowSkeleton)
            {
                ImGui::Indent();

                // Sub-toggles
                if (ImGui::Checkbox("Show Bones", &m_ShowBones))
                {
                    settingsChanged = true;
                }
                ImGui::SameLine();
                if (ImGui::Checkbox("Show Joints", &m_ShowJoints))
                {
                    settingsChanged = true;
                }

                // Size controls
                if (ImGui::DragFloat("Joint Size", &m_JointSize, 0.001f, 0.005f, 0.1f, "%.3f"))
                {
                    settingsChanged = true;
                }
                if (ImGui::DragFloat("Bone Thickness", &m_BoneThickness, 0.1f, 0.5f, 5.0f, "%.1f"))
                {
                    settingsChanged = true;
                }

                ImGui::Unindent();

                // Show info
                ImGui::TextColored(ImVec4(0.5f, 0.8f, 0.5f, 1.0f), "Skeleton rendering enabled");
                ImGui::Text("Bones: %zu, Joints: %zu",
                            skelComp.m_Skeleton->m_GlobalTransforms.size(),
                            skelComp.m_Skeleton->m_BoneNames.size());
            }

            // Sync settings with the scene for rendering
            if (settingsChanged && m_Context)
            {
                Scene::SkeletonVisualizationSettings settings;
                settings.ShowSkeleton = m_ShowSkeleton;
                settings.ShowBones = m_ShowBones;
                settings.ShowJoints = m_ShowJoints;
                settings.JointSize = m_JointSize;
                settings.BoneThickness = m_BoneThickness;
                m_Context->SetSkeletonVisualization(settings);
            }
        }
    }
} // namespace OloEngine
