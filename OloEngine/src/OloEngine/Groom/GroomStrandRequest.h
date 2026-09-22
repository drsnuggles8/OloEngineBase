#pragma once

#include "OloEngine/Containers/Array.h"

// =============================================================================
// GroomStrandRequest.h — one groom the frame wants drawn. Issue #1246.
//
// The producer/transport/consumer split is the one RayTracedShadowLightRequest
// established (Renderer/Shadow/ShadowTechnique.h): Scene publishes these
// because it is the only place that holds both the component and the resolved
// asset; Renderer3D carries them; GroomRenderPass decides what each one
// actually gets, because it is the only place that knows what the frame
// resolved. The struct lives beside the groom rather than beside the pass so
// none of the three has to include the render-graph headers to speak about it.
//
// THE ASSET IS RESOLVED BY THE PRODUCER, not by the pass. Two reasons, and the
// second is the one that bites: an AssetManager lookup on the render thread is
// a lock the frame does not need, and a groom built at runtime lives in the
// manager's memory-asset map and in no registry at all — which is exactly the
// case a pass re-resolving by handle got wrong for the debug preview (see
// Scene.cpp's note on IsAssetHandleValid).
// =============================================================================

#include "OloEngine/Asset/Asset.h"
#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Groom/GroomBinding.h"
#include "OloEngine/Groom/GroomDeformation.h"
#include "OloEngine/Groom/GroomGuideInfluence.h"
#include "OloEngine/Groom/GroomGuideSimulation.h"
// The COMPLETE GroomAsset, not a forward declaration: Ref<T> needs T to be a
// complete RefCounted to construct, copy or destroy, so a forward declaration
// here fails in every translation unit that merely HOLDS one of these — which
// after Renderer3D.h includes it is most of the renderer.
#include "OloEngine/Groom/GroomAsset.h"
#include "OloEngine/Groom/GroomCoat.h"
#include "OloEngine/Groom/GroomCoatShadow.h"
#include "OloEngine/Groom/GroomFibreScattering.h"
#include "OloEngine/Groom/GroomStrandMesh.h"
#include "OloEngine/Groom/GroomVisibility.h"

#include <glm/glm.hpp>

#include <vector>

namespace OloEngine
{

    struct GroomStrandRequest
    {
        /// The cooked groom. Never null in a published request — Scene drops
        /// the entity rather than forwarding a null, so the pass has no
        /// "asset missing" branch to get wrong.
        Ref<GroomAsset> Groom;

        /// The asset's handle, carried ONLY as the strand-buffer cache key.
        /// A memory-only groom has a handle too, so this is a usable key even
        /// where the registry knows nothing about the asset.
        AssetHandle Handle = 0;

        /// Composed world transform, and the same from last frame. The
        /// previous one drives the velocity buffer; it aliases the current one
        /// on an entity's first frame, so velocity starts at zero rather than
        /// undefined.
        glm::mat4 Transform{ 1.0f };
        glm::mat4 PreviousTransform{ 1.0f };

        /// Neutral albedo, used when this groom carries NO fibre material —
        /// #1246's unlit coat, which is still what a groom without a
        /// GroomFibreComponent renders.
        glm::vec3 Color{ 0.55f, 0.48f, 0.42f };

        // ── Fibre scattering (#1249's sibling, #1247) ────────────────
        //
        // DERIVED HERE, on the producer side, for the same reason the asset and
        // the deformation are: MakeGroomFibreParams runs a pow, a log and a sin
        // per groom, and the render thread should receive an answer rather than
        // an authoring struct to interpret. It also means the editor preview,
        // the evidence tests and the pass all light from the same bytes.

        /// True when this groom has a fibre material at all. False renders the
        /// neutral ramp, so a scene authored before this feature is unchanged.
        bool Lit = false;

        /// The derived BCSDF parameters. Meaningless unless `Lit`.
        GroomFibreParams Fibre;

        /// Which contribution to render — the separated diagnostic lobes.
        GroomFibreDebugMode FibreDebug = GroomFibreDebugMode::Full;

        /// Root-to-tip brightness floor, the same geometric ramp the debug
        /// preview uses. 1.0 is a flat colour.
        f32 RampFloor = 0.35f;

