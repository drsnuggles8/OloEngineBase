// Virtualized-geometry debug overlay (issue #629).
//
// VirtualGeometryPass writes its cluster-id / LOD / overdraw visualization into an
// RGBA8 image (VirtualMeshRegistry's debug colour target), which up to now was only
// reachable as an MCP capture target — so flipping the Statistics panel's "Debug view"
// combo changed nothing a human could see in the viewport.
//
// This composites that image over the LIT scene colour at the end of
// DeferredLightingPass. The debug image is cleared to alpha = 0 each frame and written
// with alpha = 1 only where a virtual-geometry fragment landed, so:
//
//   * virtual geometry  -> replaced by its flat debug colour
//   * everything else   -> left exactly as lit
//
// which is more useful than UE's equivalent (a fully flat frame): the Nanite geometry
// is colour-coded IN PLACE, in the real scene, so you can see which of the things in
// front of you are virtualized and which are not.
//
// Deliberately NOT tonemapped/gamma-corrected: these are identity colours (a cluster-id
// hash, a LOD ramp, an overdraw heat ramp), not radiance. Pushing them through the
// tonemapper would compress the ramp and make adjacent LOD levels indistinguishable.
// The overlay therefore runs after lighting but writes raw values, and the post-process
// chain's tonemap is what the surrounding lit pixels get — the debug pixels are meant to
// read as "not part of the image", and they do.

#type vertex
#version 450 core

layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec2 a_TexCoord;

void main()
{
    gl_Position = vec4(a_Position, 1.0);
}

#type fragment
#version 450 core

layout(location = 0) out vec4 o_Color;

layout(binding = 0) uniform sampler2D u_VirtualDebugColor;

void main()
{
    vec4 dbg = texelFetch(u_VirtualDebugColor, ivec2(gl_FragCoord.xy), 0);

    // Nothing virtual was rasterized here — keep the lit pixel. discard rather than
    // blending by alpha so the scene colour attachment is genuinely untouched (the
    // overlay must not disturb the alpha channel other passes may rely on).
    if (dbg.a < 0.5)
        discard;

    o_Color = vec4(dbg.rgb, 1.0);
}
