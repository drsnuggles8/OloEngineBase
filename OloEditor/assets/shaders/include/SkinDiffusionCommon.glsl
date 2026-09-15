#ifndef SKIN_DIFFUSION_COMMON_GLSL
#define SKIN_DIFFUSION_COMMON_GLSL

// =============================================================================
// SkinDiffusionCommon.glsl — the encoding of the diffusion hand-off, issue #1241.
//
// Small and dependency-free ON PURPOSE. It is shared by the PRODUCERS (every
// lit pass, through PBRCommon.glsl) and by the CONSUMER (SkinDiffusion.glsl,
// a fullscreen pass that wants none of PBRCommon's two thousand lines and none
// of its UBO declarations, which would collide with its own). The encoding is
// the only thing the two halves have to agree about, so the encoding is the only
// thing in here.
//
// WHAT THE HAND-OFF IS. Every lit pass that can shade skin writes a second
// render target beside scene colour, holding the DIFFUSE half of a skin pixel's
// lighting and the identity of the profile that should blur it.
// SkinDiffusion.glsl blurs that target with the profile's kernel and adds the
// DIFFERENCE back into scene colour.
//
// SCENE COLOUR STILL GETS THE WHOLE COMPOSITE. That is what makes the feature
// fail safe: with the diffusion pass culled, disabled, or not yet run, the frame
// is exactly the #1231 frame rather than a head missing its diffuse lighting.
// The pass adds `blur(aux) - aux`, which is zero when the blur is an identity.
//
// THE ALPHA LANE IS AN IDENTITY, NOT A WEIGHT. 0 means "no diffusion here" —
// what every non-skin surface writes, and what a skin surface whose profile is
// authored against transport version 0 writes too. A slot s in [0, 6] encodes as
// (s + 1) / 8: exact in a half float, and never 0 for a real slot, so a cleared
// target and slot 0 cannot be confused.
// =============================================================================

#define OLO_SKIN_DIFFUSE_SLOT_SCALE 0.125

// "This texel names no skin profile." Mirrors kSkinProfileSlotNone in
// Renderer/SkinProfile.h, and repeated here rather than taken from PBRCommon.glsl
// so this file stays standalone — ShaderUnit_SkinDiffusionEncoding.glsl asserts
// the two agree.
#define OLO_SKIN_DIFFUSE_SLOT_NONE 7

// The slot an aux texel names, or OLO_SKIN_DIFFUSE_SLOT_NONE when it names none.
//
// Rounds rather than truncates: the lane survives a half-float store, a
// multisample resolve and a bilinear fetch that lands a hair off a texel centre,
// and every one of those perturbs it in the last bits.
int oloSkinDiffusionSlot(float encoded)
{
    int slot = int(encoded / OLO_SKIN_DIFFUSE_SLOT_SCALE + 0.5) - 1;
    return (slot < 0 || slot >= OLO_SKIN_DIFFUSE_SLOT_NONE) ? OLO_SKIN_DIFFUSE_SLOT_NONE : slot;
}

// The inverse: the lane value naming `slot`.
float oloSkinDiffusionEncodeSlot(int slot)
{
    return (slot < 0 || slot >= OLO_SKIN_DIFFUSE_SLOT_NONE)
               ? 0.0
               : float(slot + 1) * OLO_SKIN_DIFFUSE_SLOT_SCALE;
}

#endif // SKIN_DIFFUSION_COMMON_GLSL
