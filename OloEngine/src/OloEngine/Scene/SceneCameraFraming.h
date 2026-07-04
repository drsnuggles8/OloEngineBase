#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"

namespace OloEngine
{
    class Scene;
    class EditorCamera;

    // @brief Point `camera` at the entity identified by `entityUuid` in `scene`
    // and fit its world-space bounds in view, preserving the camera's current
    // view direction (yaw/pitch) and re-pivoting/zooming to frame the entity.
    //
    // Shared by `EditorLayer::FrameEditorCameraOnEntity` (the live editor's
    // `olo_camera_frame_entity` wiring) and `McpHeadlessHost` (the headless MCP
    // test host's wiring of the same tool) so the bounds-computation + fit
    // logic lives in exactly one place.
    //
    // Bounds preference: `ModelComponent` / `MeshComponent` world-space
    // bounding box (composed up the parent chain), then a `TerrainComponent`
    // footprint, falling back to a radius derived from the entity's world
    // transform scale when no renderable geometry is present. The fit distance
    // is derived from `camera`'s current vertical FOV so the bounding sphere
    // fills the frame regardless of FOV.
    //
    // Returns false (leaving `camera` untouched) if `scene` is null or
    // `entityUuid` does not resolve to an entity with a `TransformComponent`.
    bool FrameCameraOnEntity(const Ref<Scene>& scene, u64 entityUuid, EditorCamera& camera);
} // namespace OloEngine