        /// Multiplies the cooked object-space diameters. An authoring lever
        /// for grooms exported at a different unit scale, not a quality knob.
        f32 WidthScale = 1.0f;

        /// Alpha cutoff for the OpaqueRibbon tier.
        f32 AlphaCutoff = 0.5f;

        /// What this groom ASKED for. What it gets is
        /// SelectGroomComposition's answer, which the pass records.
        GroomCompositionMode RequestedMode = GroomCompositionMode::StochasticAlpha;

        /// Editor picking id.
        i32 EntityID = -1;

        /// How much of the groom to turn into geometry.
        GroomStrandBuildSettings Build;

        // ── Surface binding (#1249) ──────────────────────────────────
        //
        // Null / empty for an unbound groom, which is what makes the unbound
        // path the one that existed before this issue rather than a special case
        // of a new one.
        //
        // The DEFORMATION IS EVALUATED BY THE PRODUCER, not by the pass, for the
        // same two reasons the asset is (see the file header): it needs the
        // scene's bone palettes and the target entity's mesh, neither of which
        // the render thread may touch, and the evaluation is CPU work that must
        // not sit inside a pass. The pass receives an answer and turns it into
        // geometry.

        /// The binding these transforms were produced from. Held so the pass can
        /// read each curve's rest frame, which is half of the rigid transfer.
        Ref<GroomBindingAsset> Binding;

        /// One entry per CURVE of `Groom`, or empty. Sized by
        /// EvaluateGroomRootTransforms, which fills the whole array precisely so
        /// an index into it is always safe.
        TArray<GroomRootTransform> RootTransforms;

        /// What the evaluation did, forwarded to the renderer's statistics panel
        /// so "why is this coat at the bind pose" is answerable from the editor.
        GroomDeformationStats DeformationStats;

        /// Why this groom is NOT bound, when it asked to be. None on a groom
        /// that never asked and on one that attached — the two are distinguished
        /// by `Binding` being null, which is what keeps a scene full of unbound
        /// grooms from reading as a scene full of refusals.
        GroomBindingRejectReason BindingReject = GroomBindingRejectReason::None;

        // ── Coat self-shadowing (#1248) ──────────────────────────────
        //
        // What the coat ASKED for. What it gets is SelectGroomCoatShadow's
        // answer, which depends on what the frame resolved and is recorded in
        // GroomCoatShadowStats — the same producer/transport/consumer split the
        // composition mode above already uses.
        //
        // The representation itself is NOT carried here. It is a GPU resource
        // with a lifetime longer than a frame, so it lives in the pass's cache
        // beside the strand geometry and is keyed the same way; a request
        // carrying one would either copy a volume per frame or hand the render
        // thread an owning reference to something the scene could free.

        /// The requested GroomCoatShadow::CoatShadowMode.
        GroomCoatShadow::CoatShadowMode CoatShadow = GroomCoatShadow::CoatShadowMode::None;

        /// Per-crossing extinction: how opaque one fibre is to direct light.
        /// DIMENSIONLESS and deliberately not the pigment — the pigment already
        /// attenuates inside each fibre in #1247's model, and applying it twice
        /// is the double-count the issue's scope note forbids.
        ///
        /// 1.0 HERE IS NOT THE AUTHORED DEFAULT, which is 4.0 since #1360. This
        /// is a request struct: every real request carries the value
        /// MakeGroomCoatKappa returned from the component, and this initialiser
        /// only decides what a default-constructed request means. It is left at
        /// the neutral 1.0 rather than tracking the component, because a request
        /// nobody filled in should not silently claim a coat four times as
        /// opaque as the one it never read.
        f32 CoatKappa = 1.0f;

        /// The coat volume's resolution policy. The resolution actually used is
        /// this policy's answer at the coat's apparent size, after hysteresis —
        /// see GroomCoatShadow::SelectCoatLodStep.
        GroomCoatShadow::CoatLodPolicy CoatLod{};

        /// March step in VOXELS. 3 is the measured selection: it is at or within
        /// noise of the error minimum on both reference coats and costs a third
        /// of the taps of a one-voxel march. See finding 2 in
        /// docs/analysis/groom-coat-self-shadowing-1248.md.
        f32 CoatStepVoxels = 3.0f;

