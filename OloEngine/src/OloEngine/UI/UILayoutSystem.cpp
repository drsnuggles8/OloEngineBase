#include "OloEnginePCH.h"
#include "UILayoutSystem.h"

#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"

namespace OloEngine
{
    static void ResolveRect(Scene& scene, entt::entity entity, const glm::vec2& parentPos, const glm::vec2& parentSize)
    {
        OLO_PROFILE_FUNCTION();
        if (!scene.GetAllEntitiesWith<UIRectTransformComponent>().contains(entity))
        {
            return;
        }

        auto& rt = scene.GetAllEntitiesWith<UIRectTransformComponent>().get<UIRectTransformComponent>(entity);

        // Anchor-based layout resolution (Unity-style)
        const glm::vec2 anchorMinPos = parentPos + rt.m_AnchorMin * parentSize;
        const glm::vec2 anchorMaxPos = parentPos + rt.m_AnchorMax * parentSize;
        const glm::vec2 anchorSize = anchorMaxPos - anchorMinPos;

        glm::vec2 resolvedSize;
        glm::vec2 resolvedPos;

        if (glm::abs(rt.m_AnchorMin.x - rt.m_AnchorMax.x) < 1e-5f &&
            glm::abs(rt.m_AnchorMin.y - rt.m_AnchorMax.y) < 1e-5f)
        {
            // Non-stretched: size comes from SizeDelta
            resolvedSize = rt.m_SizeDelta * rt.m_Scale;
            resolvedPos = anchorMinPos + rt.m_AnchoredPosition - rt.m_Pivot * resolvedSize;
        }
        else
        {
            // Stretched: SizeDelta acts as inset adjustment
            resolvedSize = (anchorSize + rt.m_SizeDelta) * rt.m_Scale;
            resolvedPos = anchorMinPos + rt.m_AnchoredPosition - rt.m_Pivot * resolvedSize;
        }

        // Write transient resolved rect (add or replace to avoid assertion on duplicate)
        Entity ent{ entity, &scene };
        auto& resolved = ent.AddOrReplaceComponent<UIResolvedRectComponent>();
        resolved.m_Position = resolvedPos;
        resolved.m_Size = resolvedSize;

        // If this entity has a UIScrollViewComponent, offset all children by scroll position
        glm::vec2 childOffset{ 0.0f, 0.0f };
        if (ent.HasComponent<UIScrollViewComponent>())
        {
            const auto& scrollView = ent.GetComponent<UIScrollViewComponent>();
            childOffset = -scrollView.m_ScrollPosition;
        }

        // If this entity has a UIGridLayoutComponent, override child positions
        if (ent.HasComponent<UIGridLayoutComponent>() && ent.HasComponent<RelationshipComponent>())
        {
            const auto& grid = ent.GetComponent<UIGridLayoutComponent>();
            const auto& children = ent.GetComponent<RelationshipComponent>().m_Children;

            const f32 paddingLeft = grid.m_Padding.x;
            const f32 paddingTop = grid.m_Padding.z;
            const f32 availableWidth = resolvedSize.x - grid.m_Padding.x - grid.m_Padding.y;

            // Calculate columns
            i32 columns = grid.m_ConstraintCount;
            if (columns <= 0 && grid.m_StartAxis == UIGridLayoutAxis::Horizontal)
            {
                const f32 cellSpanX = grid.m_CellSize.x + grid.m_Spacing.x;
                columns = (cellSpanX > 0.0f)
                              ? glm::max(1, static_cast<i32>((availableWidth + grid.m_Spacing.x) / cellSpanX))
                              : 1;
            }
            else if (columns <= 0)
            {
                const f32 availableHeight = resolvedSize.y - grid.m_Padding.z - grid.m_Padding.w;
                const f32 cellSpanY = grid.m_CellSize.y + grid.m_Spacing.y;
                columns = (cellSpanY > 0.0f)
                              ? glm::max(1, static_cast<i32>((availableHeight + grid.m_Spacing.y) / cellSpanY))
                              : 1;
            }

            for (sizet i = 0; i < children.size(); ++i)
            {
                auto childOpt = scene.TryGetEntityWithUUID(children[i]);
                if (!childOpt)
                {
                    continue;
                }

                i32 row, col;
                if (grid.m_StartAxis == UIGridLayoutAxis::Horizontal)
                {
                    row = static_cast<i32>(i) / columns;
                    col = static_cast<i32>(i) % columns;
                }
                else
                {
                    col = static_cast<i32>(i) / columns;
                    row = static_cast<i32>(i) % columns;
                }

                // Flip based on start corner
                f32 cellX = paddingLeft + static_cast<f32>(col) * (grid.m_CellSize.x + grid.m_Spacing.x);
                f32 cellY = paddingTop + static_cast<f32>(row) * (grid.m_CellSize.y + grid.m_Spacing.y);

                if (grid.m_StartCorner == UIGridLayoutStartCorner::UpperRight || grid.m_StartCorner == UIGridLayoutStartCorner::LowerRight)
                {
                    cellX = resolvedSize.x - grid.m_Padding.y - grid.m_CellSize.x - static_cast<f32>(col) * (grid.m_CellSize.x + grid.m_Spacing.x);
                }
                if (grid.m_StartCorner == UIGridLayoutStartCorner::LowerLeft || grid.m_StartCorner == UIGridLayoutStartCorner::LowerRight)
                {
                    cellY = resolvedSize.y - grid.m_Padding.w - grid.m_CellSize.y - static_cast<f32>(row) * (grid.m_CellSize.y + grid.m_Spacing.y);
                }

                // Directly write resolved rect for grid child (overriding normal anchor layout)
                Entity childEnt{ static_cast<entt::entity>(*childOpt), &scene };
                auto& childResolved = childEnt.AddOrReplaceComponent<UIResolvedRectComponent>();
                childResolved.m_Position = resolvedPos + glm::vec2{ cellX, cellY } + childOffset;
                childResolved.m_Size = grid.m_CellSize;

                // Recursively resolve grandchildren normally (they anchor relative to grid cell)
                if (childEnt.HasComponent<RelationshipComponent>())
                {
                    for (const UUID grandchild : childEnt.GetComponent<RelationshipComponent>().m_Children)
                    {
                        auto grandchildOpt = scene.TryGetEntityWithUUID(grandchild);
                        if (grandchildOpt)
                        {
                            ResolveRect(scene, static_cast<entt::entity>(*grandchildOpt), childResolved.m_Position, childResolved.m_Size);
                        }
                    }
                }
            }
            return; // Children already handled by grid layout
        }

        // Recurse into children via RelationshipComponent
        if (ent.HasComponent<RelationshipComponent>())
        {
            const auto& children = ent.GetComponent<RelationshipComponent>().m_Children;
            for (const UUID childUUID : children)
            {
                auto childOpt = scene.TryGetEntityWithUUID(childUUID);
                if (childOpt)
                {
                    ResolveRect(scene, static_cast<entt::entity>(*childOpt), resolvedPos + childOffset, resolvedSize);
                }
            }
        }
    }

