#pragma once

// =============================================================================
// GroomBindingAuthoring.h — building a groom binding from a scene. Issue #1249.
//
// The ONE implementation of "bind this entity's groom to that entity's body,
// cook it, write it, import it", shared by the inspector's Build Binding button
// and by the olo_groom_bind automation command.
//
// It is shared rather than duplicated for the reason #1249 has had to learn
// twice already: the editor and the runtime resolved the target surface two
// different ways, and the result was a green "Attaches" next to a coat the
// renderer was refusing. A second bind path that differed from the button's by
// one line — the search radius, the output directory, which space the roots
// were projected in — would be the same bug wearing different clothes.
// =============================================================================

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Groom/GroomBinding.h"
#include "OloEngine/Groom/GroomBindingBuilder.h"
#include "OloEngine/Groom/GroomSurfaceFrame.h"
#include "OloEngine/Scene/Entity.h"

#include <filesystem>
#include <string>

namespace OloEngine
{
    class MeshSource;
    class Scene;
    class Skeleton;
} // namespace OloEngine

namespace OloEngine::GroomAuthoring
{
    /// The body a binding targets, resolved exactly as the runtime resolves it:
    /// the animated surface the deformation pass writes (#1227), and the
    /// skeleton from the SkeletonComponent before the MeshSource's.
    struct ResolvedTarget
    {
        Ref<MeshSource> m_Surface;
        const Skeleton* m_Skeleton = nullptr;

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return static_cast<bool>(m_Surface);
        }
    };

    [[nodiscard]] ResolvedTarget ResolveTarget(Entity targetEntity);

    /// A strided view over `surface`, with the skeleton identity the signature
    /// needs. Borrows the mesh's live arrays; never outlives them.
    [[nodiscard]] GroomSurfaceView MakeSurfaceView(const MeshSource& surface, const Skeleton* skeleton);

    /// The matrix that takes the target's object space into the groom's — see
    /// GroomBindingBuildSettings::SurfaceToGroom for why a binding is expressed
    /// in the groom's space and not the body's.
    [[nodiscard]] glm::mat4 SurfaceToGroomMatrix(Scene& scene, Entity groomEntity, Entity targetEntity);

    struct BindOutcome
    {
        bool m_Ok = false;
        std::string m_Reason; ///< empty on success; a sentence otherwise
        AssetHandle m_Binding = 0;
        std::filesystem::path m_RelativePath;
        GroomBindingBuildStats m_Stats;
    };

    /**
     * @brief Bind `groomEntity`'s groom to `targetEntity`'s body and import it.
     *
     * Cooks the binding, writes it NEXT TO THE GROOM (so the pair travels
     * together in the content browser and an orphan is visible rather than filed
     * away), imports it, and hands back the handle. Does NOT touch the
     * component — the caller assigns the handle, because the two callers differ
     * in how that assignment reaches the undo stack.
     *
     * Every failure returns a reason rather than logging one: the automation
     * command has to put it in a tool result, and a function that only logged
     * would force it to invent a message.
     */
    [[nodiscard]] BindOutcome BuildAndImportBinding(Scene& scene, Entity groomEntity, Entity targetEntity,
                                                    f32 searchRadius);
} // namespace OloEngine::GroomAuthoring
