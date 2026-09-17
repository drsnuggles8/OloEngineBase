#pragma once

// =============================================================================
// GroomDeformation.h — carrying a bound groom through a deforming body.
// Issue #1249.
//
// THE WHOLE COMPUTATION, IN ONE PARAGRAPH. A binding record says a curve grows
// out of triangle T at barycentric b, and records the orthonormal frame
// (RestOrigin, RestRotation) that triangle had at bind time. Every frame, the
// three corners of T are deformed — morphs are already in the mesh's vertex
// array, skinning is applied here from the shared bone palette — and the SAME
// frame construction is run on them (MakeGroomSurfaceFrame). The strand is then
// carried rigidly from the rest frame to the deformed one:
//
//     local = conjugate(RestRotation) * (P - RestOrigin)
//     P'    = Origin + Rotation * local
//
// That is it. No per-point skinning, no blending, no simulation — see
// GroomBinding.h for why rigid is the right and the safe choice here.
//
// WHY THE SKINNING IS DONE HERE AND IS NOT A SECOND BODY-SKINNING SYSTEM. The
// issue says not to build one, and this is not one: it evaluates the SHARED
// deformation output (#1226's SkeletonData::m_FinalBoneMatrices and its
// previous-pose twin) at three vertices per strand root. A 40 000-strand coat
// touches 120 000 vertex evaluations at most, against a body mesh of 20 000
// vertices — and it touches only the vertices the roots actually sit on. The
// alternative, reading a deformed vertex buffer somebody else produced, does
// not exist on the raster path: every raster consumer deforms inside its own
// vertex stage and keeps nothing (skeletal-deformation-shared-output.md), and
// the one place that does materialize deformed vertices —
// RayTracing::DeformedSurfaceCache — is Vulkan-RT-only by construction.
//
// PREVIOUS-FRAME POSITIONS ARE COMPUTED THE SAME WAY, FROM THE PREVIOUS
// PALETTE, and that is what makes them meaningful. The alternative — carrying
// last frame's deformed positions forward in a buffer — is what ghosts: after
// an LOD switch or a teleport the buffer holds positions from a different
// surface, and nothing in it says so. Deriving the previous position from the
// previous POSE means a discontinuity is expressible: the caller holds prev
// equal to current and the frame emits exactly zero motion, which is the honest
// answer and the one SkeletalDeformationSystem already gives for bones.
//
// MORPHS ARRIVE FOR FREE AND THAT IS NOT AN ACCIDENT. MorphTargetSystem applies
// morph deltas on the CPU straight into the MeshSource vertex array, so the
// positions this file reads through GroomSurfaceView ALREADY carry the current
// expression. A face that blinks moves its lashes with no morph-specific code
// here at all. What morphs do NOT arrive with is a previous-frame surface —
// there is only one vertex array and it holds this frame's expression — which
// is exactly the rejection MorphDeformationSystem already owns for the body,
// and the caller is expected to honour it for the coat too
// (GroomHistoryResetCause::MorphSurfaceChanged).
// =============================================================================

#include "OloEngine/Core/Base.h"
#include "OloEngine/Groom/GroomBinding.h"
#include "OloEngine/Groom/GroomSurfaceFrame.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <span>
#include <string_view>
#include <vector>

namespace OloEngine
{
    class GroomAsset;

    /**
     * @brief Why a bound groom's previous-frame positions were thrown away.
     *
     * Mirrors the vocabulary of Animation::DeformationHistoryResetCause
     * deliberately — these are the same events seen from the coat's side — and
     * is a separate enum for the reason that one is separate from
     * TemporalHistoryInvalidationCause: the groom's history is per-entity CPU
     * state with causes a skeleton does not have (a binding swapped, a groom
     * re-cooked), and folding them into the skeleton's enum would put groom
     * concepts in a header every skinned consumer includes.
     *
     * Every reset is attributed. A motion vector computed across a discontinuity
     * is not a crash and not a test failure — it is a plausible wrong image that
     * shows up as a smear down the length of the coat — so the only defence is
     * that resets are counted and their cause is nameable.
     */
    enum class GroomHistoryResetCause : u8
    {
        None = 0,
        /// No previous frame yet — the first frame this groom was deformed.
        FirstUse,
        /// The bound target entity changed, or its mesh source was swapped.
        TargetChanged,
        /// The target's triangle count or connectivity changed — a conventional
        /// LOD switch. Different topology is a different surface, so the
        /// previous positions describe a coat that no longer exists.
        TargetTopologyChanged,
        /// The bone palette lost its history: the skeleton was replaced, its
        /// bone count changed, or an animation was reset or re-seeked. Read
        /// from the shared deformation output rather than guessed at.
        AnimationReset,
        /// This groom was not deformed on the frame the previous pose
        /// describes — it was disabled, refused, or its entity was not
        /// submitted. The body's previous pose is perfectly good; what is
        /// missing is a previous COAT to measure against it, and a velocity
        /// computed anyway would be the body's motion applied to a coat that
        /// was standing at its bind pose.
        DeformationSkipped,
        /// The entity moved discontinuously, or the view cut.
        Teleport,
        /// The morphed rest surface this frame is not the one last frame drew.
        /// There is only ever one morphed vertex array, so a previous position
        /// derived from it would be this frame's expression in last frame's
        /// pose — a hybrid that is neither.
        MorphSurfaceChanged,
        /// The binding or the groom asset behind this entity was swapped.
        BindingChanged,
        /// Scene load, play-mode entry/exit, or another wholesale state change.
        SceneTransition,
        /// Asked for explicitly by editor tooling or a test.
        Manual,