        // ── Coat authoring (#1251) ───────────────────────────────────
        //
        // Disabled by default, so a groom with no GroomCoatComponent builds
        // exactly the geometry it built before this issue. Assembled by Scene
        // through MakeGroomCoatSettings — the one boundary the component's
        // fields cross — with the two maps resolved to CPU pixels there, because
        // the readback is a graphics call the render thread must not make and
        // because the answer is the same on every frame.
        //
        // The per-group TABLE is not carried here: it belongs to the asset, and
        // `Groom` is already in this request. The pass builds a GroomCoatContext
        // from the two at the point of use, which is the only place both are
        // certainly alive.
        GroomCoatSettings Coat;

        // == Guide simulation (#1250) ==
        //
        // Absent by default, so a groom with no GroomSimulationComponent builds
        // exactly the geometry it built before this issue -- the same shape the
        // coat and the binding above already use.
        //
        // STEPPED BY THE PRODUCER, not by the pass, for the three reasons the
        // deformation is: it needs the scene's bone palettes and the target's
        // mesh, neither of which the render thread may touch; it is per-frame
        // CPU work that must not sit inside a pass; and it must run exactly once
        // per frame, while a pass runs once per CAMERA. That last one is not a
        // performance argument -- a coat stepped twice for a second viewport
        // would advance at twice the rate in a split-screen scene, and the two
        // views would then disagree about where the fur is.

        /// The asset's guide-to-strand influence table, shared by every entity
        /// wearing this groom. Null when this groom is not simulated.
        Ref<GroomGuideInfluenceTable> Influence;

        /// This frame's and last frame's OBJECT-space guide displacements, laid
        /// out by `SimulationGuideOffsets`. Empty when not simulated.
        TArray<glm::vec3> SimulationDisplacements;
        TArray<glm::vec3> SimulationPrevDisplacements;
        TArray<u32> SimulationGuideOffsets;

        /// Table slot -> this frame's guide index, or GroomNoGuide for a slot
        /// the budget did not simulate. Sized by the table's guide count, so a
        /// strand's slot lookup is always in range.
        TArray<u32> SimulationGuideOfSlot;
        /// Guide index -> table slot, the inverse of the above.
        TArray<u32> SimulationSlotOfGuide;

        /// What the step did, forwarded to the renderer's statistics panel so
        /// "why is this coat not moving" is answerable from the editor.
        GroomSimulationStats SimulationStats;

        /// The declared length tolerance the stats above should be read
        /// against. Carried rather than re-read from the component, because the
        /// panel that shows the measurement must show the contract it was taken
        /// against and not this build's default.
        f32 SimulationStretchTolerance = GroomSimulationLimits::DefaultStretchTolerance;

        /// The fitted body proxy, WORLD space, for the debug view. Empty when
        /// the view is off -- a capsule list per groom per frame is not carried
        /// across the bus to be ignored.
        TArray<GroomCollider> SimulationColliders;

        /// The requested debug view. What it can actually DRAW depends on the
        /// editor debug flags, which the pass checks, so this is a request in
        /// exactly the sense CoatShadow and RequestedMode are.
        GroomSimulationDebugView SimulationDebug = GroomSimulationDebugView::None;

        // ── Representation LOD (#1252) ───────────────────────────────
        //
        // Disabled by default, so a groom with no GroomLodComponent builds
        // exactly the geometry it built before this issue — the same shape the
        // coat, the binding and the simulation above already use.
        //
        // DECIDED BY THE PRODUCER, not by the pass, and this one is the least
        // obvious of the four. The other three are producer-side because they
        // need scene data the render thread may not touch; this one is because
        // the decision has to be made ONCE and then drive THREE consumers that
        // do not share a call stack: the strand geometry and the shadow volume
        // are the pass's, and the guide budget is spent in Scene before any
        // pass runs. Deciding in the pass would mean the simulation budget was
        // either a frame behind or derived from a second, parallel evaluation —
        // and two evaluations of a hysteretic decision drift by construction,
        // because each advances its own counters.
        //
        // It is also why the LOD STATE lives in Scene keyed by UUID rather than
        // in the pass's cache: a pass runs once per CAMERA, so a split-screen
        // scene would advance the hold twice per frame and halve it.

