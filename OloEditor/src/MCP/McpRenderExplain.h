#pragma once

// Pure, engine-light reasoning for the olo_render_why_not_visible "explain" tool
// (issue #306 item A, rendering half; the inspection counterpart called out by
// #316 Part 3). It is the rendering sibling of McpPhysicsExplain.h: the MCP tool
// handler in McpTools.cpp gathers raw facts off the live scene/renderer on the
// editor main thread, then hands them here to be turned into a human/machine
// "why isn't this on screen?" answer.
//
// Keeping the reasoning in a free function with NO EnTT / renderer / editor / GPU
// dependencies means it can be unit-tested directly (the test binary compiles the
// MCP dispatch core but deliberately NOT McpTools.cpp), and the headline tool's
// verdict cascade is covered without needing a live editor or GPU.
//
// Honesty constraint (see HANDOVER / CLAUDE.md): the renderer's per-frame
// occlusion (HZB) cull results and LOD selection are PRIVATE per-frame state with
// no editor-side query, so this tool deliberately does NOT claim a verdict about
// them — it reports them as not-observable in the terminal "should be visible"
// case rather than fabricating a cull reason. The camera-relative checks
// (behind-camera / frustum) are only evaluated when the editor camera pose and the
// entity's world-space bounds are both known; otherwise they are skipped honestly.
//
// Only OloEngine/Core/Base.h is pulled in (for the integer typedefs); everything
// else is the standard library. No glm — all vector/matrix math stays in the
// handler, which passes the resulting booleans in.

#include "OloEngine/Core/Base.h"

#include <string>
#include <vector>

namespace OloEngine::MCP::RenderExplain
{
    // The render-relevant facts about one entity, gathered from the live scene and
    // renderer on the main thread. Everything camera/bounds-derived is computed by
    // the handler so this header stays free of glm and the renderer.
    struct EntityRenderFacts
    {
        bool EntityExists = false; // the UUID resolves to an entity in the scene

        // Renderable presence. HasRenderable is true when the entity carries at
        // least one component the scene submits for drawing (MeshComponent,
        // ModelComponent, InstancedMeshComponent, SpriteRendererComponent,
        // CircleRendererComponent, TextComponent, ...). RenderableKind is the
        // human label of the primary one (empty when none).
        bool HasRenderable = false;
        std::string RenderableKind;

        // Geometry asset presence for the primary renderable. GeometryRequired is
        // false for renderables that need no asset (sprite/circle/text always draw
        // from their transform). GeometryPresent means the asset is non-null and
        // usable (mesh source present / model loaded / instanced has >=1 instance);
        // GeometryDetail carries a handler-built specifics string for the message.
        bool GeometryRequired = true;
        bool GeometryPresent = false;
        std::string GeometryDetail;

        // An explicit per-component on/off flag (ModelComponent.m_Visible /
        // SubmeshComponent.m_Visible / WaterComponent.m_Enabled, ...). Many
        // renderables have none — then HasVisibilityFlag is false and the gate is
        // skipped. VisibilityFlagName labels it for the message.
        bool HasVisibilityFlag = false;
        bool VisibilityFlagOn = true;
        std::string VisibilityFlagName;

        // The entity's local TransformComponent scale (exactly what the renderer
        // submits — there is no world-transform flattening in the submission path).
        // Degenerate means a component is ~0, collapsing the geometry to nothing.
        bool ScaleDegenerate = false;

        // Shader compile state for the entity material's OWN shader. Standard scene
        // meshes render through a shared deferred PBR shader rather than the
        // material's m_Shader, so this is only set when the material carries a named
        // shader we could resolve (custom-shader / shader-graph materials). A global
        // "any shader failing" hint lives on WhyNotVisibleInput instead.
        bool HasMaterialShader = false;
        std::string MaterialShaderName;
        bool MaterialShaderHasErrors = false;

        // Camera-relative facts. Only meaningful when BoundsKnown is true (the
        // entity had resolvable world-space bounds) AND WhyNotVisibleInput.CameraKnown
        // is true. BehindCamera: the bounds lie entirely behind the editor camera.
        // InFrustum: the bounds intersect the editor camera frustum.
        bool BoundsKnown = false;
        bool BehindCamera = false;
        bool InFrustum = true;
    };

    // Everything ExplainWhyNotVisible needs. The handler fills these from the live
    // scene/renderer; this function never touches the engine.
    struct WhyNotVisibleInput
    {
        bool SceneLoaded = false; // an active scene is open in the editor
        bool CameraKnown = false; // the editor camera pose was readable this frame