        Count
    };

    [[nodiscard]] std::string_view ToString(GroomHistoryResetCause cause);

    [[nodiscard]] inline constexpr bool IsValidGroomHistoryResetCause(i32 value) noexcept
    {
        return value >= 0 && value < static_cast<i32>(GroomHistoryResetCause::Count);
    }

    /**
     * @brief The skinning half of the surface, as a strided view.
     *
     * Deliberately NOT `MeshSource`: this file must stay buildable, and
     * testable, without the renderer. The caller points these at the mesh's
     * BoneInfluence array — the ids and the weights live in one 32-byte struct,
     * so both pointers address the same array at different offsets and share
     * its stride.
     *
     * An EMPTY view (no influences, or an empty palette) is not an error. It is
     * a morph-only or static target, which is a legitimate thing to bind a groom
     * to — a face rig with no skeleton is exactly that — and the deformation
     * then comes entirely from the vertex positions.
     */
    struct GroomSkinningView
    {
        /// Four u32 bone ids per vertex.
        const u32* BoneIds = nullptr;
        /// Four f32 weights per vertex, parallel to the ids.
        const f32* Weights = nullptr;
        /// Byte stride between one vertex's ids (or weights) and the next.
        u32 Stride = 0;
        u32 VertexCount = 0;

        /// The current and previous final bone matrices — #1226's shared
        /// deformation output, borrowed for the call and never retained.
        std::span<const glm::mat4> Palette{};
        std::span<const glm::mat4> PrevPalette{};

        /// Whether `PrevPalette` holds a pose this skeleton was really in.
        /// SkeletonData::HasBoneHistory(), passed through rather than re-derived
        /// — a second spelling of that fact is a second thing that can disagree
        /// with it.
        bool HasPreviousPose = false;

        /// True when there is anything to skin with at all.
        [[nodiscard]] bool IsSkinned() const noexcept
        {
            return BoneIds != nullptr && Weights != nullptr && Stride >= 32u && VertexCount > 0u &&
                   !Palette.empty();
        }
    };

    /**
     * @brief Everything a frame's deformation reads, in one argument.
     *
     * `Surface` holds the LIVE vertex array — morph-deformed in place by
     * MorphTargetSystem, which is why morphs need no separate input here.
     */
    struct GroomDeformationInputs
    {
        GroomSurfaceView Surface{};
        GroomSkinningView Skinning{};

        /// False when the caller has already decided this frame's previous
        /// positions are not comparable (a teleport, an LOD switch, the first
        /// frame). The evaluation then writes prev == current, so the frame
        /// emits exactly zero motion instead of a velocity between two
        /// unrelated surfaces.
        bool HasHistory = false;
    };

    /**
     * @brief One curve root's rest-to-deformed rigid transform, both frames.
     *
     * `Valid` is false when the deformed triangle had no frame (degenerate this
     * pose, or a corrupt record survived into this array). The strand is then
     * held at its REST position rather than sent somewhere plausible — a patch
     * of coat that does not move is a diagnosable symptom; a patch that flies
     * off the model is a bug report about the wrong subsystem.
     */
    struct GroomRootTransform
    {
        glm::vec3 Origin{ 0.0f };
        glm::quat Rotation{ 1.0f, 0.0f, 0.0f, 0.0f };
        glm::vec3 PrevOrigin{ 0.0f };
        glm::quat PrevRotation{ 1.0f, 0.0f, 0.0f, 0.0f };
        bool Valid = false;