        /// This groom's apparent size, in pixels of the render target's
        /// HEIGHT, from EstimateProjectedPixelSize against the engine's LOD
        /// view. Zero when the frame resolved no usable view.
        ///
        /// CARRIED SEPARATELY FROM `Lod.PixelSize`, which is the same number
        /// but only for a groom that opted into representation LOD: a groom
        /// with no GroomLodComponent gets IdentityGroomLodDecision, whose
        /// PixelSize is zero because no decision was made at one. The
        /// ray-tracing proxy (#1253) has to pick a tier for EVERY groom,
        /// opted in or not, so it needs the measurement rather than the
        /// decision's record of it. Computed once, by the producer, and fed
        /// to both.
        f32 ApparentPixelSize = 0.0f;

        /// The authored policy, sanitised. Carried rather than re-read from the
        /// component because the panel that shows a decision must show the
        /// contract it was taken against, and because the pass needs the
        /// compensation cap to apply it.
        GroomLodPolicy LodPolicy;

        /// This frame's answer: the tier, the three budget steps, and the first
        /// reason it is not the tier apparent size asked for.
        GroomLodDecision Lod = IdentityGroomLodDecision();

        /// The cooked level the decision selected, or NULL for the strand tier.
        ///
        /// A RAW POINTER INTO `Groom`, which this request holds alive by Ref for
        /// its whole lifetime — so the pointer cannot outlive the table it
        /// points into. A Ref to the level is not available (a level is a plain
        /// member of the asset, not a RefCounted of its own) and copying one
        /// per frame would be a megabyte of memcpy per groom.
        const GroomLodLevel* LodLevel = nullptr;

        /// The curve set the pass should build from: the level if one was
        /// selected, the base groom otherwise. Assembled at the point of use
        /// for GroomStrandSimulation's reason — a request is MOVED, and a span
        /// stored beside the vector it points into would dangle the moment the
        /// vector reallocated.
        [[nodiscard]] GroomBuildSource BuildSource() const noexcept
        {
            if (LodLevel != nullptr && Groom)
            {
                return GroomBuildSource::FromLevel(*Groom, *LodLevel);
            }
            return Groom ? GroomBuildSource::FromAsset(*Groom) : GroomBuildSource{};
        }

        /// The view over the four arrays above, assembled at the point of use.
        ///
        /// A FUNCTION rather than a stored view, because a GroomStrandRequest is
        /// MOVED (Scene builds it, pushes it into a vector and hands the vector
        /// to Renderer3D) and a span stored beside the vector it points into
        /// would dangle the moment the vector reallocated. Assembling it here
        /// costs nothing and cannot be stale.
        [[nodiscard]] GroomStrandSimulation Simulation() const noexcept
        {
            GroomStrandSimulation simulation;
            if (!Influence)
            {
                return simulation;
            }
            simulation.Influence = Influence.Raw();
            simulation.GuideOfSlot = std::span{ SimulationGuideOfSlot.GetData(), static_cast<sizet>(SimulationGuideOfSlot.Num()) };
            simulation.Displacements.GuideOffsets = std::span{ SimulationGuideOffsets.GetData(), static_cast<sizet>(SimulationGuideOffsets.Num()) };
            simulation.Displacements.Displacements = std::span{ SimulationDisplacements.GetData(), static_cast<sizet>(SimulationDisplacements.Num()) };
            simulation.Displacements.PrevDisplacements = std::span{ SimulationPrevDisplacements.GetData(), static_cast<sizet>(SimulationPrevDisplacements.Num()) };
            simulation.Displacements.SlotOfGuide = std::span{ SimulationSlotOfGuide.GetData(), static_cast<sizet>(SimulationSlotOfGuide.Num()) };
            return simulation;
        }
    };

