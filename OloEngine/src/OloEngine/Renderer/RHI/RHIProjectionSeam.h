#pragma once

// =============================================================================
// RHIProjectionSeam.h — the backend projection-convention seam (ADR 0011,
// issue #691 Phase 7 Wave C, ADR item A8).
//
// The engine authors every projection in GL clip conventions (glm RH_NO:
// y up, z in [-1, 1]). Vulkan's fixed function wants y down and clip z in
// [0, w]. The DECIDED design (phase 1.4) bakes the difference into the
// GPU-VISIBLE matrices at upload time, at these two functions and nowhere
// else, so that every shader stays source-identical between the backends and
// CPU-side math (culling, picking, cascade fitting) keeps GL-convention
// matrices throughout.
//
// TWO functions, because uploaded matrices have two DIFFERENT consumers:
//
// 1. AdjustProjectionForBackend — for matrices the RASTERIZER consumes
//    (anything a vertex stage feeds to gl_Position: CameraUBO's
//    ViewProjection/Projection, the shadow camera, DDGI capture faces, sky
//    bakes, the asset-preview block). On Vulkan it premultiplies
//        F = | 1   0   0   0 |      row1' = -row1        (y flip)
//            | 0  -1   0   0 |      row2' = (row2+row3)/2 (z [-1,1] -> [0,1])
//            | 0   0  1/2 1/2 |
//            | 0   0   0   1 |
//    i.e. clip' = F * clip: y' = -y, z' = (z + w) / 2. The z half makes the
//    post-divide depth ((ndc+1)/2) EQUAL the value GL's default glDepthRange
//    stores — the depth buffer's CONTENTS match GL bit-for-bit, which is what
//    keeps every depth-READING shader unforked.
//
// 2. AdjustProjectionForShaderReconstruction — for matrices SHADER CODE
//    consumes arithmetically against uv/depth values (the
//    `ndc = vec3(uv*2-1, depth*2-1)` reconstruction family: MotionBlurUBO's
//    u_InverseViewProjection, the froxel fog InverseProjection, prev-frame
//    reprojection). On Vulkan it premultiplies Y = diag(1, -1, 1, 1) — the
//    y flip ONLY. The z half must NOT be applied here: the depth VALUES those
//    shaders read are GL-shaped by construction (see above), and the shader's
//    own `*2-1` / `*0.5+0.5` remaps already convert them; composing F's z
//    row on top double-applies the remap and reconstructs every non-far pixel
//    at the wrong depth. Only the ROW FLIP survives into sampled data (uv v=0
//    is the top row on Vulkan), so only the row flip belongs in the matrix.
//
// UPLOADED INVERSES are recomputed from the adjusted forward matrix —
// glm::inverse(AdjustProjectionForShaderReconstruction(m)) — never
// adjusted-after-inverting; AdjustedInverseForShaderReconstruction below is
// that composition spelled once. (On GL both functions are identity, so the
// recomputed inverse is bit-identical to inverting the input directly.)
//
// The winding half of the flip (A1): F mirrors the screen vertically, but
// this does NOT need a compensating winding swap, and adding one is a bug we
// already shipped once. Vulkan computes the facing determinant in FRAMEBUFFER
// coordinates, whose y points DOWN where GL's window y points UP; that
// inversion and the seam's clip-y negation compose to identity, so a triangle
// GL calls front-facing is front-facing here. VulkanPipelineBuilder therefore
// translates a recorded GL winding to the SAME VkFrontFace (see the long note
// at its vkCmdSetFrontFace call). Call sites never hand-flip winding either.
//
// KNOWN LIMIT (deliberate, documented for the later Wave C items): a
// DIRECTION-ADDRESSED capture (cubemap face bakes — SkyCubemapBake,
// IBLPrecompute, DDGI capture atlas) rasterizes with F like everything else,
// which stores each face row-flipped relative to the GL bake while cubemap
// direction->texel addressing is API-identical. Those passes are dormant on
// Vulkan in this batch; when their port lands (items 10-15), the face bases
// must compensate (or the bake target must flip at readback) — tracked in
// the Wave C notes, NOT solved by sprinkling extra flips at call sites.
// =============================================================================

#include <glm/glm.hpp>

namespace OloEngine::RHI
{
    // Rasterizer-consumed projections (gl_Position path). Identity on GL;
    // clip' = F * clip (y flip + z [-1,1]->[0,1]) on Vulkan.
    [[nodiscard]] glm::mat4 AdjustProjectionForBackend(const glm::mat4& projection);

    // Shader-arithmetic-consumed projections (uv/depth reconstruction and
    // reprojection). Identity on GL; clip' = diag(1,-1,1,1) * clip on Vulkan.
    [[nodiscard]] glm::mat4 AdjustProjectionForShaderReconstruction(const glm::mat4& projection);

    // The uploaded-inverse form: glm::inverse(AdjustProjectionForShaderReconstruction(m)).
    // Spelled here so call sites cannot drift into inverting first.
    [[nodiscard]] glm::mat4 AdjustedInverseForShaderReconstruction(const glm::mat4& forward);

    // The KNOWN LIMIT above, closed for the passes that opt in (#691 Wave C
    // item 15). A DIRECTION-ADDRESSED capture — a cubemap/atlas face bake whose
    // consumer addresses the result by DIRECTION, not by screen uv — wants the
    // z half of the seam and NOT the y half. The y flip exists so that a
    // SCREEN-space uv convention lines up; a face bake has no screen, and the
    // flip only costs it correctness: with it, each face lands ROW-MIRRORED
    // relative to the GL bake (memory row 0 is clip y = -w on BOTH backends, so
    // negating clip y negates which row a world point lands in) while
    // direction->texel addressing stays API-identical. The relight then reads
    // the wrong row of the right face.
    //
    // Applying only the z remap leaves the stored rows byte-identical to GL's
    // and still gives the depth buffer its GL-shaped contents. Identity on GL.
    //
    // NOT expressible by negating the face's UP vector: lookAt derives right =
    // cross(forward, up), so negating up flips the RIGHT axis too — a 180-degree
    // roll, not a mirror.
    //
    // A capture that CULLS would need its winding reconsidered (without the y
    // flip the framebuffer-space facing determinant matches GL's window-space
    // one, which is the opposite of what the screen path composes); the engine's
    // face bakes all rasterize with culling disabled, so the question is moot
    // today and is called out here rather than guessed at.
    [[nodiscard]] glm::mat4 AdjustCaptureProjectionForBackend(const glm::mat4& projection);

    // The ROW-ORDER half of the same seam (#691 Phase 9, ADR 0011 amendment
    // (85)): every off-screen target is bottom-up on GL and top-down on
    // Vulkan, so this one predicate answers "does a top-left-origin consumer
    // have to flip?" for every reader — ImGui uv pairs, PNG row emission,
    // readback rect origins, entity-picking coordinates.
    //
    // It lives HERE, with the orientation convention it belongs to, rather
    // than in any one consumer: it was briefly duplicated as an inline
    // `GetAPI() == OpenGL` in three places, which is the same
    // enumerate-by-consumer drift amendment (59) is about (review finding).
    // ImGuiLayer::RenderTargetRowsAreBottomUp() is the uv-facing spelling and
    // forwards here; it is not a second source of truth.
    //
    // NOT for file-loaded textures: the stbi loader uploads those pre-flipped,
    // so they take the fixed GL-convention uv pair on both backends.
    [[nodiscard]] bool RenderTargetRowsAreBottomUp();
} // namespace OloEngine::RHI