        /// True when this root WAS evaluated and its deformed triangle had no
        /// frame. Distinct from `!Valid`, which is also the state of a curve the
        /// strand budget never selected — and the two must not be confused: a
        /// consumer that treats every invalid entry as a degeneracy reports one
        /// for most of the groom whenever a budget or a guides-only view is in
        /// effect, which is a false alarm about the body's geometry.
        bool Held = false;

        [[nodiscard]] bool operator==(const GroomRootTransform&) const = default;
    };

    /// What a frame's evaluation did, for the renderer's statistics panel and
    /// for the evidence a test asserts on. Counters, not log lines: a refusal
    /// nobody can count reads as "this never happens".
    struct GroomDeformationStats
    {
        /// Roots whose deformed frame was built successfully.
        u32 RootsDeformed = 0;
        /// Roots held at rest because the deformed triangle is degenerate this
        /// pose. Non-zero means the BODY has degenerate triangles under this
        /// animation, which is a fact about the body.
        u32 RootsHeldDegenerate = 0;
        /// Roots skipped because the record addressed a triangle out of range.
        /// Unreachable through a validated binding on a matching target; counted
        /// because the alternative to counting it is reading out of bounds.
        u32 RootsSkippedOutOfRange = 0;
        /// Roots whose skinning weights summed to zero — an unweighted vertex.
        /// The vertex is used unskinned, which is what the GPU skinning path
        /// does with the same input, so the coat matches the body it sits on.
        u32 VerticesUnweighted = 0;
        /// True when the previous transforms are genuinely the previous frame's.
        /// When false every PrevOrigin/PrevRotation aliases its current twin.
        bool HasHistory = false;

        [[nodiscard]] bool operator==(const GroomDeformationStats&) const = default;
    };

    /**
     * @brief Deform one vertex by the shared bone palette.
     *
     * Exposed because it is the testable half of the evaluation, and because
     * naming it is what stops a second, subtly different skinning loop being
     * written next to it. Linear blend skinning, four influences, exactly as
     * OloDeformSkinnedVertex does on the GPU — a different blend here would make
     * the coat sit off the body by a smoothly varying amount, which reads as a
     * binding error rather than as a skinning one.
     *
     * A vertex whose weights sum to zero is returned UNCHANGED rather than
     * collapsed to the origin, which is what a zero palette sum would otherwise
     * do, and matches the GPU path's behaviour for the same input.
     */
    [[nodiscard]] glm::vec3 SkinGroomSurfaceVertex(const GroomSkinningView& skinning, u32 vertexIndex,
                                                   const glm::vec3& restPosition,
                                                   std::span<const glm::mat4> palette, bool& outWeighted) noexcept;

    /**
     * @brief Carry one of a curve's points from its rest frame to a deformed one.
     *
     * The single application of a root transform in the engine, for the same
     * reason MakeGroomSurfaceFrame is the single construction of a frame: the
     * current and previous positions must be produced by identical arithmetic or
     * the difference between them is not a velocity.
     */
    [[nodiscard]] inline glm::vec3 ApplyGroomRootTransform(const GroomRootBinding& record,
                                                           const GroomRootTransform& transform,
                                                           const glm::vec3& restPoint, bool previous) noexcept
    {
        if (!transform.Valid)
        {
            return restPoint;
        }
        // The rest frame's inverse applied first, so `local` is the point in the
        // triangle's own coordinates — the quantity that is invariant under
        // every deformation the body can undergo.
        const glm::vec3 local = glm::conjugate(record.RestRotation) * (restPoint - record.RestOrigin);
        return previous ? transform.PrevOrigin + transform.PrevRotation * local
                        : transform.Origin + transform.Rotation * local;
    }

    /**
     * @brief Evaluate the rest-to-deformed transform of every selected root.
     *
     * `outTransforms` is resized to the groom's curve count and fully written:
     * a curve not named in `selectedCurves` gets a default (Valid == false)
     * entry, so an index into this array is always safe and a consumer that
     * forgot to select a curve gets a strand at rest rather than a strand
     * indexed out of bounds.
     *
     * @param selectedCurves the curves to evaluate, or EMPTY for all of them.
     *        The ribbon build strides over the groom (GroomStrandMesh.h), so
     *        evaluating only what will be drawn is what keeps a 200k-strand coat
     *        off the frame budget.
     *
     * Pure apart from the output: the same groom, binding and inputs always
     * produce the same transforms, which is what lets a headless test assert on
     * a deformed root position rather than on a picture of one.
     */
    GroomDeformationStats EvaluateGroomRootTransforms(const GroomAsset& groom, const GroomBindingAsset& binding,
                                                      const GroomDeformationInputs& inputs,
                                                      std::span<const u32> selectedCurves,
                                                      std::vector<GroomRootTransform>& outTransforms);
} // namespace OloEngine
