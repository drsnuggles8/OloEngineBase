#pragma once

// Capacity-versus-demand reporters for the renderer's process-wide owners (issue #1342):
// the shadow maps, the GPU Scene tables, the ray-tracing scene and its surface caches, and
// the groom pass. Owners that are objects (RenderGraph, SceneRenderPass, ReSTIRPTPass,
// VulkanFrameArena) register their own; these are the ones reached through Renderer3D's
// statics, so they register once, from Renderer::Init, and read Renderer3D only while it
// is initialized.
namespace OloEngine::RendererMemoryOwners
{
    void Register();
    void Unregister();
} // namespace OloEngine::RendererMemoryOwners