        // Global hint: at least one shader currently fails to compile. Not
        // attributable to a specific entity (the shared PBR mesh shader could be
        // among them), so it is surfaced as a warning in the terminal case rather
        // than as a hard per-entity verdict.
        bool AnyShaderHasErrors = false;
        int ShaderErrorCount = 0;

        EntityRenderFacts Entity;
    };

    struct WhyNotVisibleVerdict
    {
        // Machine-readable primary reason. One of:
        //   no_scene, entity_missing, not_renderable, geometry_missing,
        //   component_hidden, degenerate_scale, shader_compile_error,
        //   behind_camera, outside_frustum, should_be_visible.
        std::string ReasonCode;
        // One-line human explanation of the primary reason.
        std::string Summary;
        // Ordered trace of every check performed, each prefixed "[ok]"/"[fail]"/
        // "[warn]", so the agent can see exactly how far the chain got.
        std::vector<std::string> Checks;
        // True once the entity is configured such that it WOULD render if in view
        // (passed the renderable / geometry / visible-flag / scale / shader gates).
        // The camera-relative reasons (behind_camera / outside_frustum) leave this
        // true — the object is fine, it is just not in the current view.
        bool RenderableConfigOk = false;
        // True only when the entity is configured to render AND was positively
        // confirmed to be inside the current editor camera frustum. False whenever
        // a gate failed, the object is off-screen, or the camera/bounds were
        // unknown (so we could not confirm it is on screen).
        bool Visible = false;
    };