    // Published per frame into a TArray, so growth relocates it BITWISE.
    // Every member is a Ref, a TArray, a POD or an enum: the owning ones all
    // point at separately allocated blocks and nothing points back into the
    // request. Derived member-by-member rather than asserted, so a field that
    // is not relocatable breaks the build here instead of corrupting a frame.
    template<>
    struct TIsTriviallyRelocatable<GroomStrandRequest>
    {
        static constexpr bool Value = TIsTriviallyRelocatable_V<decltype(GroomStrandRequest::Groom)> &&
                                      TIsTriviallyRelocatable_V<decltype(GroomStrandRequest::Handle)> &&
                                      TIsTriviallyRelocatable_V<decltype(GroomStrandRequest::Transform)> &&
                                      TIsTriviallyRelocatable_V<decltype(GroomStrandRequest::PreviousTransform)> &&
                                      TIsTriviallyRelocatable_V<decltype(GroomStrandRequest::Color)> &&
                                      TIsTriviallyRelocatable_V<decltype(GroomStrandRequest::Lit)> &&
                                      TIsTriviallyRelocatable_V<decltype(GroomStrandRequest::Fibre)> &&
                                      TIsTriviallyRelocatable_V<decltype(GroomStrandRequest::FibreDebug)> &&
                                      TIsTriviallyRelocatable_V<decltype(GroomStrandRequest::RampFloor)> &&
                                      TIsTriviallyRelocatable_V<decltype(GroomStrandRequest::WidthScale)> &&
                                      TIsTriviallyRelocatable_V<decltype(GroomStrandRequest::AlphaCutoff)> &&
                                      TIsTriviallyRelocatable_V<decltype(GroomStrandRequest::RequestedMode)> &&
                                      TIsTriviallyRelocatable_V<decltype(GroomStrandRequest::EntityID)> &&
                                      TIsTriviallyRelocatable_V<decltype(GroomStrandRequest::Build)> &&
                                      TIsTriviallyRelocatable_V<decltype(GroomStrandRequest::Binding)> &&
                                      TIsTriviallyRelocatable_V<decltype(GroomStrandRequest::RootTransforms)> &&
                                      TIsTriviallyRelocatable_V<decltype(GroomStrandRequest::DeformationStats)> &&
                                      TIsTriviallyRelocatable_V<decltype(GroomStrandRequest::BindingReject)> &&
                                      TIsTriviallyRelocatable_V<decltype(GroomStrandRequest::CoatShadow)> &&
                                      TIsTriviallyRelocatable_V<decltype(GroomStrandRequest::CoatKappa)> &&
                                      TIsTriviallyRelocatable_V<decltype(GroomStrandRequest::CoatLod)> &&
                                      TIsTriviallyRelocatable_V<decltype(GroomStrandRequest::CoatStepVoxels)> &&
                                      TIsTriviallyRelocatable_V<decltype(GroomStrandRequest::Coat)> &&
                                      TIsTriviallyRelocatable_V<decltype(GroomStrandRequest::Influence)> &&
                                      TIsTriviallyRelocatable_V<decltype(GroomStrandRequest::SimulationDisplacements)> &&
                                      TIsTriviallyRelocatable_V<decltype(GroomStrandRequest::SimulationPrevDisplacements)> &&
                                      TIsTriviallyRelocatable_V<decltype(GroomStrandRequest::SimulationGuideOffsets)> &&
                                      TIsTriviallyRelocatable_V<decltype(GroomStrandRequest::SimulationGuideOfSlot)> &&
                                      TIsTriviallyRelocatable_V<decltype(GroomStrandRequest::SimulationSlotOfGuide)> &&
                                      TIsTriviallyRelocatable_V<decltype(GroomStrandRequest::SimulationStats)> &&
                                      TIsTriviallyRelocatable_V<decltype(GroomStrandRequest::SimulationStretchTolerance)> &&
                                      TIsTriviallyRelocatable_V<decltype(GroomStrandRequest::SimulationColliders)> &&
                                      TIsTriviallyRelocatable_V<decltype(GroomStrandRequest::SimulationDebug)> &&
                                      TIsTriviallyRelocatable_V<decltype(GroomStrandRequest::ApparentPixelSize)> &&
                                      TIsTriviallyRelocatable_V<decltype(GroomStrandRequest::LodPolicy)> &&
                                      TIsTriviallyRelocatable_V<decltype(GroomStrandRequest::Lod)> &&
                                      TIsTriviallyRelocatable_V<decltype(GroomStrandRequest::LodLevel)>;
    };
} // namespace OloEngine