    void UILayoutSystem::ResolveLayout(Scene& scene, u32 viewportWidth, u32 viewportHeight)
    {
        OLO_PROFILE_FUNCTION();
        const glm::vec2 viewport{ static_cast<f32>(viewportWidth), static_cast<f32>(viewportHeight) };

        // Clear stale resolved rects so removed UIRectTransformComponents don't linger
        {
            auto resolvedView = scene.GetAllEntitiesWith<UIResolvedRectComponent>();
            std::vector<entt::entity> toRemove;
            for (const auto entity : resolvedView)
            {
                toRemove.push_back(entity);
            }
            for (const auto entity : toRemove)
            {
                Entity ent{ entity, &scene };
                ent.RemoveComponent<UIResolvedRectComponent>();
            }
        }

        auto canvasView = scene.GetAllEntitiesWith<UICanvasComponent>();
        for (const auto entity : canvasView)
        {
            auto& canvas = canvasView.get<UICanvasComponent>(entity);
            Entity canvasEntity{ entity, &scene };

            glm::vec2 canvasSize = viewport;
            glm::vec2 canvasPos{ 0.0f, 0.0f };

            if (canvas.m_RenderMode == UICanvasRenderMode::ScreenSpaceOverlay &&
                canvas.m_ScaleMode == UICanvasScaleMode::ScaleWithScreenSize)
            {
                const glm::vec2 targetSize = canvas.m_ReferenceResolution;
                if (targetSize.x > 0.0f && targetSize.y > 0.0f)
                {
                    const glm::vec2 scale = viewport / targetSize;
                    const f32 scaleFactor = glm::min(scale.x, scale.y);
                    canvasSize = targetSize * scaleFactor;
                    canvasPos = (viewport - canvasSize) * 0.5f;
                }
            }

            // Resolve the canvas entity and its children via a single tree walk
            if (canvasEntity.HasComponent<UIRectTransformComponent>())
            {
                ResolveRect(scene, entity, canvasPos, canvasSize);
            }
            else if (canvasEntity.HasComponent<RelationshipComponent>())
            {
                // Canvas has no rect transform: resolve children directly
                const auto& children = canvasEntity.GetComponent<RelationshipComponent>().m_Children;
                for (const UUID childUUID : children)
                {
                    auto childOpt = scene.TryGetEntityWithUUID(childUUID);
                    if (childOpt)
                    {
                        ResolveRect(scene, static_cast<entt::entity>(*childOpt), canvasPos, canvasSize);
                    }
                }
            }
        }
    }
} // namespace OloEngine