    // Map the gathered facts to a verdict. A deterministic cascade from the most
    // fundamental precondition (scene loaded / entity exists / is renderable) down
    // through the configuration gates (geometry asset, visibility flag, scale,
    // shader compile) and finally the camera-relative checks (behind camera /
    // frustum). Stops and reports the first blocker, so ReasonCode is always the
    // *root* cause rather than a downstream symptom.
    [[nodiscard]] inline WhyNotVisibleVerdict ExplainWhyNotVisible(const WhyNotVisibleInput& in)
    {
        WhyNotVisibleVerdict verdict;
        auto& checks = verdict.Checks;
        const EntityRenderFacts& e = in.Entity;

        if (!in.SceneLoaded)
        {
            checks.push_back("[fail] no active scene is loaded in the editor");
            verdict.ReasonCode = "no_scene";
            verdict.Summary = "No active scene is loaded in the editor, so there is nothing to render. Open a scene first.";
            return verdict;
        }
        checks.push_back("[ok] an active scene is loaded");

        if (!e.EntityExists)
        {
            checks.push_back("[fail] the entity does not exist in the active scene");
            verdict.ReasonCode = "entity_missing";
            verdict.Summary = "That entity does not exist in the active scene — check the UUID with olo_scene_list_entities.";
            return verdict;
        }
        checks.push_back("[ok] the entity exists in the active scene");

        if (!e.HasRenderable)
        {
            checks.push_back("[fail] the entity has no renderable component");
            verdict.ReasonCode = "not_renderable";
            verdict.Summary = "The entity has no renderable component (no MeshComponent / ModelComponent / "
                              "SpriteRendererComponent / CircleRendererComponent / TextComponent / InstancedMeshComponent), "
                              "so the scene never submits it for drawing. Add the renderable component you expect it to have.";
            return verdict;
        }
        checks.push_back(std::string("[ok] the entity has a renderable component (") + e.RenderableKind + ")");

        if (e.GeometryRequired && !e.GeometryPresent)
        {
            checks.push_back(std::string("[fail] ") + e.RenderableKind + " has no usable geometry to draw");
            verdict.ReasonCode = "geometry_missing";
            verdict.Summary = std::string("The ") + e.RenderableKind + " has no usable geometry to draw" +
                              (e.GeometryDetail.empty() ? std::string() : std::string(" (") + e.GeometryDetail + ")") +
                              ", so the scene skips it at submission. Assign a mesh/model asset (or, for an "
                              "InstancedMeshComponent, add at least one instance).";
            return verdict;
        }
        if (e.GeometryRequired)
            checks.push_back(std::string("[ok] ") + e.RenderableKind + " has geometry to draw");

        if (e.HasVisibilityFlag && !e.VisibilityFlagOn)
        {
            checks.push_back(std::string("[fail] ") + e.VisibilityFlagName + " is off");
            verdict.ReasonCode = "component_hidden";
            verdict.Summary = std::string("The renderable is explicitly hidden: ") + e.VisibilityFlagName +
                              " is off, so the scene skips drawing it. Turn it back on to make the entity visible.";
            return verdict;
        }
        if (e.HasVisibilityFlag)
            checks.push_back(std::string("[ok] ") + e.VisibilityFlagName + " is on");

        if (e.ScaleDegenerate)
        {
            checks.push_back("[fail] the entity's transform scale has a zero (or near-zero) component");
            verdict.ReasonCode = "degenerate_scale";
            verdict.Summary = "The entity's transform scale has a zero (or near-zero) component, which collapses its "
                              "geometry to nothing — it is submitted but draws with no visible area. Set a non-zero "
                              "scale on all three axes.";
            return verdict;
        }
        checks.push_back("[ok] the entity's transform scale is non-degenerate");

        if (e.HasMaterialShader && e.MaterialShaderHasErrors)
        {
            checks.push_back(std::string("[fail] the material's shader '") + e.MaterialShaderName + "' failed to compile");
            verdict.ReasonCode = "shader_compile_error";
            verdict.Summary = std::string("The material's shader '") + e.MaterialShaderName +
                              "' failed to compile, so the entity cannot be shaded and does not appear. Inspect the "
                              "error with olo_shader_get (name '" +
                              e.MaterialShaderName +
                              "') and fix the shader source.";
            return verdict;
        }
        if (e.HasMaterialShader)
            checks.push_back(std::string("[ok] the material's shader '") + e.MaterialShaderName + "' compiled cleanly");

        // Past this point the entity is configured to render. Camera-relative
        // reasons below do not change that — they mean "fine, just not in view".
        verdict.RenderableConfigOk = true;

        const bool cameraChecksPossible = in.CameraKnown && e.BoundsKnown;
        if (cameraChecksPossible)
        {
            if (e.BehindCamera)
            {
                checks.push_back("[fail] the entity is entirely behind the editor camera");
                verdict.ReasonCode = "behind_camera";
                verdict.Summary = "The entity is configured to render but lies entirely behind the editor camera, so it "
                                  "is off-screen. Point the camera at it with olo_camera_frame_entity (or orbit/move the "
                                  "view), then olo_screenshot.";
                return verdict;
            }
            checks.push_back("[ok] the entity is in front of the editor camera");

            if (!e.InFrustum)
            {
                checks.push_back("[fail] the entity is outside the editor camera frustum");
                verdict.ReasonCode = "outside_frustum";
                verdict.Summary = "The entity is configured to render but its bounds fall outside the editor camera "
                                  "frustum (off to the side, or beyond the near/far clip), so it is culled from the "
                                  "current view. Frame it with olo_camera_frame_entity, then olo_screenshot.";
                return verdict;
            }
            checks.push_back("[ok] the entity is inside the editor camera frustum");
        }
        else
        {
            checks.push_back("[warn] camera-relative checks skipped (editor camera pose or entity bounds unavailable)");
        }

        // Everything observable checks out.
        verdict.ReasonCode = "should_be_visible";
        verdict.Visible = cameraChecksPossible; // only assert on-screen when we could actually test the camera

        std::string summary = "The entity is configured to render";
        if (cameraChecksPossible)
            summary += " and is inside the current editor camera frustum, so it should be on screen right now";
        else
            summary += " (the camera-relative checks could not be evaluated this frame)";
        summary += ". If you still cannot see it, the remaining causes are not observable from here: it may be "
                   "occluded by another object (HZB occlusion culling) or swapped to an invisible LOD level — neither "
                   "the per-frame occlusion result nor the selected LOD is queryable from the editor. Capture the frame "
                   "with olo_screenshot (or olo_render_capture_target for the G-buffer/depth), and try "
                   "olo_camera_frame_entity to rule out the camera.";
        if (in.AnyShaderHasErrors)
        {
            checks.push_back(std::string("[warn] ") + std::to_string(in.ShaderErrorCount) +
                             " shader(s) currently fail to compile — a broken shared shader can hide many objects");
            summary += " Note: " + std::to_string(in.ShaderErrorCount) +
                       " shader(s) currently fail to compile (see olo_shader_errors); a broken shared mesh shader can "
                       "stop this object rendering even though its own configuration is fine.";
        }
        verdict.Summary = summary;
        return verdict;
    }
} // namespace OloEngine::MCP::RenderExplain
